cmake_minimum_required(VERSION 3.21)
project(monkey_dust_tools CXX)

find_package(SDL3 REQUIRED CONFIG)

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
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/glad_impl.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/vendor/stb_image_impl.cpp
)
target_compile_definitions(monkey_dust_editor PRIVATE
    MONKEY_DUST_STANDALONE_EDITOR
    USE_SDL3
    IMGUI_IMPL_OPENGL_LOADER_CUSTOM)
# The engine lib propagates MD_SDL_GPU=PUBLIC (Vulkan-only path).
# The standalone editor uses OpenGL, not SDL_GPU — undefine it so window.h creates a GL context.
# Force-include glad.h so GL function pointers are available in imgui_impl_opengl3.cpp
# (IMGUI_IMPL_OPENGL_LOADER_CUSTOM disables ImGui's internal loader; we use glad2 instead)
target_compile_options(monkey_dust_editor PRIVATE
    -UMD_SDL_GPU
    "-include${CMAKE_SOURCE_DIR}/engine/src/vendor/glad.h")
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
    ${CMAKE_SOURCE_DIR}/engine/src/vendor
)
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
