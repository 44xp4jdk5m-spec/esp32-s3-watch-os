# ── User-facing apps ──────────────────────────────────────────────────
# Each app lives in its own subfolder and is self-contained (UI + logic).
# Apps depend on the OS core, shared widgets, and the hardware layer.

idf_component_register(
    SRCS
        # Settings app — coordinator + per-topic page modules on the
        # reusable system_widget primitives (sw_list / sw_graph / ...).
        "settings/settings_ui.c"
        "settings/settings_config.c"
        "settings/settings_display.c"
        "settings/settings_sound.c"
        "settings/settings_network.c"
        "settings/settings_power.c"
        "settings/settings_storage.c"
        "settings/settings_appview.c"
        "settings/settings_general.c"
        "settings/settings_apps.c"
        "control_center/control_center.c"
        "flashlight/flashlight_ui.c"
        "photo/photos_ui.c"
        "photo/photo_cache.c"
        "photo/webp_decoder.c"
        "calculator/calculator_ui.c"
        "noise/noise_ui.c"
        "noise/ui/ui_noise.c"
        "activity/activity_ui.c"
        "activity/activity_pages.c"
        "jsapps/js_bind.c"
        "jsapps/jsapp_loading.c"
        "jsapps/jsapp_run.c"
        "jsapps/jsapps_ui.c"
        # NOTE: the activity ui_img_*.c files (generated icon pixel arrays)
        # are intentionally NOT compiled — they are kept as regeneration
        # sources and exported to spiffs/usr/share/activity/*.bin by
        # tools/export_image_bins.py, then loaded into PSRAM via
        # app_image_loader.c (see activity_pages_load_icons()).
    INCLUDE_DIRS
        "settings"
        "control_center"
        "flashlight"
        "photo"
        "calculator"
        "noise"
        "activity"
        "jsapps"
    REQUIRES
        os
        js
        system_widget
        hw
        lvgl__lvgl
        esp_wifi
        esp_timer
        nvs_flash
        json
        espressif__esp_codec_dev
)
