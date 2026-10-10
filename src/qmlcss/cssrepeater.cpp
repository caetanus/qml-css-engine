#include "qmlcss/cssrepeater.h"

#include "qmlcss/cssbox.h"
#include "qmlcss/csscontentholder.h"
#include "qmlcss/csslayout.h"
#include "qmlcss/csslength.h"
#include "qmlcss/cssrect.h"
#include "qmlcss/csstheme.h"
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
    if (v.metaType().id() == qMetaTypeId<QJSValue>()) {
        QVariantList out = toList(v.value<QJSValue>().toVariant());
        Detail::g_repeaterConversions += int(out.size());
        return out;
    }
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
    for (const int s : std::as_const(m_order))
        destroyRow(m_pool[s]);
    trimRecycle(0);
    m_order.clear();
    m_pool.clear();
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
    // Parked rows are instances of the old delegate: they never come back. Rows still materialized
    // keep living as before, but are destroyed (not parked) when they leave.
    ++m_delegateGen;
    trimRecycle(0);
    invalidateAllMeasurements(); // its rows may be of any size
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
        m_variable = false;
        m_anchorSlot = -1;
        const Detail::LayoutBatchGuard batch(m_layout);
        if (materialize(0, int(m_order.size()) - 1, false)) {
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
    if (QQmlContext *ctx = qmlContext(this)) {
        m_layout = qobject_cast<CssLayoutEngine *>(ctx->contextProperty(QStringLiteral("cssLayout")).value<QObject *>());
        m_theme = qobject_cast<CssTheme *>(ctx->contextProperty(QStringLiteral("cssTheme")).value<QObject *>());
    }
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
        m_variable = false;
        m_anchorSlot = -1;
        trimRecycle(0);
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
    const QVariantMap &m = *static_cast<const QVariantMap *>(v.constData()); // no copy
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
    row.itemGen = m_delegateGen;
    ++m_matCount;
    return true;
}

void CssRepeater::retireRow(Row &row)
{
    // Park only a whole, current-delegate row, and only while the pool has room (its capacity is the
    // window's row count): everything else is destroyed as before.
    if (int(m_recycle.size()) >= m_recycleCap || !row.item || !row.context || row.itemGen != m_delegateGen) {
        destroyRow(row);
        return;
    }
    QElapsedTimer t;
    if (Detail::g_phaseStats.enabled)
        t.start();
    // Detached: it neither paints nor takes part in layout (the hide cascade notifies once, at its top;
    // we are inside the caller's layout batch). Its QObject parent stays us, so it dies with us.
    row.item->setParentItem(nullptr);
    m_recycle.append(Parked{ row.item, row.context, m_theme ? m_theme->reapplyEpoch() : 0, row.itemGen });
    --m_matCount;
    row.item = nullptr;
    row.context = nullptr;
    if (Detail::g_phaseStats.enabled)
        Detail::g_phaseStats.time("repeater.park", t.nsecsElapsed());
}

bool CssRepeater::rebindRow(int i, Row &row)
{
    while (!m_recycle.isEmpty()) {
        Parked p = std::move(m_recycle.last());
        m_recycle.removeLast();
        if (!p.item || !p.context) { // half of it died while parked: drop the rest
            if (p.item)
                p.item->deleteLater();
            if (p.context)
                p.context->deleteLater();
            continue;
        }
        QElapsedTimer t;
        if (Detail::g_phaseStats.enabled)
            t.start();
        // The rebind transaction (contract 8/9), all inside the caller's layout batch and before the
        // next frame — no frame shows the row half old, half new:
        // (1) back under its real ancestors FIRST: every restyle below resolves against them.
        p.item->setParentItem(parentItem() ? parentItem() : this);
        // A theme reapply while it was parked resolved it without those ancestors, and an ancestor whose
        // identity changed meanwhile restyled only its VISUAL subtree (ancestor-scoped rules): once,
        // again, now. The ancestry check is a pointer compare of interned context nodes, O(depth).
        if (m_theme) {
            if (auto *box = qobject_cast<CssBox *>(p.item.data())) {
                if (p.reapplyEpoch != m_theme->reapplyEpoch() || !box->resolvedUnderCurrentAncestry(m_theme))
                    m_theme->restyle(box);
            }
        }
        row.item = p.item;
        row.context = p.context;
        row.itemGen = p.itemGen;
        ++m_matCount;
        // (2) the new record: its bindings (text, class={row.status} -> identityChanged -> restyle) run.
        p.context->setContextProperty(QStringLiteral("modelData"), row.data);
        p.context->setContextProperty(QStringLiteral("index"), i);
        // (3) nothing of the previous record's transient state survives (certified rows are passive:
        // no focus; hover and transitions are engine state). The row is a handful of items.
        QVarLengthArray<QQuickItem *, 32> stack;
        stack.append(p.item.data());
        while (!stack.isEmpty()) {
            QQuickItem *k = stack.last();
            stack.removeLast();
            if (auto *rect = qobject_cast<CssRect *>(k))
                rect->resetTransientState();
            for (QQuickItem *c : k->childItems())
                stack.append(c);
        }
        Q_ASSERT(!p.item->hasActiveFocus());
        if (Detail::g_phaseStats.enabled)
            Detail::g_phaseStats.time("repeater.rebind", t.nsecsElapsed());
        return true;
    }
    return false;
}

