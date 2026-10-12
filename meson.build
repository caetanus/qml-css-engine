project(
  'qml-css-engine',
  'cpp',
  version: '0.2.0',
  license: 'LGPL-3.0-only',
  license_files: ['LICENSE', 'NOTICE', 'LICENSES/LGPL-3.0-only.txt', 'LICENSES/GPL-3.0-only.txt'],
  default_options: [
    'cpp_std=c++20',
    'warning_level=2',
  ],
)

qt6 = import('qt6')

# macOS: meson's qt6 module computes a Qt .framework's private-header include dir as
# "<Module>.framework/Headers/<version>" (mesonbuild/dependencies/qt.py, QtExtraFrameworkDependency
# .get_compile_args) — that path never exists. Qt's real mac layout keeps private headers under a
# SEPARATE "<Module>.framework/PrivateHeaders/" tree, so `private_headers: true` below would
# silently get a non-existent -I dir and fail to find QQuickItemPrivate. Work around it by finding
# the real directory on disk (search rather than guess the exact subpath, which is Qt-packaging-
# dependent) and injecting it ourselves. The consuming app (solid-qml-native)'s own meson.build
# carries an identical block — this is a separate meson project/scope, so it can't be shared —
# and its scripts/ci-macos.sh exports QT_MAC_FRAMEWORKS_DIR (the "…/lib" dir the .frameworks live
# in) for both to search under.
mac_qt_private_dep = declare_dependency()
if host_machine.system() == 'darwin'
  mac_fw_dir = run_command('sh', '-c', 'printf %s "${QT_MAC_FRAMEWORKS_DIR:-}"', check: true).stdout().strip()
  if mac_fw_dir == ''
    warning('QT_MAC_FRAMEWORKS_DIR is unset on macOS: the engine\'s private-header dependency ' +
            '(csstext.cpp\'s QQuickItemPrivate read) will likely fail to find its headers.')
  else
    # See the matching comment in the app's meson.build for why this searches rather than
    # guesses the subpath, and why the script always exits 0 (an empty result for one module is
    # not a configure error here -- it is handled by the warning below).
    mac_private_find = run_command('sh', '-c',
      'for m in QtCore QtGui QtQml QtQmlModels QtQuick QtQuickTemplates2; do ' +
      'p=$(find "$1/$m.framework" -type d -name private 2>/dev/null | head -1); ' +
      'if [ -n "$p" ]; then dirname "$p"; fi; done; exit 0',
      '--', mac_fw_dir, check: true)
    mac_private_dirs = mac_private_find.stdout().strip().split('\n')
    mac_private_args = []
    foreach d : mac_private_dirs
      if d != ''
        mac_private_args += ['-I' + d]
      endif
    endforeach
    message('mac_qt_private_dep: found private-header dirs under ' + mac_fw_dir + ': ' + ' '.join(mac_private_dirs))
    if mac_private_args.length() == 0
      warning('No Qt PrivateHeaders found under ' + mac_fw_dir + ' — the private-header dependency ' +
              'will likely fail to compile.')
    endif
    mac_qt_private_dep = declare_dependency(compile_args: mac_private_args)
  endif
endif

qt6_dep = dependency(
  'qt6',
  modules: ['Core', 'Gui', 'Qml', 'Quick', 'Network'],
)

qml_css_engine_sources = files(
  'src/qmlcss/csstheme.cpp',
  'src/qmlcss/cssthemeparser.cpp',
  'src/qmlcss/cssthemeloader.cpp',
  'src/qmlcss/cssthemevalues.cpp',
  'src/qmlcss/cssvar.cpp',
  'src/qmlcss/csstheme_census.cpp',
  'src/qmlcss/componentcache.cpp',
  'src/qmlcss/cssincubator.cpp',
  'src/qmlcss/cssrepeater.cpp',
  'src/qmlcss/csscontentholder.cpp',
  'src/qmlcss/csslayout.cpp',
  'src/qmlcss/csslength.cpp',
  'src/qmlcss/cssbox.cpp',
  'src/qmlcss/csshr.cpp',
  'src/qmlcss/cssimage.cpp',
  'src/qmlcss/cssfill.cpp',
  'src/qmlcss/cssrect.cpp',
  'src/qmlcss/csstext.cpp',
  'src/qmlcss/csstextlayout.cpp',
  'src/qmlcss/cssdropshadow.cpp',
  'src/qmlcss/csskeyframes.cpp',
  'src/qmlcss/cssicon.cpp',
  'src/qmlcss/cssfilllayer.cpp',
  'src/qmlcss/cssitem.cpp',
  'src/qmlcss/contrast.cpp',
  'src/qmlcss/valueparser.cpp',
)

qml_css_engine_headers = files(
  'src/qmlcss/QMLCss.h',
  'src/qmlcss/componentcache.h',
  'src/qmlcss/cssincubator.h',
  'src/qmlcss/cssrepeater.h',
  'src/qmlcss/csscontentholder.h',
  'src/qmlcss/csstheme.h',
  'src/qmlcss/csslayout.h',
  'src/qmlcss/csslength.h',
  'src/qmlcss/cssbox.h',
  'src/qmlcss/csscontextnode.h',
  'src/qmlcss/csshr.h',
  'src/qmlcss/cssimage.h',
  'src/qmlcss/cssfill.h',
  'src/qmlcss/cssrect.h',
  'src/qmlcss/csstext.h',
  'src/qmlcss/csstextlayout.h',
  'src/qmlcss/cssdropshadow.h',
  'src/qmlcss/csskeyframes.h',
  'src/qmlcss/cssicon.h',
  'src/qmlcss/cssfilllayer.h',
  'src/qmlcss/cssitem.h',
  'src/qmlcss/contrast.h',
  'src/qmlcss/valueparser.h',
)

