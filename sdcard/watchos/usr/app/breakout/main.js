/*
 * Breakout — Watch OS JavaScript app (runs from SD card via QuickJS).
 *
 * A classic Breakout clone.  The paddle is steered by TILTING the watch:
 * the QMI8658 accelerometer angle is read through OS.Sensor.tilt() and
 * turned into paddle velocity (dead-zone + smoothing).  Touch is also
 * supported — drag anywhere to move the paddle, and tap to serve.
 *
 * Requires the JS bindings added to the firmware (2026-09):
 *   OS.Sensor.tilt() / OS.Sensor.read()   QMI8658 IMU
 *   OS.LVGL.timer(ms, cb)                 periodic game loop
 *   OS.LVGL.pointer()                     current touch {x,y,down}
 *
 * Global namespaces provided by the OS sandbox:
 *   OS.LVGL    — LVGL object/anim/event/timer/pointer bindings
 *   OS.Sensor  — QMI8658 IMU (tilt in degrees, raw accel/gyro)
 *   OS.System  — time / exit
 *   OS.Storage — persistent integer key/value store (NVS-backed)
 */
(function () {
    "use strict";

    var LVGL = OS.LVGL;
    var SYS  = OS.System;
    var STORE = OS.Storage;
    var SENS = (typeof OS.Sensor !== "undefined") ? OS.Sensor : null;

    /* ══ Calibration ────────────────────────────────────────────────
     * TILT_SIGN: flip to 1 if the paddle runs the WRONG way when you
     * tilt the watch.  (This board's air-pointer maps positive "roll"
     * to cursor-left, so the paddle uses the same -1 sign.)
     * DEAD_DEG  : degrees of tilt that do nothing (rest zone).
     * MAX_DEG   : tilt angle at which the paddle reaches full speed. */
    var TILT_SIGN = -1;
    var DEAD_DEG  = 9;
    var MAX_DEG   = 38;

    /* ══ Geometry — 410x502 display with ~100px corner radius ══════
     * A full rectangle must clear the corner arcs: the safe inset is
     * R/sqrt(2) ≈ 71px, so the whole playfield lives inside that band
     * (nothing gets clipped by the physical glass corners). */
    var SW = 410, SH = 502;
    var M = 71;
    var FX0 = M;                  /* field left  = 71   */
    var FX1 = SW - M;             /* field right = 339  */
    var FW  = FX1 - FX0;          /* field width = 268  */

    var PADDLE_W = 90, PADDLE_H = 14;
    var PADDLE_Y = 390;
    var BALL_R   = 6;
    var CEIL_Y   = 112;           /* ball-centre ceiling (above the bricks) */
    var LOST_Y   = PADDLE_Y + PADDLE_H + 16;   /* 420 — clearly missed */

    var NCOL = 8, NROW = 6;
    var BRICK_GAP = 6;
    var BRICK_W   = Math.floor((FW - (NCOL - 1) * BRICK_GAP) / NCOL); /* 28 */
    var BRICK_H   = 15;
    var BRICK_ROW_GAP = 5;
    var BRICK_TOP = 150;
    var BRICK_X0  = FX0 + Math.floor((FW - (NCOL * BRICK_W + (NCOL - 1) * BRICK_GAP)) / 2);

    function brickX(c) { return BRICK_X0 + c * (BRICK_W + BRICK_GAP); }
    function brickY(r) { return BRICK_TOP + r * (BRICK_H + BRICK_ROW_GAP); }

    /* ══ Colours ─────────────────────────────────────────────────── */
    var C_BG    = 0x0A0E1A;   /* deep navy background                  */
    var C_HUD   = 0xD7DBDD;
    var C_HINT  = 0x7F8C8D;
    var C_PAD   = 0x8FD3F4;   /* paddle                                 */
    var C_BALL  = 0xF6F1D5;   /* ball (soft cream)                      */
    var C_BEST  = 0xF1C40F;
    var ROW_COLORS = [ 0xE74C3C, 0xE67E22, 0xF1C40F, 0x2ECC71, 0x3498DB, 0x9B59B6 ];

    /* ══ Ball / paddle speed (px per ~16 ms frame) ───────────────── */
    var FRAME_MS = 16;
    var BALL_BASE = 3.4;
    var BALL_PER_LVL = 0.28;
    var MAX_PAD_V = 11;

    /* ══ State ───────────────────────────────────────────────────── */
    var state;              /* "wait" | "play" | "clear" | "over"      */
    var level = 1, lives = 3, score = 0, best = 0;
    var alive = 0;
    var bx = 0, by = 0, vx = 0, vy = 0;
    var paddleX = 0, paddleV = 0;
    var serveFrames = 0;   /* frames of continuous tilting that auto-serve */

    var screen;
    var paddle, ball;
    var bricks = [];        /* bricks[r][c] = LVGL obj | null */
    var scoreLbl, livesLbl, hintLbl, bestLbl;
    var overlay = null;     /* active modal (game-over / level-clear)  */
    var hintLast = "";      /* last hint text we actually wrote          */
    var HINT_WAIT = "TAP TO LAUNCH  -  TILT OR DRAG TO AIM";

    function ballSpeed() {
        return Math.min(BALL_BASE + (level - 1) * BALL_PER_LVL, 6.5);
    }

    /* ══ Small helpers ───────────────────────────────────────────── */
    function box(parent, x, y, w, h, color, radius) {
        var o = LVGL.create(parent);
        o.setSize(w, h);
        o.setPos(Math.round(x), Math.round(y));
        o.setRadius(radius);
        o.setBgColor(color);
        o.setBgOpa(LVGL.OPA.COVER);
        o.setBorderWidth(0);
        o.clearFlag(LVGL.FLAG.SCROLLABLE);
        return o;
    }

    /* ══ HUD ─────────────────────────────────────────────────────── */
    function fmt(n) { return String(n); }
    function pad(n) {
        var s = String(n);
        while (s.length < 4) s = "0" + s;
        return s;
    }

    function updateHud() {
        scoreLbl.setText("SCORE " + pad(score));
        livesLbl.setText("LIVES " + fmt(lives));
        bestLbl.setText("BEST " + pad(Math.max(best, score)));
    }

    /* ══ Brick wall ──────────────────────────────────────────────── */
    function clearWall() {
        for (var r = 0; r < NROW; r++)
            for (var c = 0; c < NCOL; c++) {
                if (bricks[r] && bricks[r][c]) {
                    bricks[r][c].del();
                    bricks[r][c] = null;
                }
            }
    }

    function buildWall() {
        clearWall();
        bricks = [];
        alive = NCOL * NROW;
        for (var r = 0; r < NROW; r++) {
            bricks[r] = [];
            for (var c = 0; c < NCOL; c++) {
                var o = box(screen, brickX(c), brickY(r),
                            BRICK_W, BRICK_H, ROW_COLORS[r], 3);
                bricks[r][c] = o;
            }
        }
    }

    /* Read the watch tilt once per frame (QMI8658 → roll in degrees). */
    function readTilt() {
        if (SENS && SENS.tilt) {
            var t = SENS.tilt();
            if (t) return t.roll;
        }
        return 0;
    }

    /* ══ Paddle ──────────────────────────────────────────────────── */
    function updatePaddle(p, roll) {
        if (p && p.down) {
            /* Finger down: chase the finger (touch control). */
            paddleV = 0;
            var target = p.x - PADDLE_W / 2;
            paddleX += (target - paddleX) * 0.45;
        } else {
            /* No finger: steer with watch tilt (roll angle). */
            var dir = 0;
            if (roll > DEAD_DEG)       dir = 1;
            else if (roll < -DEAD_DEG) dir = -1;
            var mag = Math.min(Math.abs(roll), MAX_DEG);
            var f = (mag - DEAD_DEG) / (MAX_DEG - DEAD_DEG);
            if (f < 0) f = 0;
            var targetV = TILT_SIGN * dir * f * MAX_PAD_V;
            paddleV += (targetV - paddleV) * 0.22;   /* exponential smoothing */
            paddleX += paddleV;
        }

        if (paddleX < FX0) paddleX = FX0;
        if (paddleX > FX1 - PADDLE_W) paddleX = FX1 - PADDLE_W;
    }

    /* ══ Ball helpers ────────────────────────────────────────────── */
    function placeBallOnPaddle() {
        bx = paddleX + PADDLE_W / 2;
        by = PADDLE_Y - BALL_R - 1;
        vx = 0; vy = 0;
    }

    function launch() {
        state = "play";
        var spd = ballSpeed();
        /* aim roughly straight up with a little random spread */
        var a = (Math.random() * 0.7 - 0.35);          /* ±20° */
        vx = Math.sin(a) * spd;
        vy = -Math.cos(a) * spd;
        if (Math.random() < 0.5) vx = -vx;
        if (vy > -1.2) vy = -1.2;
    }

    function bounceWalls() {
        if (bx - BALL_R < FX0) { bx = FX0 + BALL_R; vx = Math.abs(vx); }
        if (bx + BALL_R > FX1) { bx = FX1 - BALL_R; vx = -Math.abs(vx); }
        if (by - BALL_R < CEIL_Y) { by = CEIL_Y + BALL_R; vy = Math.abs(vy); }
    }

    function hitBrick(r, c) {
        var o = bricks[r][c];
        if (o) { o.del(); }
        bricks[r][c] = null;
        alive--;

        score += (NROW - r) * 10;
        updateHud();

        var x0 = brickX(c), x1 = x0 + BRICK_W;
        var y0 = brickY(r), y1 = y0 + BRICK_H;
        var nx = Math.max(x0 - bx, 0, bx - x1);
        var ny = Math.max(y0 - by, 0, by - y1);

        if (nx > ny) {
            if (bx < (x0 + x1) / 2) { bx = x0 - BALL_R - 1; vx = -Math.abs(vx); }
            else                    { bx = x1 + BALL_R + 1; vx =  Math.abs(vx); }
        } else if (ny > nx) {
            if (by < (y0 + y1) / 2) { by = y0 - BALL_R - 1; vy = -Math.abs(vy); }
            else                    { by = y1 + BALL_R + 1; vy =  Math.abs(vy); }
        } else {
            /* deep/corner hit: push out along the dominant velocity */
            if (Math.abs(vx) >= Math.abs(vy)) {
                bx = (vx > 0) ? x1 + BALL_R + 1 : x0 - BALL_R - 1;
                vx = -vx;
            } else {
                by = (vy > 0) ? y1 + BALL_R + 1 : y0 - BALL_R - 1;
                vy = -vy;
            }
        }
    }

    function collideBricks() {
        for (var r = 0; r < NROW && state === "play"; r++) {
            for (var c = 0; c < NCOL; c++) {
                if (!bricks[r][c]) continue;
                var x0 = brickX(c), y0 = brickY(r);
                var nx = Math.max(x0 - bx, 0, bx - (x0 + BRICK_W));
                var ny = Math.max(y0 - by, 0, by - (y0 + BRICK_H));
                if (nx * nx + ny * ny <= BALL_R * BALL_R) {
                    hitBrick(r, c);
                    if (alive === 0) { levelClear(); }
                    return;   /* one brick per sub-step is plenty */
                }
            }
        }
    }

    function collidePaddle() {
        if (vy <= 0) return;   /* only when the ball is travelling DOWN */
        var pTop = PADDLE_Y, pBot = PADDLE_Y + PADDLE_H;
        if (by + BALL_R >= pTop && by - BALL_R <= pBot) {
            if (bx >= paddleX - BALL_R && bx <= paddleX + PADDLE_W + BALL_R) {
                /* bounce the ball up; the hit offset steers the angle */
                by = pTop - BALL_R - 1;
                var spd = ballSpeed();
                var off = (bx - (paddleX + PADDLE_W / 2)) / (PADDLE_W / 2);
                if (off > 1) off = 1; else if (off < -1) off = -1;
                vx = off * Math.min(spd * 1.15, 5.5);
                var rem = Math.sqrt(Math.max(spd * spd - vx * vx, 0.6));
                vy = -rem;
                if (Math.abs(vx) < 0.8) vx = 0.8 * (bx >= paddleX + PADDLE_W / 2 ? 1 : -1);
            }
        }
    }

    function stepBall() {
        /* 2 sub-steps keep the fast ball from tunnelling through bricks */
        for (var s = 0; s < 2 && state === "play"; s++) {
            bx += vx * 0.5;
            by += vy * 0.5;
            bounceWalls();
            collideBricks();
            collidePaddle();
        }
        if (state === "play" && by > LOST_Y) loseLife();
    }

    function loseLife() {
        lives--;
        updateHud();
        if (lives <= 0) { showOver(); return; }
        state = "wait";
        placeBallOnPaddle();
    }

    /* ══ Level / game flow ───────────────────────────────────────── */
    function levelClear() {
        state = "clear";
        showOverlay("LEVEL " + fmt(level) + " CLEAR!",
                    "SCORE " + pad(score),
                    "TAP TO CONTINUE");
    }

    function nextLevel() {
        hideOverlay();
        level++;
        buildWall();
        state = "wait";
        placeBallOnPaddle();
    }

    function resetGame() {
        hideOverlay();
        level = 1; lives = 3; score = 0;
        buildWall();
        updateHud();
        state = "wait";
        placeBallOnPaddle();
    }

    function showOver() {
        state = "over";
        if (score > best) {
            best = score;
            STORE.setInt("breakout_best", best);
        }
        showOverlay("GAME OVER", "SCORE " + pad(score) +
                    (score >= best && score > 0 ? "  NEW BEST!" : ""),
                    "TAP TO RESTART");
    }

    function showOverlay(title, sub, cta) {
        hideOverlay();
        overlay = LVGL.create(screen);
        overlay.setSize(SW, SH);
        overlay.setPos(0, 0);
        overlay.setRadius(0);
        overlay.setBgColor(0x000000);
        overlay.setBgOpa(205);
        overlay.addFlag(LVGL.FLAG.CLICKABLE);
        overlay.clearFlag(LVGL.FLAG.SCROLLABLE);

        var t = LVGL.label(overlay, title);
        t.setTextColor(0xFFFFFF);
        t.setFont(LVGL.FONT.LARGE);
        t.setTextAlign(LVGL.TEXT_ALIGN.CENTER);
        t.center();
        t.setPos(t.getX(), t.getY() - 90);

        var s = LVGL.label(overlay, sub);
        s.setTextColor(C_HUD);
        s.setFont(LVGL.FONT.MED_SMALL);
        s.setTextAlign(LVGL.TEXT_ALIGN.CENTER);
        s.center();
        s.setPos(s.getX(), s.getY() - 26);

        var btn = LVGL.create(overlay);
        btn.setSize(190, 46);
        btn.setRadius(23);
        btn.setBgColor(C_PAD);
        btn.setBgOpa(LVGL.OPA.COVER);
        btn.addFlag(LVGL.FLAG.CLICKABLE);
        btn.clearFlag(LVGL.FLAG.SCROLLABLE);
        btn.center();
        btn.setPos(btn.getX(), btn.getY() + 60);

        var b = LVGL.label(btn, cta);
        b.setTextColor(0x0A0E1A);
        b.setFont(LVGL.FONT.MED_SMALL);
        b.setTextAlign(LVGL.TEXT_ALIGN.CENTER);
        b.center();

        btn.on("clicked", function () {
            if (state === "over") resetGame();
            else if (state === "clear") nextLevel();
        });
        /* tapping anywhere on the dimmed screen also continues */
        overlay.on("clicked", function () {
            if (state === "over") resetGame();
            else if (state === "clear") nextLevel();
        });
    }

    function hideOverlay() {
        if (overlay) { overlay.del(); overlay = null; }
    }

    /* ══ Main loop (one LVGL.timer tick per frame) ───────────────── */
    function step() {
        var p = LVGL.pointer();
        var roll = readTilt();

        if (state === "wait" || state === "play") {
            updatePaddle(p, roll);
            if (state === "wait") {
                placeBallOnPaddle();
                /* Tap serves (screen "clicked" event).  Holding a steady
                 * tilt also serves after ~1s, without ever touching. */
                if (p && p.down) {
                    serveFrames = 0;
                } else if (Math.abs(roll) > DEAD_DEG) {
                    serveFrames++;
                    if (serveFrames > 60) launch();
                } else {
                    serveFrames = 0;
                }
            } else {
                stepBall();
            }
            var hint = (state === "wait") ? HINT_WAIT : "";
            if (hint !== hintLast) { hintLast = hint; hintLbl.setText(hint); }
        }
        render();
    }

    function render() {
        paddle.setPos(Math.round(paddleX), PADDLE_Y);
        ball.setPos(Math.round(bx) - BALL_R, Math.round(by) - BALL_R);
    }

    /* ══ UI construction ─────────────────────────────────────────── */
    function build() {
        screen = LVGL.screen();
        screen.setBgColor(C_BG);
        screen.setBgOpa(LVGL.OPA.COVER);
        screen.addFlag(LVGL.FLAG.CLICKABLE);
        screen.clearFlag(LVGL.FLAG.SCROLLABLE);
        screen.on("clicked", function () {
            if (state === "wait") launch();   /* tap to serve */
        });

        /* HUD (inside the rounded-corner safe band) */
        var title = LVGL.label(screen, "BREAKOUT");
        title.setTextColor(C_HINT);
        title.setFont(LVGL.FONT.SMALL);
        title.setTextAlign(LVGL.TEXT_ALIGN.CENTER);
        title.alignTo(screen, LVGL.ALIGN.TOP_MID, 0, 70);

        scoreLbl = LVGL.label(screen, "SCORE 0000");
        scoreLbl.setTextColor(C_HUD);
        scoreLbl.setFont(LVGL.FONT.SMALL);
        scoreLbl.setPos(M, 100);

        bestLbl = LVGL.label(screen, "BEST 0000");
        bestLbl.setTextColor(C_BEST);
        bestLbl.setFont(LVGL.FONT.SMALL);
        bestLbl.setPos(M + 104, 100);

        livesLbl = LVGL.label(screen, "LIVES 3");
        livesLbl.setTextColor(C_HUD);
        livesLbl.setFont(LVGL.FONT.SMALL);
        livesLbl.alignTo(screen, LVGL.ALIGN.TOP_RIGHT, -M, 100);

        hintLbl = LVGL.label(screen, "");
        hintLbl.setTextColor(C_HINT);
        hintLbl.setFont(LVGL.FONT.SMALL);
        hintLbl.setTextAlign(LVGL.TEXT_ALIGN.CENTER);
        hintLbl.alignTo(screen, LVGL.ALIGN.TOP_MID, 0, 120);

        /* playfield */
        paddle = box(screen, 0, PADDLE_Y, PADDLE_W, PADDLE_H, C_PAD, 7);
        ball = box(screen, 0, 0, BALL_R * 2, BALL_R * 2, C_BALL, BALL_R);

        buildWall();

        paddleX = FX0 + (FW - PADDLE_W) / 2;   /* centred in the field */
        state = "wait";
        placeBallOnPaddle();
        updateHud();
        render();
        hintLbl.setText(HINT_WAIT);
        hintLast = HINT_WAIT;
    }

    /* ══ Start ───────────────────────────────────────────────────── */
    best = STORE.getInt("breakout_best") || 0;
    build();
    LVGL.timer(FRAME_MS, step);
})();