void CssRepeater::trimRecycle(int cap)
{
    while (int(m_recycle.size()) > cap) {
        QElapsedTimer t;
        if (Detail::g_phaseStats.enabled)
            t.start();
        Parked p = std::move(m_recycle.last());
        m_recycle.removeLast();
        if (p.item)
            p.item->deleteLater();
        if (p.context)
            p.context->deleteLater();
        if (Detail::g_phaseStats.enabled)
            Detail::g_phaseStats.time("repeater.destroyRow", t.nsecsElapsed());
    }
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
    // A model set from INSIDE a reconciliation (a row's binding/handler reacting to its new modelData or
    // index, a JS getter run by a conversion) must not reconcile on top of the half-done one: it runs
    // right after, against the latest model.
    if (m_rebuilding) {
        m_rebuildPending = true;
        return;
    }
    m_rebuilding = true;
    struct Reset {
        bool &flag;
        ~Reset() { flag = false; }
    } reset{ m_rebuilding };
    do {
        m_rebuildPending = false;
        reconcile();
    } while (m_rebuildPending);
}

void CssRepeater::reconcile()
{
    bool changed = false;
    bool done = false;
    // A JS array (the model of every <For>): identity-first, converting only new/changed elements.
    if (m_model.metaType() == QMetaType::fromType<QJSValue>()) {
        const QJSValue array = *static_cast<const QJSValue *>(m_model.constData()); // m_model may change under us
        if (array.isArray())
            done = rebuildKeyedJs(array, &changed);
    }
    if (!done) {
        QElapsedTimer t;
        if (Detail::g_phaseStats.enabled)
            t.start();
        const QVariantList data = toList(m_model);
        if (Detail::g_phaseStats.enabled)
            Detail::g_phaseStats.time("repeater.toList", t.nsecsElapsed());
        rebuildFromList(data, &changed);
    }
    // Survivors may now sit anywhere (a swap moves a materialized row out of the window): sweep every
    // logical row once — the matching above was O(n) already.
    int a, b;
    targetRange(&a, &b);
    if (materialize(a, b, true))
        changed = true;
    if (changed) {
        QElapsedTimer t;
        if (Detail::g_phaseStats.enabled)
            t.start();
        restack();
        if (Detail::g_phaseStats.enabled)
            Detail::g_phaseStats.time("repeater.restack", t.nsecsElapsed());
        notifyLayout();
    }
}

int CssRepeater::allocSlot()
{
    if (!m_free.isEmpty())
        return m_free.takeLast();
    m_pool.append(Row());
    return int(m_pool.size()) - 1;
}

void CssRepeater::releaseSlot(int slot)
{
    m_pool[slot] = Row();
    m_free.append(slot);
}

void CssRepeater::buildKeyIndex()
{
    // Rows left by the value-matching path carry no key: derive them from their (converted) data, first
    // row per key wins — exactly the lookup the pre-5c keyed path rebuilt on every reconciliation. Runs
    // once per switch from that path, never per keyed update.
    m_byKey.clear();
    m_byKey.reserve(m_order.size());
    for (const int s : std::as_const(m_order)) {
        Row &r = m_pool[s];
        r.key = keyOf(r.data);
        if (!r.key.isEmpty() && !m_byKey.contains(r.key))
            m_byKey.insert(r.key, s);
    }
    m_keyIndexValid = true;
}

void CssRepeater::finishOrder(bool *changed)
{
    // Rows no element claimed this round leave: their item/context die, their key and slot are freed.
    // Raw pointers in the O(n) loops: QList's operator[] re-checks detach per access. releaseSlot()
    // never reallocates m_pool (it only appends to m_free).
    Row *const pool = m_pool.data();
    const int *const order = m_order.constData();
    const int oldN = int(m_order.size());
    // Before any row leaves: an anchor row that is leaving hands the anchor to a survivor (5v-3).
    anchorAfterReorder(order, oldN);
    for (int i = 0; i < oldN; ++i) {
        const int s = order[i];
        Row &r = pool[s];
        if (r.gen == m_gen)
            continue;
        retireRow(r); // parked while windowed and the pool has room (5v-2)
        dropMeasurement(-1, r);
        m_treeDirty = true;
        if (m_keyIndexValid && !r.key.isEmpty()) {
            const auto it = m_byKey.constFind(r.key);
            if (it != m_byKey.cend() && *it == s)
                m_byKey.erase(it);
        }
        releaseSlot(s);
        *changed = true;
    }
    m_order.swap(m_next);
    m_next.clear(); // keeps its capacity for the next reconciliation
    if (m_order.isEmpty()) {
        // A cleared list gives its row storage back (slots are otherwise reused, never compacted).
        m_pool = QVector<Row>();
        m_free = QVector<int>();
        m_next = QVector<int>();
        m_byKey.clear();
        trimRecycle(0);
        m_anchorSlot = -1;
        m_treeDirty = true;
    } else if (m_anchorSlot >= 0) {
        // The anchor row survived. Rows inserted/removed around it SHIFT it (a neighbour is still its
        // neighbour): the viewport follows it. A row MOVED away from both neighbours (a swap, a sort)
        // would drag the viewport wherever it went: anchor the row now at its old index instead.
        const int n = int(m_order.size());
        int ni = m_pool[m_anchorSlot].pos;
        if (m_anchorPrev != -2 && ni != m_anchorIndex && ni >= 0 && ni < n) {
            const int np = ni > 0 ? m_order[ni - 1] : -1;
            const int nn = ni + 1 < n ? m_order[ni + 1] : -1;
            if (np != m_anchorPrev && nn != m_anchorNext) {
                ni = std::clamp(m_anchorIndex, 0, n - 1);
                m_anchorSlot = m_order[ni];
            }
        }
        if (ni >= 0 && ni < n && m_order[ni] == m_anchorSlot) {
            m_anchorIndex = ni;
            m_anchorPrev = ni > 0 ? m_order[ni - 1] : -1;
            m_anchorNext = ni + 1 < n ? m_order[ni + 1] : -1;
        } else {
            m_anchorSlot = -1;
        }
    }
    m_winValid = false; // the laid-out window describes the old order
}

