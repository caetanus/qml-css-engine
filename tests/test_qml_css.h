#pragma once

#include <QObject>

class QmlCssTests final : public QObject {
    Q_OBJECT

private slots:
    // Register the classic C++ types (CssRect/CssHr/CssImage) into the `qmlcss` module once,
    // before any test — so `import qmlcss` resolves everywhere (incl. the CssFill shim).
    void initTestCase();

    void cssRectLoadsAndRestyles();
    void cssTextUsesStandaloneDefaults();
    void cssItemAppliesToParent();

    // Layout (geometry) tests — exercise CssLayoutEngine directly.
    void layoutFlexColumnStacks();
    void layoutClampsMaxAndMinWidth();
    void layoutHonoursFlexOrder();
    void layoutAppliesFlexBasis();
    void layoutVisibilityHiddenKeepsSpace();
    void layoutGridTemplateAreas();
    void layoutFlexShrinkOverflow();
    void layoutAlignContentCenter();
    void layoutBoxSizingBorderBox();
    void buttonLabelInheritsColor();
    void deepInheritanceIsLinear();
    void fillImageLayersAreLazy();

    // New CSS property mapping tests.
    void transitionSkipsFirstStyle();
    void cssRectZIndex();
    void cssRectOverflowHidden();
    void cssTextDecorationUnderline();
    void cssTextWhiteSpaceNowrap();
    void cssTextOverflowEllipsis();

    // C++ CssRect — composition translation of CssRect.qml.
    void cssRectCppComposesShapeAndContains();
    void cssRectBorderStyleDottedStrokes();
    void scenelessResolveHealsOnSceneAttach();

    // C++ CssHr — composition translation of CssHr.qml.
    void cssHrCppComposesRealShape();

    // C++ CssImage — composition translation of CssImage.qml.
    void cssImageCppComposesImageEffectAndMask();
    void cssImageCppNoRadiusShowsImageDirect();

    // C++ CssText — composition translation of CssText.qml.
    void cssTextCppComposesRealText();

    // C++ CssFill — composition translation of CssFill.qml.
    void cssFillCppComposesImageRectAndHostsChildren();

    // C++ CssDropShadow — composition translation of CssDropShadow.qml.
    void cssDropShadowCppComposesMultiEffect();

    // C++ CssKeyframes — composition translation of CssKeyframes.qml.
    void cssKeyframesCppInterpolatesTarget();

    // C++ CssRect @keyframes animation driver — live NumberAnimation on animTick.
    void cssRectCppKeyframesDriverLive();

    // C++ CssIcon — composition translation of CssIcon.qml.
    void cssIconCppComposesImageAndEffect();

    // C++ CssFillLayer — composition translation of CssFillLayer.qml.
    void cssFillLayerCppComposesShape();

    // C++ CssItem — composition translation of CssItem.qml (migrated from cssItemAppliesToParent).
    void cssItemCppAppliesToParent();

    // C++ Contrast — WCAG contrast utilities, ported from Contrast.js.
    void contrastCppPure();
    void contrastQmlSingleton();

    // notifyParentLayout must duck-type like the QML original: a plain-Item grandparent
    // (no requestRelayout()) is skipped silently, not invoked-and-warned.
    void layoutNotifyParentSkipsNonBoxAncestors();

    // A viewport height change alone must re-run layout: vh lengths resolve at layout
    // time, and a tiling WM resizes in ONE event (no second event to converge on).
    void viewportChangeRelayoutsVhHeights();
    // A viewport change must flush once per batch, not once per registered root: R known
    // roots previously meant R synchronous flush() calls (and any cascade re-doing boxes
    // already laid out in an earlier flush within the same resize).
    void viewportChangeIsOneFlush();

    // Ancestor-scoped rules end-to-end: `.nav button` styles only buttons under `.nav`,
    // and a class/state change on an ANCESTOR restyles the scoped descendants too.
    void ancestorScopedRulesStyleTheTree();

