# ── Boot / application entry point ────────────────────────────────────
# This component contains only the app entry point (main.c) plus the
# LVGL custom allocator and boot wiring.  All drivers, OS services, apps
# and widgets live in components/.

idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    REQUIRES shell)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
# Compile the LVGL custom PSRAM allocator into the lvgl library itself so the
# lv_malloc_core/lv_free_core/lv_realloc_core symbols resolve from lv_mem.c
# within the same archive (the app component is linked last).
target_sources(${LVGL_LIB} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lv_mem_custom.c)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
)
# ── LVGL perf-monitor CPU fix ────────────────────────────────────────
# The LVGL perf monitor shows "CPU %" as 100 - LV_SYSMON_GET_IDLE().
# Its default implementation (lv_os_get_idle_percent) only works when the
# FreeRTOS traceTASK_SWITCHED_IN/OUT hooks are wired to
# lv_freertos_task_switch_in/out(), which ESP-IDF does not do, so it always
# returns 0 and the monitor is stuck at 100 %.
# Point it at LVGL's own idle measurement (lv_timer_get_idle) instead, so
# the monitor reflects the real time lv_timer_handler() spends working.
target_compile_definitions(${LVGL_LIB} PRIVATE LV_SYSMON_GET_IDLE=lv_timer_get_idle)
