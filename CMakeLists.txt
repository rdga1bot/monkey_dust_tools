cmake_minimum_required(VERSION 3.21)
project(monkey_dust_tools CXX)

# SDL3 required only for editor + flare_demo; flare_convert is standalone.
# Allows CI to build flare_convert without SDL3 installed.
option(MONKEY_DUST_TOOLS_NEED_SDL3 "Require SDL3 (editor + flare_demo)" ON)
if(MONKEY_DUST_TOOLS_NEED_SDL3)
    find_package(SDL3 REQUIRED CONFIG)
endif()

# ── monkey_dust_editor ────────────────────────────────────
# Standalone 3D editor. UI: Dear ImGui via imgui_impl_sdl3/sdlgpu3 (SDL_GPU=Vulkan).
# Same backend as the game — enables full TerrainRenderer/PropRenderer reuse.

# All editor panel .cpp files (map_view + all wicked-style panels).
file(GLOB EDITOR_PANEL_SOURCES "editor/*.cpp")
list(REMOVE_ITEM EDITOR_PANEL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/editor/main.cpp")

set(EDITOR_IMGUI_SRC
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/imgui.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/imgui_draw.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/imgui_tables.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/imgui_widgets.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/backends/imgui_impl_sdl3.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/backends/imgui_impl_sdlgpu3.cpp
)

if(NOT MONKEY_DUST_TOOLS_NEED_SDL3)
    message(STATUS "[tools] SDL3 disabled — skipping monkey_dust_editor + md_flare_demo")
    # Only flare_convert builds without SDL3
else()

add_executable(monkey_dust_editor
    editor/main.cpp
    embedded_fonts.cpp
    # Header-only files: listed here so ninja tracks them and rebuilds on change
    editor/editor_char_preview_sdlgpu.h
    editor/character_editor.h
    ${EDITOR_PANEL_SOURCES}
    ${EDITOR_IMGUI_SRC}
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/glad_impl.cpp    # satisfies engine lib GL refs (never called in SDL_GPU mode)
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/render/cgltf_impl.cpp
    # Third-party panels
    ${CMAKE_SOURCE_DIR}/tools/third_party/imnodes/imnodes.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/ImGuiColorTextEdit/TextEditor.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-flame-graph/imgui_widget_flamegraph.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src/ImSequencer.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src/ImGradient.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_node_editor.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_node_editor_api.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_canvas.cpp
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/crude_json.cpp
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
    $<IF:$<BOOL:${MD_STATIC_DEPS}>,SDL3::SDL3-static,SDL3::SDL3>
)
target_include_directories(monkey_dust_editor PRIVATE
    editor/
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/backends
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor
    ${CMAKE_SOURCE_DIR}/tools/third_party/ImGuiColorTextEdit
    ${CMAKE_SOURCE_DIR}/tools/third_party/imnodes
    ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src
    ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-flame-graph
    ${CMAKE_SOURCE_DIR}/engine/third_party/recastnavigation/Recast/Include
    ${CMAKE_SOURCE_DIR}/engine/third_party/recastnavigation/Detour/Include
    ${CMAKE_SOURCE_DIR}/engine/third_party/recastnavigation/DetourCrowd/Include
    ${CMAKE_SOURCE_DIR}/engine/src/vendor
    ${CMAKE_SOURCE_DIR}/engine/src/render
)
# (third-party sources already added above)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(monkey_dust_editor PRIVATE -O2 -march=native)
endif()

# ── editor_panels hot-reload .so ─────────────────────────────────────────────
# Optional: cmake -DMONKEY_DUST_EDITOR_HOT_RELOAD=ON
# Produces: build/hot/libeditor_panels.so
# Reload:   edit any editor/*.cpp → ninja monkey_dust_editor_panels (~5-10s)
#           → F5 in editor reloads panels without closing the window.
option(MONKEY_DUST_EDITOR_HOT_RELOAD "Build libeditor_panels.so for editor hot-reload" ON)
if(MONKEY_DUST_EDITOR_HOT_RELOAD)
    # Panel .cpp files — same set as EDITOR_PANEL_SOURCES (minus main.cpp)
    # + entry point + third-party panels + game-side sources used by panels.
    # Note: many editor panels are header-only (faction_editor.h, item_editor.h, etc.)
    # and are pulled in transitively — no explicit listing needed.
    file(GLOB _EDITOR_PANELS_CPP CONFIGURE_DEPENDS "editor/*.cpp")
    list(REMOVE_ITEM _EDITOR_PANELS_CPP "${CMAKE_CURRENT_SOURCE_DIR}/editor/main.cpp")
    # ui_smoke_tests.cpp (Phase 5) needs the imgui_test_engine include paths/
    # defines set up below under MD_UI_TESTS — those are only added to the
    # monkey_dust_editor target, not this hot-reload .so, so building this
    # target with MD_UI_TESTS=ON would otherwise fail on a missing header.
    list(REMOVE_ITEM _EDITOR_PANELS_CPP "${CMAKE_CURRENT_SOURCE_DIR}/editor/ui_smoke_tests.cpp")
    set(EDITOR_PANELS_HOT_SRCS
        ${_EDITOR_PANELS_CPP}
        # Engine sources needed by panels (not pulled via static lib at dlopen time)
        ${CMAKE_SOURCE_DIR}/engine/src/render/char_customization.cpp
        # Third-party panels
        ${CMAKE_SOURCE_DIR}/tools/third_party/imnodes/imnodes.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/ImGuiColorTextEdit/TextEditor.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-flame-graph/imgui_widget_flamegraph.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src/ImSequencer.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src/ImGradient.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_node_editor.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_node_editor_api.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_canvas.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/crude_json.cpp
    )
    add_library(monkey_dust_editor_panels SHARED ${EDITOR_PANELS_HOT_SRCS})
    target_compile_definitions(monkey_dust_editor_panels PRIVATE
        MONKEY_DUST_STANDALONE_EDITOR MONKEY_DUST_EDITOR
        MD_SDL_GPU USE_SDL3 IMGUI_DEFINE_MATH_OPERATORS USE_GLM MD_LOG_NO_RAYLIB
        DEBUG)  # MONKEY_DUST_EDITOR always enables DEBUG (see engine/CMakeLists.txt)
    target_include_directories(monkey_dust_editor_panels PRIVATE
        editor/
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui/backends
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor
        ${CMAKE_SOURCE_DIR}/tools/third_party/ImGuiColorTextEdit
        ${CMAKE_SOURCE_DIR}/tools/third_party/imnodes
        ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-flame-graph
        ${CMAKE_SOURCE_DIR}/engine/include
        ${CMAKE_SOURCE_DIR}/engine/src/vendor
        ${CMAKE_SOURCE_DIR}/engine/src/render
        ${CMAKE_SOURCE_DIR}/engine/src/audio
        ${CMAKE_SOURCE_DIR}/engine/third_party/flecs
        ${CMAKE_SOURCE_DIR}/engine/third_party/glm
        ${CMAKE_SOURCE_DIR}/engine/third_party/JoltPhysics
        ${CMAKE_SOURCE_DIR}/engine/third_party/ozz-animation/include
        ${CMAKE_SOURCE_DIR}/engine/third_party/recastnavigation/Recast/Include
        ${CMAKE_SOURCE_DIR}/engine/third_party/recastnavigation/Detour/Include
        ${CMAKE_SOURCE_DIR}/engine/third_party/recastnavigation/DetourCrowd/Include
    )
    # Symbols resolved from host exe at dlopen time (host built with --export-dynamic).
    target_link_options(monkey_dust_editor_panels PRIVATE -Wl,--allow-shlib-undefined)
    target_link_options(monkey_dust_editor PRIVATE -Wl,--export-dynamic)
    target_compile_definitions(monkey_dust_editor PRIVATE MONKEY_DUST_EDITOR_HOT_RELOAD)
    set_target_properties(monkey_dust_editor_panels PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME              "editor_panels"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/hot"
    )
    message(STATUS "[monkey_dust_editor] hot-reload: libeditor_panels.so → build/hot/")
endif()

# ── ImGui Test Engine panel smoke tests ──────────────────────────────────────
# EDITOR_AUTOMATION_PLAN_v1.md Phase 5. Vendored under
# third_party/imgui_test_engine/ (submodule, pinned to v1.92.8 — matches
# this repo's third_party/imgui exactly). License: "Dear ImGui Test Engine
# License" (free for individuals/hobby/open-source use — see
# third_party/imgui_test_engine/imgui_test_engine/LICENSE.txt Article 1.1;
# this project qualifies, not a concern here per explicit project decision).
# Requires MONKEY_DUST_EDITOR_HOT_RELOAD=ON (the default) — see main.cpp's
# MD_UI_TESTS branch doc comment for why.
option(MD_UI_TESTS "Build ImGui Test Engine panel smoke tests into monkey_dust_editor" OFF)
if(MD_UI_TESTS AND MONKEY_DUST_TOOLS_NEED_SDL3)
    set(_ITE_DIR ${CMAKE_SOURCE_DIR}/tools/third_party/imgui_test_engine/imgui_test_engine)
    target_sources(monkey_dust_editor PRIVATE
        editor/ui_smoke_tests.cpp
        ${_ITE_DIR}/imgui_te_context.cpp
        ${_ITE_DIR}/imgui_te_coroutine.cpp
        ${_ITE_DIR}/imgui_te_engine.cpp
        ${_ITE_DIR}/imgui_te_exporters.cpp
        ${_ITE_DIR}/imgui_te_perftool.cpp
        ${_ITE_DIR}/imgui_te_ui.cpp
        ${_ITE_DIR}/imgui_te_utils.cpp
        ${_ITE_DIR}/imgui_capture_tool.cpp
    )
    target_include_directories(monkey_dust_editor PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui_test_engine
        ${_ITE_DIR}
        ${_ITE_DIR}/thirdparty
        ${_ITE_DIR}/thirdparty/Str
        ${_ITE_DIR}/thirdparty/stb
    )
    target_compile_definitions(monkey_dust_editor PRIVATE
        MD_UI_TESTS
        IMGUI_ENABLE_TEST_ENGINE
        IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
    )
    set_source_files_properties(
        ${_ITE_DIR}/imgui_te_context.cpp
        ${_ITE_DIR}/imgui_te_coroutine.cpp
        ${_ITE_DIR}/imgui_te_engine.cpp
        ${_ITE_DIR}/imgui_te_exporters.cpp
        ${_ITE_DIR}/imgui_te_perftool.cpp
        ${_ITE_DIR}/imgui_te_ui.cpp
        ${_ITE_DIR}/imgui_te_utils.cpp
        ${_ITE_DIR}/imgui_capture_tool.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    message(STATUS "[monkey_dust_editor] MD_UI_TESTS: ENABLED (ImGui Test Engine panel smoke tests, --ui-tests)")
endif()

# ── PCH for editor own sources ───────────────────────────────────────────────
# PCH on engine lib (PRIVATE) doesn't propagate to editor .cpp files.
# We add it separately here. Third-party sources (imgui-node-editor, imnodes,
# ImGuiColorTextEdit, imguizmo) are excluded via SET_SOURCE_FILES_PROPERTIES
# SKIP_PRECOMPILE_HEADERS to avoid conflicts with their own internal defines.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.16")
    target_precompile_headers(monkey_dust_editor PRIVATE
        <cstdint> <cstdlib> <cstring> <cstdio>
        <imgui.h>
        <SDL3/SDL_gpu.h>
    )
    # Third-party ImGui panels have conflicting macros — skip PCH for them.
    set(_EDITOR_THIRDPARTY_CPP
        ${CMAKE_SOURCE_DIR}/tools/third_party/imnodes/imnodes.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/ImGuiColorTextEdit/TextEditor.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-flame-graph/imgui_widget_flamegraph.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src/ImSequencer.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imguizmo/src/ImGradient.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_node_editor.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_node_editor_api.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/imgui_canvas.cpp
        ${CMAKE_SOURCE_DIR}/tools/third_party/imgui-node-editor/crude_json.cpp
        ${CMAKE_SOURCE_DIR}/engine/src/vendor/glad_impl.cpp
        ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp
        ${CMAKE_SOURCE_DIR}/engine/src/render/cgltf_impl.cpp
    )
    set_source_files_properties(${_EDITOR_THIRDPARTY_CPP} PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON)
    message(STATUS "[monkey_dust_editor] PCH: ENABLED (imgui + SDL_GPU)")
endif()

endif() # MONKEY_DUST_TOOLS_NEED_SDL3 — editor + hot-reload panels end here

# ── md_flare_convert ─────────────────────────────────────
# Standalone INI→JSON converter — no SDL3, no engine, no third_party.
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

# ── kenshi_navmesh_extract ────────────────────────────────
# Standalone Havok .hkt navmesh -> OBJ geometry extractor — no SDL3, no
# engine. See tools/third_party/hkxparse/README_MONKEYDUST.md for the parser
# reverse-engineering writeup (Kenshi's real per-zone navmesh geometry,
# extracted from game data/newland/land/navtiles/tile*.hkt).
add_executable(kenshi_navmesh_extract
    kenshi_navmesh_extract/main.cpp
    third_party/hkxparse/src/HKXMapping.cpp
    third_party/hkxparse/src/HKXTagfileParser.cpp
    third_party/hkxparse/src/HKXTypes.cpp
    third_party/hkxparse/src/Deserializer.cpp
)
target_include_directories(kenshi_navmesh_extract PRIVATE
    third_party/hkxparse/include
)
target_compile_options(kenshi_navmesh_extract PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hkxparse/include/hkxparse/msvc_compat.h")
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(kenshi_navmesh_extract PRIVATE -O2)
endif()

if(MONKEY_DUST_TOOLS_NEED_SDL3)
# ── md_flare_demo ─────────────────────────────────────────
# Pure SDL_GPU demo: plain SDL3 window, no OpenGL context, no Raylib.
add_executable(md_flare_demo
    flare_demo/main.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/glad_impl.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/render/cgltf_impl.cpp
)
target_compile_definitions(md_flare_demo PRIVATE
    MD_SDL_GPU
    USE_SDL3
    USE_GLM
)
target_link_libraries(md_flare_demo PRIVATE
    monkey_dust::engine
    Recast Detour
    $<IF:$<BOOL:${MD_STATIC_DEPS}>,SDL3::SDL3-static,SDL3::SDL3>
)
target_include_directories(md_flare_demo PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src/vendor
    ${CMAKE_SOURCE_DIR}/engine/third_party/recastnavigation/Recast/Include
    ${CMAKE_SOURCE_DIR}/engine/third_party/recastnavigation/Detour/Include
)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(md_flare_demo PRIVATE -O2 -march=native)
endif()
endif() # MONKEY_DUST_TOOLS_NEED_SDL3 — md_flare_demo
