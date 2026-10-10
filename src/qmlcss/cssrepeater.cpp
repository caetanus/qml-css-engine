#include "qmlcss/cssrepeater.h"

#include "qmlcss/cssbox.h"
#include "qmlcss/csscontentholder.h"
#include "qmlcss/csslayout.h"
#include "qmlcss/csslength.h"
#include "csstheme_p.h" // LayoutBatchGuard

#include <QAccessible>
#include <QDebug>
#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QJSValue>
#include <QMetaMethod>
#include <QQmlContext>
#include <QQmlEngine>
#include <QVarLengthArray>
#include <algorithm>
#include <cmath>

namespace QmlCss {

namespace {

// The model arrives from QML bindings as a QJSValue array; normalize to a QVariantList.
QVariantList toList(const QVariant &v)
{
    if (v.metaType().id() == qMetaTypeId<QJSValue>())
        return toList(v.value<QJSValue>().toVariant());
    if (v.metaType().id() == QMetaType::QVariantList)
        return v.toList();
    if (v.metaType().id() == QMetaType::QStringList) {
        QVariantList out;
        for (const QString &s : v.toStringList())
            out.append(s);
        return out;
    }
    // A number N behaves like Repeater's integer model: N empty rows.
    bool ok = false;
    const int n = v.toInt(&ok);
    if (ok && n > 0) {
        QVariantList out;
        for (int i = 0; i < n; ++i)
            out.append(i);
        return out;
    }
    return {};
}


} // namespace

CssRepeater::CssRepeater(QQuickItem *parent)
    : QQuickItem(parent)
{
    setVisible(false);
    setFlag(ItemHasContents, false);
}

CssRepeater::~CssRepeater()
{
    // Each row leaving the holder requests a relayout of the container; unbatched, every one
    // re-laid the remaining rows synchronously (O(N^2) per destroyed list). One flush at the end
    // settles a surviving container; a dying one no longer receives the requests (its
    // connections are gone by the time ~QObject deletes us), so nothing touches it. m_layout is a
    // QPointer: the guard is a no-op if the engine already died.
    const Detail::LayoutBatchGuard batch(m_layout);
    if (m_layout && m_registeredHolder)
        m_layout->unregisterVirtualRepeater(m_registeredHolder, this);
    for (Row &row : m_rows)
        destroyRow(row);
    m_rows.clear();
}

void CssRepeater::setModel(const QVariant &v)
{
    m_model = v;
    emit modelChanged();
    if (isComponentComplete())
        rebuild();
}

void CssRepeater::setDelegate(QQmlComponent *c)
{
    if (m_delegate == c)
        return;
    m_delegate = c;
    emit delegateChanged();
    if (isComponentComplete())
        rebuild();
}

void CssRepeater::setVirtualize(bool v)
{
    if (m_virtualize == v)
        return;
    m_virtualize = v;
    emit virtualizeChanged();
    if (!isComponentComplete())
        return;
    registerWithEngine();
    if (!m_virtualize) {
        // Back to plain eager: every logical row gets its item.
        watchFlickable(nullptr);
        m_mode = Mode::Eager;
        const Detail::LayoutBatchGuard batch(m_layout);
        if (materialize(0, int(m_rows.size()) - 1, false)) {
            restack();
            notifyLayout();
        }
        return;
    }
    // Rows that exist stay until the holder's next layout decides to window them.
    notifyLayout();
}

void CssRepeater::componentComplete()
{
    QQuickItem::componentComplete();
    if (QQmlContext *ctx = qmlContext(this))
        m_layout = qobject_cast<CssLayoutEngine *>(ctx->contextProperty(QStringLiteral("cssLayout")).value<QObject *>());
    registerWithEngine();
    // Virtual and registered: materialize nothing yet — the holder's first layout (which runs when
    // its box completes, with the scroll composed and the container style resolved) decides between
    // the window and eager. Creating every row first would defeat the point for a 10k-row model.
    if (m_registeredHolder)
        m_mode = Mode::Pending;
    rebuild();
}

void CssRepeater::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);
    if (change != ItemParentHasChanged || !isComponentComplete() || !m_virtualize)
        return;
    registerWithEngine();
    if (!m_registeredHolder && m_mode != Mode::Eager) {
        // No layout will window us any more (detached, or a holder already taken): eager.
        watchFlickable(nullptr);
        m_mode = Mode::Eager;
    }
}

