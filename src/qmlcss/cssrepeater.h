#pragma once

#include <QHash>
#include <QJSValue>
#include <QPointer>
#include <QQmlComponent>
#include <QQuickItem>
#include <QVariantList>
#include <QVector>

namespace QmlCss {

class CssLayoutEngine;
class CssTheme;

// Keyed repeater with FLYWEIGHT semantics — Solid's <For>: when the model array changes,
// entries whose VALUE survives keep their existing delegate (moved/re-stacked to the new
// position, never recreated); removed entries destroy theirs; only genuinely new entries
// instantiate. A plain QML Repeater tears everything down on any wholesale model change —
// a drag-and-drop reorder recreated every card.
//
// Like Repeater, delegates are reparented to OUR parent (the box's contentHolder), so the
// layout engine treats them as direct children; document order (childItems order) follows
// the model via stacking. Each delegate's context exposes `modelData` and `index`.
// VIRTUAL mode (5v-1, `virtualize: true` — emitted by the transpiler only for rows it proved passive):
// when the repeater is also RUNTIME-eligible (re-checked by every layout of its holder — see
// syncVirtual), it keeps the logical keys/order of all N entries but instantiates only the rows that
// intersect [contentY - viewport, contentY + 2 * viewport] of the enclosing scroll box; the layout
// engine places that window at its logical positions and publishes the LOGICAL extent. Not eligible
// (or accessibility active) -> every row exists, exactly the eager behaviour.
// RECYCLING (5v-2): in windowed mode a row leaving the window is PARKED (detached from the holder, still
// our QObject child) in a pool bounded by the window's row count, and a row entering takes a parked one
// and REBINDS it — re-parented into the holder first (CSS resolves under its real ancestors), then
// modelData/index, then its transient state reset (hover, transitions) — instead of creating a new
// delegate. The pool drains on clear, on leaving windowed mode, on a delegate change and with us.
// VARIABLE EXTENTS (5v-3): rows whose main size comes from their own content (auto height, wrapping
// text, or fixed heights that disagree) are windowed too. Each logical row (its m_pool slot) keeps its
// last MEASURED margin-box size (gap-free), valid for its data and for the container width / font /
// theme revision it was measured under; unseen rows count as the mean of the valid measurements. Fenwick
// trees over the measured sizes plus the container's CURRENT gap per slot give y(i), the total and
// index-at-offset in O(log N) — a gap change re-prices every row without re-measuring any. The viewport is ANCHORED to a row
// (slot) and an offset into it: every pass that moves rows (measurements, estimate, model, width) puts
// contentY back on the anchor in the same layout pass — no frame shows the jump.
class CssRepeater : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QVariant model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(QQmlComponent *delegate READ delegate WRITE setDelegate NOTIFY delegateChanged)
    Q_PROPERTY(bool virtualize READ virtualize WRITE setVirtualize NOTIFY virtualizeChanged)

public:
    explicit CssRepeater(QQuickItem *parent = nullptr);
    ~CssRepeater() override;

    QVariant model() const { return m_model; }
    void setModel(const QVariant &v);

    QQmlComponent *delegate() const { return m_delegate; }
    void setDelegate(QQmlComponent *c);

    bool virtualize() const { return m_virtualize; }
    void setVirtualize(bool v);

    // Census probes (5a): live row contexts (materialized rows) held by this repeater instance, and
    // the logical entry count (equal unless windowed).
    int rowCount() const { return m_matCount; }
    int materializedCount() const { return m_matCount; }
    int logicalCount() const { return int(m_order.size()); }
    // Recycling pool (5v-2): detached rows kept for reuse, and its current bound.
    int pooledCount() const { return int(m_recycle.size()); }
    int poolCapacity() const { return m_recycleCap; }

