# Wrist-Flick model

Place the trained `flick.espdl` here to activate the wrist-flick "back"
gesture.  Until this file exists, the feature compiles but stays inert
(`wrist_flick_start()` logs "model not embedded" and the toggle in
Settings → Experimental has no effect).

## Train it

1. **Collect data** with Settings → General → Experimental → Gesture Recorder.
   Pick a **Mode** first:
   - **Flick** (default): record once, do a wrist flick, then tap **Mark flick**
     after *each* flick.  Repeat for all your flicks in one session, stop,
     save → `flick.csv` (contains ONLY the marked 1.5 s flick windows, no
     idle gaps).  One file holds all your flicks — no need for many files.
   - **NotFlick**: just start recording and use the watch normally / leave it
     idle (typing, scrolling, raising your wrist, walking, turning).  No
     marking — save → `notflick.csv` (the whole capture).
   - Copy the CSV files from the SD card (`/sdcard/gestures/`) to
     `D:\ESP32-S3-Touch-AMOLED-2.06\ESP-IDF-v5.4.2\tinyml_mpu\espdl_model\data\`
     as `flick.csv` and `notflick.csv`.  This folder lives NEXT to the
     training script on purpose — the `notflick.csv` (idle) data is reused
     when you train a new gesture later.  (Record as much notflick data as
     flick data — ideally more.)

2. **Train** (in the `tinyml_mpu` venv):
   ```powershell
   $env:PYTHONUTF8='1'
   cd D:\ESP32-S3-Touch-AMOLED-2.06\ESP-IDF-v5.4.2\tinyml_mpu
   .\venv\Scripts\Activate.ps1
   python espdl_model\train_flick.py
   ```
   This trains, quantizes and **automatically copies** `flick.espdl` here.
   Check `deploy_info.txt` for accuracy.  Current model: 90 flicks + ~20k
   not-flick samples, quantised acc/prec/rec = 1.0 (0 false positives on the
   clean test).

3. **Rebuild the firmware**:
   ```powershell
   cd D:\ESP32-S3-Touch-AMOLED-2.06\ESP-IDF-v5.4.2\watch
   idf.py build
   ```
   The build auto-detects `flick.espdl` (top-level `CMAKE_CONFIGURE_DEPENDS`)
   and embeds it — training writes a fresh file, so the next build embeds it
   automatically.  (If you ever restore an *old* copy of the file whose
   timestamp is not newer than the build, run `idf.py reconfigure` first.)

## Tuning the detector

- **Confidence** is now a runtime slider: Settings → Accessibility & Gesture →
  **Flick Confidence** (20–99 %, default 96).  The current model outputs are
  very clean — flicks saturate at ~0.99, not-flick is <0.06 — so 96% is strict
  yet catches ~all real flicks; lower it to catch weaker flicks, raise it for
  even fewer false triggers.  Persisted, no rebuild needed.
- `wrist_flick.cpp` physical gate (keeps non-flick motion from reaching the
  model): `FLICK_GYRO_GATE_NORM` (peak gyro, 450 deg/s) and
  `FLICK_SHARP_GATE_NORM` (peak gyro rise, 160 deg/s).
- `FLICK_CONSECUTIVE` — consecutive positive windows (debounce).
- `FLICK_COOLDOWN_MS` — how long after a trigger to ignore further flicks.

Re-tuning the gates after re-recording data: `watch/tmp/analyze_flick_gyro.py`
and `analyze_flick_deriv.py` report the flick vs not-flick separation.

The normalisation constants (`FLICK_ACCEL_DIV=32`, `FLICK_GYRO_DIV=512`)
MUST match the training pipeline — do not change them independently.