void CssRepeater::registerWithEngine()
{
    QQuickItem *holder = m_virtualize ? parentItem() : nullptr;
    if (m_registeredHolder && m_registeredHolder != holder) {
        if (m_layout)
            m_layout->unregisterVirtualRepeater(m_registeredHolder, this);
        m_registeredHolder = nullptr;
    }
    // Only a box's content holder is laid out by the engine (and so ever syncs us).
    if (!holder || !m_layout || m_registeredHolder || !qobject_cast<CssContentHolder *>(holder))
        return;
    if (m_layout->registerVirtualRepeater(holder, this))
        m_registeredHolder = holder;
}

void CssRepeater::destroyRow(Row &row)
{
    if (!row.item && !row.context)
        return; // a logical row that was never materialized (virtual mode)
    QElapsedTimer t;
    if (Detail::g_phaseStats.enabled)
        t.start();
    if (row.item) {
        --m_matCount;
        row.item->setParentItem(nullptr);
        row.item->deleteLater();
    }
    if (row.context)
        row.context->deleteLater();
    row.item = nullptr;
    row.context = nullptr;
    if (Detail::g_phaseStats.enabled)
        Detail::g_phaseStats.time("repeater.destroyRow", t.nsecsElapsed());
}

namespace {

// An entry's key when it is an object with an "id" (a JSON row from the app's model); empty
// otherwise.
QString keyOf(const QVariant &v)
{
    if (v.metaType().id() != QMetaType::QVariantMap)
        return {};
    const QVariantMap m = v.toMap();
    const auto it = m.constFind(QStringLiteral("id"));
    return it == m.cend() ? QString() : it->toString();
}

} // namespace

bool CssRepeater::createRow(int i, const QVariant &data, Row &row)
{
    row.data = data;
    // createRow stores the context via qmlContext(item) — keep our own pointer for index
    // updates on later moves.
    QQmlContext *creation = m_delegate->creationContext();
    if (!creation)
        creation = qmlContext(this);
    QElapsedTimer t;
    if (Detail::g_phaseStats.enabled)
        t.start();
    auto *ctx = new QQmlContext(creation, this);
    ctx->setBaseUrl(creation->baseUrl());
    ctx->setContextProperty(QStringLiteral("modelData"), data);
    ctx->setContextProperty(QStringLiteral("index"), i);
    if (Detail::g_phaseStats.enabled)
        Detail::g_phaseStats.time("repeater.contextCreate", t.nsecsElapsed());
    // STAGED creation (C22): parent the row BEFORE it completes. create() would run componentComplete()
    // on a parentless row — every styled element resolves its CSS against a truncated ancestor chain,
    // then resolves all over again when the row joins the window (the scene-join heal): 2x per element.
    if (Detail::g_phaseStats.enabled)
        t.start();
    QObject *o = m_delegate->beginCreate(ctx);
    if (Detail::g_phaseStats.enabled)
        Detail::g_phaseStats.time("repeater.beginCreate", t.nsecsElapsed());
    auto *item = qobject_cast<QQuickItem *>(o);
    if (item) {
        item->setParent(this);
        item->setParentItem(parentItem() ? parentItem() : this);
    }
    if (Detail::g_phaseStats.enabled)
        t.start();
    m_delegate->completeCreate(); // also required after a null beginCreate() (Qt's own create() does it)
    if (Detail::g_phaseStats.enabled)
        Detail::g_phaseStats.time("repeater.completeCreate", t.nsecsElapsed());
    if (!item) {
        if (o)
            o->deleteLater();
        delete ctx;
        qWarning("CssRepeater: failed to create delegate: %s", qPrintable(m_delegate->errorString()));
        return false;
    }
    row.item = item;
    row.context = ctx;
    ++m_matCount;
    return true;
}

