# Branch `bg-image-cssrect` — merge notes (owner is editing master in parallel)

**Goal:** CssRect (a `div`, the common page/element) never painted `background-image`
(only CssFill did). This adds an image layer to CssRect so a page/element background image
renders.

**Files touched: ONLY `src/qmlcss/cssrect.cpp`. csstheme.cpp is NOT touched (owner directive).**

Exact edit sites in `src/qmlcss/cssrect.cpp`:

1. **Anonymous namespace (top of file, near `isCssUrl`)** — added two local helpers copied
   verbatim from `cssfill.cpp`'s anon namespace: `imageSource(QString)` (strip url()/quotes,
   file:// a bare absolute path) and `fillModeFor(QString background-size)` (cover/contain/…
   → QQuickImage::FillMode int). Kept local to avoid a shared-header change; if master later
   promotes these to a shared header, delete the local copies here.

2. **`kRenderShell` (the QML string)** — (a) three new `cssIn` readonly props:
   `bgImageSource` (url string), `bgImageFillMode` (int), `bgImageOpacity` (real); (b) a
   `QQuickImage` layer inserted directly ABOVE the solid/gradient fill and BELOW the border
   Loader, `visible: bgImageSource !== ""`, `source`/`fillMode`/`opacity` from those props,
   `anchors.fill: parent`, clipped by the same radius as the fill.

3. **style-apply (near the `bgLayers`/`orderedLayers` construction, ~L940)** — compute
   `imageValue` from `background-image` (or a url in `background`), `bgImageSource`, opacity,
   and `bgImageFillMode` from `background-repeat` (repeat/repeat-x/repeat-y → Tile/TileH/TileV)
   else `background-size` (cover/contain/stretch). Image composites over the solid fill (CSS).

4. **`cssIn` push block (`QVariantMap in;`, ~L980)** — three `in.insert(...)` for the new
   props.

**Merge strategy when master lands:** these are additive; the only conflict risk is if the
owner's master reworks `kRenderShell` or the cssIn push block. Re-apply the four sites above
by hand if `git merge` conflicts. No behavior change to existing boxes (image props default
empty → the layer is `visible: false`).

Test added: `tests/test_qml_css.{h,cpp}` — `cssRectPaintsBackgroundImage` (cover fillMode +
tile). Also touched on this branch: those two test files (additive, one new method).

Delete this file on merge.
