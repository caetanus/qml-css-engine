#!/usr/bin/env python3
"""Fail when CMakeLists.txt's source list drifts from meson.build's.

meson is the primary build; the CMake mirror exists for find_package/FetchContent
consumers. There is no meson->cmake generator, so the mirror is kept honest by
this check (wired into CI).
"""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parent.parent
meson = (root / 'meson.build').read_text()
cmake = (root / 'CMakeLists.txt').read_text()

meson_block = re.search(r"qml_css_engine_sources = files\((.*?)\)", meson, re.S).group(1)
meson_sources = set(re.findall(r"'([^']+\.cpp)'", meson_block))

cmake_block = re.search(r"set\(QML_CSS_ENGINE_SOURCES(.*?)\)", cmake, re.S).group(1)
cmake_sources = set(re.findall(r"(src/[^\s)]+\.cpp)", cmake_block))

missing = sorted(meson_sources - cmake_sources)
extra = sorted(cmake_sources - meson_sources)
if missing or extra:
    if missing:
        print("in meson.build but not CMakeLists.txt:", *missing, sep="\n  ")
    if extra:
        print("in CMakeLists.txt but not meson.build:", *extra, sep="\n  ")
    sys.exit(1)
print(f"cmake/meson source lists in sync ({len(meson_sources)} files)")
