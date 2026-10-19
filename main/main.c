/**
 * @file main.c
 * @brief Watch firmware entry point.
 *
 * The boot sequence and main loop live in the `shell` component
 * (components/shell/watch_os.c).  This file is intentionally minimal —
 * it only hands control to the shell.
 *
 * NOTE: watch_os_run() must run on an INTERNAL-RAM stack.  ESP32-S3 forbids
 * task stacks in PSRAM: esp_cache_freeze_caches_disable_interrupts() asserts
 * the current task stack is in internal RAM (flash/NVS ops freeze the cache,
 * making PSRAM unreachable).  The OS therefore runs on the main task, whose
 * stack is enlarged via CONFIG_ESP_MAIN_TASK_STACK_SIZE (32KB) to give
 * QuickJS enough C-stack for parsing/evaluating app scripts.
 */

#include "watch_os.h"

void app_main(void)
{
    watch_os_run();
}
