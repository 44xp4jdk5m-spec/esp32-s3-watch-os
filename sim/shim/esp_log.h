/**
 * @file esp_log.h — host stub that maps ESP_LOGx to colourised stdout.
 *
 * Levels are filtered by the SIM_LOG environment variable:
 *   0 = none .. 5 = verbose (default 3 = info)
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_LOG_NONE,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE
} esp_log_level_t;

void esp_log_level_set(const char *tag, esp_log_level_t level);
int  sim_log_enabled(esp_log_level_t level);
void sim_log_write(esp_log_level_t level, const char *tag,
                   const char *file, int line, const char *fmt, ...);

#define ESP_LOGE(tag, fmt, ...) sim_log_write(ESP_LOG_ERROR,   tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) sim_log_write(ESP_LOG_WARN,    tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) sim_log_write(ESP_LOG_INFO,    tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) sim_log_write(ESP_LOG_DEBUG,   tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) sim_log_write(ESP_LOG_VERBOSE, tag, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define ESP_LOG_BUFFER_HEXDUMP(tag, buf, len, lvl) do { (void)(tag); (void)(buf); (void)(len); (void)(lvl); } while (0)
#define ESP_LOG_LEVEL_LOCAL(lvl, tag, fmt, ...) do { (void)(tag); } while (0)
#define ESP_LOG_LEVEL(lvl, tag, fmt, ...) do { (void)(tag); } while (0)

#ifdef __cplusplus
}
#endif
