/**
 * @file ulp_wor.c
 * @brief ULP-RISC-V light-sleep sampler: QMI8658 raise gate only.
 *
 * Runs on the ESP32-S3 ULP-RISC-V coprocessor while the main CPU is in
 * light sleep (display off).  It:
 *   1. bit-bangs I2C on GPIO14 (SCL) / GPIO15 (SDA) — both RTC-capable —
 *      to read the QMI8658 (0x6B) 6-axis data burst (0x35..0x40, 12 bytes)
 *      at ~50 Hz into a 1.5 s ring buffer in RTC slow memory;
 *   2. runs a cheap energy pre-filter, then a PURE-INTEGER 3-layer CNN
 *      (wor_cnn.c) over the ring and wakes the main CPU only on a raise;
 *   3. wakes the main CPU when the CNN gate fires.
 *
 * Touch wake is NOT done here: hardware testing showed the bit-banged FT3168
 * read mostly fails and wedges the touch controller after wake, while the
 * FT3168 INT (GPIO38) already wakes reliably.  The host handles touch wake
 * via GPIO38 and only uses the ULP for the IMU raise gate.
 *
 * The main CPU reads wor_ring / wor_idx / wor_full / wor_gate_hit /
 * wor_cnn_logit through the generated ulp_ulp_wor.h symbols.  The CNN
 * weights are generated + validated by tinyml_mpu/espdl_model/train_ulp_cnn.py
 * (100% fixed-point test accuracy, 0 false positives on recorded data).
 */

#include <stdint.h>
#include "ulp_riscv.h"
#include "ulp_riscv_utils.h"
#include "ulp_riscv_gpio.h"
#include "wor_cnn.h"

/* ── I2C bit-bang pins (RTC-capable GPIOs; the board I2C bus) ───────── */
#define WOR_I2C_SCL      14   /* GPIO14 */
#define WOR_I2C_SDA      15   /* GPIO15 */
#define QMI8658_ADDR     0x6B

/* ── QMI8658 registers ──────────────────────────────────────────────── */
#define QMI8658_WHO_AM_I 0x00
#define QMI8658_AX_L     0x35   /* burst read 12 bytes: AX..AZ,GX..GZ */

/* ── Sampling / window ──────────────────────────────────────────────── */
#define WOR_WINDOW       75     /* 1.5 s @ 50 Hz */
#define WOR_SAMPLE_DELAY_MS 18  /* ~20 ms period minus ~2 ms I2C read */
#define WOR_GATE_EVERY   3      /* evaluate the gate every ~60 ms (fast wake) */

/* ── Cheap energy pre-filter (skips the CNN when the window is calm) ──
 * Saves ULP compute + power while the watch is truly still.  Thresholds are
 * ~2.5x below the gentlest recorded raise (589 dps gyro / ~5 m/s^2 dyn
 * accel), so no real raise is ever gated out by this.
 *   gyro 200 dps  = 200*32 = 6400 raw  -> (6400)^2
 *   accel 1.4 g   = 1.4*4096 = 5734 raw -> (5734)^2                          */
#define WOR_PRE_GYRO_SQ   ((uint32_t)(200 * 32UL) * (200 * 32UL))
#define WOR_PRE_ACCEL_SQ  ((uint32_t)(4096UL * 14 / 10) * (4096UL * 14 / 10))

/* ── Shared with the main CPU (RTC slow memory) ────────────────────── */
volatile int16_t  wor_ring[WOR_WINDOW][6];  /* raw AX..AZ, GX..GZ */
volatile uint32_t wor_idx;                  /* next ring write slot  */
volatile uint32_t wor_full;                 /* ring filled >= window */
volatile uint32_t wor_gate_hit;             /* IMU gate fired -> wake */
volatile uint32_t wor_imu_enabled;          /* 1 = sample IMU + gate  */
volatile uint32_t wor_running;              /* ULP alive flag        */
volatile uint32_t wor_sample_count;         /* total IMU samples taken */
volatile int32_t  wor_cnn_logit;            /* last CNN logit (Q15)   */