qml_css_engine_qt_sources = qt6.preprocess(
  moc_headers: files('src/qmlcss/csstheme.h', 'src/qmlcss/csslayout.h', 'src/qmlcss/cssincubator.h', 'src/qmlcss/cssrepeater.h', 'src/qmlcss/csscontentholder.h', 'src/qmlcss/cssbox.h', 'src/qmlcss/csshr.h', 'src/qmlcss/cssimage.h', 'src/qmlcss/cssfill.h', 'src/qmlcss/cssrect.h', 'src/qmlcss/csstext.h', 'src/qmlcss/cssdropshadow.h', 'src/qmlcss/csskeyframes.h', 'src/qmlcss/cssicon.h', 'src/qmlcss/cssfilllayer.h', 'src/qmlcss/cssitem.h', 'src/qmlcss/contrast.h'),
)

qml_css_engine_include = include_directories('src')

# Private Qt, all in csstext.cpp: it reads the LayoutMirroring a composed label would inherit
# (QQuickItemPrivate). CssTextLayout elides through public API only. Private to the library: no installed
# header includes it. (QuickPrivate pulls GuiPrivate transitively, so that's covered too.)
qt6_quick_private_dep = declare_dependency(dependencies: [
  dependency('qt6', modules: ['Core', 'Gui', 'Qml', 'Quick'], private_headers: host_machine.system() != 'darwin'),
  mac_qt_private_dep,
])

qml_css_engine_lib = static_library(
  'qmlcssengine',
  qml_css_engine_sources,
  qml_css_engine_qt_sources,
  include_directories: qml_css_engine_include,
  dependencies: [qt6_dep, qt6_quick_private_dep],
  install: true,
)

# Consumers (e.g. the solid-qml app, or qbar) link this and get the qmlcss/ headers + the
# qrc:/qmlcss QML resources. Exposed as a meson subproject variable: `qml_css_engine_dep`.
qml_css_engine_dep = declare_dependency(
  link_with: qml_css_engine_lib,
  include_directories: qml_css_engine_include,
  dependencies: qt6_dep,
  sources: qml_css_engine_qt_sources,
)

install_headers(qml_css_engine_headers, subdir: 'qmlcss')

# .pc file: the universal consumption path (CMake users too, via
# pkg_check_modules(... IMPORTED_TARGET qml-css-engine)). The in-tree
# CMakeLists.txt additionally provides native find_package support.
pkgconfig = import('pkgconfig')
pkgconfig.generate(
  qml_css_engine_lib,
  name: 'qml-css-engine',
  description: 'A real CSS engine for Qt Quick',
  requires: ['Qt6Core', 'Qt6Gui', 'Qt6Qml', 'Qt6Quick', 'Qt6Network'],
)

# Tests build when standalone, or when a parent opts in via `-Dtests=enabled` (the app does this so
# `meson test` from the app runs the engine suites too); `-Dtests=disabled` forces them off.
if get_option('tests').enabled() or (not meson.is_subproject() and not get_option('tests').disabled())
  qt6_test_dep = dependency(
    'qt6',
    modules: ['Core', 'Gui', 'Qml', 'Quick', 'Test'],
  )

  css_test_moc = qt6.preprocess(moc_headers: files('tests/test_css.h'))
  css_test = executable(
    'test-css',
    files('tests/test_css.cpp'),
    css_test_moc,
    dependencies: [qml_css_engine_dep, qt6_test_dep],
  )
  test('css', css_test)

  qml_css_test_moc = qt6.preprocess(moc_headers: files('tests/test_qml_css.h'))
  qml_test = executable(
    'test-qml-css',
    files('tests/test_qml_css.cpp'),
    qml_css_test_moc,
    dependencies: [qml_css_engine_dep, qt6_test_dep],
  )
  test('qml-css', qml_test, env: ['QT_QPA_PLATFORM=offscreen'])

  layout_test_moc = qt6.preprocess(moc_headers: files('tests/test_layout.h'))
  layout_test = executable(
    'test-layout',
    files('tests/test_layout.cpp'),
    layout_test_moc,
    dependencies: [qml_css_engine_dep, qt6_test_dep],
  )
  test('layout', layout_test, env: ['QT_QPA_PLATFORM=offscreen'])

# The text oracle reads QQuickTextPrivate (what a real QQuickText draws, and where): private headers, test only.
# tests/data holds the OFL test font the font-revision test registers.
text_test_moc = qt6.preprocess(moc_headers: files('tests/test_text.h'))
text_test = executable(
  'test-text',
  files('tests/test_text.cpp'),
  text_test_moc,
  dependencies: [qml_css_engine_dep, qt6_test_dep, qt6_quick_private_dep],
  cpp_args: ['-DSQ_TEST_DATA_DIR="' + (meson.current_source_dir() / 'tests' / 'data') + '"'],
)
test('text', text_test, env: ['QT_QPA_PLATFORM=offscreen'])
endif
