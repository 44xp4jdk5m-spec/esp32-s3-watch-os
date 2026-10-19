/**
 * @file screen_metrics.h
 * @brief Single source of truth for the display resolution.
 *
 * Every system widget, app layout and theme constant derives its pixel
 * sizes from EOS_DISPLAY_W / EOS_DISPLAY_H here.  All dimension math uses
 * integer constant expressions, so the compiler folds every value into a
 * literal at build time — there is NO runtime division or scaling, which
 * keeps the render path as fast as a hand-tuned fixed-resolution build.
 *
 * To port to a different panel, change only EOS_DISPLAY_W / EOS_DISPLAY_H.
 * Values designed against the reference 410x502 panel are re-derived
 * automatically by EOS_WX()/EOS_WY() (X / Y axis) and EOS_PCT_W()/EOS_PCT_H()
 * (percentage of the screen) — the whole UI stays proportional.
 */

#pragma once

/* Reference panel this firmware was designed against. */
#define EOS_REF_W   410
#define EOS_REF_H   502

/* ── Target panel resolution (override at build time with -D if needed) ── */
#ifndef EOS_DISPLAY_W
#define EOS_DISPLAY_W   410
#endif
#ifndef EOS_DISPLAY_H
#define EOS_DISPLAY_H   502
#endif

/* Scale a horizontal value from the 410-wide reference design. */
#define EOS_WX(v)   (((v) * (EOS_DISPLAY_W)) / EOS_REF_W)
/* Scale a vertical value from the 502-tall reference design. */
#define EOS_WY(v)   (((v) * (EOS_DISPLAY_H)) / EOS_REF_H)
/* Pixel size = p% of the full screen width (constant-folded). */
#define EOS_PCT_W(p)    (((EOS_DISPLAY_W) * (p)) / 100)
/* Pixel size = p% of the full screen height (constant-folded). */
#define EOS_PCT_H(p)    (((EOS_DISPLAY_H) * (p)) / 100)
