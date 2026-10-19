# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_compile_options(-Wno-format)
project(lvgl_demo_v9)

# ── Re-configure when the wrist-flick model changes ───────────────────
# Dropping in a freshly trained flick.espdl (or removing it) must re-run
# CMake so the model gets (un)embedded.  This has to be set at the TOP
# level — component-level CMAKE_CONFIGURE_DEPENDS is not honoured by
# ESP-IDF's cmake_check_build_system, so `idf.py build` would silently
# keep the old binary otherwise.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/components/os/wrist_flick/model/flick.espdl"
    "${CMAKE_SOURCE_DIR}/components/os/double_tap/model/tap.espdl")

# ── SPIFFS partition image ───────────────────────────────────────────
# Generates a SPIFFS image from the ./spiffs folder and flashes it to
# the "storage" partition (7M, see partitions.csv) together with the
# firmware.  Files are mounted at runtime under /spiffs/ following a
# Linux FHS layout (see spiffs/README.md):
#   spiffs/etc/watch_config.json      → /spiffs/etc/watch_config.json
#   spiffs/usr/share/lang/en.json     → /spiffs/usr/share/lang/en.json
#   spiffs/usr/share/dict/words.bin   → /spiffs/usr/share/dict/words.bin
#   spiffs/usr/share/watchfaces/...   → /spiffs/usr/share/watchfaces/...
spiffs_create_partition_image(storage spiffs FLASH_IN_PROJECT)
