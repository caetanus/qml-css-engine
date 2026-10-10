#include "qmlcss/cssrepeater.h"

#include "qmlcss/csslayout.h"
#include "csstheme_p.h" // LayoutBatchGuard

#include <QDebug>
#include <QHash>
#include <QSet>
#include <QJSValue>
#include <QMetaMethod>
#include <QQmlContext>
#include <QQmlEngine>
#include <QVarLengthArray>
#include <algorithm>

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

void CssRepeater::componentComplete()
{
    QQuickItem::componentComplete();
    if (QQmlContext *ctx = qmlContext(this))
        m_layout = qobject_cast<CssLayoutEngine *>(ctx->contextProperty(QStringLiteral("cssLayout")).value<QObject *>());
    rebuild();
}

void CssRepeater::destroyRow(Row &row)
{
    if (row.item) {
        row.item->setParentItem(nullptr);
        row.item->deleteLater();
    }
    if (row.context)
        row.context->deleteLater();
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
    auto *ctx = new QQmlContext(creation, this);
    ctx->setBaseUrl(creation->baseUrl());
    ctx->setContextProperty(QStringLiteral("modelData"), data);
    ctx->setContextProperty(QStringLiteral("index"), i);
    QObject *o = m_delegate->create(ctx);
    auto *item = qobject_cast<QQuickItem *>(o);
    if (!item) {
        if (o)
            o->deleteLater();
        delete ctx;
        qWarning("CssRepeater: failed to create delegate: %s", qPrintable(m_delegate->errorString()));
        return false;
    }
    item->setParent(this);
    item->setParentItem(parentItem() ? parentItem() : this);
    row.item = item;
    row.context = ctx;
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
    const QVariantList data = toList(m_model);

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

    QVector<Row> next;
    next.reserve(data.size());
    QVector<bool> used(m_rows.size(), false);
    bool changed = false;
    QHash<QString, int> byKey;
    if (keyed) {
        for (int j = 0; j < m_rows.size(); ++j)
            if (m_rows[j].item) {
                const QString k = keyOf(m_rows[j].data);
                if (!k.isEmpty() && !byKey.contains(k))
                    byKey.insert(k, j);
            }
    }
    for (int i = 0; i < data.size(); ++i) {
        int found = -1;
        if (keyed) {
            const auto it = byKey.constFind(keys.at(i));
            if (it != byKey.cend() && !used[*it])
                found = *it;
        } else {
            // Reconcile by VALUE, stable first-match: surviving entries keep their delegate.
            for (int j = 0; j < m_rows.size(); ++j) {
                if (!used[j] && m_rows[j].item && m_rows[j].data == data.at(i)) {
                    found = j;
                    break;
                }
            }
        }
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
            if (row.context && found != i)
                row.context->setContextProperty(QStringLiteral("index"), i);
            if (found != i)
                changed = true;
            next.append(row);
        } else {
            Row row;
            if (!createRow(i, data.at(i), row))
                continue;
            // createRow() parents the new item to parentItem() (or us) via setParentItem(), which
            // APPENDS it to the end of that parent's children — in a header/footer-wrapped list that
            // lands it after the footer. Stack it after its model predecessor right away (or after US
            // for the very first row), so restack()'s fast path finds everything already in place and
            // never falls back to the O(n^2) loop just because we create/append next to a static
            // sibling. When creating/appending (no concurrent reorder) this stackAfter is cheap: the new
            // item and its anchor both sit at/near the END of the sibling list, so its two lastIndexOf()
            // scans (which run from the back) and the siblingOrderChanged bookkeeping only touch the
            // few items after the anchor (e.g. the footer). If the same rebuild also reorders, the
            // anchor may still be at its old position — that one call is then O(n), as before, and
            // restack() fixes the final order regardless.
            QQuickItem *anchor = next.isEmpty() ? this : next.constLast().item.data();
            if (anchor && row.item && anchor->parentItem() == row.item->parentItem())
                row.item->stackAfter(anchor);
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
    if (changed) {
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
    for (const Row &row : m_rows)
        if (row.item)
            items.append(row.item);
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
