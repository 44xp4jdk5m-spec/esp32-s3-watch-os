# ── Application shell (boot + main loop) ─────────────────────────────
# Top-level orchestrator: brings up the OS, then runs the event loop that
# dispatches to the watch faces, launcher menus, apps and services.
# Nothing depends on this component; it is the composition root.

idf_component_register(
    SRCS "watch_os.c"
    INCLUDE_DIRS "."
    REQUIRES
        os
        apps
        hw
        lvgl__lvgl
        esp_timer
        esp_driver_gpio
        waveshare__esp32_s3_touch_amoled_2_06
)
