#pragma once

#include <QPointer>
#include <QQmlComponent>
#include <QQuickItem>
#include <QVariantList>
#include <QVector>

namespace QmlCss {

class CssLayoutEngine;

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
    int logicalCount() const { return int(m_rows.size()); }

    // --- layout-engine interface (5v-1) -----------------------------------------------------------
    // What the container's own style allows: a vertical scroll box laying its holder out as ONE
    // non-wrapping column with justify-content start. `gap` is the evaluated main-axis gap, `originY`
    // the holder's content origin (padding-top), `flick` the box's scroll Flickable.
    struct VirtualContainer {
        bool eligible = false;
        double gap = 0;
        double originY = 0;
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
    };
    // Called by CssLayoutEngine::layout() for this repeater's holder BEFORE it collects children:
    // re-validates runtime eligibility (container, rows, accessibility) and the window, materializing
    // or destroying rows as needed. True = windowed: the holder's flow is exactly our window.
    bool syncVirtual(const VirtualContainer &container);
    const VirtualFlow &virtualFlow() const { return m_flow; }

signals:
    void modelChanged();
    void delegateChanged();
    void virtualizeChanged();

protected:
    void componentComplete() override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;

private:
    struct Row {
        QVariant data;
        QPointer<QQuickItem> item;
        QPointer<QQmlContext> context;
    };
    // Eager: every row exists. Pending: virtual, waiting for the holder's first layout to decide
    // (nothing materialized). Windowed: only the window exists.
    enum class Mode : quint8 { Eager, Pending, Windowed };

    void rebuild();          // diff m_rows against the new model
    bool createRow(int i, const QVariant &data, Row &row);
    void destroyRow(Row &row);
    void restack();          // enforce model order among the holder's children
    void notifyLayout();

    // Make exactly [a, b] materialized (b < a: none). `sweepAll` scans every logical row for items
    // outside the range (after a reconciliation); otherwise only the previously materialized range.
    bool materialize(int a, int b, bool sweepAll);
    void targetRange(int *a, int *b) const;
    void windowRange(int *a, int *b) const;
    bool measureRows(VirtualFlow *flow) const;
    bool holderHoldsOnlyOurRows() const;
    void registerWithEngine();
    void watchFlickable(QQuickItem *flick);

private slots:
    // The scroll viewport moved or resized: slide the window (windowed mode only).
    void viewportMoved();

private:

    QVariant m_model;
    QQmlComponent *m_delegate = nullptr;
    QVector<Row> m_rows;     // ALL logical entries, in model order; `item` null when not materialized
    QPointer<CssLayoutEngine> m_layout; // resolved at completion; brackets each reconciliation

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
};

} // namespace QmlCss
