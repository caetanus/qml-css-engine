# Changelog

Consumers: read this before bumping the submodule/subproject — behavioral changes
and breaking API moves are called out per entry.

## Unreleased (hardening pass, 2026-07-04)

### Breaking
- **All public C++ types moved into `namespace QmlCss`** (`CssTheme`,
  `CssLayoutEngine`, the components). Qualify or `using namespace QmlCss;`.
  QML module names (`import qmlcss 1.0`) are unchanged.

### Added
- **CMake support for packaging/consumers**: a mirrored `CMakeLists.txt`
  (static lib + tests + install/export: `find_package(qml-css-engine)` →
  `QmlCssEngine::QmlCssEngine`), a generated `qml-css-engine.pc` from the
  meson build, and `tools/check-cmake-parity.py` wired into CI so the two
  source lists cannot drift silently. meson remains the primary build.
- **Remote `@import`**: http(s) sheets are inlined like local ones — sha1-keyed
  disk cache (offline after first fetch), asynchronous cold fetch with an
  automatic theme reload when the last import lands, URL-relative resolution
  inside remote sheets, per-session failure memory. `loadFromString` also
  expands imports now (it previously ignored them).
- **Layout test suite** (`tests/test_layout.cpp`): 12 cases pinning the
  documented flex/grid/box-model/viewport/absolute contract — the layout
  engine previously had no coverage.
- **Waybar-compat selector corpus** in `tests/test_css.cpp`: the idioms
  migrating waybar users rely on (`window#waybar` + the `primitive` resolve
  parameter, state classes, `#custom-*`, `#workspaces button.focused`,
  universal rules) are pinned so matching changes cannot break consumers
  silently again.
- CI (GitHub Actions, Debian stable): build + tests workflows.

### Changed
- `csstheme.cpp` split by topic: parsing helpers (`cssthemeparser.cpp`),
  load/import/font pipeline (`cssthemeloader.cpp`), value-parsing API
  (`cssthemevalues.cpp`); resolution/apply core stays in `csstheme.cpp`.
  Shared internals live in `csstheme_p.h` (`QmlCss::Detail`, not installed).

### Compat notes for earlier changes
- Type-qualified selectors (`window#waybar`) match strictly: a bare
  `resolve(id)` no longer matches them — pass the `primitive` parameter
  (correct CSS behavior; broke qbar's waybar-compat rule on 2026-07-03).
- Gradient `transparent` stops inherit the neighbour hue (premultiplied
  parity); resolved maps normalize `transparent` to `#00000000`.
