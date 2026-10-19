# ── Watch OS core component ──────────────────────────────────────────
# Platform services (config, events, power, wifi, time, dictionary, BLE/ANCS,
# notifications, watch-face renderer) + the launcher/menu UI + all watch
# faces + shared UI assets (fonts, app icons).
# Depends on: hw (drivers).  Apps and widgets depend on this component.

idf_component_register(
    SRCS
        # Platform services
        "app_config.c"
        "app_events.c"
        "app_power.c"
        "wake_log.c"
        "app_registry.c"
        "app_wifi.c"
        "app_time_region.c"
        "app_dictionary.c"
        "app_ble_ancs.c"
        "ble_ancs.c"
        "app_notification_store.c"
        "app_notification_center.c"
        "app_ui.c"
        "wake_on_raise/wake_on_raise.cpp"
        "wake_on_raise/ulp/wor_cnn.c"
        "wrist_flick/wrist_flick.cpp"
        "air_pointer/air_pointer.c"
        "double_tap/double_tap.cpp"
        "gesture_fusion/gesture_fusion.cpp"
        "lang/app_lang.c"
        # Launcher / menus
        "honeycomb_menu.c"
        "icon_rearrange.c"
        "list_view_menu.c"
        "list_view_font.c"
        # SPIFFS -> PSRAM image loader (on-demand image assets)
        "app_image_loader.c"
        # Watch faces
        "watch_faces/watch_face_registry.c"
        "watch_faces/watch_face_classic.c"
        "watch_faces/watch_face_minimal.c"
        "watch_faces/watch_face_flux.c"
        "watch_faces/watch_face_parallax.c"
        "watch_faces/watch_face_hands.c"
        "watch_faces/watch_face_california.c"
        "watch_faces/watch_face_picker.c"
        "watch_faces/watch_face_editor.c"
        "watch_faces/ui/ui.c"
        "watch_faces/ui/ui_boot_up.c"
        "watch_faces/ui/ui_progress_ring.c"
        "watch_faces/ui/ui_watch_face.c"
        "watch_faces/ui/ui_charging.c"
        "watch_faces/ui/ui_comp_hook.c"
        "watch_faces/ui/ui_helpers.c"
        "watch_faces/ui/ui_font_NotoSansCondensedBold80.c"
        "watch_faces/ui/ui_font_NotoSansExtraCondensedSemiBold25.c"
        # NOTE: flux_digits.c, ui_img_charge_png.c and ui_img_137261120.c
        # (large generated pixel arrays) are intentionally NOT compiled
        # here.  They are kept as regeneration sources and exported to
        # spiffs/usr/share/images/*.bin by tools/export_image_bins.py, then
        # loaded into PSRAM on demand via app_image_loader.c.
        # Shared assets (generated arrays)
        "png_images_rgb565.c"
        "png_images_circle.c"
        "ui_font_QilkaBold42.c"
        "fonts/noto_font_14.c"
        "fonts/noto_font_24.c"
        "fonts/noto_font_48.c"
        "fonts/noto_font_48_bold.c"
        "fonts/noto_font_24_bold.c"
        "fonts/mi_font_24.c"
        "fonts/mi_font_60.c"
        # System services
        "services/app_temperature_monitor.c"
        "services/gesture_recorder.c"
        "services/optimized_charging.c"
        "services/battery_history.c"
        "services/app_usage.c"
        "services/power_stats.c"
        "services/ui_warning_screen.c"
        "services/shutdown_ui.c"
        "services/screen_capture.c"
        "services/google_drive_uploader.c"
        "services/sd_cache_manager.c"
        "services/ota_manager.c"
        "services/js_app_registry.c"
        "services/storage_manager.c"
        "services/upload_notice_ui.c"
        "services/tinyusb_msc.c"
        "services/wifi_file_server.c"
    INCLUDE_DIRS
        "."
        "lang"
        "fonts"
        "services"
        "wake_on_raise"
        "wake_on_raise/ulp"
        "wrist_flick"
        "air_pointer"
        "double_tap"
        "gesture_fusion"
        "watch_faces"
        "watch_faces/ui"
    REQUIRES
        hw
        esp-dl
        lvgl__lvgl
        system_widget
        waveshare__esp32_s3_touch_amoled_2_06
        esp_wifi
        bt
        spiffs
        json
        esp_netif
        esp_event
        esp_timer
        nvs_flash
        esp_driver_gpio
        esp_http_client
        esp_http_server
        app_update
        espressif__tinyusb
        sdmmc
        usb
        esp_driver_tsens
        ulp
)