void CssRepeater::rebuild()
{
    if (!m_delegate)
        return;
    // One reconciliation = ONE layout flush. Without the batch every created/destroyed row fired the
    // holder's childrenChanged -> a synchronous full relayout of the container: O(N^2) per append/clear.
    // The guard flushes on scope exit, so geometry is settled when rebuild() returns (same contract) —
    // when this is the OUTERMOST batch. Nested inside another one (a <For> in a delegate being created,
    // a theme reapply, the loader's mount), settling happens where that outer batch closes.
    const Detail::LayoutBatchGuard batch(m_layout);
    QElapsedTimer t;
    if (Detail::g_phaseStats.enabled)
        t.start();
    const QVariantList data = toList(m_model);
    if (Detail::g_phaseStats.enabled) {
        Detail::g_phaseStats.time("repeater.toList", t.nsecsElapsed());
        t.start();
    }

    // KEYED when every entry is an object with a distinct "id": rows match by id in O(n), and a
    // surviving row whose content changed gets the new value as its modelData IN PLACE. A model
    // rebuilt from fresh JSON (every object new, most of them equal, some edited) keeps every
    // delegate instead of re-creating the edited ones — and skips the O(n²) value matching.
    QStringList keys;
    keys.reserve(data.size());
    bool keyed = !data.isEmpty();
    {
        QSet<QString> seen;
        for (const QVariant &d : data) {
            const QString k = keyOf(d);
            if (k.isEmpty() || seen.contains(k)) { keyed = false; break; }
            seen.insert(k);
            keys.append(k);
        }
    }

    // m_rows holds EVERY logical entry (an item only when materialized): matching runs over all of
    // them, so keys/order survive for rows outside a virtual window, and only materialize() below
    // creates or destroys items.
    QVector<Row> next;
    next.reserve(data.size());
    QVector<bool> used(m_rows.size(), false);
    bool changed = false;
    QHash<QString, int> byKey;
    if (keyed) {
        for (int j = 0; j < m_rows.size(); ++j) {
            const QString k = keyOf(m_rows[j].data);
            if (!k.isEmpty() && !byKey.contains(k))
                byKey.insert(k, j);
        }
    }
    if (Detail::g_phaseStats.enabled) {
        // Key building + byKey: one sample for the whole up-front pass (the survivor scan below
        // adds its own, per data entry).
        Detail::g_phaseStats.time("repeater.match", t.nsecsElapsed());
    }
    for (int i = 0; i < data.size(); ++i) {
        if (Detail::g_phaseStats.enabled)
            t.start();
        int found = -1;
        if (keyed) {
            const auto it = byKey.constFind(keys.at(i));
            if (it != byKey.cend() && !used[*it])
                found = *it;
        } else {
            // Reconcile by VALUE, stable first-match: surviving entries keep their delegate.
            for (int j = 0; j < m_rows.size(); ++j) {
                if (!used[j] && m_rows[j].data == data.at(i)) {
                    found = j;
                    break;
                }
            }
        }
        if (Detail::g_phaseStats.enabled)
            Detail::g_phaseStats.time("repeater.match", t.nsecsElapsed());
        if (found >= 0) {
            used[found] = true;
            Row row = m_rows[found];
            if (keyed && row.data != data.at(i)) {
                row.data = data.at(i);
                if (row.context)
                    row.context->setContextProperty(QStringLiteral("modelData"), row.data);
            }
            // Only a row that MOVED gets a new index (its old index is its old position `found`):
            // re-setting an unchanged value still notifies every binding that reads `index`.
            if (row.context && found != i) {
                if (Detail::g_phaseStats.enabled)
                    t.start();
                row.context->setContextProperty(QStringLiteral("index"), i);
                if (Detail::g_phaseStats.enabled)
                    Detail::g_phaseStats.time("repeater.index", t.nsecsElapsed());
            }
            if (found != i)
                changed = true;
            next.append(row);
        } else {
            Row row;
            row.data = data.at(i);
            next.append(row);
            changed = true;
        }
    }
    for (int j = 0; j < m_rows.size(); ++j) {
        if (!used[j]) {
            destroyRow(m_rows[j]);
            changed = true;
        }
    }
    m_rows = next;
    // Survivors may now sit anywhere (a swap moves a materialized row out of the window): sweep every
    // logical row once — the matching above was O(n) already.
    int a, b;
    targetRange(&a, &b);
    if (materialize(a, b, true))
        changed = true;
    if (changed) {
        if (Detail::g_phaseStats.enabled)
            t.start();
        restack();
        if (Detail::g_phaseStats.enabled)
            Detail::g_phaseStats.time("repeater.restack", t.nsecsElapsed());
        notifyLayout();
    }
}

