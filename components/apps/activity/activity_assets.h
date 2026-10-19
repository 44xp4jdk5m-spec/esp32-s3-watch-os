/**
 * @file activity_assets.h
 * @brief Image assets for the Activity app (ported from my_ui / SquareLine).
 *
 * The icon bitmaps are no longer compiled into the firmware.  They now
 * ship as raw .bin under /spiffs/usr/share/activity/ and are loaded into
 * PSRAM by activity_pages_load_icons() via app_image_loader.c.  The old
 * generated sources (ui_img_*.c in this folder) are kept for regeneration
 * via tools/export_image_bins.py.
 */

#pragma once
