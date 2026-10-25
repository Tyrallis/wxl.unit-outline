cmake_minimum_required(VERSION 3.21)

project(wxl-unit-outline VERSION 1.1.0 LANGUAGES NONE)

# A checkout of WarcraftXL/sdk beside this folder.
set(WXL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the WarcraftXL SDK")
include("${WXL_SDK_DIR}/cmake/wxl_add_mod.cmake")

# x64 only: wxl.gx is published by the runtime the recompiled client loads, and only there.
wxl_add_mod(unit-outline
    SOURCES src/mod.cpp
    MANIFEST mod.json
    TARGETS win-x64)
