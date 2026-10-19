/**
 * @file wor_cnn.c
 * @brief Pure-integer 3-layer 1D-CNN for the wake-on-raise gate.
 *
 * Architecture (1D over time, 3 layers), identical to the Python reference
 * in tinyml_mpu/espdl_model/train_ulp_cnn.py:
 *
 *   input(75x6 Q15) -> conv1(6->8, k5, SAME) -> ReLU -> maxpool2
 *                   -> conv2(8->16, k5, SAME) -> ReLU -> maxpool2
 *                   -> fc(288->1) -> logit (Q15)
 *
 * Fixed-point scheme (all integer, ULP-safe):
 *   - weights        int8   (per-layer scale folded into the requant M)
 *   - activations    int16  Q15 (value = int16/32768)
 *   - accumulator    int32  (int8*int16 = 24-bit products, safe over <=40 taps)
 *   - requant        out = clamp16( (int64)acc*M >> S  +  b ); then ReLU
 *                    (RV32IMC emits mul/mulh inline for the 64-bit product)
 *
 * Input conversion (matches training):
 *   accel  x15 = clamp16( (raw * WOR_CNN_IN_K_A) >> WOR_CNN_IN_S_A )  [2.4518x]
 *   gyro   x15 = clamp16( raw )                                       [1.0x]
 *
 * The activations buffers are static and REUSED across layers to stay well
 * inside the ULP's RTC memory budget:
 *   wor_in[6*79]  padded input for conv1, then padded pool1 for conv2
 *   wor_a[600]    h1[8][75], then h2[16][37]
 *   wor_b[296]    p1[8][37],  then p2[16][18]
 */

#include "wor_cnn.h"

/* ── Fixed activation buffers (in .bss = RTC slow memory on the ULP) ── */
#define WOR_IN_STRIDE   (WOR_CNN_L + 4)        /* 79  (2 zero taps each side) */
#define WOR_P1_STRIDE   (WOR_CNN_POOLED1 + 4)  /* 41  (2 zero taps each side) */
static int16_t wor_in[WOR_CNN_CIN * WOR_IN_STRIDE];   /* 6*79 = 474 int16 */
static int16_t wor_a[WOR_CNN_C1 * WOR_CNN_L];         /* 8*75 = 600 int16 */
static int16_t wor_b[WOR_CNN_C1 * WOR_CNN_POOLED1];   /* 8*37 = 296 int16 */