/* ── I2C bit-bang (open-drain SDA, push-pull SCL) ──────────────────── */
static inline void wor_scl_hi(void) { ulp_riscv_gpio_output_level(WOR_I2C_SCL, 1); }
static inline void wor_scl_lo(void) { ulp_riscv_gpio_output_level(WOR_I2C_SCL, 0); }
static inline void wor_sda_hi(void) { ulp_riscv_gpio_output_level(WOR_I2C_SDA, 1); }
static inline void wor_sda_lo(void) { ulp_riscv_gpio_output_level(WOR_I2C_SDA, 0); }
static inline uint32_t wor_sda_read(void) { return ulp_riscv_gpio_get_level(WOR_I2C_SDA); }

static inline void wor_i2c_delay(void)
{
    /* ~5 us half bit-time at ~17.5 cycles/us -> ~88 cycles */
    ulp_riscv_delay_cycles(5 * ULP_RISCV_CYCLES_PER_US);
}

static void wor_i2c_start(void)
{
    wor_sda_hi(); wor_scl_hi(); wor_i2c_delay();
    wor_sda_lo(); wor_i2c_delay();
    wor_scl_lo(); wor_i2c_delay();
}

static void wor_i2c_stop(void)
{
    wor_sda_lo(); wor_i2c_delay();
    wor_scl_hi(); wor_i2c_delay();
    wor_sda_hi(); wor_i2c_delay();
}

static void wor_i2c_write_bit(uint32_t bit)
{
    if (bit) wor_sda_hi(); else wor_sda_lo();
    wor_i2c_delay();
    wor_scl_hi(); wor_i2c_delay();
    wor_scl_lo(); wor_i2c_delay();
}

static uint32_t wor_i2c_read_bit(void)
{
    wor_sda_hi();            /* release SDA (open-drain) */
    wor_i2c_delay();
    wor_scl_hi(); wor_i2c_delay();
    uint32_t b = wor_sda_read();
    wor_scl_lo(); wor_i2c_delay();
    return b;
}

/* returns 1 on NACK, 0 on ACK */
static uint32_t wor_i2c_write_byte(uint8_t b)
{
    int i;
    for (i = 7; i >= 0; i--) wor_i2c_write_bit((b >> i) & 1);
    return wor_i2c_read_bit();
}

static uint8_t wor_i2c_read_byte(uint32_t ack)
{
    uint8_t b = 0;
    int i;
    for (i = 0; i < 8; i++) b = (uint8_t)((b << 1) | wor_i2c_read_bit());
    wor_i2c_write_bit(ack ? 0 : 1);   /* ack=1 -> send ACK */
    return b;
}

/* burst read of n bytes starting at reg of device dev; returns 0 on success */
static uint32_t wor_i2c_read_regs(uint8_t dev, uint8_t reg, uint8_t *buf, uint32_t n)
{
    uint32_t i;
    wor_i2c_start();
    if (wor_i2c_write_byte((dev << 1) | 0)) { wor_i2c_stop(); return 1; }
    if (wor_i2c_write_byte(reg))            { wor_i2c_stop(); return 1; }
    wor_i2c_start();                                  /* repeated start   */
    if (wor_i2c_write_byte((dev << 1) | 1)) { wor_i2c_stop(); return 1; }
    for (i = 0; i < n; i++) buf[i] = wor_i2c_read_byte(i < n - 1);
    wor_i2c_stop();
    return 0;
}

/* ── Gate: cheap energy pre-filter, then the pure-integer 3-layer CNN ── */
static uint32_t wor_pre_filter(void)
{
    uint32_t gp_sq = 0, am_sq = 0;
    uint32_t k, i;

    for (k = 0; k < WOR_WINDOW; k++) {
        i = (wor_idx + k) % WOR_WINDOW;
        int32_t gx = wor_ring[i][3], gy = wor_ring[i][4], gz = wor_ring[i][5];
        int32_t ax = wor_ring[i][0], ay = wor_ring[i][1], az = wor_ring[i][2];
        uint32_t gsq = (uint32_t)(gx * gx) + (uint32_t)(gy * gy) + (uint32_t)(gz * gz);
        uint32_t asq = (uint32_t)(ax * ax) + (uint32_t)(ay * ay) + (uint32_t)(az * az);
        if (gsq > gp_sq) gp_sq = gsq;
        if (asq > am_sq) am_sq = asq;
    }
    return (gp_sq >= WOR_PRE_GYRO_SQ) || (am_sq >= WOR_PRE_ACCEL_SQ);
}