    // `property Item cssAncestor: owner` re-anchors the ancestor walk for subtrees Qt
    // reparents out of the item tree (Popup contents under the window Overlay), so
    // `.wg-select .popup` keeps matching.
    void cssAncestorReanchorsDetachedSubtree();

    // CssText must paint `background-color` (+ border-radius) behind the composed Text,
    // like the web does for any element (cssgaps `.wrap/.nowrap/.ellipsis` tiles).
    void cssTextCppPaintsBackground();

    // Applying a style with NO `display` must not touch the item's `visible` — an imperative
    // setVisible(true) destroys author bindings like `visible: cond` (the <Show> guard).
    void styleApplyPreservesVisibleBinding();

    // text-shadow keeps the composed Text visible (a hidden MultiEffect source renders empty).
    void cssTextShadowKeepsLabelVisible();

    // CSS transitions must animate the declared properties, not snap:
    // item opacity (`transition: opacity`) and layout-applied width (`transition: width`).
    void transitionAnimatesItemOpacity();
    void transitionAnimatesLayoutWidth();

    // An applicable `:hover` rule makes the engine hover-track the element itself (the web
    // hovers ANY element; the transpiler only wires interactive ones).
    void hoverRuleEnablesEngineHoverTracking();

    // overflow-y: auto/scroll composes a real Flickable hosting the content (QML's native
    // scrolling), with contentHeight tracking the laid-out children.
    void overflowScrollComposesFlickable();

    // CssRepeater is KEYED (Solid's <For>): a reorder MOVES the existing delegates
    // (same instances, re-stacked); only added entries create, removed ones destroy.
    void cssRepeaterReordersWithoutRecreating();
    // Objects with an "id" match by it: a model rebuilt from fresh JSON keeps every delegate,
    // and a row whose content changed is updated in place (modelData), not re-created.
    void cssRepeaterKeysObjectsById();

    // Shape x Rectangle policy: rectangle-safe styles compose a cheap QQuickRectangle; a
    // reapply that needs Shape features swaps the composition, and back.
    void rectanglePolicyFastPathAndSwap();

    // A box holding ONLY a foreign (non-Css) component behaves like a web REPLACED element:
    // intrinsic size, no cross-axis stretch, capped at the container with the ratio kept.
    void layoutReplacedForeignBoxKeepsIntrinsicSize();

    // Complexity probes (step 1 of the native-beats-web roadmap): layout work is counted in child
    // visits, so O(N) vs O(N^2) is asserted without timing.
    void layoutCountsChildVisits();
    void cssRepeaterAppendAndClearAreLinear();
    void contentHolderReportsLateForeignChild();
    void scrollExtentFollowsLayoutOnClear();
    void scrollExtentFollowsLateCssChildGrowth();

    // Task 4 review fixes: the scroll extent must use TARGET (not mid-transition) geometry for
    // flow children, keep the old childrenRect() padding-top breathing, and still pick up a
    // foreign child whose height is set explicitly (no Css children at all).
    void scrollExtentUsesTransitionTarget();
    void scrollExtentKeepsPaddingTopBreathing();
    void scrollExtentFollowsForeignChildSetHeight();

    // Step 2 (native beats web): registry and model bookkeeping stay O(1) per element.
    void themeRegistryForgetsDestroyedItems();
    // Step 2 Task 2: a class/state change restyles its subtree only, not every registered object.
    void stateChangeRestylesOnlyTheSubtree();
    // Step 2 Task 3: restack keeps document order through create/swap/remove/append, including
    // when new rows land after a static sibling (the footer).
    void cssRepeaterSwapKeepsDocumentOrder();
    // Step 2 Task 4: a surviving row that did not move must not have `index` re-set — re-setting
    // an unchanged context property still notifies every binding that reads it.
    void cssRepeaterWritesIndexOnlyWhenChanged();
    // Task 4 review: a row that DID move must still get the new index written (the
    // positive branch of `found != i` — the companion to ...WhenChanged above, which only
    // covers the negative/skip branch).
    void cssRepeaterMovedRowGetsNewIndex();
};
