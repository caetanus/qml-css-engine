# CMake mirror of meson.build — kept in sync by tools/check-cmake-parity.py (CI fails
# on drift). meson is the primary build; this exists for find_package/FetchContent
# consumers and distro packaging that prefers CMake.
cmake_minimum_required(VERSION 3.21)
project(qml-css-engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick Network)
# Private Qt, all in csstext.cpp: it reads the inherited LayoutMirroring (QQuickItemPrivate). Library-private.
# QuickPrivate pulls GuiPrivate transitively, so that's covered too.
#
# find_package(Qt6 COMPONENTS QuickPrivate) only succeeds where the distro ships
# Qt6QuickPrivateConfig.cmake. Arch does; Debian does NOT: qt6-declarative-dev and
# qt6-declarative-private-dev install the private HEADERS (verified via each package's
# file list on packages.debian.org, trixie, Qt 6.8.2) but zero *.cmake files for any
# "*Private" component — a Debian packaging gap, not a Qt-version issue (CI failure:
# "Could NOT find Qt6QuickPrivate ... Expected Config file at
# .../cmake/Qt6QuickPrivate/Qt6QuickPrivateConfig.cmake does NOT exist"). The engine's
# meson build never hit this because meson's qt6 module does not use a CMake "Private"
# component at all — it derives the private include directory purely from a path
# convention (mesonbuild/dependencies/qt.py, _qt_get_private_includes): the module's
# public include root + "/Qt<Module>/<Qt version>/Qt<Module>/private". Mirror that
# convention here as a fallback so the CMake build works both where the official
# component exists (Arch) and where only the headers do (Debian).
find_package(Qt6 QUIET COMPONENTS QuickPrivate)
if(NOT TARGET Qt6::QuickPrivate)
    get_target_property(_qt6_quick_include_dirs Qt6::Quick INTERFACE_INCLUDE_DIRECTORIES)
    set(_qt6_quick_private_include_dir "")
    foreach(_dir IN LISTS _qt6_quick_include_dirs)
        if(_dir MATCHES "^\\$<.*>$")
            continue() # unevaluated generator expression (e.g. BUILD_INTERFACE on a non-existent future path); skip
        endif()
        if(EXISTS "${_dir}/QtQuick/${Qt6Quick_VERSION}/QtQuick/private")
            set(_qt6_quick_private_include_dir "${_dir}/QtQuick/${Qt6Quick_VERSION}/QtQuick/private")
            break()
        endif()
        get_filename_component(_dirname "${_dir}" NAME)
        if(_dirname STREQUAL "QtQuick" AND EXISTS "${_dir}/${Qt6Quick_VERSION}/QtQuick/private")
            set(_qt6_quick_private_include_dir "${_dir}/${Qt6Quick_VERSION}/QtQuick/private")
            break()
        endif()
    endforeach()
    if(_qt6_quick_private_include_dir STREQUAL "")
        message(FATAL_ERROR "Qt6 Quick private headers not found under any of: ${_qt6_quick_include_dirs} "
            "(looked for <dir>/QtQuick/${Qt6Quick_VERSION}/QtQuick/private); install the distro's Qt6 "
            "declarative private-headers package (e.g. qt6-declarative-private-dev on Debian).")
    endif()
    add_library(Qt6::QuickPrivate INTERFACE IMPORTED)
    target_include_directories(Qt6::QuickPrivate INTERFACE "${_qt6_quick_private_include_dir}")
    target_link_libraries(Qt6::QuickPrivate INTERFACE Qt6::Quick Qt6::Gui Qt6::Qml)
endif()
qt_standard_project_setup()

# Source list mirrored from meson.build (qml_css_engine_sources).
set(QML_CSS_ENGINE_SOURCES
    src/qmlcss/csstheme.cpp
    src/qmlcss/cssthemeparser.cpp
    src/qmlcss/cssthemeloader.cpp
    src/qmlcss/cssthemevalues.cpp
    src/qmlcss/cssvar.cpp
    src/qmlcss/csstheme_census.cpp
    src/qmlcss/componentcache.cpp
    src/qmlcss/cssincubator.cpp
    src/qmlcss/cssrepeater.cpp
    src/qmlcss/csscontentholder.cpp
    src/qmlcss/csslayout.cpp
    src/qmlcss/csslength.cpp
    src/qmlcss/cssbox.cpp
    src/qmlcss/csshr.cpp
    src/qmlcss/cssimage.cpp
    src/qmlcss/cssfill.cpp
    src/qmlcss/cssrect.cpp
    src/qmlcss/csstext.cpp
    src/qmlcss/csstextlayout.cpp
    src/qmlcss/cssdropshadow.cpp
    src/qmlcss/csskeyframes.cpp
    src/qmlcss/cssicon.cpp
    src/qmlcss/themeiconprovider.cpp
    src/qmlcss/cssfilllayer.cpp
    src/qmlcss/cssitem.cpp
    src/qmlcss/contrast.cpp
    src/qmlcss/valueparser.cpp
)

qt_add_library(qmlcssengine STATIC ${QML_CSS_ENGINE_SOURCES})
add_library(QmlCssEngine::QmlCssEngine ALIAS qmlcssengine)

target_include_directories(qmlcssengine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(qmlcssengine PUBLIC
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Network
)
target_link_libraries(qmlcssengine PRIVATE Qt6::QuickPrivate)

# ---- install + find_package support -----------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS qmlcssengine EXPORT qml-css-engineTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
file(GLOB QML_CSS_ENGINE_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/src/qmlcss/*.h)
list(FILTER QML_CSS_ENGINE_HEADERS EXCLUDE REGEX "_p\\.h$") # internals stay internal
install(FILES ${QML_CSS_ENGINE_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qmlcss)

install(EXPORT qml-css-engineTargets
        NAMESPACE QmlCssEngine::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qml-css-engine)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/qml-css-engineConfigVersion.cmake
    COMPATIBILITY AnyNewerVersion)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/qml-css-engineConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/qml-css-engineConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qml-css-engine)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/qml-css-engineConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/qml-css-engineConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qml-css-engine)

# ---- tests -------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    foreach(t IN ITEMS test_css test_layout test_qml_css)
        string(REPLACE "_" "-" tname ${t})
        qt_add_executable(${tname} tests/${t}.cpp tests/${t}.h)
        target_link_libraries(${tname} PRIVATE qmlcssengine Qt6::Test)
        add_test(NAME ${tname} COMMAND ${tname})
        set_tests_properties(${tname} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    # The text oracle reads QQuickTextPrivate: private headers, test only. tests/data: the OFL test font.
    qt_add_executable(test-text tests/test_text.cpp tests/test_text.h)
    target_link_libraries(test-text PRIVATE qmlcssengine Qt6::Test Qt6::QuickPrivate)
    target_compile_definitions(test-text PRIVATE SQ_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME test-text COMMAND test-text)
    set_tests_properties(test-text PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
