cmake_minimum_required(VERSION 3.21)
project(monkey_dust_tools CXX)

# ── monkey_dust_editor ────────────────────────────────────
# Standalone JSON data editor. UI: Dear ImGui via rlImGui.
set(EDITOR_IMGUI_SRC
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/third_party/rlImGui/rlImGui.cpp
)

add_executable(monkey_dust_editor
    editor/main.cpp
    ${EDITOR_IMGUI_SRC}
)
target_link_libraries(monkey_dust_editor PRIVATE raylib)
target_include_directories(monkey_dust_editor PRIVATE
    editor/
    ${CMAKE_SOURCE_DIR}/third_party/raylib/src
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/rlImGui
)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(monkey_dust_editor PRIVATE -O2 -march=native)
endif()

# ── md_flare_convert ─────────────────────────────────────
add_executable(md_flare_convert
    flare_convert/flare_ini_converter.cpp
    flare_convert/flare_convert_main.cpp
)
target_link_libraries(md_flare_convert PRIVATE raylib)
target_include_directories(md_flare_convert PRIVATE
    flare_convert/
    ${CMAKE_SOURCE_DIR}/third_party/raylib/src
)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(md_flare_convert PRIVATE -O2 -march=native)
endif()
