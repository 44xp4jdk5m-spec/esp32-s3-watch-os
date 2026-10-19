/**
 * @file hw_vibrator.c
 * @brief Vibration motor control using ESP32-S3 LEDC PWM on GPIO18.
 *
 * Implements configurable haptic patterns using timed PWM duty sequences.
 * Patterns are defined as arrays of (duty_percent, duration_ms) segments.
 *
 * Pattern timing scaling:
 *   When a pattern's natural duration differs from the requested duration_ms,
 *   the segment times are scaled proportionally so the pattern always fits
 *   within the requested total time.
 */

#include "hw_vibrator.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "HW_VIBR";

/* ── Pattern name labels (for settings UI picker) ──────────────────── */
const char *hw_vibe_pattern_names[HW_VIBE_PATTERN_COUNT] = {
    [HW_VIBE_PATTERN_CLICK]        = "Click",
    [HW_VIBE_PATTERN_DOUBLE_CLICK] = "Double Click",
    [HW_VIBE_PATTERN_TRIPLE_CLICK] = "Triple Click",
    [HW_VIBE_PATTERN_HEARTBEAT]    = "Heartbeat",
    [HW_VIBE_PATTERN_RISING]       = "Rising",
    [HW_VIBE_PATTERN_DECAYING]     = "Decaying",
    [HW_VIBE_PATTERN_PULSING]      = "Pulsing",
    [HW_VIBE_PATTERN_SIREN]        = "Siren",
    [HW_VIBE_PATTERN_SOS]          = "SOS",
    [HW_VIBE_PATTERN_BUZZ]         = "Buzz",
};

/* ── Pattern segment: (duty_percent, duration_ms) ──────────────────── */
typedef struct {
    uint8_t  duty_pct;    /**< Duty cycle as percentage of max (0-100). */
    uint16_t duration_ms; /**< How long to hold this segment. */
} vibe_segment_t;

/**
 * @brief Pattern definitions.
 *
 * Each pattern is built from segments that define the vibration profile.
 * Percentages are relative to the configured strength (0-100%).
 */