void CssRepeater::anchorAfterReorder(const int *oldOrder, int oldN)
{
    if (!m_variable || m_anchorSlot < 0 || m_anchorSlot >= m_pool.size()) {
        m_anchorSlot = -1;
        return;
    }
    const Row *pool = m_pool.constData();
    const Row &a = pool[m_anchorSlot];
    if (a.gen == m_gen)
        return; // it stays: finishOrder checks it after the swap
    const int ai = a.pos; // an unclaimed row keeps its old index
    if (ai < 0 || ai >= oldN || oldOrder[ai] != m_anchorSlot) {
        m_anchorSlot = -1;
        return;
    }
    // Its successor (its predecessor at the end) takes the anchor and keeps ITS place on screen: the
    // offset becomes relative to that row's slot top, both read from the old layout.
    int pick = -1;
    for (int j = ai + 1; j < oldN && pick < 0; ++j)
        if (pool[oldOrder[j]].gen == m_gen)
            pick = j;
    for (int j = ai - 1; j >= 0 && pick < 0; --j)
        if (pool[oldOrder[j]].gen == m_gen)
            pick = j;
    if (pick < 0) {
        m_anchorSlot = -1;
        return;
    }
    m_anchorOffset -= slotTop(pick) - slotTop(ai);
    m_anchorSlot = oldOrder[pick];
    m_anchorPrev = m_anchorNext = -2; // replaced, not moved: no reorder test after the swap
}

