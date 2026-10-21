# Karp: autonomous Doom bot, built into crispy-doom only (see docs/plan.md).
#
# Sources are added straight to the crispy-doom executable, so there is no
# link cycle between the doom static library (which calls the Karp hooks) and
# Karp (which reads the doom globals).
#
# All paths are absolute: cmake_minimum_required(3.7.2) leaves CMP0076 OLD, so
# target_sources() would not resolve relative paths against this directory.

set(KARP_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(IMGUI_DIR "${KARP_DIR}/third_party/imgui")

set(KARP_SOURCE_FILES
    "${KARP_DIR}/src/karp.c"            "${KARP_DIR}/src/karp.h"
    "${KARP_DIR}/src/k_log.c"           "${KARP_DIR}/src/k_log.h")

set(IMGUI_SOURCE_FILES
    "${IMGUI_DIR}/imgui.cpp"            "${IMGUI_DIR}/imgui.h"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl2.h"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.h")

target_sources("${PROGRAM_PREFIX}doom" PRIVATE
               ${KARP_SOURCE_FILES} ${IMGUI_SOURCE_FILES})

target_include_directories("${PROGRAM_PREFIX}doom" PRIVATE
                           "${KARP_DIR}/src"
                           "${IMGUI_DIR}"
                           "${IMGUI_DIR}/backends"
                           "${PROJECT_SOURCE_DIR}/src"
                           "${PROJECT_SOURCE_DIR}/src/doom")

target_compile_features("${PROGRAM_PREFIX}doom" PRIVATE cxx_std_17)

# The engine hooks in the doom library include karp.h.
target_include_directories(doom PRIVATE "${KARP_DIR}/src")
