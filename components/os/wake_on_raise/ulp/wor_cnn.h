/**
 * @file wor_cnn.h
 * @brief Pure-integer 3-layer 1D-CNN for the wake-on-raise gate.
 *
 * Compiled for BOTH the ULP-RISC-V (17.5 MHz, no FPU, ~8 kB RTC memory)
 * and the main CPU.  The math is all integer: int8 weights, int16 Q15
 * activations, int64 accumulator, requant `(int64)acc*M>>S + b15`, so the
 * ULP and the host compute byte-identical logits from the same ring.
 *
 * The weights/constants are in wor_cnn_model.h, generated + validated by
 * tinyml_mpu/espdl_model/train_ulp_cnn.py (100% fixed-point test accuracy,
 * 0 false positives on the recorded data).
 */

#ifndef WOR_CNN_H
#define WOR_CNN_H

#include <stdint.h>
#include "wor_cnn_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Classify one 1.5 s window held in a circular ring buffer.
 *
 * @param ring  pointer to the ring, layout [WOR_CNN_L][6] raw int16 counts
 *              (AX..AZ, GX..GZ).  May be `volatile`; the caller guarantees
 *              it is stable for the duration of the call (ULP halted).
 * @param idx   index of the OLDEST sample (next slot to be written).
 *              Slot `(idx+k) % WOR_CNN_L` is `k` samples old.
 *
 * @return the network logit as a Q15 integer (range ~[-32768, 32767]).
 *         Raise  if  logit >= WOR_CNN_LOGIT_TH  (prob >= ~0.5).
 */
int32_t wor_cnn_score(const int16_t *ring, uint32_t idx);

#ifdef __cplusplus
}
#endif

#endif /* WOR_CNN_H */