static const struct {
    const vibe_segment_t *segments;
    uint8_t               count;
    uint16_t              natural_duration_ms; /**< Sum of segment durations. */
} s_patterns[HW_VIBE_PATTERN_COUNT] = {

    /* HW_VIBE_PATTERN_CLICK — Single sharp pulse */
    [HW_VIBE_PATTERN_CLICK] = {
        .segments = (const vibe_segment_t[]){
            {100, 50},
            {0,   30},
        },
        .count = 2,
        .natural_duration_ms = 80,
    },

    /* HW_VIBE_PATTERN_DOUBLE_CLICK — Two quick taps */
    [HW_VIBE_PATTERN_DOUBLE_CLICK] = {
        .segments = (const vibe_segment_t[]){
            {100, 40},
            {0,   30},
            {100, 40},
            {0,   30},
        },
        .count = 4,
        .natural_duration_ms = 140,
    },

    /* HW_VIBE_PATTERN_TRIPLE_CLICK — Three quick taps */
    [HW_VIBE_PATTERN_TRIPLE_CLICK] = {
        .segments = (const vibe_segment_t[]){
            {100, 30},
            {0,   25},
            {100, 30},
            {0,   25},
            {100, 30},
            {0,   25},
        },
        .count = 6,
        .natural_duration_ms = 165,
    },

    /* HW_VIBE_PATTERN_HEARTBEAT — Lub-dub rhythm */
    [HW_VIBE_PATTERN_HEARTBEAT] = {
        .segments = (const vibe_segment_t[]){
            {100, 80},
            {0,   40},
            {60,  60},
            {0,   40},
            {100, 80},
            {0,   40},
            {60,  60},
            {0,   40},
        },
        .count = 8,
        .natural_duration_ms = 440,
    },

    /* HW_VIBE_PATTERN_RISING — Gradual ramp up */
    [HW_VIBE_PATTERN_RISING] = {
        .segments = (const vibe_segment_t[]){
            {10, 60},  {20, 60},  {30, 60},  {40, 60},
            {50, 60},  {60, 60},  {70, 60},  {80, 60},
            {90, 60},  {100, 60},
        },
        .count = 10,
        .natural_duration_ms = 600,
    },

    /* HW_VIBE_PATTERN_DECAYING — Gradual ramp down */
    [HW_VIBE_PATTERN_DECAYING] = {
        .segments = (const vibe_segment_t[]){
            {100, 60}, {90, 60},  {80, 60},  {70, 60},
            {60, 60},  {50, 60},  {40, 60},  {30, 60},
            {20, 60},  {10, 60},
        },
        .count = 10,
        .natural_duration_ms = 600,
    },

    /* HW_VIBE_PATTERN_PULSING — On/off cycling */
    [HW_VIBE_PATTERN_PULSING] = {
        .segments = (const vibe_segment_t[]){
            {100, 100},
            {0,   100},
            {100, 100},
            {0,   100},
            {100, 100},
            {0,   100},
        },
        .count = 6,
        .natural_duration_ms = 600,
    },

    /* HW_VIBE_PATTERN_SIREN — Fast alternating intensity */
    [HW_VIBE_PATTERN_SIREN] = {
        .segments = (const vibe_segment_t[]){
            {100, 50}, {30,  50},
            {100, 50}, {30,  50},
            {100, 50}, {30,  50},
            {100, 50}, {30,  50},
        },
        .count = 8,
        .natural_duration_ms = 400,
    },

    /* HW_VIBE_PATTERN_SOS — Morse SOS: ··· --- ··· */
    [HW_VIBE_PATTERN_SOS] = {
        .segments = (const vibe_segment_t[]){
            /* S: dit-dit-dit */
            {100, 80},  {0, 80},
            {100, 80},  {0, 80},
            {100, 80},  {0, 160},
            /* O: dah-dah-dah */
            {100, 240}, {0, 80},
            {100, 240}, {0, 80},
            {100, 240}, {0, 160},
            /* S: dit-dit-dit */
            {100, 80},  {0, 80},
            {100, 80},  {0, 80},
            {100, 80},  {0, 80},
        },
        .count = 18,
        .natural_duration_ms = 2160,
    },

    /* HW_VIBE_PATTERN_BUZZ — Continuous buzz */
    [HW_VIBE_PATTERN_BUZZ] = {
        .segments = (const vibe_segment_t[]){
            {100, 500},
            {0,   50},
        },
        .count = 2,
        .natural_duration_ms = 550,
    },
};

/* ── State ─────────────────────────────────────────────────────────── */
static bool              s_initialized = false;
static bool              s_muted = false;
static volatile bool     s_playing = false;
static volatile bool     s_stop_requested = false;
static TaskHandle_t      s_task = NULL;
static QueueHandle_t     s_cmd_queue = NULL;
static hw_vibrator_config_t s_config = {
    .strength     = HW_VIBRATOR_DEFAULT_STRENGTH,
    .pattern_index = HW_VIBE_PATTERN_CLICK,
    .duration_ms  = HW_VIBRATOR_DEFAULT_DURATION_MS,
};

typedef enum {
    HW_VIBRATOR_CMD_PLAY,
    HW_VIBRATOR_CMD_PATTERN,
    HW_VIBRATOR_CMD_STOP,
} hw_vibrator_cmd_type_t;

typedef struct {
    hw_vibrator_cmd_type_t type;
    hw_vibe_pattern_t      pattern;
    uint8_t                strength;
    uint16_t               duration_ms;
} hw_vibrator_cmd_t;

#define HW_VIBRATOR_CMD_QUEUE_LENGTH 4

/* ── Low-level PWM helpers ─────────────────────────────────────────── */

static void _set_duty_raw(uint8_t duty)
{
    if (!s_initialized) return;
    ledc_set_duty(HW_VIBRATOR_LEDC_MODE, HW_VIBRATOR_LEDC_CHANNEL, duty);
    ledc_update_duty(HW_VIBRATOR_LEDC_MODE, HW_VIBRATOR_LEDC_CHANNEL);
}

