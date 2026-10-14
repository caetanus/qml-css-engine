#pragma once

#include <QColor>
#include <QPointer>
#include <QQmlListProperty>
#include <QQuickItem>
#include <QVariant>
#include <QVarLengthArray>
#include <QVariantList>
#include <QVariantMap>

#include "qmlcss/cssbox.h"
#include "qmlcss/csscontentholder.h"

#include <memory>

class QVariantAnimation;

namespace QmlCss {
class CssTheme;
class CssLayoutEngine;

// Native CSS box, translated 1:1 from qml/qmlcss/CssRect.qml — BY COMPOSITION, not
// reimplementation. A CssRect is BOTH a painted CSS box AND a layout container for its content.
//
// It does NOT paint anything itself: it instantiates a REAL QtQuick render subtree via the Qt
// type-system (a `Shape` fill — a closed rounded-rect ShapePath, so solid colour, linear/radial
// gradients, per-corner border-radius and the alpha-fix all fall out of the Shape's fill; a
// separate border `Shape`; per-side border + inset-bevel lines as `Shape`/`PathRectangle` bars;
// `box-shadow` via a REAL QtQuick.Effects `MultiEffect`) and drives that subtree's inputs from
// C++ (the CSS→value resolution the QML `readonly property` bindings did, via cssTheme). Rectangle
// is deliberately avoided per the owner's rule (it is capped for several effects); Shape is the
// general vector path.
//
// The fill's alpha rides on the fill Shape's `opacity` (a translucent Shape fill blends wrong —
// render opaque, alpha via opacity), and the render subtree is a SIBLING of the content holder,
// so that opacity dims only the fill, never the element's content.
//
// As a container it hosts declared children in an inner `contentHolder` (the QML
// `default property alias content: contentHolder.data`) and drives cssLayout.requestLayout(...)
// on geometry/style/children changes — the box model (flex/grid/calc/…) runs in C++
// (CssLayoutEngine); this only TRIGGERS a relayout.
//
// Carries the standard CSS signature (cssId/cssAlternateId/cssClass/cssState/cssPrimitive/
// cssPart/style); resolved rules are pushed back into `style` by cssTheme.loadCss(this).
class CssRect : public CssBox {
    Q_OBJECT
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)

    // Engine-driven hover: the theme flips cssHoverStyled when an applicable `:hover` rule
    // exists (we then compose a HoverHandler); the handler writes cssEngineHover, which
    // re-applies the style with the extra "hover" state class (CssBox::identityChanged).
    Q_PROPERTY(bool cssHoverStyled READ cssHoverStyled WRITE setCssHoverStyled NOTIFY cssHoverStyledChanged)
    Q_PROPERTY(bool cssEngineHover READ cssEngineHover WRITE setCssEngineHover NOTIFY cssStateChanged)

public:
    // --- CSS transitions (item-level) ----------------------------------------------------
    // The declared `transition` (property/duration/easing), parsed at style apply. The layout
    // engine consults this when writing width/height, so declared geometry transitions
    // animate instead of snapping. Covers-check: empty/`all` or the exact property.
    bool transitionCovers(QLatin1String prop) const
    {
        // m_transMs > 0 implies the extra holds the parsed spec.
        return m_transMs > 0
            && (m_extra->transProp.isEmpty() || m_extra->transProp == QLatin1String("all")
                || m_extra->transProp == prop);
    }
    // Animate width/height to `target`; returns false when no covering transition (the caller
    // writes directly). Idempotent while already animating toward the same target.
    bool animateGeometry(QLatin1String prop, qreal target);

    // Task 4 review finding #1: CssLayoutEngine::layout()'s scroll-extent walk must use this, not
    // height(), for a CssRect child — while a `transition: height` is ticking (animateGeometry),
    // height() is the CURRENT animated frame, not where the box is headed. Falls back to height()
    // when no height animation is running.
    qreal targetHeight() const;

    // 5v-2: a row recycled by a virtual CssRepeater starts clean — no engine hover left from the row's
    // previous record, no geometry transition still ticking (snapped to its target), and the fast fill
    // re-pushed with transitions off so a colour change caused by the NEW record is not faded from the
    // OLD record's colour. Called after the row was rebound to its new record, under its ancestors.
    void resetTransientState();

private:

    // Renderer defaults (used when the style omits the longhand) — the QML `default*` props,
    // set by the CssFill shim.
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)
    Q_PROPERTY(QColor defaultColor READ defaultColor WRITE setDefaultColor NOTIFY defaultColorChanged)
    Q_PROPERTY(QColor defaultBorderColor READ defaultBorderColor WRITE setDefaultBorderColor NOTIFY defaultBorderColorChanged)
    Q_PROPERTY(qreal defaultBorderWidth READ defaultBorderWidth WRITE setDefaultBorderWidth NOTIFY defaultBorderWidthChanged)

    // Inherited text properties (CSS inheritance) — own style wins, else the CSS-inheriting
    // ancestor (the containing CssRect at parent.parent). CssText children read these.
    Q_PROPERTY(QString inheritedColor READ inheritedColor NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontFamily READ inheritedFontFamily NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontSize READ inheritedFontSize NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontWeight READ inheritedFontWeight NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedLineHeight READ inheritedLineHeight NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedLetterSpacing READ inheritedLetterSpacing NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedTextTransform READ inheritedTextTransform NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedTextAlign READ inheritedTextAlign NOTIFY inheritedChanged)

    // Written by CssLayoutEngine::applyAnim (transform keyframe interpolation); drive the item's
    // rotation/scale/translate while an `animation` runs.
    Q_PROPERTY(qreal _animRotate READ animRotate WRITE setAnimRotate NOTIFY animChanged)
    Q_PROPERTY(qreal _animScale READ animScale WRITE setAnimScale NOTIFY animChanged)
    Q_PROPERTY(qreal _animTx READ animTx WRITE setAnimTx NOTIFY animChanged)
    // The scroll position of an `overflow: auto` box, as the DOM names it (0 when it does not
    // scroll): an app windows a long list on these, e.g. `onScroll` near the end loads more.
    Q_PROPERTY(qreal scrollTop READ scrollTop NOTIFY scrollChanged)
    Q_PROPERTY(qreal scrollHeight READ scrollHeight NOTIFY scrollChanged)
    Q_PROPERTY(qreal clientHeight READ clientHeight NOTIFY scrollChanged)
    Q_PROPERTY(qreal _animTy READ animTy WRITE setAnimTy NOTIFY animChanged)

    // Normalised animation position 0→1; driven by the composed NumberAnimation (see
    // componentComplete). On every tick, the setter calls cssLayout.applyAnim() to interpolate
    // the @keyframes stops and write _animRotate/_animScale/_animTx/_animTy. Equivalent to the
    // QML `property real animTick` + `onAnimTickChanged` + `NumberAnimation on animTick`.
    Q_PROPERTY(qreal animTick READ animTick WRITE setAnimTick NOTIFY animTickChanged)

    // The container's default child slot (QML `default property alias content: contentHolder.data`).
    Q_PROPERTY(QQmlListProperty<QObject> content READ content)
    Q_CLASSINFO("DefaultProperty", "content")

