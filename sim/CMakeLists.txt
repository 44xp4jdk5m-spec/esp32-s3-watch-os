# ─────────────────────────────────────────────────────────────────────
#  Host (desktop) simulator for the ESP32-S3 watch OS.
#
#  Builds a curated subset of the *real* firmware sources against a thin
#  ESP-IDF compatibility shim, then renders the LVGL UI into an SDL2
#  window.  Nothing here is compiled into the ESP32 firmware.
#
#  Deps: cmake >= 3.16, a C/C++ compiler, SDL2 dev headers, network
#        access on the first configure (LVGL is fetched by git).
# ─────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.16)
project(watch_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)

set(WATCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(SIM_DIR    "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# ── LVGL ─────────────────────────────────────────────────────────────
include(FetchContent)
set(LV_CONF_PATH "${SIM_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_INCLUDE_SIMPLE ON CACHE BOOL "" FORCE)

FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.5.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(lvgl)

# ── Generated spiffs image assets ────────────────────────────────────
# Some faces load pre-decoded .bin files from /spiffs at runtime.  On the
# host we just point that mount at the source folder.
add_compile_definitions(
    SIM_BUILD=1
    SIM_SPIFFS_ROOT="${WATCH_ROOT}/spiffs"
)

# ── Real firmware sources (UI layer only) ────────────────────────────
set(WF   "${WATCH_ROOT}/components/os/watch_faces")

set(SIM_SOURCES
    # Boot splash + shared radial progress widget
    "${WF}/ui/ui_progress_ring.c"
    "${WF}/ui/ui_boot_up.c"

    # SquareLine generated screens + shared fonts
    "${WF}/ui/ui.c"
    "${WF}/ui/ui_helpers.c"
    "${WF}/ui/ui_watch_face.c"
    "${WF}/ui/ui_charging.c"
    "${WF}/ui/ui_comp_hook.c"
    "${WF}/ui/ui_font_NotoSansCondensedBold80.c"
    "${WF}/ui/ui_font_NotoSansExtraCondensedSemiBold25.c"

    # Watch-face registry + faces
    "${WF}/watch_face_registry.c"
    "${WF}/watch_face_minimal.c"
    "${WF}/watch_face_classic.c"
    "${WF}/watch_face_hands.c"
    "${WF}/watch_face_flux.c"
    "${WF}/watch_face_california.c"

    # Long-press face picker (opened by holding the mouse on the face)
    "${WF}/watch_face_picker.c"
    "${WATCH_ROOT}/components/system_widget/spinner/spinner.c"

    # Shared generated fonts / icons used by the launcher
    "${WATCH_ROOT}/components/os/ui_font_QilkaBold42.c"
    "${WATCH_ROOT}/components/os/fonts/noto_font_14.c"
    "${WATCH_ROOT}/components/os/fonts/noto_font_24.c"
    "${WATCH_ROOT}/components/os/fonts/noto_font_24_bold.c"
    "${WATCH_ROOT}/components/os/fonts/noto_font_48.c"
    "${WATCH_ROOT}/components/os/fonts/noto_font_48_bold.c"
    "${WATCH_ROOT}/components/os/fonts/mi_font_24.c"
    "${WATCH_ROOT}/components/os/fonts/mi_font_60.c"

    # Honeycomb launcher
    "${WATCH_ROOT}/components/os/honeycomb_menu.c"

    # Apps (one at a time; each is a self-contained screen)
    "${WATCH_ROOT}/components/apps/calculator/calculator_ui.c"
    "${WATCH_ROOT}/components/apps/activity/activity_ui.c"
    "${WATCH_ROOT}/components/apps/activity/activity_pages.c"
    "${WATCH_ROOT}/components/apps/noise/noise_ui.c"
    "${WATCH_ROOT}/components/apps/noise/ui/ui_noise.c"

    # Simulator glue
    "${SIM_DIR}/src/main.c"
    "${SIM_DIR}/src/sim_display.c"
    "${SIM_DIR}/src/sim_input.c"
    "${SIM_DIR}/src/sim_platform.c"
    "${SIM_DIR}/src/sim_mocks.c"
)

add_executable(watch_sim ${SIM_SOURCES})

# ── Icon arrays ──────────────────────────────────────────────────────
# png_images_circle.c and png_images_rgb565.c export the same symbol
# names (different pixel formats).  ESP-IDF keeps them in a static
# archive so the linker only pulls the object it actually needs; do the
# same here instead of linking both objects unconditionally.
add_library(sim_icons_circle STATIC
    "${WATCH_ROOT}/components/os/png_images_circle.c")
add_library(sim_icons_rgb565 STATIC
    "${WATCH_ROOT}/components/os/png_images_rgb565.c")
foreach(lib sim_icons_circle sim_icons_rgb565)
    target_include_directories(${lib} PRIVATE
        "${SIM_DIR}/shim" "${lvgl_SOURCE_DIR}/src"
        "${WATCH_ROOT}/components/os")
    target_compile_definitions(${lib} PRIVATE LV_CONF_PATH="${SIM_DIR}/lv_conf.h")
    target_link_libraries(${lib} PUBLIC lvgl)
endforeach()

target_include_directories(watch_sim PRIVATE
    "${SIM_DIR}/shim"                       # esp_*.h / freertos stubs
    "${SIM_DIR}/shim/freertos"
    "${lvgl_SOURCE_DIR}/src"                # e.g. core/lv_obj_scroll_private.h
    "${SIM_DIR}/src"
    "${WATCH_ROOT}/main"
    "${WATCH_ROOT}/components/hw"
    "${WATCH_ROOT}/components/hw/common"
    "${WATCH_ROOT}/components/hw/display"
    "${WATCH_ROOT}/components/hw/power"
    "${WATCH_ROOT}/components/hw/audio"
    "${WATCH_ROOT}/components/hw/sensors"
    "${WATCH_ROOT}/components/hw/rtc"
    "${WATCH_ROOT}/components/hw/input"
    "${WATCH_ROOT}/components/hw/actuator"
    "${WATCH_ROOT}/components/os"
    "${WATCH_ROOT}/components/os/watch_faces"
    "${WATCH_ROOT}/components/os/watch_faces/ui"
    "${WATCH_ROOT}/components/os/fonts"
    "${WATCH_ROOT}/components/os/lang"
    "${WATCH_ROOT}/components/os/services"
    "${WATCH_ROOT}/components/apps/calculator"
    "${WATCH_ROOT}/components/apps/activity"
    "${WATCH_ROOT}/components/apps/noise"
    "${WATCH_ROOT}/components/apps/noise/ui"
    "${WATCH_ROOT}/components/system_widget"
    "${WATCH_ROOT}/components/system_widget/keyboard"
    "${WATCH_ROOT}/components/system_widget/spinner"
)

target_compile_definitions(watch_sim PRIVATE
    LV_CONF_PATH="${SIM_DIR}/lv_conf.h"
    LV_LVGL_H_INCLUDE_SIMPLE
)

target_compile_options(watch_sim PRIVATE
    -Wno-format
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-missing-field-initializers
    -g
)

target_link_libraries(watch_sim PRIVATE lvgl sim_icons_circle sim_icons_rgb565 ${SDL2_LIBRARIES} m)
target_include_directories(watch_sim PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(watch_sim PRIVATE ${SDL2_LIBRARY_DIRS})
