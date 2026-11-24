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
set(STB_DIR "${KARP_DIR}/third_party/stb")

set(KARP_SOURCE_FILES
    "${KARP_DIR}/src/karp.c"            "${KARP_DIR}/src/karp.h"
    "${KARP_DIR}/src/k_area.c"          "${KARP_DIR}/src/k_area.h"
    "${KARP_DIR}/src/k_brain.c"         "${KARP_DIR}/src/k_brain.h"
    "${KARP_DIR}/src/k_combat.c"        "${KARP_DIR}/src/k_combat.h"
    "${KARP_DIR}/src/k_condition.c"     "${KARP_DIR}/src/k_condition.h"
    "${KARP_DIR}/src/k_console.cpp"     "${KARP_DIR}/src/k_console.h"
    "${KARP_DIR}/src/k_demo.c"          "${KARP_DIR}/src/k_demo.h"
    "${KARP_DIR}/src/k_graph.c"         "${KARP_DIR}/src/k_graph.h"
    "${KARP_DIR}/src/k_items.c"         "${KARP_DIR}/src/k_items.h"
    "${KARP_DIR}/src/k_jump.c"          "${KARP_DIR}/src/k_jump.h"
    "${KARP_DIR}/src/k_log.c"           "${KARP_DIR}/src/k_log.h"
    "${KARP_DIR}/src/k_map.c"           "${KARP_DIR}/src/k_map.h"
    "${KARP_DIR}/src/k_nav.c"           "${KARP_DIR}/src/k_nav.h"
    "${KARP_DIR}/src/k_overlay.c"       "${KARP_DIR}/src/k_overlay.h"
    "${KARP_DIR}/src/k_path.c"          "${KARP_DIR}/src/k_path.h"
    "${KARP_DIR}/src/k_salt.c"          "${KARP_DIR}/src/k_salt.h"
    "${KARP_DIR}/src/k_settings.c"      "${KARP_DIR}/src/k_settings.h"
    "${KARP_DIR}/src/k_sight.c"         "${KARP_DIR}/src/k_sight.h"
    "${KARP_DIR}/src/k_specials.c"      "${KARP_DIR}/src/k_specials.h"
    "${KARP_DIR}/src/k_threat.c"        "${KARP_DIR}/src/k_threat.h"
    "${KARP_DIR}/src/k_trace.c"         "${KARP_DIR}/src/k_trace.h")

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
                           "${STB_DIR}"
                           "${PROJECT_SOURCE_DIR}/src"
                           "${PROJECT_SOURCE_DIR}/src/doom")

target_compile_features("${PROGRAM_PREFIX}doom" PRIVATE cxx_std_17)

# The overlay's logo (-karpoverlay), copied next to the executable. Not a
# POST_BUILD step (the executable is defined in another directory), nor
# $<TARGET_FILE_DIR> (under this project's old policies, CMP0112, that would
# make the copy depend on the executable, and the executable on it): its
# folder is the target's build directory, per configuration where there are
# several.
get_target_property(KARP_EXE_DIR "${PROGRAM_PREFIX}doom" BINARY_DIR)
get_property(KARP_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(KARP_MULTI_CONFIG)
    set(KARP_EXE_DIR "${KARP_EXE_DIR}/$<CONFIG>")
endif()

add_custom_target(karp_assets
                  COMMAND "${CMAKE_COMMAND}" -E make_directory "${KARP_EXE_DIR}"
                  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                          "${KARP_DIR}/assets/karp_logo.png" "${KARP_EXE_DIR}"
                  COMMENT "Karp: the overlay's logo next to the executable")
add_dependencies("${PROGRAM_PREFIX}doom" karp_assets)

# The engine hooks in the doom library include karp.h.
target_include_directories(doom PRIVATE "${KARP_DIR}/src")
