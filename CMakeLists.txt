cmake_minimum_required(VERSION 3.21)
project(monkey_dust_tools CXX)

# ── monkey_dust_editor ────────────────────────────────────
# Standalone JSON + map editor. UI: Dear ImGui via imgui_impl_sdl3/opengl3.
set(EDITOR_IMGUI_SRC
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdl3.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_opengl3.cpp
)

add_executable(monkey_dust_editor
    editor/main.cpp
    editor/editor_map_view.cpp
    ${EDITOR_IMGUI_SRC}
    # glad + stb_image: provide symbols that game/ gets from Raylib but the
    # standalone editor doesn't (no Raylib linked here).
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/glad_impl.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp
)
target_compile_definitions(monkey_dust_editor PRIVATE MONKEY_DUST_STANDALONE_EDITOR)
target_link_libraries(monkey_dust_editor PRIVATE
    monkey_dust::engine
    Recast Detour
    SDL3::SDL3
)
target_include_directories(monkey_dust_editor PRIVATE
    editor/
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Recast/Include
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Detour/Include
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

# ── md_flare_demo ─────────────────────────────────────────
add_executable(md_flare_demo
    flare_demo/main.cpp
)
target_link_libraries(md_flare_demo PRIVATE
    monkey_dust::engine
    Recast Detour
    raylib
)
target_include_directories(md_flare_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/raylib/src
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Recast/Include
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Detour/Include
)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(md_flare_demo PRIVATE -O2 -march=native)
endif()