static void _stop_output(void)
{
    if (!s_initialized) return;
    ledc_stop(HW_VIBRATOR_LEDC_MODE, HW_VIBRATOR_LEDC_CHANNEL, 0);
    gpio_set_level(HW_VIBRATOR_GPIO, 0);
}

static void _set_duty(uint8_t duty)
{
    if (duty == 0) {
        _stop_output();
    } else {
        _set_duty_raw(duty);
    }
}

static void _vibrator_play_constant(uint8_t strength, uint16_t duration_ms)
{
    if (duration_ms == 0) return;
    _set_duty(strength);
    uint32_t end_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(duration_ms);
    while (!s_stop_requested && xTaskGetTickCount() < end_ticks) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    _set_duty(0);
}

static void _vibrator_play_pattern_internal(hw_vibe_pattern_t pattern, uint8_t strength, uint16_t duration_ms)
{
    const vibe_segment_t *segs = s_patterns[pattern].segments;
    uint8_t seg_count = s_patterns[pattern].count;
    uint16_t natural_ms = s_patterns[pattern].natural_duration_ms;

    if (seg_count == 0 || natural_ms == 0) return;

    float scale = (float)duration_ms / (float)natural_ms;
    if (scale < 0.1f) scale = 0.1f;

    for (uint8_t i = 0; i < seg_count && !s_stop_requested; i++) {
        uint8_t duty = (uint8_t)((uint16_t)strength * segs[i].duty_pct / 100);
        uint16_t seg_ms = (uint16_t)((float)segs[i].duration_ms * scale);
        if (seg_ms < 10) seg_ms = 10;

        _set_duty(duty);
        if (seg_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(seg_ms));
        }
    }

    _set_duty(0);
}

static void _hw_vibrator_task(void *arg)
{
    (void)arg;
    hw_vibrator_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd.type == HW_VIBRATOR_CMD_STOP) {
            s_stop_requested = true;
            _set_duty(0);
            s_playing = false;
            continue;
        }

        if (s_muted) {
            continue;
        }

        s_stop_requested = false;
        s_playing = true;

        if (cmd.type == HW_VIBRATOR_CMD_PLAY) {
            _vibrator_play_constant(cmd.strength, cmd.duration_ms);
        } else if (cmd.type == HW_VIBRATOR_CMD_PATTERN) {
            if (cmd.pattern < HW_VIBE_PATTERN_COUNT) {
                _vibrator_play_pattern_internal(cmd.pattern, cmd.strength, cmd.duration_ms);
            }
        }

        s_playing = false;
        s_stop_requested = false;
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

esp_err_t hw_vibrator_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing vibration motor on GPIO%d", HW_VIBRATOR_GPIO);

    /* Configure the GPIO as output first */
    gpio_reset_pin(HW_VIBRATOR_GPIO);
    gpio_set_direction(HW_VIBRATOR_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(HW_VIBRATOR_GPIO, 0);

    /* Configure LEDC timer */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = HW_VIBRATOR_LEDC_MODE,
        .duty_resolution = HW_VIBRATOR_LEDC_DUTY_RES,
        .timer_num       = HW_VIBRATOR_LEDC_TIMER,
        .freq_hz         = HW_VIBRATOR_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    /* Configure LEDC channel */
    ledc_channel_config_t chan_cfg = {
        .gpio_num   = HW_VIBRATOR_GPIO,
        .speed_mode = HW_VIBRATOR_LEDC_MODE,
        .channel    = HW_VIBRATOR_LEDC_CHANNEL,
        .timer_sel  = HW_VIBRATOR_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chan_cfg));

    s_cmd_queue = xQueueCreate(HW_VIBRATOR_CMD_QUEUE_LENGTH, sizeof(hw_vibrator_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create vibrator command queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_ret = xTaskCreate(
        _hw_vibrator_task,
        "hw_vibrator",
        2048,
        NULL,
        tskIDLE_PRIORITY + 1,
        &s_task
    );
    if (task_ret != pdPASS) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        ESP_LOGE(TAG, "Failed to create vibrator task");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Vibration motor ready");
    return ESP_OK;
}

void hw_vibrator_deinit(void)
{
    if (!s_initialized) return;
    hw_vibrator_stop();
    _stop_output();

    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    if (s_cmd_queue) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }

    s_initialized = false;
    s_playing = false;
    ESP_LOGI(TAG, "Vibration motor deinitialized");
}