bool CssRepeater::rebuildKeyedJs(const QJSValue &array, bool *changed)
{
    // KEYED when every element is an object with a distinct "id" (keyOf's rule on its conversion), same
    // predicate as the value path. Rows are matched by IDENTITY first (Solid web <For>): an element that
    // is the very object its row last received is that row, untouched — no conversion, no context write
    // (an object mutated in place and re-sent is NOT re-read: owner decision 2026-10-10). Only a new
    // object is converted (one toVariant) and matched by its id; a matched row gets it as modelData in
    // place (C23), an unmatched id is a new row.
    //
    // Two passes so a model that turns out unkeyed half-way changes nothing: (1) claim a row slot per
    // element (m_gen stamps; a slot claimed twice = a duplicate id), staging new modelData in the row;
    // (2) commit modelData/index writes and destroy unclaimed rows.
    const bool stats = Detail::g_phaseStats.enabled;
    QElapsedTimer t, ct;
    qint64 convNs = 0;
    if (stats)
        t.start();
    const int n = array.property(QStringLiteral("length")).toInt();
    if (n <= 0)
        return false; // empty: the value path (nothing to match) clears every row
    if (++m_gen == 0) { // wrapped: no row may look claimed by the new round
        for (Row &r : m_pool)
            r.gen = 0;
        m_gen = 1;
    }
    const quint32 gen = m_gen;
    const int oldN = int(m_order.size());
    // Raw pointers in the O(n) loops (QList's operator[] re-checks detach per access). `pool` is
    // re-fetched after allocSlot(), the only call that can grow m_pool.
    const int *const oldOrder = m_order.constData();
    Row *pool = m_pool.data();
    m_next.resize(n); // capacity kept across reconciliations: no allocation in the steady state
    int *const next = m_next.data();
    int filled = 0;
    bool keyed = true;
    // `j`: the old position expected next. Starts aligned and re-syncs after every row found elsewhere
    // (to just past its old position), so a removal or an insertion costs ONE lookup and the rows after
    // it are aligned again; a swap costs four.
    int j = 0;
    for (int i = 0; i < n; ++i) {
        QJSValue e = array.property(quint32(i));
        int slot = -1;
        int byFastKey = -1; // the row the element's id names, when that lookup ran
        QString fastKey;
        // Aligned: the same object as the next old row (an update/append/remove leaves almost all so).
        if (j < oldN) {
            const Row &r = pool[oldOrder[j]];
            if (r.js.isObject() && e.strictlyEquals(r.js))
                slot = oldOrder[j];
        }
        // Moved: the same object elsewhere — its row found through its id, confirmed by identity.
        if (slot < 0 && e.isObject()) {
            const QJSValue id = e.property(QStringLiteral("id"));
            if (id.isString())
                fastKey = id.toString();
            else if (id.isNumber() || id.isBool())
                fastKey = id.toVariant().toString(); // keyOf's formatting of the converted value
            if (!fastKey.isEmpty()) {
                if (!m_keyIndexValid)
                    buildKeyIndex();
                const auto it = m_byKey.constFind(fastKey);
                if (it != m_byKey.cend()) {
                    byFastKey = *it;
                    if (pool[*it].js.isObject() && e.strictlyEquals(pool[*it].js))
                        slot = *it;
                }
            }
        }
        if (slot >= 0) {
            if (pool[slot].gen == gen) { // the same object twice: a duplicate id
                keyed = false;
                break;
            }
        } else {
            // A new object: convert it once; its key is keyOf's, on the conversion.
            if (stats)
                ct.start();
            QVariant v = e.toVariant();
            ++Detail::g_repeaterConversions;
            if (stats)
                convNs += ct.nsecsElapsed();
            const QString k = keyOf(v);
            if (k.isEmpty()) {
                keyed = false;
                break;
            }
            if (!m_keyIndexValid)
                buildKeyIndex();
            int found = -1;
            if (k == fastKey) { // the lookup above already ran with this very key
                found = byFastKey;
            } else {
                const auto it = m_byKey.constFind(k);
                found = it != m_byKey.cend() ? *it : -1;
            }
            if (found >= 0) {
                slot = found;
                Row &r = pool[slot];
                if (r.gen == gen) {
                    keyed = false;
                    break;
                }
                r.incoming = std::move(v);
                r.hasIncoming = true;
                r.js = std::move(e);
            } else {
                slot = allocSlot();
                pool = m_pool.data();
                Row &r = pool[slot];
                r.data = std::move(v);
                r.key = k;
                r.js = std::move(e);
                r.pos = -1;
                m_byKey.insert(k, slot);
            }
        }
        Row &claimed = pool[slot];
        claimed.gen = gen;
        if (claimed.pos >= 0)
            j = claimed.pos + 1;
        next[filled++] = slot;
    }

    if (!keyed) {
        // Undo pass 1: new rows go, staged values are dropped; the value path then runs as before 5c.
        for (int i = 0; i < filled; ++i) {
            const int s = next[i];
            Row &r = pool[s];
            if (r.pos < 0) {
                m_byKey.remove(r.key);
                releaseSlot(s);
            } else if (r.hasIncoming) {
                r.incoming = QVariant();
                r.hasIncoming = false;
            }
        }
        m_next.clear();
        if (stats) {
            Detail::g_phaseStats.time("repeater.toList", convNs);
            Detail::g_phaseStats.time("repeater.match", t.nsecsElapsed() - convNs);
        }
        return false;
    }

    // Pass 2, in model order: modelData for a row that received a new object (only when the value
    // differs — today's rule), `index` only for a row that MOVED (re-setting an unchanged value still
    // notifies every binding that reads it). New rows get both at creation (materialize).
    qint64 indexNs = 0;
    for (int i = 0; i < n; ++i) {
        Row &r = pool[next[i]];
        if (r.hasIncoming) {
            r.hasIncoming = false;
            if (r.data != r.incoming) {
                r.data = std::move(r.incoming);
                dropMeasurement(r.pos, r); // its size was measured for the previous record (5v-3)
                if (r.context)
                    r.context->setContextProperty(QStringLiteral("modelData"), r.data);
            }
            r.incoming = QVariant();
        }
        if (r.pos != i) {
            if (r.context) {
                if (stats)
                    ct.start();
                r.context->setContextProperty(QStringLiteral("index"), i);
                if (stats)
                    indexNs += ct.nsecsElapsed();
            }
            r.pos = i;
            m_treeDirty = true; // the logical order changed: the prefix sums are rebuilt (5v-3)
            *changed = true;
        }
    }
    finishOrder(changed);
    if (stats) {
        Detail::g_phaseStats.time("repeater.toList", convNs);
        Detail::g_phaseStats.time("repeater.match", t.nsecsElapsed() - convNs - indexNs);
        if (indexNs)
            Detail::g_phaseStats.time("repeater.index", indexNs);
    }
    return true;
}

