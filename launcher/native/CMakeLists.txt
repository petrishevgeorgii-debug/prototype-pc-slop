# SPDX-License-Identifier: GPL-2.0-or-later
# PROTOTYPE launcher, the native window (launcher-ui): SDL3 + Dear ImGui (third_party/imgui, MIT)
# with FreeType for the system's fonts. build.sh builds it; Prototype.exe runs it.
cmake_minimum_required(VERSION 3.20)
project(prototype_launcher_ui CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/../../gpu/third_party/imgui)
set(SOURCES main.cpp core.cpp pages.cpp
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp
    ${IMGUI}/misc/freetype/imgui_freetype.cpp)
if (WIN32)
    enable_language(RC)
    list(APPEND SOURCES launcher-ui.rc)
endif()
add_executable(launcher-ui WIN32 ${SOURCES})
target_compile_definitions(launcher-ui PRIVATE IMGUI_ENABLE_FREETYPE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_include_directories(launcher-ui PRIVATE ${IMGUI} ${IMGUI}/backends)
target_link_libraries(launcher-ui PRIVATE PkgConfig::SDL3 PkgConfig::FREETYPE)
set_source_files_properties(main.cpp core.cpp pages.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