    // --- layout-engine interface (5v-1) -----------------------------------------------------------
    // What the container's own style allows: a vertical scroll box laying its holder out as ONE
    // non-wrapping column with justify-content start. `gap` is the evaluated main-axis gap, `originY`
    // the holder's content origin (padding-top), `flick` the box's scroll Flickable.
    // `variable` (5v-3): rows are not of one pitch — the window's first slot starts at originY + firstTop,
    // the engine lays the window out as the eager column would, hands its computed main sizes to
    // measureWindow() and publishes [extentTop, extentBottom]; `content` is the logical main content size.
    struct VirtualContainer {
        bool eligible = false;
        double gap = 0;
        double originY = 0;
        double width = 0;     // the holder's content width (measurements are valid for one width)
        double padBottom = 0; // the box's padding-bottom (the scroll extent's breathing room)
        QQuickItem *flick = nullptr;
    };
    // The materialized window, as the engine lays it out: rows [first, first + count) are the flow
    // (in document order); row i's main-axis slot starts at originY + i * pitch (its top margin
    // follows), its border box is rowH tall; `total` logical rows.
    struct VirtualFlow {
        int first = 0;
        int total = 0;
        double gap = 0;
        double pitch = 0;
        double marginTop = 0;
        double rowH = 0;
        bool variable = false;
        double firstTop = 0;
        double marginBottom = 0;
        double content = 0;
        double extentTop = 0;
        double extentBottom = 0;
    };
    // Called by CssLayoutEngine::layout() for this repeater's holder BEFORE it collects children:
    // re-validates runtime eligibility (container, rows, accessibility) and the window, materializing
    // or destroying rows as needed. True = windowed: the holder's flow is exactly our window.
    bool syncVirtual(const VirtualContainer &container);
    const VirtualFlow &virtualFlow() const { return m_flow; }
    // Variable flow (5v-3), called by the engine right after it laid the window out: `mains[k]` is
    // window row k's margin-box main size as computed by the layout. Records the settled rows'
    // measurements, refreshes the estimate and the logical extent (virtualFlow()), and returns how far
    // the window's first slot moved (estimates above it changed): the engine shifts the window by it.
    double measureWindow(const double *mains, int count);
    // ...and once the extent is published: the anchor correction (same pass), then a re-window request
    // if the viewport is not covered.
    void commitAnchor();
    // ...and when the engine's flush has finished (the next frame shows its result): re-base the anchor
    // on the displayed position.
    void layoutSettled();

signals:
    void modelChanged();
    void delegateChanged();
    void virtualizeChanged();

protected:
    void componentComplete() override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;

private:
    struct Row {
        QVariant data;                  // modelData (the element converted once, when it arrived)
        QPointer<QQuickItem> item;
        QPointer<QQmlContext> context;
        QJSValue js;                    // the element object as received (identity); keyed JS models only
        QString key;                    // its id; set while the key index (m_byKey) is valid
        QVariant incoming;              // staged new modelData of a matched row (the JS pass, 5c)
        int pos = -1;                   // logical index after the last reconciliation (-1: new row)
        quint32 gen = 0;                // reconciliation that claimed it (m_gen)
        quint32 itemGen = 0;            // m_delegateGen of the delegate that built `item`
        double measured = -1;           // margin-box main size when measured, gap-free (5v-3)
        quint32 measuredEpoch = 0;      // valid iff == m_measureEpoch (width/font/theme/delegate)
        bool hasIncoming = false;
    };
    // A row item parked in the recycling pool (5v-2): detached, not materialized, reusable.
    struct Parked {
        QPointer<QQuickItem> item;
        QPointer<QQmlContext> context;
        quint64 reapplyEpoch = 0;       // the theme's reapplyEpoch() when it was parked
        quint32 itemGen = 0;
    };
    // Eager: every row exists. Pending: virtual, waiting for the holder's first layout to decide
    // (nothing materialized). Windowed: only the window exists.
    enum class Mode : quint8 { Eager, Pending, Windowed };

    void rebuild();          // diff the rows against the new model (re-entrant calls are deferred)
    void reconcile();        // one reconciliation pass
    // Identity-first keyed reconciliation of a JS array model (5c). False = the array is not keyed
    // (an element without an id, or a duplicate id): nothing was changed, the caller takes the
    // value-matching path.
    bool rebuildKeyedJs(const QJSValue &array, bool *changed);
    void rebuildFromList(const QVariantList &data, bool *changed);
    void buildKeyIndex();
    int allocSlot();
    void releaseSlot(int slot);
    void finishOrder(bool *changed); // destroy unclaimed rows, adopt m_next as the new order
    Row &rowAt(int i) { return m_pool[m_order[i]]; }
    const Row &rowAt(int i) const { return m_pool[m_order[i]]; }
    bool createRow(int i, const QVariant &data, Row &row);
    void destroyRow(Row &row);
    // Recycling pool (5v-2). retireRow: park the row's item if the pool has room, else destroy it.
    // rebindRow: give logical row i a parked item (false: the pool is empty). trimRecycle: destroy
    // parked items beyond `cap` (0 drains).
    void retireRow(Row &row);
    bool rebindRow(int i, Row &row);
    void trimRecycle(int cap);
    void restack();          // enforce model order among the holder's children
    void notifyLayout();