void CssRepeater::rebuildFromList(const QVariantList &data, bool *changed)
{
    // The value path (pre-5c behaviour, unchanged): C++ lists, integer models, QStringList, and JS arrays
    // that are not keyed. KEYED when every entry is an object with a distinct "id": rows match by id in
    // O(n), and a surviving row whose content changed gets the new value as its modelData IN PLACE. A model
    // rebuilt from fresh JSON (every object new, most of them equal, some edited) keeps every delegate
    // instead of re-creating the edited ones — and skips the O(n²) value matching. Otherwise rows match by
    // VALUE, stable first match.
    QElapsedTimer t;
    if (Detail::g_phaseStats.enabled)
        t.start();
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
    // This path keeps no identities and no persistent key index: the JS path rebuilds it on demand.
    m_byKey.clear();
    m_keyIndexValid = false;
    if (++m_gen == 0) {
        for (Row &r : m_pool)
            r.gen = 0;
        m_gen = 1;
    }
    const quint32 gen = m_gen;
    QHash<QString, int> byKey;
    if (keyed) {
        for (const int s : std::as_const(m_order)) {
            const QString k = keyOf(m_pool[s].data);
            if (!k.isEmpty() && !byKey.contains(k))
                byKey.insert(k, s);
        }
    }
    m_next.clear();
    m_next.reserve(data.size());
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
            if (it != byKey.cend() && m_pool[*it].gen != gen)
                found = *it;
        } else {
            // Reconcile by VALUE, stable first-match: surviving entries keep their delegate.
            for (const int s : std::as_const(m_order)) {
                if (m_pool[s].gen != gen && m_pool[s].data == data.at(i)) {
                    found = s;
                    break;
                }
            }
        }
        if (Detail::g_phaseStats.enabled)
            Detail::g_phaseStats.time("repeater.match", t.nsecsElapsed());
        if (found >= 0) {
            Row &row = m_pool[found];
            row.gen = gen;
            row.js = QJSValue();
            row.key.clear();
            if (keyed && row.data != data.at(i)) {
                row.data = data.at(i);
                dropMeasurement(row.pos, row);
                if (row.context)
                    row.context->setContextProperty(QStringLiteral("modelData"), row.data);
            }
            // Only a row that MOVED gets a new index (its old index is its old position):
            // re-setting an unchanged value still notifies every binding that reads `index`.
            if (row.pos != i) {
                if (row.context) {
                    if (Detail::g_phaseStats.enabled)
                        t.start();
                    row.context->setContextProperty(QStringLiteral("index"), i);
                    if (Detail::g_phaseStats.enabled)
                        Detail::g_phaseStats.time("repeater.index", t.nsecsElapsed());
                }
                row.pos = i;
                m_treeDirty = true;
                *changed = true;
            }
            m_next.append(found);
        } else {
            const int s = allocSlot();
            Row &row = m_pool[s];
            row.data = data.at(i);
            row.pos = i;
            row.gen = gen;
            m_next.append(s);
            m_treeDirty = true;
            *changed = true;
        }
    }
    finishOrder(changed);
}