bool CssRepeater::materialize(int a, int b, bool sweepAll)
{
    const int n = int(m_rows.size());
    a = std::max(a, 0);
    b = std::min(b, n - 1);
    bool changed = false;
    // Rows leaving: everything outside [a, b] (only the previous range can hold items, unless a
    // reconciliation just moved them).
    const int lo = sweepAll ? 0 : std::max(m_matFirst, 0);
    const int hi = sweepAll ? n - 1 : std::min(m_matLast, n - 1);
    for (int i = lo; i <= hi; ++i) {
        if ((i < a || i > b) && (m_rows[i].item || m_rows[i].context)) {
            destroyRow(m_rows[i]);
            changed = true;
        }
    }
    // Rows entering, in model order. setParentItem() APPENDS a new item to the holder's children (in
    // a header/footer-wrapped list that lands it after the footer): stack it after its model
    // predecessor right away (or after US for the first row), so restack()'s fast path finds
    // everything already in place and never falls back to the O(n^2) loop just because we
    // create/append next to a static sibling. Creating at the end, the new item and its anchor both
    // sit at/near the END of the sibling list, so stackAfter's two lastIndexOf() scans (which run
    // from the back) only touch the few items after the anchor; restack() fixes any reorder.
    QQuickItem *anchor = this;
    for (int i = a; i <= b; ++i) {
        Row &row = m_rows[i];
        if (!row.item) {
            if (row.context) // its item died under us: drop the orphaned context before recreating
                destroyRow(row);
            if (!createRow(i, row.data, row))
                continue;
            if (anchor->parentItem() == row.item->parentItem())
                row.item->stackAfter(anchor);
            changed = true;
        }
        anchor = row.item;
    }
    if (a <= b) {
        m_matFirst = a;
        m_matLast = b;
    } else {
        m_matFirst = 0;
        m_matLast = -1;
    }
    return changed;
}

void CssRepeater::targetRange(int *a, int *b) const
{
    switch (m_mode) {
    case Mode::Eager:
        *a = 0;
        *b = int(m_rows.size()) - 1;
        return;
    case Mode::Pending:
        *a = 0;
        *b = -1;
        return;
    case Mode::Windowed:
        windowRange(a, b);
        return;
    }
}

void CssRepeater::windowRange(int *a, int *b) const
{
    const int n = int(m_rows.size());
    if (n == 0 || !(m_flow.pitch > 0)) {
        *a = 0;
        *b = -1;
        return;
    }
    double cy = 0, vh = 0;
    if (m_flick) {
        cy = m_flick->property("contentY").toReal();
        vh = m_flick->height();
    }
    if (const QQuickItem *holder = parentItem())
        cy -= holder->y();
    // Rows intersecting [contentY - viewport, contentY + 2 * viewport]: row i occupies
    // [base + i * pitch, base + i * pitch + rowH).
    const double base = m_container.originY + m_flow.marginTop;
    const double lo = cy - vh;
    const double hi = cy + 2 * vh;
    const double firstF = std::floor((lo - base - m_flow.rowH) / m_flow.pitch) + 1;
    const double lastF = std::ceil((hi - base) / m_flow.pitch) - 1;
    int first = int(std::clamp(firstF, 0.0, double(n - 1)));
    int last = int(std::clamp(lastF, 0.0, double(n - 1)));
    // Keep at least one row: its style is what re-validates the pitch on the next layout.
    if (last < first)
        last = first;
    *a = first;
    *b = last;
}