public:
    qreal scrollTop() const { QQuickItem *f = scrollFlickable(); return f ? f->property("contentY").toReal() : 0.0; }
    qreal scrollHeight() const { QQuickItem *f = scrollFlickable(); return f ? f->property("contentHeight").toReal() : height(); }
    qreal clientHeight() const { QQuickItem *f = scrollFlickable(); return f ? f->height() : height(); }
    // Engine-internal: CssLayoutEngine::layout() checks this BEFORE walking children to publish
    // the scroll extent — only scroll boxes need that work (Task 4 review finding #3).
    bool hasScroll() const { return m_extra && m_extra->flickable; }
    // Engine-internal (5v-1): the composed scroll Flickable (null when the box does not scroll).
    QQuickItem *scrollFlickable() const { return m_extra ? m_extra->flickable.data() : nullptr; }
    // Engine-internal: the layout engine this box requests its relayouts from.
    CssLayoutEngine *layoutEngine() const;
    explicit CssRect(QQuickItem *parent = nullptr);
    ~CssRect() override;

    // Engine-internal: the layout pass inspects a CHILD box's content to classify it (e.g. the
    // replaced-foreign inference) — direct pointer, no list-property indirection in the hot path.
    QQuickItem *layoutContentHolder() const { return m_contentHolder; }

    // Called by CssLayoutEngine after each layout of this box: the vertical extent of the children it
    // placed. Drives the scroll contentHeight (instead of QQuickItem::childrenRect()).
    void setLaidOutExtent(qreal top, qreal bottom);

    QVariantMap style() const { return m_style; }
    void setStyle(const QVariantMap &v);

    bool cssHoverStyled() const { return m_hoverStyled; }
    void setCssHoverStyled(bool v);
    bool cssEngineHover() const { return m_engineHover; }
    void setCssEngineHover(bool v);
    Q_SLOT void onEngineHoverChanged(); // composed HoverHandler.hoveredChanged -> cssEngineHover
    // Compose the Flickable and move the content holder into it (overflow-y: auto/scroll).
    void ensureScrollable();
    // Lazy composition of the per-box extras (most boxes never need them).
    void ensureTranslate();
    void ensureRotationX();
    void updateRotationXOrigin();
    void ensureAnim();
    // Compose the paint subtree on demand (lean shell: layout-only boxes skip it entirely).
    void ensureRenderShell();
    // The cheap Rectangle path of the Shape x Rectangle policy (see needsShape in the .cpp).
    void ensureFastRect();
    void pushFastRect();
    // Item-level effects that apply with or without the shell (static transform + @keyframes).
    void applyStaticTransformAndAnim();
    Q_SLOT void syncScrollContent();

    qreal radius() const { return m_extra ? m_extra->radius : 0.0; }
    void setRadius(qreal v);
    QColor defaultColor() const { return m_extra ? m_extra->defaultColor : QColor(Qt::transparent); }
    void setDefaultColor(const QColor &v);
    QColor defaultBorderColor() const { return m_extra ? m_extra->defaultBorderColor : QColor(Qt::transparent); }
    void setDefaultBorderColor(const QColor &v);
    qreal defaultBorderWidth() const { return m_extra ? m_extra->defaultBorderWidth : 0.0; }
    void setDefaultBorderWidth(qreal v);

    QString inheritedColor() const;
    QString inheritedFontFamily() const;
    QString inheritedFontSize() const;
    QString inheritedFontWeight() const;
    QString inheritedLineHeight() const;
    QString inheritedLetterSpacing() const;
    QString inheritedTextTransform() const;
    QString inheritedTextAlign() const;
    bool takesPartInInheritance() const override { return true; }

    qreal animRotate() const { return m_extra ? m_extra->animRotate : 0.0; }
    void setAnimRotate(qreal v);
    qreal animScale() const { return m_extra ? m_extra->animScale : 1.0; }
    void setAnimScale(qreal v);
    qreal animTx() const { return m_extra ? m_extra->animTx : 0.0; }
    void setAnimTx(qreal v);
    qreal animTy() const { return m_extra ? m_extra->animTy : 0.0; }
    void setAnimTy(qreal v);

    qreal animTick() const { return m_extra ? m_extra->animTick : 0.0; }
    void setAnimTick(qreal v);

    QQmlListProperty<QObject> content();

    // QML `requestRelayout()` — invoked by CssLayoutEngine::notifyParentLayout and on our own
    // geometry/style/children changes. Must be invokable (invokeMethod by name).
    Q_INVOKABLE void requestRelayout();

    // Connect each FOREIGN (non-Css) child's implicit-size signals to a relayout. A Css child
    // self-notifies its parent on implicit change (see componentComplete), but a hand-written .qml
    // escape hatch is a plain Item with no such wiring — so its late/dynamic implicit growth would
    // otherwise never re-flow the box (and a scroll's contentHeight would stay short of it).
    void watchForeignChildren();
    void watchForeignChild(QQuickItem *k);

signals:
    void scrollChanged();
    void styleChanged();
    void cssHoverStyledChanged();
    void radiusChanged();
    void defaultColorChanged();
    void defaultBorderColorChanged();
    void defaultBorderWidthChanged();
    void inheritedChanged();
    void animChanged();
    void animTickChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    // The typed hooks that replaced this box's per-item connections (see CssBox).
    void ownImplicitSizeChanged() override;
    void ownVisibilityChanged() override;
    void inheritSourceChanged() override;
    void contentChildAdded(QQuickItem *child) override;
    void contentChildRemoved(QQuickItem *child) override;

private:
    // Re-run the CSS→value resolution the QML `readonly property` bindings did (colours, radii,
    // gradient, border, box-shadow, transform, transition) and push it onto the composed render
    // subtree. The equivalent of every `style`-derived binding in CssRect.qml.
    void recompute();
    // anchors.fill equivalent: paint layers span the border box; the content holder (or its scroll
    // Flickable) is the PADDING BOX — inset by the cached border widths (web content area).
    void layoutChildren();
    // Refresh m_borderInsets from the resolved style (style-set time, never per-resize).
    void updateBorderInsets();
    // Applies the active transform — if _animActive (stops.length >= 2), uses the animated
    // _animRotate/_animScale/_animTx/_animTy (written each tick by applyAnim); otherwise uses the
    // static transform resolved from style["transform"]. Pivot is the element centre (CSS default,
    // QML: transformOrigin: Item.Center). Faithful to the QML rotation/scale/transform bindings.
    void applyTransform();
    // Re-style through the reverse-slot engine path when identity changes (QML onCss*Changed).
    void maybeLoadCss() override;
    void applyResolvedStyle(const QVariantMap &style) override { setStyle(style); }
    QQuickItem *inheritanceParent() const override;

protected:
    // Protected so C++ widget subclasses (the widgets-to-cpp port) can chain their own
    // item-change handling through the scene-attach resolve heal.
    void itemChange(ItemChange change, const ItemChangeData &data) override;