    // Make exactly [a, b] materialized (b < a: none). `sweepAll` scans every logical row for items
    // outside the range (after a reconciliation); otherwise only the previously materialized range.
    bool materialize(int a, int b, bool sweepAll);
    void targetRange(int *a, int *b);
    void windowRange(int *a, int *b);
    bool measureRows(VirtualFlow *flow) const;
    // Variable extents (5v-3).
    bool autoRowsEligible(VirtualFlow *flow) const;
    bool measuredValid(const Row &r) const { return r.measured >= 0 && r.measuredEpoch == m_measureEpoch; }
    void setMeasured(int i, Row &r, double v);
    void dropMeasurement(int treeIndex, Row &r);
    void invalidateAllMeasurements();
    void checkMeasureKey(double width);
    void ensureTree();
    void fenwickAdd(int i, double ds, int dc);
    double prefix(int k) const;      // logical slot sizes of rows [0, k), estimates included
    int indexAt(double off) const;   // the row whose slot contains `off` (from the first slot), clamped
    double slotTop(int i) const;     // row i's slot top from the first slot: as laid out, else the tree
    double viewportTop(double *vh) const; // contentY in holder coordinates (and the viewport height)
    bool userMoving() const;
    void captureAnchor();
    void anchorAfterReorder(const int *oldOrder, int oldN);
    bool holderHoldsOnlyOurRows() const;
    void registerWithEngine();
    void watchFlickable(QQuickItem *flick);

private slots:
    // The scroll viewport moved or resized: slide the window (windowed mode only).
    void viewportMoved();

private:

    QVariant m_model;
    QQmlComponent *m_delegate = nullptr;
    // ALL logical entries: m_order[i] is the m_pool slot of logical row i (in model order); `item` null
    // when not materialized. Slots are STABLE across reconciliations (a shift moves ints in m_order, not
    // rows), which is what lets m_byKey persist: key -> slot, updated only for inserted/removed rows.
    QVector<Row> m_pool;
    QVector<int> m_order;
    QVector<int> m_next;     // the order being built (swapped with m_order; capacity kept)
    QVector<int> m_free;     // released m_pool slots
    QHash<QString, int> m_byKey;
    bool m_keyIndexValid = false; // m_byKey maps every row's key (first row per key) to its slot
    quint32 m_gen = 0;
    bool m_rebuilding = false;
    bool m_rebuildPending = false;
    QPointer<CssLayoutEngine> m_layout; // resolved at completion; brackets each reconciliation
    QPointer<CssTheme> m_theme;         // resolved at completion; restyles rows recycled across a reapply

    // virtual mode
    bool m_virtualize = false;
    bool m_syncing = false;
    Mode m_mode = Mode::Eager;
    int m_matFirst = 0;      // materialized range [m_matFirst, m_matLast] (contiguous), empty when
    int m_matLast = -1;      // m_matLast < m_matFirst
    int m_matCount = 0;
    VirtualContainer m_container;
    VirtualFlow m_flow;
    QPointer<QQuickItem> m_registeredHolder;
    QPointer<QQuickItem> m_flick;

    // variable extents (5v-3). Fenwick trees (1-based, m_fwN rows in m_order's order) over the VALID
    // measurements: their sizes and their count; an unmeasured row counts m_estimate. Rebuilt O(N) only
    // when the order changed (insert/remove/move — counted as repeater.extentRebuild) or every
    // measurement was invalidated; a single measurement or data change is a point update.
    bool m_variable = false;
    quint32 m_measureEpoch = 1;
    bool m_measureKeyValid = false;
    double m_measureWidth = 0;
    int m_measureFont = 0;
    quint64 m_measureRules = 0;
    double m_measSum = 0;     // sum / count of the valid measurements of rows in m_order
    int m_measCount = 0;
    double m_estimate = -1;   // their mean, gap-free (kept across invalidations as a placeholder); <= 0: unknown
    QVector<double> m_fwSum;
    QVector<int> m_fwCnt;
    int m_fwN = -1;
    int m_fwTop = 0;          // highest power of two <= m_fwN (binary descent)
    bool m_treeDirty = true;
    // the window as last laid out: its first row, the first slot's top and each row's slot size
    bool m_winValid = false;
    int m_winFirst = 0;
    double m_winTop = 0;
    QVector<double> m_winSize;
    // the anchor: a row (its slot — identity across reconciliations) and the viewport top's offset from
    // that row's slot top; its index and neighbours when captured tell a shift from a reorder.
    int m_anchorSlot = -1;
    int m_anchorIndex = -1;
    int m_anchorPrev = -1;
    int m_anchorNext = -1;
    double m_anchorOffset = 0;
    bool m_correcting = false; // contentY written by commitAnchor: not a user scroll
    bool m_wasAtEnd = false;   // the viewport was at the end before this pass (bottom-stick)
    bool m_rewindow = false;   // the window must be recomputed after this pass
    bool m_anchorSettle = false; // a pass committed the anchor: re-base it when the flush ends

    // recycling pool (5v-2): parked items, LIFO; capacity = the window's row count while windowed, 0
    // otherwise. Its storage keeps its capacity: no allocation per scroll in the steady state.
    QVector<Parked> m_recycle;
    int m_recycleCap = 0;
    quint32 m_delegateGen = 0; // bumped by setDelegate: items of an older delegate are never parked
};

} // namespace QmlCss