void hw_vibrator_set_muted(bool muted)
{
    if (muted && !s_muted) {
        /* Mute was just activated — stop any ongoing vibration */
        hw_vibrator_stop();
    }
    s_muted = muted;
    ESP_LOGI(TAG, "Vibration %s", muted ? "MUTED" : "UNMUTED");
}

bool hw_vibrator_is_muted(void)
{
    return s_muted;
}

esp_err_t hw_vibrator_play(uint8_t strength, uint16_t duration_ms)
{
    if (!s_initialized || !s_cmd_queue) return ESP_ERR_INVALID_STATE;
    if (s_muted) return ESP_OK;  /* Silently ignore when muted */
    if (duration_ms == 0) return ESP_OK;
    if (strength > HW_VIBRATOR_MAX_DUTY) strength = HW_VIBRATOR_MAX_DUTY;

    hw_vibrator_cmd_t cmd = {
        .type = HW_VIBRATOR_CMD_PLAY,
        .pattern = HW_VIBE_PATTERN_CLICK,
        .strength = strength,
        .duration_ms = duration_ms,
    };

    if (xQueueSendToBack(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Vibrator command queue full, dropping play request");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t hw_vibrator_play_pattern(hw_vibe_pattern_t pattern, uint8_t strength, uint16_t duration_ms)
{
    if (!s_initialized || !s_cmd_queue) return ESP_ERR_INVALID_STATE;
    if (s_muted) return ESP_OK;  /* Silently ignore when muted */
    if (pattern >= HW_VIBE_PATTERN_COUNT) return ESP_ERR_INVALID_ARG;
    if (strength > HW_VIBRATOR_MAX_DUTY) strength = HW_VIBRATOR_MAX_DUTY;

    hw_vibrator_cmd_t cmd = {
        .type = HW_VIBRATOR_CMD_PATTERN,
        .pattern = pattern,
        .strength = strength,
        .duration_ms = duration_ms,
    };

    if (xQueueSendToBack(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Vibrator command queue full, dropping pattern request");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t hw_vibrator_play_configured(void)
{
    return hw_vibrator_play_pattern(
        (hw_vibe_pattern_t)s_config.pattern_index,
        s_config.strength,
        s_config.duration_ms
    );
}

void hw_vibrator_stop(void)
{
    if (!s_initialized) return;

    if (s_cmd_queue) {
        hw_vibrator_cmd_t cmd = {
            .type = HW_VIBRATOR_CMD_STOP,
            .pattern = HW_VIBE_PATTERN_CLICK,
            .strength = 0,
            .duration_ms = 0,
        };
        xQueueSendToBack(s_cmd_queue, &cmd, 0);
    }

    s_stop_requested = true;
    _set_duty(0);
    s_playing = false;
}

bool hw_vibrator_is_playing(void)
{
    return s_playing;
}

hw_vibrator_config_t hw_vibrator_get_config(void)
{
    return s_config;
}

void hw_vibrator_set_config(const hw_vibrator_config_t *config)
{
    if (!config) return;
    s_config = *config;
    if (s_config.strength > HW_VIBRATOR_MAX_DUTY) s_config.strength = HW_VIBRATOR_MAX_DUTY;
    if (s_config.pattern_index >= HW_VIBE_PATTERN_COUNT) s_config.pattern_index = 0;
    if (s_config.duration_ms < 50) s_config.duration_ms = 50;
    if (s_config.duration_ms > HW_VIBRATOR_MAX_DURATION_MS) s_config.duration_ms = HW_VIBRATOR_MAX_DURATION_MS;
}

void hw_vibrator_click(void)
{
    if (s_muted || !s_initialized) return;
    hw_vibrator_play_pattern((hw_vibe_pattern_t)s_config.pattern_index,
                             s_config.strength,
                             s_config.duration_ms);
}