bool CssRepeater::measureRows(VirtualFlow *flow) const
{
    if (!m_layout || m_matCount == 0)
        return false;
    bool have = false;
    for (int i = std::max(m_matFirst, 0); i <= m_matLast && i < m_rows.size(); ++i) {
        QQuickItem *item = m_rows[i].item;
        const auto *box = qobject_cast<const CssBox *>(item);
        if (!box || !item->isVisible())
            return false;
        const CssLayoutStyle &st = box->layoutStyle();
        // A definite, positive px height that nothing else on the main axis can change.
        if (st.displayNone || st.positionAbsolute || st.height.kind != CssLength::Number || !(st.height.value > 0)
            || st.minHeight.kind != CssLength::Absent || st.maxHeight.kind != CssLength::Absent
            || st.flexBasis.kind != CssLength::Absent || st.grow != 0 || st.shrink != 0 || st.order != 0)
            return false;
        double h = st.height.value;
        if (!st.borderBox) {
            // content-box: the laid-out border box adds padding + border (layoutFlex's rule).
            const auto p = m_layout->paddingOf(st);
            const auto bo = m_layout->borderOf(st);
            h += p[0] + p[2] + bo[0] + bo[2];
        }
        const auto m = m_layout->marginOf(st);
        if (!have) {
            flow->rowH = h;
            flow->marginTop = m[0];
            flow->pitch = m[0] + h + m[2]; // + gap, by the caller
            have = true;
        } else if (h != flow->rowH || m[0] != flow->marginTop || m[0] + h + m[2] != flow->pitch) {
            return false; // rows disagree: no fixed pitch
        }
    }
    return have;
}

bool CssRepeater::holderHoldsOnlyOurRows() const
{
    const QQuickItem *holder = parentItem();
    if (!holder)
        return false;
    const auto kids = holder->childItems();
    for (const QQuickItem *k : kids) {
        if (k == this || k->parent() == this || !k->isVisible())
            continue; // us, our rows (QObject children), or out of flow
        return false;
    }
    return true;
}

void CssRepeater::watchFlickable(QQuickItem *flick)
{
    if (m_flick == flick)
        return;
    if (m_flick)
        disconnect(m_flick, nullptr, this, nullptr);
    m_flick = flick;
    if (flick) {
        connect(flick, SIGNAL(contentYChanged()), this, SLOT(viewportMoved()));
        connect(flick, SIGNAL(heightChanged()), this, SLOT(viewportMoved()));
    }
}

bool CssRepeater::syncVirtual(const VirtualContainer &container)
{
    if (!m_virtualize || m_syncing || !m_delegate)
        return m_mode == Mode::Windowed;
    m_container = container;
    watchFlickable(container.flick);
    const int n = int(m_rows.size());
    if (n == 0) {
        m_mode = Mode::Pending; // nothing to window; the eager path lays out zero rows identically
        return false;
    }
    // One batch for every row created/destroyed here; declared BEFORE the re-entrancy flag's reset so
    // the flag is clear again when the batch's flush relayouts the holder (outside a flush).
    const Detail::LayoutBatchGuard batch(m_layout);
    m_syncing = true;
    struct Reset {
        bool &flag;
        ~Reset() { flag = false; }
    } reset{ m_syncing };

    bool changed = false;
    const bool a11y = Detail::g_virtualA11yOverride >= 0 ? Detail::g_virtualA11yOverride != 0
                                                         : QAccessible::isActive();
    Mode want = Mode::Eager;
    VirtualFlow flow;
    if (container.eligible && !a11y && holderHoldsOnlyOurRows()) {
        if (m_matCount == 0) {
            // Probe: one materialized row proves (or refutes) the pitch — where the window was, if a
            // pitch is known from before.
            int a = 0, b = -1;
            windowRange(&a, &b);
            changed |= materialize(a, a, false);
        }
        if (measureRows(&flow)) {
            flow.gap = container.gap;
            flow.pitch += container.gap;
            flow.total = n;
            want = Mode::Windowed;
        }
    }
    m_mode = want;
    if (want == Mode::Windowed)
        m_flow = flow;
    int a, b;
    targetRange(&a, &b);
    changed |= materialize(a, b, false);
    if (changed)
        restack();
    m_flow.first = m_matFirst;
    return m_mode == Mode::Windowed;
}

void CssRepeater::viewportMoved()
{
    if (m_mode != Mode::Windowed || m_syncing)
        return;
    int a, b;
    windowRange(&a, &b);
    if (a == m_matFirst && b == m_matLast)
        return;
    const Detail::LayoutBatchGuard batch(m_layout);
    if (materialize(a, b, false)) {
        restack();
        notifyLayout();
    }
}

