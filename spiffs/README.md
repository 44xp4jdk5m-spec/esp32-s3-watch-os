# SPIFFS partition source — Linux FHS layout

This folder is the source for the `storage` SPIFFS partition (7 MB, mounted at
`/spiffs`). It follows the Linux Filesystem Hierarchy Standard (FHS):

```
/spiffs
├── etc/                 # system configuration (read-mostly)
│   └── watch_config.json
├── usr/share/           # read-only application data (never written at runtime)
│   ├── dict/words.bin   #   keyboard dictionary (built by build_dict.py)
│   ├── lang/*.json      #   translations (en / jp / tc)
│   ├── watchfaces/      #   pre-decoded face bitmaps, loaded into PSRAM on
│   │   ├── parallax/    #     demand by app_image_loader.c
│   │   ├── flux_digit_*.bin       #   flux digits (A8)
│   │   ├── ui_img_137261120.bin   #   classic face earth (RGB565A8)
│   │   └── ui_img_charge_png.bin  #   charge icon (RGB565A8)
│   ├── activity/        #   activity app icons (I8 / RGB565A8), loaded on
│   │                    #     first app open (activity_pages_load_icons)
│   ├── launcher/        #   (future) honeycomb icon bitmaps
│   └── keyboard/        #   (future) keyboard glyphs
└── var/lib/             # writable runtime state — created at boot by
    └── README.txt       #   app_config_init() (SPIFFS drops empty dirs)
```

Rule of thumb:

- **`/etc`** — configuration the OS reads at boot.
- **`/usr/share`** — read-only assets; anything here is safe to `spiffs_create_partition_image` and never written on-device.
- **`/var`** — runtime-generated state (battery history, screen usage, wake log).
  Never commit real files here; the flash image starts with a placeholder so the
  directory exists, and `app_config_init()` re-creates it at boot as a fallback.

The `var/lib/README.txt` placeholder also guarantees the directory survives in
the generated partition image.
