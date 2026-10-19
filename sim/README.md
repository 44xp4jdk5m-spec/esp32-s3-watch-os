# Watch Simulator (desktop)

Run the watch OS on your computer — no ESP32, no AMOLED panel, no sensors.

The simulator compiles a curated subset of the **real firmware sources**
(`components/os/watch_faces`, the SquareLine UI, the honeycomb launcher and
the generated fonts/icons) against a thin ESP-IDF compatibility shim, then
renders LVGL into an SDL2 window at the panel's native 410 × 502 resolution.

Hardware values are synthesised, because there is nothing to read:

| Signal | Simulated value |
|---|---|
| Battery | starts at 87 %, discharges ~1 %/3 min |
| Charger | never plugged in (toggle in `sim_platform.c`) |
| Chip / battery temperature | 31.5 °C / 29.0 °C, slowly drifting |
| RTC | host wall clock, or a fixed 10:08:30 with `--demo-time` |
| IMU / tilt | right-drag the mouse |
| Vibration | no-op |
| Audio | not simulated |
| WiFi / BLE / OTA / QuickJS / ESP-DL | not simulated |

## What runs

- Boot splash with the radial progress ring
- Watch faces: Classic, Minimal, Flux, Parallax (placeholder), Hands, California
- Watch-face picker — hold the left mouse button on the face for 450 ms
- Honeycomb launcher (pre-scaled icons)
- **Calculator** — tap its launcher tile, or start with `--calculator`
- **Activity** — tap its launcher tile, or start with `--activity`
- Touch input (mouse) and the two hardware buttons (keys `1` / `2`)

Not simulated (they need hardware services or offline-generated models):

- Parallax face — shows a placeholder; build the depth layers with
  `tools/parallax_face/build_parallax_face.py` to run it on the device
- Settings, Photo and Noise apps (next on the list)
- QuickJS apps, BLE/ANCS, WiFi, OTA, USB MSC

## Prerequisites

- CMake ≥ 3.16 and a C/C++17 compiler
- SDL2 development headers
- Network access the first time you configure (LVGL 9.5 is fetched by git)
- Optional: Python 3 + Pillow to convert the BMP screenshots

Debian / Raspberry Pi OS:

```bash
sudo apt install build-essential cmake pkg-config libsdl2-dev
```

Fedora:

```bash
sudo dnf install gcc gcc-c++ cmake pkgconf-pkg-config SDL2-devel
```

macOS (Homebrew):

```bash
brew install cmake sdl2
```

## Build

From the repository root:

```bash
cmake -S sim -B build-sim -DCMAKE_BUILD_TYPE=Release
cmake --build build-sim -j
./build-sim/watch_sim
```

The first configure clones LVGL into `build-sim/_deps/`; later builds are
incremental. If you already have a checkout of LVGL you can point CMake at
it with `-DFETCHCONTENT_SOURCE_DIR_LVGL=/path/to/lvgl`.

## Running

```bash
./build-sim/watch_sim              # native size (410 × 502)
./build-sim/watch_sim --scale 2    # 2× window (820 × 1004)
./build-sim/watch_sim --scale 3
```

| Key / input | Action |
|---|---|
| `1` (hold) | GPIO0 / BOOT button — click: face ↔ launcher; long press (1 s): no-op |
| `2` (hold) | GPIO10 / PWR button — click/long press (2 s): control centre / shutdown (not simulated, logged only) |
| Left click / drag | touch |
| Left press 450 ms | open the watch-face picker (same gesture as the device) |
| Right drag | tilt the watch (IMU) |
| `Esc` | quit |

Faces are chosen exactly like on the watch: long-press the face, swipe the
carousel, tap the centred card. There are no face hotkeys.

## Headless screenshots

Useful for documentation and for checking a change without a display:

```bash
# render the Minimal face and write a BMP, then exit
SDL_VIDEODRIVER=dummy ./build-sim/watch_sim \
    --demo-time --face 1 --shot minimal.bmp

# the honeycomb launcher
SDL_VIDEODRIVER=dummy ./build-sim/watch_sim \
    --demo-time --launcher --shot launcher.bmp --shot-after 4000
```

`--demo-time` freezes the clock at 10:08:30 so repeated runs are identical
(the Flux face uses the seconds to sweep its panels). `--shot-after` is the
delay in milliseconds from process start.

Convert to PNG:

```bash
python3 -c "from PIL import Image; Image.open('minimal.bmp').save('minimal.png')"
```

## Layout

```
sim/
├─ CMakeLists.txt     # LVGL fetch, source list, include paths
├─ lv_conf.h          # LVGL config mirroring the firmware
├─ shim/              # esp_log.h, esp_err.h, esp_timer.h, freertos/*, …
└─ src/
   ├─ main.c          # boot flow, hotkeys, capture
   ├─ sim_display.c   # SDL window + RGB565 framebuffer
   ├─ sim_input.c     # mouse → touch, keyboard → keypad
   ├─ sim_platform.c  # logging, timing, FreeRTOS stubs, fake sensors
   └─ sim_mocks.c     # g_config, hw_* and app_* stubs, image loader
```

## How the shim works

The firmware sources are compiled **unmodified**. Anything they expect from
ESP-IDF is provided by headers under `sim/shim/` (compiled as ordinary x86
or ARM code) and by definitions in `sim_platform.c` / `sim_mocks.c`. The
simulator never modifies `components/`.

Two details are worth knowing if you extend it:

- `png_images_circle.c` and `png_images_rgb565.c` export the same symbol
  names. On device they live in a static archive so the linker pulls only
  one; the simulator reproduces this by building each into its own static
  library.
- The pre-decoded `.bin` assets are read straight from `spiffs/` on disk,
  so the simulator always matches the SPIFFS image the device would flash.

## Adding more of the UI

1. Add the source file to `SIM_SOURCES` in `sim/CMakeLists.txt`.
2. Add its directory to `target_include_directories`.
3. Build, then implement whatever new `esp_*` / `hw_*` / `app_*` symbols
   the linker reports in `sim_mocks.c`.

Because the shim surface is small, most new screens need only a handful of
stub functions.