bool CssRepeater::materialize(int a, int b, bool sweepAll)
{
    const int n = int(m_order.size());
    a = std::max(a, 0);
    b = std::min(b, n - 1);
    bool changed = false;
    // Recycling (5v-2) only while windowed: the pool holds at most one window of rows. Leaving rows
    // are parked first, so the entering ones below take them instead of creating new delegates.
    m_recycleCap = m_mode == Mode::Windowed && a <= b ? b - a + 1 : 0;
    // Rows leaving: everything outside [a, b] (only the previous range can hold items, unless a
    // reconciliation just moved them).
    const int lo = sweepAll ? 0 : std::max(m_matFirst, 0);
    const int hi = sweepAll ? n - 1 : std::min(m_matLast, n - 1);
    {
        Row *const pool = m_pool.data(); // raw: the sweep is O(n) after every reconciliation
        const int *const order = m_order.constData();
        for (int i = lo; i <= hi; ++i) {
            if (i >= a && i <= b)
                continue;
            Row &row = pool[order[i]];
            if (!row.item.isNull() || !row.context.isNull()) {
                retireRow(row);
                changed = true;
            }
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
        Row &row = rowAt(i);
        if (!row.item) {
            if (row.context) // its item died under us: drop the orphaned context before recreating
                destroyRow(row);
            // A parked row if there is one (in any mode: a flip to eager consumes the pool first).
            if (!rebindRow(i, row) && !createRow(i, row.data, row))
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
    trimRecycle(m_recycleCap); // what the window did not take back, beyond its bound (all, unless windowed)
    return changed;
}

void CssRepeater::targetRange(int *a, int *b)
{
    switch (m_mode) {
    case Mode::Eager:
        *a = 0;
        *b = int(m_order.size()) - 1;
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

void CssRepeater::windowRange(int *a, int *b)
{
    const int n = int(m_order.size());
    if (m_variable) {
        if (n == 0) {
            *a = 0;
            *b = -1;
            return;
        }
        if (!(m_estimate > 0)) {
            // Nothing measured yet: keep what exists (at least one probe row) until it is measured.
            *a = std::clamp(m_matFirst, 0, n - 1);
            *b = m_matLast >= *a ? std::min(m_matLast, n - 1) : *a;
            m_rewindow = true;
            return;
        }
        ensureTree();
        double vh = 0;
        double off = viewportTop(&vh) - m_container.originY;
        // Where commitAnchor will put the viewport: build the window around it.
        if (m_anchorSlot >= 0 && !userMoving()) {
            const int ai = m_pool[m_anchorSlot].pos;
            if (ai >= 0 && ai < n && m_order[ai] == m_anchorSlot)
                off = prefix(ai) + m_anchorOffset;
        }
        // Rows intersecting [contentY - viewport, contentY + 2 * viewport].
        *a = indexAt(off - vh);
        *b = indexAt(off + 2 * vh);
        return;
    }
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
    for (int i = std::max(m_matFirst, 0); i <= m_matLast && i < m_order.size(); ++i) {
        QQuickItem *item = rowAt(i).item;
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
    const int n = int(m_order.size());
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
    bool variable = false;
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
        } else if (autoRowsEligible(&flow)) {
            // 5v-3: rows sized by their own content (or fixed heights that disagree).
            want = Mode::Windowed;
            variable = true;
        }
    }
    // The fixed pitch this list had until now, without its gap (measurements are gap-free).
    const double oldPitch = m_mode == Mode::Windowed && !m_variable ? m_flow.pitch - m_flow.gap : 0.0;
    m_mode = want;
    if (m_variable && !variable) {
        m_anchorSlot = -1;
        m_winValid = false;
    }
    m_variable = variable;
    if (want == Mode::Windowed) {
        if (variable) {
            checkMeasureKey(container.width);
            ensureTree();
            if (!(m_estimate > 0)) {
                // A first estimate: the fixed pitch this list had until now, else the rows that exist
                // and have a size (a list turning windowed from eager); none (a fresh probe row): the
                // window waits for its first measurement.
                if (oldPitch > 0) {
                    m_estimate = oldPitch;
                } else {
                    double sum = 0;
                    int cnt = 0;
                    for (int i = std::max(m_matFirst, 0); i <= m_matLast && i < n; ++i) {
                        const QQuickItem *item = rowAt(i).item;
                        if (item && item->height() > 0) {
                            sum += item->height() + flow.marginTop + flow.marginBottom;
                            ++cnt;
                        }
                    }
                    if (cnt)
                        m_estimate = sum / cnt;
                }
            }
            flow.variable = true;
            flow.gap = container.gap;
            flow.total = n;
        }
        m_flow = flow;
    }
    int a, b;
    targetRange(&a, &b);
    changed |= materialize(a, b, false);
    if (changed)
        restack();
    m_flow.first = m_matFirst;
    if (m_variable)
        m_flow.firstTop = prefix(m_matFirst);
    return m_mode == Mode::Windowed;
}

void CssRepeater::viewportMoved()
{
    if (m_mode != Mode::Windowed || m_syncing || m_correcting)
        return;
    // A user scroll (or a resize) chose a new viewport: anchor it before the window moves (5v-3).
    if (m_variable)
        captureAnchor();
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

// --- variable extents (5v-3) --------------------------------------------------------------------

bool CssRepeater::autoRowsEligible(VirtualFlow *flow) const
{
    if (!m_layout || m_matCount == 0)
        return false;
    // A size the row's own content and style decide: auto/px heights and min/max (never % or viewport
    // units — the container's height), nothing on the main axis from the container (grow/shrink/basis).
    const auto own = [](const CssLength &l) {
        return l.kind == CssLength::Absent || l.kind == CssLength::Invalid || l.kind == CssLength::Number;
    };
    bool first = true;
    for (int i = std::max(m_matFirst, 0); i <= m_matLast && i < m_order.size(); ++i) {
        const QQuickItem *item = rowAt(i).item;
        const auto *box = qobject_cast<const CssBox *>(item);
        if (!box || !item->isVisible())
            return false;
        const CssLayoutStyle &st = box->layoutStyle();
        if (st.displayNone || st.positionAbsolute || !own(st.height) || !own(st.minHeight) || !own(st.maxHeight)
            || st.flexBasis.kind != CssLength::Absent || st.grow != 0 || st.shrink != 0 || st.order != 0)
            return false;
        if (first) {
            // The first and last rows' outer margins bound the extent; unseen rows share this row's.
            const auto m = m_layout->marginOf(st);
            flow->marginTop = m[0];
            flow->marginBottom = m[2];
            first = false;
        }
    }
    return !first;
}

void CssRepeater::setMeasured(int i, Row &r, double v)
{
    const bool was = measuredValid(r);
    if (was && r.measured == v)
        return;
    if (!m_treeDirty && i < m_fwN)
        fenwickAdd(i + 1, was ? v - r.measured : v, was ? 0 : 1);
    if (was) {
        m_measSum += v - r.measured;
    } else {
        m_measSum += v;
        ++m_measCount;
    }
    r.measured = v;
    r.measuredEpoch = m_measureEpoch;
    if (Detail::g_phaseStats.enabled)
        Detail::g_phaseStats.bump("repeater.measure");
}

void CssRepeater::dropMeasurement(int treeIndex, Row &r)
{
    if (measuredValid(r)) {
        m_measSum -= r.measured;
        --m_measCount;
        if (!m_treeDirty && treeIndex >= 0 && treeIndex < m_fwN)
            fenwickAdd(treeIndex + 1, -r.measured, -1);
    }
    r.measured = -1;
}

void CssRepeater::invalidateAllMeasurements()
{
    // O(1): every measurement carries the epoch it was taken under. The estimate stays as a
    // placeholder until the window is measured again.
    if (++m_measureEpoch == 0) {
        for (Row &r : m_pool)
            r.measured = -1;
        m_measureEpoch = 1;
    }
    m_measSum = 0;
    m_measCount = 0;
    m_treeDirty = true;
}

void CssRepeater::checkMeasureKey(double width)
{
    const int font = m_theme ? m_theme->fontRevision() : 0;
    const quint64 rules = m_theme ? m_theme->ruleGeneration() : 0;
    if (m_measureKeyValid && width == m_measureWidth && font == m_measureFont && rules == m_measureRules)
        return;
    if (m_measureKeyValid)
        invalidateAllMeasurements(); // another width, font or rule set: every row may wrap differently
    m_measureKeyValid = true;
    m_measureWidth = width;
    m_measureFont = font;
    m_measureRules = rules;
}

void CssRepeater::ensureTree()
{
    const int n = int(m_order.size());
    if (!m_treeDirty && m_fwN == n)
        return;
    // O(N) (contract 12): a structural change (insert/remove/move) or a bulk invalidation. Linear
    // Fenwick construction from the per-slot measurements, which survive any reorder.
    QElapsedTimer t;
    if (Detail::g_phaseStats.enabled)
        t.start();
    m_fwSum.fill(0.0, n + 1);
    m_fwCnt.fill(0, n + 1);
    double *const fs = m_fwSum.data();
    int *const fc = m_fwCnt.data();
    const Row *const pool = m_pool.constData();
    const int *const order = m_order.constData();
    double sum = 0;
    int cnt = 0;
    for (int i = 0; i < n; ++i) {
        const Row &r = pool[order[i]];
        if (measuredValid(r)) {
            fs[i + 1] = r.measured;
            fc[i + 1] = 1;
            sum += r.measured;
            ++cnt;
        }
    }
    for (int j = 1; j <= n; ++j) {
        const int k = j + (j & -j);
        if (k <= n) {
            fs[k] += fs[j];
            fc[k] += fc[j];
        }
    }
    m_measSum = sum; // exact again (point updates accumulate rounding)
    m_measCount = cnt;
    if (cnt)
        m_estimate = sum / cnt;
    m_fwN = n;
    m_fwTop = n > 0 ? int(1u << (31 - qCountLeadingZeroBits(quint32(n)))) : 0;
    m_treeDirty = false;
    if (Detail::g_phaseStats.enabled)
        Detail::g_phaseStats.time("repeater.extentRebuild", t.nsecsElapsed());
}

void CssRepeater::fenwickAdd(int i, double ds, int dc)
{
    double *const fs = m_fwSum.data();
    int *const fc = m_fwCnt.data();
    for (; i <= m_fwN; i += i & -i) {
        fs[i] += ds;
        fc[i] += dc;
    }
}

double CssRepeater::prefix(int k) const
{
    k = std::clamp(k, 0, std::max(m_fwN, 0));
    double s = 0;
    int c = 0;
    for (int j = k; j > 0; j -= j & -j) {
        s += m_fwSum[j];
        c += m_fwCnt[j];
    }
    // Measurements and the estimate are gap-free margin boxes: the container's CURRENT gap is added
    // per slot, so a gap change re-prices every row at once (no stale measured pitch).
    return s + (k - c) * std::max(m_estimate, 0.0) + k * m_flow.gap;
}

int CssRepeater::indexAt(double off) const
{
    // Binary descent: the largest prefix of rows that ENDS at or before `off` — the next row holds it.
    const double est = std::max(m_estimate, 0.0);
    const double gap = m_flow.gap;
    int pos = 0;
    double acc = 0;
    for (int step = m_fwTop; step > 0; step >>= 1) {
        const int nx = pos + step;
        if (nx > m_fwN)
            continue;
        const int span = nx & -nx;
        const double v = m_fwSum[nx] + (span - m_fwCnt[nx]) * est + span * gap;
        if (acc + v <= off) {
            pos = nx;
            acc += v;
        }
    }
    return std::clamp(pos, 0, std::max(m_fwN - 1, 0));
}

double CssRepeater::slotTop(int i) const
{
    // A row of the window as it was laid out (an unsettled row's real size, not its estimate)...
    if (m_winValid && i >= m_winFirst && i < m_winFirst + m_winSize.size()) {
        double t = m_winTop;
        const double *const sz = m_winSize.constData();
        for (int k = m_winFirst; k < i; ++k)
            t += sz[k - m_winFirst];
        return t;
    }
    // ...else the prefix sums.
    return prefix(i);
}

double CssRepeater::viewportTop(double *vh) const
{
    double cy = 0;
    *vh = 0;
    if (m_flick) {
        cy = m_flick->property("contentY").toReal();
        *vh = m_flick->height();
    }
    if (const QQuickItem *holder = parentItem())
        cy -= holder->y();
    return cy;
}

bool CssRepeater::userMoving() const
{
    // A drag or a flick in progress owns contentY: corrections wait (the content may shift under it).
    // Not `moving` alone: it is also true while the Flickable's own fixup timeline settles a position.
    return m_flick && (m_flick->property("dragging").toBool() || m_flick->property("flicking").toBool());
}

void CssRepeater::captureAnchor()
{
    const int n = int(m_order.size());
    if (!m_variable || n == 0 || !m_flick) {
        m_anchorSlot = -1;
        return;
    }
    double vh = 0;
    const double off = viewportTop(&vh) - m_container.originY;
    int idx = -1;
    double top = 0;
    if (m_winValid && off >= m_winTop) {
        // The first laid-out row whose slot ends below the viewport top (O(window)).
        double t = m_winTop;
        const int cnt = int(m_winSize.size());
        for (int k = 0; k < cnt; ++k) {
            if (off < t + m_winSize[k]) {
                idx = m_winFirst + k;
                top = t;
                break;
            }
            t += m_winSize[k];
        }
    }
    double size = 0;
    if (idx >= 0) {
        size = m_winSize[idx - m_winFirst];
    } else {
        if (!(m_estimate > 0)) {
            m_anchorSlot = -1;
            return;
        }
        ensureTree();
        idx = indexAt(off);
        top = prefix(idx);
        size = prefix(idx + 1) - top;
    }
    // A viewport top in the gap (or bottom margin) after a row anchors on the next row — the first one
    // actually visible — with a negative offset.
    if (idx + 1 < n && off >= top + size - m_flow.gap - m_flow.marginBottom) {
        top += size;
        ++idx;
    }
    m_anchorSlot = m_order[idx];
    m_anchorIndex = idx;
    m_anchorOffset = off - top;
    m_anchorPrev = idx > 0 ? m_order[idx - 1] : -1;
    m_anchorNext = idx + 1 < n ? m_order[idx + 1] : -1;
}

double CssRepeater::measureWindow(const double *mains, int count)
{
    const int n = int(m_order.size());
    const int first = m_flow.first;
    const double gap = m_flow.gap;
    const double usedTop = m_flow.firstTop;
    ensureTree();
    m_winSize.resize(count); // capacity kept: no allocation per pass in the steady state
    double *const sz = m_winSize.data();
    for (int k = 0; k < count; ++k)
        sz[k] = mains[k] + gap; // the slot as laid out in this pass
    // The flow is exactly the window (syncVirtual made sure), in order. A row whose layout is still
    // queued has a stale size: it is measured on a later pass, when it settled.
    const bool aligned = count > 0 && first == m_matFirst && count == m_matLast - m_matFirst + 1;
    if (aligned) {
        for (int k = 0; k < count; ++k) {
            Row &r = rowAt(first + k);
            if (!r.item || (m_layout && m_layout->subtreeQueued(r.item))) {
                m_rewindow = true;
                continue;
            }
            setMeasured(first + k, r, mains[k]); // gap-free: the container's gap is added analytically
        }
        if (m_measCount > 0)
            m_estimate = m_measSum / m_measCount;
    }
    m_winValid = aligned;
    m_winFirst = first;
    // The first slot moves when estimates of rows above it changed: the engine shifts the window.
    const double top = n > 0 ? prefix(first) : 0.0;
    m_winTop = top;
    m_flow.firstTop = top;
    const double total = prefix(n);
    m_flow.content = n > 0 ? total - gap : 0.0;
    // The LOGICAL extent, what the eager walk finds: the first row's top, the last row's bottom.
    m_flow.extentTop = m_container.originY + m_flow.marginTop;
    m_flow.extentBottom = m_container.originY + total - gap - m_flow.marginBottom;
    // Bottom-stick: was the viewport at the end before this pass publishes a new extent?
    m_wasAtEnd = false;
    if (m_flick) {
        const double maxY = m_flick->property("contentHeight").toReal() - m_flick->height();
        m_wasAtEnd = maxY > 0.5 && m_flick->property("contentY").toReal() >= maxY - 0.5;
    }
    return top - usedTop;
}

void CssRepeater::commitAnchor()
{
    if (!m_variable || !m_flick)
        return;
    const int n = int(m_order.size());
    // The anchor stays fixed through every pass of the flush (re-taking it from a corrected,
    // pixel-rounded position would let rounding accumulate pass after pass): each pass rounds once,
    // relative to the position the flush started from. layoutSettled() re-bases it on what is shown.
    // Taken afresh here only when there is none, while the user moves, or when the end clamps.
    bool recapture = m_anchorSlot < 0;
    // The correction, in the same pass as the geometry and the extent it compensates.
    if (userMoving()) {
        recapture = true;
    } else {
        const double cy = m_flick->property("contentY").toReal();
        const double maxY = std::max(0.0, m_flick->property("contentHeight").toReal() - m_flick->height());
        double want = cy;
        if (m_wasAtEnd) {
            want = maxY;
            recapture = true;
        } else if (m_anchorSlot >= 0) {
            const int ai = m_pool[m_anchorSlot].pos;
            if (ai >= 0 && ai < n && m_order[ai] == m_anchorSlot) {
                const double holderY = parentItem() ? parentItem()->y() : 0.0;
                const double at = holderY + m_container.originY + slotTop(ai) + m_anchorOffset;
                want = std::clamp(at, 0.0, maxY);
                recapture |= std::abs(want - at) > 0.5;
            } else {
                recapture = true;
            }
        }
        // Whole pixels when we move it: the Flickable snaps a fractional position itself, later, through
        // its fixup timeline (QQuickFlickablePrivate::fixup) — a second, unanchored move. The anchored
        // row stays within half a pixel.
        if (std::abs(want - cy) > 1e-6)
            want = std::round(want);
        m_anchorSettle = true;
        if (std::abs(want - cy) > 1e-6) {
            m_correcting = true;
            m_flick->setProperty("contentY", want);
            m_correcting = false;
            if (Detail::g_phaseStats.enabled)
                Detail::g_phaseStats.bump("repeater.anchorCorrect");
        }
    }
    m_wasAtEnd = false;
    if (recapture)
        captureAnchor();
    // The viewport must be covered by materialized rows; otherwise (or with rows still unmeasured)
    // one more pass re-windows — the engine's drain runs it before the frame.
    bool again = m_rewindow;
    m_rewindow = false;
    if (!again && m_estimate > 0 && n > 0) {
        double vh = 0;
        const double off = viewportTop(&vh) - m_container.originY;
        again = indexAt(off) < m_matFirst || indexAt(off + vh) > m_matLast;
    }
    if (again)
        notifyLayout();
}

void CssRepeater::layoutSettled()
{
    // The flush is over and its result is what the next frame shows: anchor exactly that.
    if (!m_anchorSettle)
        return;
    m_anchorSettle = false;
    if (m_variable && m_mode == Mode::Windowed && !m_correcting)
        captureAnchor();
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
    for (int i = std::max(m_matFirst, 0); i <= m_matLast && i < m_order.size(); ++i)
        if (rowAt(i).item)
            items.append(rowAt(i).item);
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