private:
    // The CSS-inheriting ancestor (containing CssRect at parent.parent), or null.
    CssRect *cssParent() const;
    // emit inheritedChanged + walk the followers (every inherited change this box reports).
    void notifyInherited();

    // --- memory step 1: hot state inline, feature state in a lazily allocated extra -------------------
    // Inline: what the layout pass, the CSS apply and the geometry path read on EVERY box. Everything a
    // box only needs once a feature is in use (renderer defaults, transforms/@keyframes, transitions,
    // scroll, the paint shells, the hover handler) lives in Extra, allocated on first use (ensureExtra)
    // and never freed before the box: zero cost for a layout-only box, one allocation for the rest.
    struct AnimStop { double offset, rotate, scale, tx, ty, rotX; };
    struct Extra {
        // Renderer defaults (the QML default* props, set by the CssFill shim).
        qreal radius = 0;
        QColor defaultColor = QColor(Qt::transparent);
        QColor defaultBorderColor = QColor(Qt::transparent);
        qreal defaultBorderWidth = 0;
        // Animated transform channels — written each tick by setAnimTick()/CssLayoutEngine::applyAnim().
        qreal animRotate = 0, animScale = 1, animTx = 0, animTy = 0, animRotX = 0;
        // Static (non-animated) transform resolved from `style` (rotate deg / scale / tx,ty px).
        qreal staticRotate = 0, staticScale = 1, staticTx = 0, staticTy = 0, staticRotX = 0;
        // transform-origin, as fractions of the box (CSS default: the centre). It pivots rotate and
        // scale (the item's transformOrigin: the nine anchor points) and rotateX (the Rotation's origin).
        qreal originFx = 0.5, originFy = 0.5;
        // @keyframes driver: the normalised position (QML `property real animTick`), the stops
        // (buildAnimStops output) and the same stops pre-parsed for the per-frame tick (the QVariantMap
        // form costs allocations EVERY frame at 60fps), and the REAL NumberAnimation on animTick (0→1).
        qreal animTick = 0.0;
        QVariantList animStops;
        QList<AnimStop> animStopsFast;
        QPointer<QObject> anim;
        QPointer<QObject> translate; // the QtQuick Translate in our transform list
        QPointer<QObject> rotationX; // the QtQuick Rotation about X (rotateX), created on first use
        // CSS transition (parsed from `transition` at style apply; m_transMs stays inline).
        int transEasing = 1; // QEasingCurve::InOutQuad set at parse; int to avoid the header dep
        QString transProp;
        QVariantAnimation *opacityAnim = nullptr; // lazy; animates our own opacity
        QVariantAnimation *widthAnim = nullptr;   // lazy; driven via animateGeometry
        QVariantAnimation *heightAnim = nullptr;
        qreal widthAnimTarget = -1;
        qreal heightAnimTarget = -1;
        // overflow(-y): auto/scroll — composed Flickable hosting the contentHolder.
        QPointer<QQuickItem> flickable;
        // The paint shells: the REAL QtQuick render subtree (Shapes + MultiEffect), or the fast
        // QQuickRectangle when the style is rectangle-safe.
        QPointer<QQuickItem> render;
        QPointer<QQuickItem> fastRect;
        // The fast Rectangle that already received its FIRST style (a transition animates a change,
        // never the first style). A pointer compare per push — no meta-object lookup on the hot path;
        // QPointer, so a Rectangle recreated at a recycled address still counts as new.
        QPointer<QQuickItem> primedFast;
        QPointer<QObject> hoverHandler; // composed QtQuick HoverHandler (created on demand)
    };
    Extra &ensureExtra()
    {
        if (!m_extra)
            m_extra.reset(new Extra);
        return *m_extra;
    }

    // Resolved border widths [t,r,b,l] — cached at style-set time so layoutChildren (per-resize
    // hot path) never parses style strings. The content holder is inset by these (padding box).
    double m_borderInsets[4] = { 0.0, 0.0, 0.0, 0.0 };
    QPointer<CssLayoutEngine> m_layout;
    // Hosts declared children (the layout participants). Created in the constructor, a QObject child we
    // never delete before ourselves: a plain pointer.
    CssContentHolder *m_contentHolder = nullptr;
    qreal m_extentTop = 0.0;    // vertical extent of the last layout pass — drives scroll contentHeight
    qreal m_extentBottom = 0.0;
    std::unique_ptr<Extra> m_extra;
    // Transition duration (0: none) — read by the layout on every geometry write (transitionCovers).
    int m_transMs = 0;
    bool m_animActive = false; // _animStops.length >= 2
    // Engine-driven hover tracking (see cssHoverStyled/cssEngineHover above).
    bool m_hoverStyled = false;
    bool m_engineHover = false;
};

} // namespace QmlCss