# ── Embed the wake-on-raise ESP-DL model in .rodata ───────────────────
# Exposes the symbol _binary_wake_on_raise_espdl_start used by wake_on_raise.cpp.
idf_component_get_property(espdl_dir esp-dl COMPONENT_DIR)
set(cmake_dir ${espdl_dir}/fbs_loader/cmake)
include(${cmake_dir}/utilities.cmake)
set(embed_files wake_on_raise/model/wake_on_raise.espdl)
target_add_aligned_binary_data(${COMPONENT_LIB} ${embed_files} BINARY)

# ── Embed the wrist-flick model ONLY if it has been trained ───────────
# Before training there is no flick.espdl, so the feature compiles but is
# inert (wrist_flick_start() returns ESP_ERR_NOT_FOUND).  Drop the trained
# model at wrist_flick/model/flick.espdl and rebuild to activate it.
set(FLICK_MODEL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/wrist_flick/model/flick.espdl")
# Re-run CMake automatically when the model file appears / disappears /
# is replaced, so adding a freshly trained model needs no manual step.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FLICK_MODEL_FILE})
if(EXISTS ${FLICK_MODEL_FILE})
    target_add_aligned_binary_data(${COMPONENT_LIB} "wrist_flick/model/flick.espdl" BINARY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WRIST_FLICK_MODEL_PRESENT=1)
    message(STATUS "wrist_flick: embedded flick.espdl (${FLICK_MODEL_FILE})")
else()
    message(STATUS "wrist_flick: flick.espdl NOT present — detector inactive until trained")
endif()

# ── Embed the double-tap model ONLY if it has been trained ───────────
# Same pattern as wrist_flick: tap.espdl (gesture_01.csv vs shared notflick)
# at double_tap/model/tap.espdl activates the double-tap click detector.
set(TAP_MODEL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/double_tap/model/tap.espdl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TAP_MODEL_FILE})
if(EXISTS ${TAP_MODEL_FILE})
    target_add_aligned_binary_data(${COMPONENT_LIB} "double_tap/model/tap.espdl" BINARY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DOUBLE_TAP_MODEL_PRESENT=1)
    message(STATUS "double_tap: embedded tap.espdl (${TAP_MODEL_FILE})")
else()
    message(STATUS "double_tap: tap.espdl NOT present — detector inactive until trained")
endif()

# ── Embed the unified 3-state gesture model ONLY if it has been trained ──
# Same pattern: gesture.espdl (trained by train_gesture.py from flick/tap/
# notflick CSVs) at gesture_fusion/model/gesture.espdl activates the single
# idle/wrist/double-tap detector that replaces flick+tap when both features
# are enabled.  Without it the separate detectors keep working.
set(GESTURE_MODEL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/gesture_fusion/model/gesture.espdl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${GESTURE_MODEL_FILE})
if(EXISTS ${GESTURE_MODEL_FILE})
    target_add_aligned_binary_data(${COMPONENT_LIB} "gesture_fusion/model/gesture.espdl" BINARY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE GESTURE_FUSION_MODEL_PRESENT=1)
    message(STATUS "gesture_fusion: embedded gesture.espdl (${GESTURE_MODEL_FILE})")
else()
    message(STATUS "gesture_fusion: gesture.espdl NOT present — fused detector inactive until trained")
endif()
# ── ULP-RISC-V wake-on-raise sampler (bit-banged IMU + gate) ─────────
# Built by the riscv32-esp-elf toolchain, loaded into RTC slow memory while
# the main CPU is in light sleep.  Generates ulp_wor.h with the shared
# symbols (wor_ring, wor_idx, wor_full, wor_gate_hit) used by
# wake_on_raise.cpp when it runs the ESP-DL CNN on a gate wake.
set(ulp_app_name ulp_wor)
set(ulp_riscv_sources "wake_on_raise/ulp/ulp_wor.c"
                      "wake_on_raise/ulp/wor_cnn.c")
set(ulp_exp_dep_srcs "wake_on_raise/wake_on_raise.cpp")
ulp_embed_binary(${ulp_app_name} "${ulp_riscv_sources}" "${ulp_exp_dep_srcs}")
# Make tusb_config.h visible to the TinyUSB component
idf_build_set_property(COMPILE_OPTIONS "-I${CMAKE_CURRENT_SOURCE_DIR}" APPEND)