static inline int16_t wor_clamp16(int32_t v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* ── raw int16 ring (circular, oldest at idx) -> padded Q15 feature buf ── */
static void wor_cnn_build_input(const int16_t *ring, uint32_t idx)
{
    uint32_t i, c;
    for (c = 0; c < WOR_CNN_CIN; c++) {
        wor_in[c * WOR_IN_STRIDE + 0] = 0;
        wor_in[c * WOR_IN_STRIDE + 1] = 0;
        wor_in[c * WOR_IN_STRIDE + WOR_CNN_L + 2] = 0;
        wor_in[c * WOR_IN_STRIDE + WOR_CNN_L + 3] = 0;
    }
    for (i = 0; i < WOR_CNN_L; i++) {
        const int16_t *s = &ring[((idx + i) % WOR_CNN_L) * 6];
        for (c = 0; c < 3; c++) {   /* accel:  x15 = (raw*K_A)>>S_A */
            int32_t v = ((int32_t)s[c] * WOR_CNN_IN_K_A) >> WOR_CNN_IN_S_A;
            wor_in[c * WOR_IN_STRIDE + i + 2] = wor_clamp16(v);
        }
        for (c = 3; c < 6; c++) {   /* gyro:   x15 = raw */
            wor_in[c * WOR_IN_STRIDE + i + 2] = wor_clamp16((int32_t)s[c]);
        }
    }
}

/* ── conv1: wor_in(6x79) -> wor_a as h1[8][75] ──────────────────────── */
static void wor_cnn_conv1(void)
{
    uint32_t o, t, c;
    for (o = 0; o < WOR_CNN_C1; o++) {
        for (t = 0; t < WOR_CNN_L; t++) {
            int32_t acc = 0;
            for (c = 0; c < WOR_CNN_CIN; c++) {
                const int8_t  *w = &wor_cnn_w1[(o * WOR_CNN_CIN + c) * WOR_CNN_K];
                const int16_t *x = &wor_in[c * WOR_IN_STRIDE + t];
                acc += (int32_t)w[0] * x[0] + (int32_t)w[1] * x[1]
                     + (int32_t)w[2] * x[2] + (int32_t)w[3] * x[3]
                     + (int32_t)w[4] * x[4];
            }
            int32_t v = (int32_t)(((int64_t)acc * WOR_CNN_Q1_M) >> WOR_CNN_Q1_S);
            v += wor_cnn_b1[o];
            v = wor_clamp16(v);
            if (v < 0) v = 0;                       /* ReLU */
            wor_a[o * WOR_CNN_L + t] = (int16_t)v;
        }
    }
}

/* ── pool1: wor_a(h1[8][75]) -> wor_b(p1[8][37]) -> repack into wor_in ── */
static void wor_cnn_pool1(void)
{
    uint32_t o, t;
    for (o = 0; o < WOR_CNN_C1; o++) {
        for (t = 0; t < WOR_CNN_POOLED1; t++) {
            int16_t a = wor_a[o * WOR_CNN_L + 2 * t];
            int16_t b = wor_a[o * WOR_CNN_L + 2 * t + 1];
            wor_b[o * WOR_CNN_POOLED1 + t] = (a > b) ? a : b;
        }
        /* repack row into wor_in with 2 zero taps each side (stride 41) */
        wor_in[o * WOR_P1_STRIDE + 0] = 0;
        wor_in[o * WOR_P1_STRIDE + 1] = 0;
        wor_in[o * WOR_P1_STRIDE + WOR_CNN_POOLED1 + 2] = 0;
        wor_in[o * WOR_P1_STRIDE + WOR_CNN_POOLED1 + 3] = 0;
        for (t = 0; t < WOR_CNN_POOLED1; t++) {
            wor_in[o * WOR_P1_STRIDE + t + 2] = wor_b[o * WOR_CNN_POOLED1 + t];
        }
    }
}

/* ── conv2: wor_in(p1[8][41] padded) -> wor_a as h2[16][37] ──────────── */
static void wor_cnn_conv2(void)
{
    uint32_t o, t, c;
    for (o = 0; o < WOR_CNN_C2; o++) {
        for (t = 0; t < WOR_CNN_POOLED1; t++) {
            int32_t acc = 0;
            for (c = 0; c < WOR_CNN_C1; c++) {
                const int8_t  *w = &wor_cnn_w2[(o * WOR_CNN_C1 + c) * WOR_CNN_K];
                const int16_t *x = &wor_in[c * WOR_P1_STRIDE + t];
                acc += (int32_t)w[0] * x[0] + (int32_t)w[1] * x[1]
                     + (int32_t)w[2] * x[2] + (int32_t)w[3] * x[3]
                     + (int32_t)w[4] * x[4];
            }
            int32_t v = (int32_t)(((int64_t)acc * WOR_CNN_Q2_M) >> WOR_CNN_Q2_S);
            v += wor_cnn_b2[o];
            v = wor_clamp16(v);
            if (v < 0) v = 0;                       /* ReLU */
            wor_a[o * WOR_CNN_POOLED1 + t] = (int16_t)v;
        }
    }
}

/* ── pool2: wor_a(h2[16][37]) -> wor_b as p2[16][18] ─────────────────── */
static void wor_cnn_pool2(void)
{
    uint32_t o, t;
    for (o = 0; o < WOR_CNN_C2; o++) {
        for (t = 0; t < WOR_CNN_POOLED2; t++) {
            int16_t a = wor_a[o * WOR_CNN_POOLED1 + 2 * t];
            int16_t b = wor_a[o * WOR_CNN_POOLED1 + 2 * t + 1];
            wor_b[o * WOR_CNN_POOLED2 + t] = (a > b) ? a : b;
        }
    }
}

/* ── fc: wor_b(p2 flattened, 288) -> logit (Q15) ─────────────────────── */
static int16_t wor_cnn_fc(void)
{
    int32_t acc = 0;
    uint32_t i;
    for (i = 0; i < WOR_CNN_FC_IN; i++) {
        acc += (int32_t)wor_cnn_wfc[i] * wor_b[i];
    }
    int32_t v = (int32_t)(((int64_t)acc * WOR_CNN_QF_M) >> WOR_CNN_QF_S);
    v += wor_cnn_bfc[0];
    return wor_clamp16(v);
}

int32_t wor_cnn_score(const int16_t *ring, uint32_t idx)
{
    wor_cnn_build_input(ring, idx);
    wor_cnn_conv1();
    wor_cnn_pool1();
    wor_cnn_conv2();
    wor_cnn_pool2();
    return (int32_t)wor_cnn_fc();
}
