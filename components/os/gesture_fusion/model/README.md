# Unified gesture model (idle / wrist / double-tap)

Place the trained `gesture.espdl` here to activate the **single 3-state
detector** that replaces the separate wrist-flick + double-tap models
when BOTH features are enabled.

Until this file exists the fused detector compiles but stays inert
(`gesture_fusion_start()` logs "model not embedded"), and the two separate
detectors keep working exactly as before (wrist-flick and motion pointer
now coexist — see `components/os/wrist_flick`).

## What it does

When both **Back Gesture (Wrist Flick)** and **Motion Pointer** are ON,
one 1.5 s IMU window → one inference → one of three states:

| Class | Action |
|---|---|
| idle | nothing |
| wrist | haptic + back (same as the wrist-flick detector) |
| double-tap | haptic + click at the cursor (same as the double-tap detector) |

While it is active the separate detectors pause (they check
`gesture_fusion_is_active()`).

## Train it

1. **Collect / reuse data** in `D:\ESP32-S3-Touch-AMOLED-2.06\ESP-IDF-v5.4.2\tinyml_mpu\espdl_model\data\`:
   - `flick.csv`    — wrist flicks (already segmented 1.5 s windows)
   - `tap.csv`      — double-taps (continuous capture, events extracted)
   - `notflick.csv` — idle / everything-else (shared)
2. **Train** (in the `tinyml_mpu` venv):
   ```powershell
   $env:PYTHONUTF8='1'
   cd D:\ESP32-S3-Touch-AMOLED-2.06\ESP-IDF-v5.4.2\tinyml_mpu
   .\venv\Scripts\Activate.ps1
   python espdl_model\train_gesture.py
   ```
   This trains a 3-class CNN, quantizes it and **automatically copies**
   `gesture.espdl` here. Check `deploy_info.txt` for per-class metrics.

3. **Rebuild the firmware** — CMake re-runs automatically when the model
   appears (CMAKE_CONFIGURE_DEPENDS), embeds it, and the fused detector
   activates.

## Output layout (runtime contract)

Class index 0 = idle, 1 = wrist, 2 = double-tap (softmax probabilities).
The runtime applies the existing physical gates per class (sharp-gyro for
wrist, ≥2 accel impulses + ≤35° tilt for double-tap) on top of the model.
