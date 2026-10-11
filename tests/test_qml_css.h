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
    void listTeardownIsLinear();
    void listShowIsLinear();
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

    // Rows complete INSIDE the scene (staged creation, C22): one CSS resolve per styled element, with
    // ancestor-scoped rules already applied by it — no truncated resolve healed on scene join.
    void cssRepeaterRowsResolveOnceInsideTheScene();

    // An incubated page is parented in setInitialState (between beginCreate and completeCreate): one resolve.
    void cssIncubatorPageResolvesOnceInsideTheScene();

    // Step 3 Task 1 (C21): every item an engine type composes via QQmlComponent::create/
    // beginCreate must be a QObject CHILD of its composing owner too, not just a visual
    // (setParentItem) child — ~QQuickItem only detaches visual children on destruction, it
    // never deletes them, so a composed part with no QObject parent leaks.
    void composedItemsDieWithTheirOwner();

    // 4b-1: every element type that carries a CSS identity is a CssBox, and the typed accessors
    // return what QML set. CssIcon (style sink, no identity) is NOT a CssBox.
    void everyIdentityTypeIsACssBox();

    // 4b-1 Task 2: resolving a CssBox reads its identity and its ancestors' identities by
    // member, not by name.
    void identityReadsAreTyped();

    // 4b-1 Task 3: the layout reads a CssBox child's resolved style by member, not by name.
    void layoutReadsStyleTyped();

    // 4b-1 Task 4: an identity write on a registered box restyles it exactly ONCE (the setter used
    // to resolve AND the theme's notify connection re-resolved), and every identity field restyles —
    // alternateId and cssPart included; CssImage state/primitive and CssHr primitive too.
    void identityChangeRestylesOnce();
    void engineHoverRestylesOnce();
    void alternateIdChangeRestyles();
    void cssPartChangeRestyles();
    void imageStateAndPrimitiveRestyle();
    void hrPrimitiveRestyles();

    // 4b-2 Task 1: every box points to an interned selector-context node (its identity + its nearest
    // CSS ancestor's node). Identical siblings share one node; an ancestor identity change, a reparent
    // and a rule rebuild each give the box a new node; nodes die with the last box that holds them.
    void contextNodesAreSharedByIdenticalSiblings();
    void contextNodeFollowsAncestorIdentityAndReparenting();
    void contextNodesDieWithTheirBoxes();
    // var(): an ancestor's custom property reaches a descendant, follows the ancestor's identity, and rows
    // still share one context node.
    void customPropertiesCascadeThroughContextNodes();
    // 4b-2 Task 2: own resolves are memoised on the node: 100 identical rows resolve each own input once.
    void identicalRowsResolveOnce();

    // 4b-4 Task 1: a CssText computes its inherited text values through ONE typed walk (CssBox::
    // inheritedText), never by QObject::property("inherited*") on a CssBox ancestor.
    void inheritanceWalkIsTyped();
    // ...and every inherited* getter returns exactly what it returned before, on a nested tree with
    // own/inherited values, a level that sets nothing and the CssText grandparent rule.
    void inheritanceGettersUnchanged();

    // 4b-4 Task 2: applyToText diffs a typed payload against the last one applied — a re-apply with
    // nothing changed writes nothing to the composed label; one changed value writes one property.
    void textPayloadWritesOnlyChanges();
    // Step 5d: a CssText builds its payload once per actual input change — a mount resolves each text once,
    // an ancestor restyle that changes nothing inherited resolves nothing, a real change resolves once.
    void textPayloadResolvedOncePerChange();

    // 4b-3 Task 2: a box's flex style is parsed ONCE per resolved style — a second full relayout of
    // 100 identical rows parses no length/box/shorthand string at all.
    void layoutParsesOncePerStyle();

    // Step 5a Task 1: the opt-in phase timers (SQ_PHASE_STATS) cost nothing when disabled — a
    // 100-row mount leaves every bucket at count 0 — and count exactly what they're told to when on.
    void phaseStatsAreFreeWhenDisabled();

    // Step 5b Task 2: identical classes resolve to the SAME shared map (the context node's own-resolve
    // memo, 4b-2), but alternating creation order used to defeat the one-entry cssSharedLayoutStyle
    // memo — every box got its OWN parsed CssLayoutStyle. The bounded cache shares across the whole
    // rotation: 300 boxes of 3 distinct classes hold only 3 distinct CssLayoutStyle pointers.
    void typedStylesSharedAcrossAlternatingClasses();

    // Step 5v-1: a `virtualize: true` CssRepeater in a vertically scrolling flex/block column with a
    // runtime-proven FIXED row pitch instantiates only the rows in the viewport +- one viewport,
    // publishes the exact LOGICAL extent, renders pixel-identical to the eager list, touches only
    // window rows on keyed updates, and falls back to eager when any eligibility condition breaks.
    void virtualListMaterializesOnlyTheWindow();
    void virtualListScrollsExactly();
    void virtualListMatchesEagerPixels();
    void virtualListKeyedUpdate();
    void virtualListFallsBackEager();

    // Step 5c: identity-first incremental reconciliation. Unchanged elements (the SAME JS object) cost
    // no conversion and no context write; a new object with the same id still updates in place (C23);
    // an object mutated in place is not re-read (owner decision, Solid web semantics); duplicate ids
    // fall back to unkeyed value matching exactly as before.
    void repeaterIdentityFastPath();
    void repeaterNewObjectSameIdUpdates();
    void repeaterMutatedSameObjectIsNotReread();
    void repeaterDuplicateIdsFallBackToUnkeyed();
    // A model set from inside a reconciliation (a row reacting to its new modelData) runs after it.
    void repeaterReentrantModelChangeIsDeferred();
    void virtualListIdentityOps();
    void repeaterKeyedOpsBench();

    // Step 5v-2: rows leaving the virtual window are parked in a BOUNDED pool and rebound to rows
    // entering it — a full traversal creates ~one window of rows, recycled rows show their new record
    // (data, class, pixels), carry no hover/transition state over, the pool drains on clear / eager /
    // delegate change / destruction, and a theme reload while rows are parked stays bounded and correct.
    void virtualPoolBoundsCreations();
    void virtualPoolRecycledRowsAreCorrect();
    void virtualPoolResetsState();
    void virtualPoolDrains();
    void virtualPoolThemeReload();

    // Step 5v-3: rows whose height comes from their own content (auto height, wrapping labels) are
    // windowed too, over a measured/estimated logical extent (Fenwick prefix sums), and the viewport is
    // anchored to a row key so estimate corrections, invalidations and removals never move what is on
    // screen. Fixed-height lists keep the exact 5v-1 path without any estimation.
    void virtualAutoHeightWindows();
    void virtualAnchorNoJump();
    void virtualAutoHeightPixelParity();
    void virtualMeasurementInvalidation();
    void virtualAnchorRemoved();
    void virtualFixedPitchStaysExact();

    // Correctness round after the scrolling-list gate: (A) a row parked while an ANCESTOR's identity
    // changed comes back styled for the new ancestry; (B) a container gap change re-prices every
    // measured row (offscreen ones included) — extent and positions equal eager at once; (C) a screen
    // reader activating late (no relayout of its own) turns virtual lists eager, and windowed again
    // when it leaves.
    void virtualPoolAncestorChangeWhileParked();
    void virtualGapChangeRepricesMeasuredRows();
    void virtualLateAccessibilityActivation();

    // Memory step 1 (slim items): the per-row census of an EAGER list row (CssRect > 2 lean CssText) —
    // sizeof ceilings for the two hot types and the signal connections a mounted row costs (the theme's,
    // the layout's and the ancestors' included). Cold, feature-only state lives in a lazily allocated
    // extra; per-item self/ancestor/theme connections became typed hooks and intrusive registries.
    void perRowMemoryCensus();
    // The registries that replaced those connections keep their semantics: an ancestor's inherited change
    // reaches the rows' texts (and a foreign ancestor's signal still does), a font registration re-resolves
    // every text, implicit-size/visibility changes still relayout the parent, `:focus` still restyles, and a
    // follower or source destroyed mid-propagation is skipped/never touched.
    void slimItemsKeepSemantics();

    // A box's OWN definite (px) `height`/`width` is its implicit size, even though it has no CSS
    // parent to apply it (a ListView delegate root: the Flickable positions it off implicitHeight,
    // never calling layoutFlex's place()). Content shorter/narrower than the declared size must not
    // shrink it; `auto`/`%` stay content-derived (no container to resolve the percentage against here).
    void cssRectDefiniteSizeWinsOverContentImplicit();
    // End-to-end: a real QtQuick ListView whose delegate root is a CssRect styled only by a class
    // (no explicit height/width binding in QML, exactly as the transpiler emits <VirtualList>) ends
    // up at the CSS height, not the text's content height.
    void listViewDelegateHonoursCssHeight();
};
