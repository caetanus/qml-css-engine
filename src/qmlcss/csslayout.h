#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QExplicitlySharedDataPointer>

#include <array>

class QQuickItem;
class QTimer;

namespace QmlCss {
class CssTheme;
struct CssLength;
struct CssLayoutStyle;

// The CSS box-model layout engine, in C++ (the QML/V4 implementation it replaces was a
// prototype). Given a styled element (a CssRect) and its content holder, it lays the content
// children out per the resolved CSS — display flex/grid/block, gap, justify/align, padding,
// margin, sizing (px/%/calc/min/max/clamp/vw/vh), aspect-ratio, and absolute positioning —
// assigning each child's geometry and reporting the element's content size back via
// implicitWidth/Height. It also drives transform-keyframe animation interpolation.
//
// QML side is a thin shim: CssRect's size/style/children triggers call requestLayout(); the
// engine coalesces to one pass per event-loop turn.
class CssLayoutEngine : public QObject {
    Q_OBJECT

public:
    explicit CssLayoutEngine(CssTheme *theme, QObject *parent = nullptr);

    // Queue a (re)layout of `root`'s content (held by `content`); coalesced.
    Q_INVOKABLE void requestLayout(QQuickItem *root, QQuickItem *content);
    Q_INVOKABLE void notifyParentLayout(QQuickItem *item);
    // notifyParentLayout for an item's (effective) visibility change — except when it merely
    // followed a hidden or detached parent. Hiding or unparenting a subtree flips every
    // descendant, deepest first; each one re-laying its already hidden box made tearing down a
    // list O(N^2). Showing again notifies deepest first too: a box whose container is still to be
    // notified only records its relayout, and the cascade's top flushes once.
    void notifyVisibilityChanged(QQuickItem *item);

    // Layout hibernation (owner's batch gate): while a batch is open, requestLayout only
    // RECORDS work; endBatch runs ONE flush for everything. Brackets bulk style passes
    // (reapplyAll / descendant sweeps) so N applies cost one layout, not N. Outside a
    // batch the flush stays synchronous (the resize stale-frame guarantee is untouched).
    void beginBatch() { ++m_batchDepth; }
    void endBatch();
    // Lay out now.
    Q_INVOKABLE void layout(QQuickItem *root, QQuickItem *content);

    // Evaluate a CSS length value (px/%/calc()/min()/max()/clamp()/vw/vh/…) against an axis
    // length; NaN when absent/non-numeric. Exposed for any QML that still needs it.
    Q_INVOKABLE qreal length(const QString &value, qreal avail) const;

    // Test/bench probe: children visited across every layout() call. One integer add per call —
    // lets tests assert O(N) vs O(N^2) layout work without timing. A running total for the engine's
    // lifetime (never reset): callers diff a before/after pair.
    quint64 childVisits() const { return m_childVisits; }

    // Transform-keyframe animation. buildAnimStops normalizes `@keyframes` frames into
    // [{offset, rotate, scale, tx, ty}] (identity-filled, 0%/100% synthesised); applyAnim
    // writes the interpolated transform at `progress` onto root's _animRotate/Scale/Tx/Ty.
    Q_INVOKABLE QVariantList buildAnimStops(const QVariantList &frames) const;
    Q_INVOKABLE void applyAnim(QQuickItem *root, const QVariantList &stops, qreal progress) const;

public slots:
    // Re-queue layout for every registered root — wired to CssTheme::viewportChanged so
    // vh/vw-derived lengths follow the window on single-event (tiling) resizes.
    void relayoutKnownRoots();

private slots:
    void flush();

private:
    struct Track {
        enum Kind { Px, Fr, Auto } kind = Auto;
        double value = 0;
    };

    // length / calc
    double evalExpr(const QString &value, double avail) const;
    double calcUnit(const QString &tok, double avail) const;

    // A parsed length of `st` evaluated against `avail` (percentages) and the CURRENT viewport —
    // exactly what evalExpr/calcUnit return for its source string. NaN when absent/invalid.
    double eval(const CssLayoutStyle &st, const CssLength &l, double avail) const;
    double evalSlow(const CssLayoutStyle &st, const CssLength &l, double avail) const;

    // box-model helpers (public: containers read padding-bottom for scroll breathing)
public:
    // Box sides [top, right, bottom, left] of a parsed style.
    using Sides = std::array<double, 4>;
    Sides paddingOf(const CssLayoutStyle &st) const;
    Sides borderOf(const CssLayoutStyle &st) const;
    Sides marginOf(const CssLayoutStyle &st) const;
    // Clamp a resolved width/height by the item's min-/max- of that axis.
    double clampSize(const CssLayoutStyle &st, const CssLength &mn, const CssLength &mx, double v, double avail) const;
    // String path, kept for the grid.
    double sizeOf(const QVariantMap &style, const QString &key, double avail) const;
    double aspectOf(const QVariantMap &style) const;

private:
    // The typed style of a layout child: the cached one of every CssBox (a reference held for the
    // pass, as the old by-value map copy was); a foreign item that exposes a `style` property is read
    // by name and parsed afresh (rare, counted by g_namedStyleReads).
    using StyleRef = QExplicitlySharedDataPointer<CssLayoutStyle>;
    static StyleRef layoutStyleOf(const QQuickItem *k);

    // geometry assignment. `style` is the child's own parsed style, already held by every
    // caller (read once per child per pass, never re-read here).
    void place(QQuickItem *k, const CssLayoutStyle &style, double x, double y, double w, double h) const;
    void placeAbsolute(QQuickItem *k, const CssLayoutStyle &ks, double ox, double oy, double cw, double ch) const;

    // flex + grid; return the content [width, height] occupied
    QPair<double, double> layoutFlex(const QList<QQuickItem *> &flow, const CssLayoutStyle &rootStyle,
                                     double cw, double ch, double ox, double oy, bool isFlex) const;
    QPair<double, double> layoutGrid(const QList<QQuickItem *> &flow, const QVariantMap &rootStyle,
                                     double cw, double ch, double ox, double oy) const;

    // grid tracks
    QVector<Track> parseTracks(const QString &spec, double avail) const;
    Track parseTrack(const QString &token, double avail) const;
    QString expandRepeat(const QString &spec) const;
    QStringList gridSplit(const QString &spec) const;
    QVector<double> resolveTracks(const QVector<Track> &tracks, double avail, double gap,
                                  const QVector<double> &contentSizes) const;

    CssTheme *m_theme;
    QHash<QQuickItem *, QPointer<QQuickItem>> m_pending; // root -> content
    QHash<QQuickItem *, QPointer<QQuickItem>> m_known;   // every root ever laid out (viewport relayout)
    QTimer *m_flushTimer;
    int m_batchDepth = 0;
    bool m_flushing = false;
    quint64 m_childVisits = 0;
};

} // namespace QmlCss
