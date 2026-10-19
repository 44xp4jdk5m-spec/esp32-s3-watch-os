/**
 * @file gesture_recorder.h
 * @brief IMU gesture data recorder — used to collect training data for the
 *        wrist-flick (back gesture) ESP-DL classifier.
 *
 * While recording, a background task samples the QMI8658 at 50 Hz (matching
 * the training pipeline) and appends each sample (ax, ay, az, gx, gy, gz)
 * to a growable PSRAM buffer.
 *
 * Two capture modes:
 *  - FLICK:   one continuous session; tap "Mark" right after each flick.
 *             Each mark copies the last 1.5 s (75 samples) into a segment
 *             buffer.  Saving writes a flick.csv made ONLY of those marked
 *             flick windows (no idle gaps) — one file holds all your flicks.
 *  - NOTFLICK: just record idle / normal movement continuously, no marking.
 *             Saving writes the whole capture as notflick.csv.
 *
 * Units match the training data: accel in m/s^2, gyro in deg/s.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Default name for a new recording (short 8.3 FAT name). */
#define GESTURE_REC_DEFAULT_NAME  "gesture_01.csv"

/** Suggested file names matching the training pipeline. */
#define GESTURE_REC_FLICK_NAME     "flick.csv"
#define GESTURE_REC_NOTFLICK_NAME  "notflick.csv"

/** Directory on the SD card where recordings are written. */
#define GESTURE_REC_DIR           "/sdcard/gestures"

/** Capture mode. */
typedef enum {
    GESTURE_REC_MODE_FLICK = 0,    /**< Mark each flick with a tap */
    GESTURE_REC_MODE_NOTFLICK = 1, /**< Continuous idle / movement capture */
} gesture_rec_mode_t;

/**
 * @brief Initialise the recorder (creates the recording task + mutex).
 *        Safe to call more than once.
 */
esp_err_t gesture_recorder_init(void);

/**
 * @brief Select the capture mode.  Applies from the next start() onwards.
 */
void gesture_recorder_set_mode(gesture_rec_mode_t mode);

/**
 * @brief Get the current capture mode.
 */
gesture_rec_mode_t gesture_recorder_get_mode(void);

/**
 * @brief Start a fresh recording (clears buffers for the current mode).
 * @return true if recording started.
 */
bool gesture_recorder_start(void);

/**
 * @brief Stop recording (keeps the captured data for saving).
 */
void gesture_recorder_stop(void);

/**
 * @brief True while a recording is in progress.
 */
bool gesture_recorder_is_recording(void);

/**
 * @brief (FLICK mode) Mark the just-finished flick: copies the last
 *        1.5 s window (75 samples) into the segment buffer.
 * @return true if a window was captured.
 */
bool gesture_recorder_mark_flick(void);

/**
 * @brief Number of flicks marked so far (FLICK mode).
 */
uint32_t gesture_recorder_flick_count(void);

/**
 * @brief Number of raw samples captured so far.
 */
uint32_t gesture_recorder_sample_count(void);

/**
 * @brief Peak gyro magnitude (deg/s) seen during this recording — handy
 *        on-screen feedback so the user can tell how strong the flicks are.
 */
float gesture_recorder_peak_gyro(void);

/**
 * @brief Discard the captured data (segments + raw buffer).
 */
void gesture_recorder_clear(void);

/**
 * @brief True when the SD card is mounted and writable.
 */
bool gesture_recorder_sd_ready(void);

/**
 * @brief Save the capture to /sdcard/gestures/<filename>.
 *
 * The filename must be a short 8.3 name (long FAT names are disabled).
 * Should be called while NOT recording (the UI stops first).
 *
 * FLICK mode writes ONLY the marked flick windows (75 rows each);
 * NOTFLICK mode writes the whole continuous capture.
 *
 * @param filename  e.g. "flick.csv" or "notflick.csv"
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no SD card,
 *         ESP_ERR_NO_MEM if nothing was recorded, ESP_FAIL on write error.
 */
esp_err_t gesture_recorder_save(const char *filename);

#ifdef __cplusplus
}
#endif