void CssRepeater::restack()
{
    // Document order == childItems order: rows must sit right after US, in model order. stackAfter()
    // costs two linear sibling searches EVEN for an item already in place, so calling it for every row
    // made each rebuild O(n^2). Move only what is out of place:
    //  - fast path: rows already contiguous after us in model order (create at the end, append,
    //    remove, update, clear) → no calls;
    //  - otherwise keep the longest run of rows whose CURRENT order already matches the model (longest
    //    increasing subsequence of their sibling positions) and move the others after their model
    //    predecessor — a swap moves 2 rows. A final O(n) check falls back to the full loop if a
    //    foreign sibling still sits between rows (always correct).
    QQuickItem *parent = parentItem();
    if (!parent)
        return;
    QVarLengthArray<QQuickItem *, 256> items;
    for (int i = std::max(m_matFirst, 0); i <= m_matLast && i < m_rows.size(); ++i)
        if (m_rows[i].item)
            items.append(m_rows[i].item);
    const int n = int(items.size());
    if (n == 0)
        return;

    // Fast-path check WITHOUT a hash: a plain sequential scan of childItems() — cheap, and the common
    // case (rows already contiguous right after us, in model order) needs nothing else. Built from
    // creation-time anchoring (above) plus removal (closing a gap doesn't reorder survivors), this
    // covers create/append/remove/update/clear; only an actual reorder (e.g. a swap) needs the LIS
    // below, which reuses `siblings`.
    const QList<QQuickItem *> siblings = parent->childItems();
    const qsizetype selfIdx = siblings.indexOf(this);
    bool inPlace = selfIdx >= 0 && selfIdx + n < siblings.size() + 1;
    for (int i = 0; i < n && inPlace; ++i)
        inPlace = siblings.value(selfIdx + 1 + i) == items[i];
    if (inPlace)
        return;

    QHash<const QQuickItem *, int> pos;
    pos.reserve(siblings.size());
    for (int i = 0; i < siblings.size(); ++i)
        pos.insert(siblings[i], i);
    const int selfPos = int(selfIdx);

    // Longest increasing subsequence (patience sorting, O(n log n)) over positions after us.
    QVarLengthArray<int, 256> p(n), prev(n), tails;
    for (int i = 0; i < n; ++i) {
        p[i] = pos.value(items[i], -1);
        prev[i] = -1;
        if (p[i] <= selfPos)
            continue; // before us: must move
        int lo = 0, hi = int(tails.size());
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (p[tails[mid]] < p[i]) lo = mid + 1; else hi = mid;
        }
        if (lo > 0)
            prev[i] = tails[lo - 1];
        if (lo == tails.size()) tails.append(i); else tails[lo] = i;
    }
    QVarLengthArray<bool, 256> keep(n);
    std::fill(keep.begin(), keep.end(), false);
    for (int i = tails.isEmpty() ? -1 : tails.last(); i >= 0; i = prev[i])
        keep[i] = true;

    QQuickItem *anchor = this;
    for (int i = 0; i < n; ++i) {
        if (!keep[i])
            items[i]->stackAfter(anchor);
        anchor = items[i];
    }

    // Kept rows may still have a foreign sibling between them: verify once, fall back if so.
    const QList<QQuickItem *> after = parent->childItems();
    const int self2 = int(after.indexOf(this));
    bool ok = self2 >= 0 && self2 + n < after.size() + 1;
    for (int i = 0; ok && i < n; ++i)
        ok = after.value(self2 + 1 + i) == items[i];
    if (!ok) {
        anchor = this;
        for (int i = 0; i < n; ++i) {
            items[i]->stackAfter(anchor);
            anchor = items[i];
        }
    }
}

void CssRepeater::notifyLayout()
{
    // Ask the owning box to relayout (same duck-typed climb the engine uses).
    QQuickItem *p = parentItem();
    for (int hops = 0; p && hops < 5; ++hops, p = p->parentItem()) {
        const QMetaObject *mo = p->metaObject();
        const int idx = mo->indexOfMethod("requestRelayout()");
        if (idx >= 0) {
            mo->method(idx).invoke(p);
            return;
        }
    }
}

} // namespace QmlCss
