/*
 * 2048 — Watch OS JavaScript app (runs from SD card via QuickJS).
 *
 * A faithful clone of the original 2048 game (ratio, margins, colours,
 * slide + merge animations, game-over check and persistent high score).
 *
 * Global namespaces provided by the OS sandbox:
 *   OS.LVGL    — thin LVGL object/anim/event bindings
 *   OS.System  — time / exit
 *   OS.Storage — persistent integer key/value store (NVS-backed)
 */
(function () {
    "use strict";

    var LVGL   = OS.LVGL;
    var SYS    = OS.System;
    var STORE  = OS.Storage;

    /* ── Board geometry — copies the original 2048 ratio/margins ────
     * The display is 410x502 with ~100px rounded corners.  The safe
     * inset that keeps a full rectangle off the corner arc for a corner
     * radius R is R/sqrt(2) ≈ 71px, so every element is placed inside
     * that band (nothing gets clipped by the physical glass corners). */
    var SW = 410, SH = 502;
    var CORNER_R = 100;                    /* display corner radius       */
    var MARGIN = Math.ceil(CORNER_R / Math.SQRT2);   /* 71 */
    var GAP = 10;
    var BOARD = SW - MARGIN * 2;               /* 268 */
    /* 5 gaps: 2 outer padding + 3 inner → cells sit symmetrically inside */
    var TILE  = Math.floor((BOARD - GAP * 5) / 4);  /* 54 */
    var BOARD_X = MARGIN;
    var HEADER_H = 56;
    var BOARD_Y = MARGIN + HEADER_H + 6;       /* 133 */

    /* ── Original 2048 colour palette ─────────────────────────────── */
    var C_BOARD  = 0xBBADA0;
    var C_CELL   = 0xCDC1B4;
    var C_HEADER = 0x776E65;
    var C_WHITE  = 0xF9F6F2;
    var C_DARK   = 0x776E65;

    var TILE_COLORS = {
        2:     0xEEE4DA, 4:     0xEDE0C8, 8:     0xF2B179,
        16:    0xF59563, 32:    0xF67C5F, 64:    0xF65E3B,
        128:   0xEDCF72, 256:   0xEDCC61, 512:   0xEDC850,
        1024:  0xEDC53F, 2048:  0xEDC22E, 4096:  0x3C3A32,
        8192:  0x3C3A32, 16384: 0x3C3A32
    };

    function tileBg(v) {
        return TILE_COLORS[v] || 0x3C3A32;
    }
    function tileFg(v) {
        return (v <= 4) ? C_DARK : C_WHITE;
    }
    function tileFont(v) {
        if (v >= 10000) return LVGL.FONT.SMALL;      /* 5 digits */
        if (v >= 100)   return LVGL.FONT.MED_SMALL;  /* 3-4 digits */
        return LVGL.FONT.MEDIUM;                     /* 1-2 digits */
    }

    /* ── State ────────────────────────────────────────────────────── */
    var SIZE = 4;
    var tiles = [];          /* tiles[r][c] = {r,c,value,obj,lbl} | null */
    var score = 0;
    var best = 0;
    var moving = false;
    var over = false;

    var screen, gameover, goBtn, scoreVal, bestVal;

    function cellX(c) { return BOARD_X + GAP + c * (TILE + GAP); }
    function cellY(r) { return BOARD_Y + GAP + r * (TILE + GAP); }

    /* ── Tile creation ────────────────────────────────────────────── */
    function makeTile(r, c, value) {
        var obj = LVGL.create(screen);
        obj.setSize(TILE, TILE);
        obj.setPos(cellX(c), cellY(r));
        obj.setRadius(8);
        obj.setBgColor(tileBg(value));
        obj.setBgOpa(LVGL.OPA.COVER);
        obj.centerPivot();

        var lbl = LVGL.label(obj, String(value));
        lbl.setTextColor(tileFg(value));
        lbl.setFont(tileFont(value));
        lbl.setTextAlign(LVGL.TEXT_ALIGN.CENTER);
        lbl.center();

        return { r: r, c: c, value: value, obj: obj, lbl: lbl };
    }

    function relabel(t) {
        t.lbl.setText(String(t.value));
        t.lbl.setTextColor(tileFg(t.value));
        t.lbl.setFont(tileFont(t.value));
        t.obj.setBgColor(tileBg(t.value));
    }

    function spawnTile() {
        var empty = [];
        for (var r = 0; r < SIZE; r++)
            for (var c = 0; c < SIZE; c++)
                if (!tiles[r][c]) empty.push({ r: r, c: c });
        if (empty.length === 0) return;
        var p = empty[Math.floor(Math.random() * empty.length)];
        var value = Math.random() < 0.9 ? 2 : 4;
        var t = makeTile(p.r, p.c, value);
        tiles[p.r][p.c] = t;
        t.obj.setScale(0);
        LVGL.anim(t.obj, { scale: 256 }, 110, LVGL.ANIM.EASE_OUT, null);
    }

    /* ── Slide/merge computation ──────────────────────────────────── */
    var DIRS = {
        up:    { dr: -1, dc: 0 },
        down:  { dr: 1,  dc: 0 },
        left:  { dr: 0,  dc: -1 },
        right: { dr: 0,  dc: 1 }
    };

    function linesFor(dir) {
        var lines = [];
        var d = DIRS[dir];
        /* order cells so the destination edge is visited first */
        for (var i = 0; i < SIZE; i++) {
            var line = [];
            for (var k = 0; k < SIZE; k++) {
                var r, c;
                if (d.dr !== 0) { r = (d.dr > 0) ? SIZE - 1 - k : k; c = i; }
                else            { c = (d.dc > 0) ? SIZE - 1 - k : k; r = i; }
                line.push([r, c]);
            }
            lines.push(line);
        }
        return lines;
    }

    function computeMove(dir) {
        var lines = linesFor(dir);
        var moves = [];       /* {tile, r, c} — where each surviving tile ends up */
        var merges = [];      /* {base, mover} — mover animates onto base then dies */
        var moved = false;

        for (var li = 0; li < lines.length; li++) {
            var line = lines[li];
            var entries = [];
            for (var k = 0; k < line.length; k++) {
                var t = tiles[line[k][0]][line[k][1]];
                if (t) entries.push(t);
            }
            var placed = [];           /* final order for this line */
            var mergedFlag = [];       /* placed[i] already merged this turn */

            for (var e = 0; e < entries.length; e++) {
                var cur = entries[e];
                var pos = placed.length;
                if (pos > 0 && placed[pos - 1].value === cur.value && !mergedFlag[pos - 1]) {
                    /* merge: current slides onto the previous tile */
                    var base = placed[pos - 1];
                    base.value *= 2;
                    score += base.value;
                    mergedFlag[pos - 1] = true;
                    merges.push({ base: base, mover: cur });
                    moves.push({ tile: cur, r: base.r, c: base.c });
                    moved = true;
                } else {
                    var tr = line[pos][0], tc = line[pos][1];
                    placed.push(cur);
                    if (cur.r !== tr || cur.c !== tc) {
                        moves.push({ tile: cur, r: tr, c: tc });
                        moved = true;
                    }
                }
            }
            /* rebuild this line in the tiles matrix */
            for (var m = 0; m < line.length; m++) {
                tiles[line[m][0]][line[m][1]] = (m < placed.length) ? placed[m] : null;
            }
            for (var p = 0; p < placed.length; p++) {
                placed[p].r = line[p][0];
                placed[p].c = line[p][1];
            }
        }
        return { moved: moved, moves: moves, merges: merges };
    }

    /* ── Move + animation pipeline ────────────────────────────────── */
    function doMove(dir) {
        if (moving || over) return;
        var res = computeMove(dir);
        if (!res.moved) return;
        moving = true;

        var remaining = res.moves.length;
        if (remaining === 0) { finishMove(res); return; }

        for (var i = 0; i < res.moves.length; i++) {
            (function (m) {
                LVGL.anim(m.tile.obj, { x: cellX(m.c), y: cellY(m.r) },
                          90, LVGL.ANIM.EASE_OUT, function () {
                    if (--remaining === 0) finishMove(res);
                });
            })(res.moves[i]);
        }
    }

    function pop(obj) {
        obj.centerPivot();
        LVGL.anim(obj, { scale: 300 }, 80, LVGL.ANIM.EASE_OUT, function () {
            LVGL.anim(obj, { scale: 256 }, 80, LVGL.ANIM.EASE_IN_OUT, null);
        });
    }

    function finishMove(res) {
        for (var i = 0; i < res.merges.length; i++) {
            var m = res.merges[i];
            m.mover.obj.del();
            tiles[m.mover.r][m.mover.c] = null;
            relabel(m.base);
            pop(m.base.obj);
        }
        updateScore();
        spawnTile();
        moving = false;
        if (isGameOver()) showGameOver();
    }

    /* ── Game over ────────────────────────────────────────────────── */
    function hasMoves() {
        for (var r = 0; r < SIZE; r++)
            for (var c = 0; c < SIZE; c++) {
                if (!tiles[r][c]) return true;
                var v = tiles[r][c].value;
                if (r + 1 < SIZE && tiles[r + 1][c] && tiles[r + 1][c].value === v) return true;
                if (c + 1 < SIZE && tiles[r][c + 1] && tiles[r][c + 1].value === v) return true;
            }
        return false;
    }
    function isGameOver() { return !hasMoves(); }

    function showGameOver() {
        over = true;
        if (gameover) return;
        gameover = LVGL.create(screen);
        gameover.setPos(BOARD_X, BOARD_Y);
        gameover.setSize(BOARD, BOARD);
        gameover.setRadius(8);
        gameover.setBgColor(0xFFFFFF);
        gameover.setBgOpa(190);
        gameover.addFlag(LVGL.FLAG.CLICKABLE);   /* eat touches */
        gameover.clearFlag(LVGL.FLAG.SCROLLABLE);

        var title = LVGL.label(gameover, "Game Over");
        title.setTextColor(C_DARK);
        title.setFont(LVGL.FONT.LARGE);
        title.center();

        goBtn = LVGL.create(gameover);
        goBtn.setSize(140, 44);
        goBtn.setRadius(22);
        goBtn.setBgColor(0x8F7A66);
        goBtn.setBgOpa(LVGL.OPA.COVER);
        goBtn.addFlag(LVGL.FLAG.CLICKABLE);
        goBtn.clearFlag(LVGL.FLAG.SCROLLABLE);
        goBtn.center();
        goBtn.setPos(goBtn.getX(), goBtn.getY() + 44);

        var bl = LVGL.label(goBtn, "Try Again");
        bl.setTextColor(C_WHITE);
        bl.setFont(LVGL.FONT.MED_SMALL);
        bl.center();

        goBtn.on("clicked", function () { reset(); });
    }

    function hideGameOver() {
        over = false;
        if (gameover) { gameover.del(); gameover = null; goBtn = null; }
    }

    /* ── Score / best ─────────────────────────────────────────────── */
    function updateScore() {
        scoreVal.setText(String(score));
        if (score > best) {
            best = score;
            bestVal.setText(String(best));
            STORE.setInt("best", best);
        }
    }

    /* ── New game ─────────────────────────────────────────────────── */
    function reset() {
        hideGameOver();
        for (var r = 0; r < SIZE; r++)
            for (var c = 0; c < SIZE; c++)
                if (tiles[r][c]) { tiles[r][c].obj.del(); tiles[r][c] = null; }
        score = 0;
        updateScore();
        spawnTile();
        spawnTile();
    }

    /* ── Input ────────────────────────────────────────────────────── */
    function onGesture(dir) {
        if (dir === "left" || dir === "right" || dir === "up" || dir === "down")
            doMove(dir);
    }

    /* ── UI construction ──────────────────────────────────────────── */
    function build() {
        screen = LVGL.screen();
        screen.setBgColor(0xFAF8EF);       /* classic page background */
        screen.setBgOpa(LVGL.OPA.COVER);
        screen.addFlag(LVGL.FLAG.CLICKABLE);
        screen.clearFlag(LVGL.FLAG.SCROLLABLE);
        screen.on("gesture", onGesture);

        /* header (inset from the rounded corners) */
        var title = LVGL.label(screen, "2048");
        title.setPos(MARGIN, MARGIN + 2);
        title.setTextColor(C_HEADER);
        title.setFont(LVGL.FONT.MEDIUM);

        function scoreBox(x, caption, valObj) {
            var box = LVGL.create(screen);
            box.setPos(x, MARGIN);
            box.setSize(72, HEADER_H);
            box.setRadius(6);
            box.setBgColor(C_BOARD);
            box.setBgOpa(LVGL.OPA.COVER);
            box.clearFlag(LVGL.FLAG.SCROLLABLE);

            var cap = LVGL.label(box, caption);
            cap.setTextColor(0xEEE4DA);
            cap.setFont(LVGL.FONT.SMALL);
            cap.center();
            cap.setPos(cap.getX(), cap.getY() - 14);

            var val = LVGL.label(box, "0");
            val.setTextColor(C_WHITE);
            val.setFont(LVGL.FONT.MED_SMALL);
            val.center();
            val.setPos(val.getX(), val.getY() + 12);
            return val;
        }
        scoreVal = scoreBox(SW - MARGIN - 72 - 72 - 8, "SCORE");
        bestVal  = scoreBox(SW - MARGIN - 72, "BEST");

        /* board background + empty cells (non-scrollable so swipes become
         * gestures instead of scrolling the panel) */
        var board = LVGL.create(screen);
        board.setPos(BOARD_X, BOARD_Y);
        board.setSize(BOARD, BOARD);
        board.setRadius(8);
        board.setBgColor(C_BOARD);
        board.setBgOpa(LVGL.OPA.COVER);
        board.clearFlag(LVGL.FLAG.SCROLLABLE);

        for (var r = 0; r < SIZE; r++)
            for (var c = 0; c < SIZE; c++) {
                var cell = LVGL.create(board);
                cell.setPos(c * (TILE + GAP) + GAP, r * (TILE + GAP) + GAP);
                cell.setSize(TILE, TILE);
                cell.setRadius(8);
                cell.setBgColor(C_CELL);
                cell.setBgOpa(LVGL.OPA.COVER);
                cell.clearFlag(LVGL.FLAG.SCROLLABLE);
            }

        /* new game button */
        var ng = LVGL.create(screen);
        ng.setPos(MARGIN, BOARD_Y + BOARD + 6);
        ng.setSize(120, 22);
        ng.setRadius(11);
        ng.setBgColor(C_HEADER);
        ng.setBgOpa(LVGL.OPA.COVER);
        ng.addFlag(LVGL.FLAG.CLICKABLE);
        ng.clearFlag(LVGL.FLAG.SCROLLABLE);
        var ngl = LVGL.label(ng, "New Game");
        ngl.setTextColor(C_WHITE);
        ngl.setFont(LVGL.FONT.SMALL);
        ngl.center();
        ng.on("clicked", function () { reset(); });
    }

    /* ── Start ────────────────────────────────────────────────────── */
    for (var r = 0; r < SIZE; r++) {
        tiles[r] = [];
        for (var c = 0; c < SIZE; c++) tiles[r][c] = null;
    }
    best = STORE.getInt("best") || 0;
    build();
    updateScore();
    spawnTile();
    spawnTile();
})();
