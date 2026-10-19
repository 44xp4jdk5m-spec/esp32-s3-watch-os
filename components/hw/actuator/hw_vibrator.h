/**
 * @file hw_vibrator.h
 * @brief Vibration motor (haptic) control using PWM on GPIO18.
 *
 * Uses ESP32-S3 LEDC peripheral for PWM control of an ERM vibration motor.
 * Provides haptic patterns, strength control, and duration management.
 * Respects the silent mode setting from Sound & Haptics.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** GPIO pin connected to the vibration motor. */
#define HW_VIBRATOR_GPIO        GPIO_NUM_18

/** LEDC configuration for the vibrator. */
#define HW_VIBRATOR_LEDC_TIMER      LEDC_TIMER_0
#define HW_VIBRATOR_LEDC_CHANNEL    LEDC_CHANNEL_0
#define HW_VIBRATOR_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define HW_VIBRATOR_LEDC_DUTY_RES   LEDC_TIMER_8_BIT   /**< 8-bit resolution (0-255). */
#define HW_VIBRATOR_LEDC_FREQ_HZ    200                 /**< 200 Hz typical for ERM motors. */

/** Maximum duty cycle value (255 for 8-bit). */
#define HW_VIBRATOR_MAX_DUTY        255

/** Default vibration strength (0-255) mapped to duty cycle. */
#define HW_VIBRATOR_DEFAULT_STRENGTH  180

/** Default vibration duration in milliseconds. */
#define HW_VIBRATOR_DEFAULT_DURATION_MS  100

/** Maximum vibration duration in milliseconds. */
#define HW_VIBRATOR_MAX_DURATION_MS  5000

/**
 * @brief Predefined haptic patterns.
 *
 * Each pattern is a timed sequence of PWM duty cycles that create
 * different tactile sensations.
 */
typedef enum {
    HW_VIBE_PATTERN_CLICK = 0,          /**< Single short pulse. */
    HW_VIBE_PATTERN_DOUBLE_CLICK,       /**< Two short pulses. */
    HW_VIBE_PATTERN_TRIPLE_CLICK,       /**< Three short pulses. */
    HW_VIBE_PATTERN_HEARTBEAT,          /**< Strong-weak-strong (heartbeat rhythm). */
    HW_VIBE_PATTERN_RISING,             /**< Gradual ramp up. */
    HW_VIBE_PATTERN_DECAYING,           /**< Gradual ramp down. */
    HW_VIBE_PATTERN_PULSING,            /**< Continuous on/off pulsing. */
    HW_VIBE_PATTERN_SIREN,              /**< Alternating fast intensity changes. */
    HW_VIBE_PATTERN_SOS,                /**< SOS in morse (···---···). */
    HW_VIBE_PATTERN_BUZZ,               /**< Continuous buzz at full strength. */
    HW_VIBE_PATTERN_COUNT
} hw_vibe_pattern_t;

/** Human-readable names for each pattern (for settings UI). */
extern const char *hw_vibe_pattern_names[HW_VIBE_PATTERN_COUNT];

/* ── Configuration ─────────────────────────────────────────────────── */

/**
 * @brief Vibration motor configuration (persisted in app_config).
 */
typedef struct {
    uint8_t  strength;       /**< Vibration strength 0-255 (mapped to PWM duty). */
    uint8_t  pattern_index;  /**< Current pattern index (hw_vibe_pattern_t). */
    uint16_t duration_ms;    /**< Duration in milliseconds (50-5000). */
} hw_vibrator_config_t;

/* ── API ────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the vibration motor PWM on GPIO18.
 *
 * Configures the LEDC timer and channel for PWM output.
 * Must be called once before any vibrator functions.
 *
 * @return ESP_OK on success.
 */
esp_err_t hw_vibrator_init(void);

/**
 * @brief Deinitialize the vibration motor.
 *
 * Stops vibration and releases LEDC resources.
 */
void hw_vibrator_deinit(void);

/**
 * @brief Set the master mute state.
 *
 * When true, all vibration is suppressed regardless of play requests.
 * Connected to the "Silent Mode" setting in Sound & Haptics.
 *
 * @param muted true to mute all vibration.
 */
void hw_vibrator_set_muted(bool muted);

/**
 * @brief Check if the vibrator is currently muted.
 *
 * @return true if vibration is muted.
 */
bool hw_vibrator_is_muted(void);

/**
 * @brief Play a vibration at the given strength for the given duration.
 *
 * Simple continuous vibration. Useful for short feedback pulses.
 * The request is scheduled on a dedicated FreeRTOS vibrator task and
 * returns immediately, so long vibration durations do not block LVGL.
 *
 * @param strength  PWM duty cycle 0-255.
 * @param duration_ms Duration in milliseconds.
 * @return ESP_OK on success, or an error if the vibrator task is unavailable.
 */
esp_err_t hw_vibrator_play(uint8_t strength, uint16_t duration_ms);

/**
 * @brief Play a predefined haptic pattern.
 *
 * Plays the pattern using the specified strength and total duration.
 * The pattern's internal timing is scaled to fit within duration_ms.
 * The request is scheduled asynchronously on the vibrator task.
 *
 * @param pattern   The pattern to play.
 * @param strength  Maximum PWM duty cycle 0-255.
 * @param duration_ms Total duration in milliseconds.
 * @return ESP_OK on success, or an error if the vibrator task is unavailable.
 */
esp_err_t hw_vibrator_play_pattern(hw_vibe_pattern_t pattern, uint8_t strength, uint16_t duration_ms);

/**
 * @brief Play the currently configured pattern from settings.
 *
 * Uses the strength, pattern, and duration from the active config.
 * Does nothing if muted.
 *
 * @return ESP_OK on success.
 */
esp_err_t hw_vibrator_play_configured(void);

/**
 * @brief Stop the vibration immediately.
 */
void hw_vibrator_stop(void);

/**
 * @brief Check if the vibrator is currently active.
 *
 * @return true if vibration is currently playing.
 */
bool hw_vibrator_is_playing(void);

/**
 * @brief Get the current vibrator configuration.
 */
hw_vibrator_config_t hw_vibrator_get_config(void);

/**
 * @brief Set the vibrator configuration.
 *
 * @param config New configuration.
 */
void hw_vibrator_set_config(const hw_vibrator_config_t *config);

/**
 * @brief Quick haptic feedback — single click at configured strength.
 *
 * Convenience function for short feedback pulses (e.g. button press,
 * screen change).  Takes ~50ms at full strength.
 * Does nothing if muted.
 */
void hw_vibrator_click(void);

#ifdef __cplusplus
}
#endif