/* 3-layer pure-integer CNN (see wor_cnn.c / wor_cnn_model.h).  Wakes the
 * main CPU ONLY on a CNN raise — normal arm movement no longer wakes it
 * (this is the power fix) and gentle raises that a 500 dps rule would miss
 * are still caught (the sensitivity fix).  The host re-runs the SAME CNN on
 * the ring for the final decision + diagnostics. */
static uint32_t wor_cnn_gate(void)
{
    int32_t logit = wor_cnn_score((const int16_t *)(const void *)wor_ring, wor_idx);
    wor_cnn_logit = logit;
    return (uint32_t)(logit >= WOR_CNN_LOGIT_TH);
}

int main(void)
{
    uint8_t  buf[12];
    uint8_t  who[1];
    uint32_t since_gate = 0;

    /* Init RTC GPIOs for the I2C bit-bang (open-drain SDA). */
    ulp_riscv_gpio_init(WOR_I2C_SCL);
    ulp_riscv_gpio_init(WOR_I2C_SDA);
    ulp_riscv_gpio_set_output_mode(WOR_I2C_SCL, RTCIO_MODE_OUTPUT);
    ulp_riscv_gpio_set_output_mode(WOR_I2C_SDA, RTCIO_MODE_OUTPUT_OD);
    ulp_riscv_gpio_output_enable(WOR_I2C_SCL);
    ulp_riscv_gpio_output_enable(WOR_I2C_SDA);
    ulp_riscv_gpio_input_enable(WOR_I2C_SDA);
    wor_scl_hi();
    wor_sda_hi();

    wor_running = 1;
    wor_gate_hit = 0;
    wor_cnn_logit = 0;

    /* Verify the IMU is on the bus (non-fatal). */
    if (wor_i2c_read_regs(QMI8658_ADDR, QMI8658_WHO_AM_I, who, 1) == 0 && who[0] == 0x05) {
        /* QMI8658 present */
    }

    for (;;) {
        /* IMU sampling + raise gate — only when the host enables it (wor_imu_enabled
         * is set by the main CPU after ulp_riscv_run; the ULP picks it up next loop).
         *
         * NOTE: the ULP no longer polls the FT3168 touch controller.  Hardware
         * testing showed the bit-banged FT3168 read fails ~176/180 times and
         * leaves the touch controller wedged after wake, while the FT3168 INT
         * (GPIO38) already provides reliable touch wake.  Keeping the ULP off
         * the touch IC keeps the shared bus pristine so touch survives sleep. */
        if (wor_imu_enabled) {
            if (wor_i2c_read_regs(QMI8658_ADDR, QMI8658_AX_L, buf, 12) == 0) {
                volatile int16_t *r = &wor_ring[wor_idx][0];
                r[0] = (int16_t)((buf[1] << 8) | buf[0]);   /* ax */
                r[1] = (int16_t)((buf[3] << 8) | buf[2]);   /* ay */
                r[2] = (int16_t)((buf[5] << 8) | buf[4]);   /* az */
                r[3] = (int16_t)((buf[7] << 8) | buf[6]);   /* gx */
                r[4] = (int16_t)((buf[9] << 8) | buf[8]);   /* gy */
                r[5] = (int16_t)((buf[11] << 8) | buf[10]); /* gz */
                wor_idx = (wor_idx + 1) % WOR_WINDOW;
                if (wor_idx == 0) wor_full = 1;
                wor_sample_count++;

                if (++since_gate >= WOR_GATE_EVERY) {
                    since_gate = 0;
                    if (wor_full && wor_pre_filter() && wor_cnn_gate()) {
                        wor_gate_hit = 1;
                        ulp_riscv_wakeup_main_processor();
                    }
                }
            }
        }

        /* ~20 ms period -> ~50 Hz */
        ulp_riscv_delay_cycles(WOR_SAMPLE_DELAY_MS * ULP_RISCV_CYCLES_PER_MS);
    }

    /* never reached */
    return 0;
}
