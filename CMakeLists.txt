cmake_minimum_required(VERSION 3.21)
project(monkey_dust_tools CXX)

find_package(SDL3 REQUIRED CONFIG)

# ── monkey_dust_editor ────────────────────────────────────
# Standalone 3D editor. UI: Dear ImGui via imgui_impl_sdl3/sdlgpu3 (SDL_GPU=Vulkan).
# Same backend as the game — enables full TerrainRenderer/PropRenderer reuse.

# All editor panel .cpp files (map_view + all wicked-style panels).
file(GLOB EDITOR_PANEL_SOURCES "editor/*.cpp")
list(REMOVE_ITEM EDITOR_PANEL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/editor/main.cpp")

set(EDITOR_IMGUI_SRC
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdl3.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdlgpu3.cpp
)

add_executable(monkey_dust_editor
    editor/main.cpp
    # Header-only files: listed here so ninja tracks them and rebuilds on change
    editor/editor_char_preview_sdlgpu.h
    editor/character_editor.h
    ${EDITOR_PANEL_SOURCES}
    ${EDITOR_IMGUI_SRC}
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/glad_impl.cpp    # satisfies engine lib GL refs (never called in SDL_GPU mode)
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/render/cgltf_impl.cpp
    ${CMAKE_SOURCE_DIR}/game/src/world/character_def.cpp
    # Third-party panels
    ${CMAKE_SOURCE_DIR}/third_party/imnodes/imnodes.cpp
    ${CMAKE_SOURCE_DIR}/third_party/ImGuiColorTextEdit/TextEditor.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui-flame-graph/imgui_widget_flamegraph.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imguizmo/src/ImSequencer.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imguizmo/src/ImGradient.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui-node-editor/imgui_node_editor.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui-node-editor/imgui_node_editor_api.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui-node-editor/imgui_canvas.cpp
    ${CMAKE_SOURCE_DIR}/third_party/imgui-node-editor/crude_json.cpp
)
target_compile_definitions(monkey_dust_editor PRIVATE
    MONKEY_DUST_STANDALONE_EDITOR
    MONKEY_DUST_EDITOR
    MD_SDL_GPU
    USE_SDL3
    IMGUI_DEFINE_MATH_OPERATORS
)
target_link_libraries(monkey_dust_editor PRIVATE
    monkey_dust::engine
    Recast Detour DetourCrowd
    SDL3::SDL3
)
target_include_directories(monkey_dust_editor PRIVATE
    editor/
    ${CMAKE_SOURCE_DIR}/third_party/imgui
    ${CMAKE_SOURCE_DIR}/third_party/imgui/backends
    ${CMAKE_SOURCE_DIR}/third_party/imgui-node-editor
    ${CMAKE_SOURCE_DIR}/third_party/ImGuiColorTextEdit
    ${CMAKE_SOURCE_DIR}/third_party/imnodes
    ${CMAKE_SOURCE_DIR}/third_party/imguizmo/src
    ${CMAKE_SOURCE_DIR}/third_party/imgui-flame-graph
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Recast/Include
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Detour/Include
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/DetourCrowd/Include
    ${CMAKE_SOURCE_DIR}/engine/src/vendor
    ${CMAKE_SOURCE_DIR}/engine/src/render
    ${CMAKE_SOURCE_DIR}/game/src
)
# (third-party sources already added above)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(monkey_dust_editor PRIVATE -O2 -march=native)
endif()

# ── md_flare_convert ─────────────────────────────────────
add_executable(md_flare_convert
    flare_convert/flare_ini_converter.cpp
    flare_convert/flare_convert_main.cpp
)
target_include_directories(md_flare_convert PRIVATE
    flare_convert/
)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(md_flare_convert PRIVATE -O2 -march=native)
endif()

# ── md_flare_demo ─────────────────────────────────────────
# Pure SDL_GPU demo: plain SDL3 window, no OpenGL context, no Raylib.
# glad_impl + stb_image_impl compiled directly (not via engine lib) to resolve
# GL symbols from the dual-backend engine and provide stb_image texture loading.
add_executable(md_flare_demo
    flare_demo/main.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/glad_impl.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp
)
target_compile_definitions(md_flare_demo PRIVATE
    MD_SDL_GPU
    USE_SDL3
    USE_GLM
)
target_link_libraries(md_flare_demo PRIVATE
    monkey_dust::engine
    Recast Detour
    SDL3::SDL3
)
target_include_directories(md_flare_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src/vendor
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Recast/Include
    ${CMAKE_SOURCE_DIR}/third_party/recastnavigation/Detour/Include
)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(md_flare_demo PRIVATE -O2 -march=native)
endif()
