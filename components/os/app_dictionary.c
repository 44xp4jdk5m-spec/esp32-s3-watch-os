/**
 * @file app_dictionary.c
 * @brief Compact English dictionary for the watch keyboard (auto-correct).
 *
 * File format (little-endian, produced by build_dict.py):
 *   offset 0     : magic "WDIC" (4)
 *   4            : version u32, word count N u32, max word len u32
 *   16           : letter_start u32[26]   (word index where each letter starts)
 *   120          : top3 u32[702][3]        (precomputed top-3 word indices by
 *                                           score for 26 single-letter + 676
 *                                           two-letter prefixes; 0xFFFFFFFF = none)
 *   8544         : offsets u32[N+1]        (byte offset of each word in blob)
 *   8544+4*(N+1) : scores u16[N]           (log2-scaled frequency)
 *   + 2*N        : strings blob            (NUL-terminated, sorted)
 *
 * The whole file is read once into PSRAM and searched by direct pointer
 * dereference (ESP32 is little-endian, so no byte swapping is needed).
 */

#include "app_dictionary.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "DICT";

#define DICT_PATH       "/spiffs/usr/share/dict/words.bin"
#define DICT_MAGIC      "WDIC"
#define DICT_TOP3_OFF   120
#define DICT_NOIDX      0xFFFFFFFFu

static uint8_t            *s_buf = NULL;    /* whole file in PSRAM */
static uint32_t            s_n = 0;
static uint32_t            s_max_len = 0;
static const uint32_t     *s_letter_start = NULL;
static const uint32_t     *s_top3 = NULL;
static const uint32_t     *s_offsets = NULL;
static const uint16_t     *s_scores = NULL;
static const char         *s_strings = NULL;

/* ── QWERTY positions for weighted substitution costs ─────────────── */
static const int8_t qw_x[26] = { 0,5,3,2,2,3,4,5,7,6,7,8,7,6,8,9,0,3,1,4,6,4,1,2,5,1 };
static const int8_t qw_y[26] = { 1,2,2,1,0,1,1,1,0,1,1,1,2,2,0,0,0,0,1,0,0,2,0,2,0,2 };

static const char *word_at(uint32_t i) { return s_strings + s_offsets[i]; }
static uint16_t score_at(uint32_t i)   { return s_scores[i]; }

static int sub_cost(char a, char b)
{
    if (a == b) return 0;
    int ai = a - 'a', bi = b - 'a';
    if (ai < 0 || ai > 25 || bi < 0 || bi > 25) return 3;
    int dx = qw_x[ai] - qw_x[bi]; if (dx < 0) dx = -dx;
    int dy = qw_y[ai] - qw_y[bi]; if (dy < 0) dy = -dy;
    int d = dx + dy;
    return (d < 1) ? 1 : d;
}

/* ====================================================================
 * Load
 * ==================================================================== */
esp_err_t app_dict_init(void)
{
    if (s_buf) return ESP_OK;

    FILE *f = fopen(DICT_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "dictionary not found: %s", DICT_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (32u << 20)) {
        fclose(f);
        ESP_LOGE(TAG, "bad dictionary size: %ld", sz);
        return ESP_ERR_INVALID_SIZE;
    }

    s_buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!s_buf) s_buf = malloc((size_t)sz);   /* fallback if PSRAM short */
    if (!s_buf) {
        fclose(f);
        ESP_LOGE(TAG, "no memory for dictionary");
        return ESP_ERR_NO_MEM;
    }

    size_t rd = fread(s_buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz || memcmp(s_buf, DICT_MAGIC, 4) != 0) {
        ESP_LOGE(TAG, "dictionary read/format failed (%d/%ld)", (int)rd, sz);
        heap_caps_free(s_buf);
        s_buf = NULL;
        return ESP_FAIL;
    }

    /* Parse layout pointers. */
    {
        const uint32_t *hdr = (const uint32_t *)(s_buf + 4);
        s_n = hdr[1];
        s_max_len = hdr[2];
        s_letter_start = (const uint32_t *)(s_buf + 16);
        s_top3         = (const uint32_t *)(s_buf + DICT_TOP3_OFF);
        s_offsets      = (const uint32_t *)(s_buf + DICT_TOP3_OFF + 702u * 3u * 4u);
        s_scores       = (const uint16_t *)(s_buf + DICT_TOP3_OFF + 702u * 3u * 4u + (s_n + 1u) * 4u);
        s_strings      = (const char *)(s_scores + s_n);
    }

    ESP_LOGI(TAG, "loaded: %u words (max len %u), %ld bytes", s_n, s_max_len, sz);
    return ESP_OK;
}

bool app_dict_ready(void) { return s_buf != NULL; }

/* ====================================================================
 * Prefix autocomplete
 * ==================================================================== */
int app_dict_suggest(const char *prefix, app_dict_entry_t *out, int max_n)
{
    if (!s_buf || !prefix || !prefix[0] || max_n <= 0) return 0;

    int plen = (int)strlen(prefix);
    if (plen > (int)s_max_len) plen = (int)s_max_len;
    char c0 = prefix[0];
    if (c0 < 'a' || c0 > 'z') return 0;

    int count = 0;

    /* 1- and 2-letter prefixes: use the precomputed top-3 cache. */
    if (plen == 1) {
        const uint32_t *t = s_top3 + (uint32_t)(c0 - 'a') * 3u;
        for (int k = 0; k < 3 && k < max_n; k++) {
            uint32_t idx = t[k];
            if (idx == DICT_NOIDX) break;
            out[count].word = word_at(idx);
            out[count].score = score_at(idx);
            count++;
        }
        return count;
    }
    if (plen == 2 && prefix[1] >= 'a' && prefix[1] <= 'z') {
        uint32_t slot = 26u + (uint32_t)(c0 - 'a') * 26u + (uint32_t)(prefix[1] - 'a');
        const uint32_t *t = s_top3 + slot * 3u;
        for (int k = 0; k < 3 && k < max_n; k++) {
            uint32_t idx = t[k];
            if (idx == DICT_NOIDX) break;
            out[count].word = word_at(idx);
            out[count].score = score_at(idx);
            count++;
        }
        return count;
    }

    /* 3+ letters: binary search the prefix range, scan top-3 by score. */
    uint32_t lo = s_letter_start[c0 - 'a'];
    uint32_t hi = (c0 - 'a' + 1 < 26) ? s_letter_start[c0 - 'a' + 1] : s_n;

    uint32_t l = lo, r = hi;
    while (l < r) {
        uint32_t m = (l + r) / 2;
        if (strcmp(word_at(m), prefix) < 0) l = m + 1; else r = m;
    }

    uint32_t best[3] = { DICT_NOIDX, DICT_NOIDX, DICT_NOIDX };
    uint16_t best_s[3] = { 0, 0, 0 };

    for (uint32_t i = l; i < hi; i++) {
        const char *w = word_at(i);
        if (strncmp(w, prefix, (size_t)plen) != 0) break;
        uint16_t sc = score_at(i);
        for (int k = 0; k < 3; k++) {
            if (best[k] == DICT_NOIDX || sc > best_s[k]) {
                for (int j = 2; j > k; j--) { best[j] = best[j - 1]; best_s[j] = best_s[j - 1]; }
                best[k] = i; best_s[k] = sc;
                break;
            }
        }
    }

    for (int k = 0; k < 3 && k < max_n; k++) {
        if (best[k] == DICT_NOIDX) break;
        out[count].word = word_at(best[k]);
        out[count].score = best_s[k];
        count++;
    }
    return count;
}

/* ====================================================================
 * Typo correction — weighted Damerau-Levenshtein
 * ==================================================================== */

/* Bounded weighted edit distance.  Returns distance, or a large value if it
 * exceeds max_cost (early exit). */
static float edit_distance(const char *a, const char *b, float max_cost)
{
    int la = (int)strlen(a), lb = (int)strlen(b);
    int dlen = la - lb; if (dlen < 0) dlen = -dlen;
    if (dlen > (int)max_cost) return 999.0f;

    float prev2[APP_DICT_MAX_WORD + 2];
    float prev[APP_DICT_MAX_WORD + 2];
    float cur[APP_DICT_MAX_WORD + 2];

    for (int j = 0; j <= lb; j++) prev[j] = (float)j;   /* delete row */
    for (int j = 0; j <= lb; j++) prev2[j] = 999.0f;

    for (int i = 1; i <= la; i++) {
        cur[0] = (float)i;                                 /* insert col */
        float row_min = cur[0];
        for (int j = 1; j <= lb; j++) {
            float sub = (float)sub_cost(a[i - 1], b[j - 1]);
            float v = prev[j - 1] + sub;
            float d = prev[j] + 1.0f;    if (d < v) v = d;   /* delete */
            float ins = cur[j - 1] + 1.0f; if (ins < v) v = ins;  /* insert */
            /* Damerau transposition */
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
                float t = prev2[j - 2] + 1.0f;
                if (t < v) v = t;
            }
            cur[j] = v;
            if (v < row_min) row_min = v;
        }
        if (row_min > max_cost) return 999.0f;   /* prune */
        for (int j = 0; j <= lb; j++) { prev2[j] = prev[j]; prev[j] = cur[j]; }
    }
    return prev[lb];
}

int app_dict_correct(const char *word, app_dict_entry_t *out, int max_n)
{
    if (!s_buf || !word || !word[0] || max_n <= 0) return 0;

    const float max_cost = 2.5f;
    int wlen = (int)strlen(word);

    /* best results by (distance asc, then score desc) */
    float best_d[3] = { 999.0f, 999.0f, 999.0f };
    uint32_t best_i[3] = { DICT_NOIDX, DICT_NOIDX, DICT_NOIDX };
    uint16_t best_s[3] = { 0, 0, 0 };

    for (uint32_t i = 0; i < s_n; i++) {
        const char *w = word_at(i);
        int lw = (int)strlen(w);
        int dlen = lw - wlen; if (dlen < 0) dlen = -dlen;
        if (dlen > (int)max_cost) continue;

        float d = edit_distance(w, word, max_cost);
        if (d > max_cost) continue;

        uint16_t sc = score_at(i);
        for (int k = 0; k < 3; k++) {
            if (best_i[k] == DICT_NOIDX || d < best_d[k] ||
                (d == best_d[k] && sc > best_s[k])) {
                for (int j = 2; j > k; j--) {
                    best_d[j] = best_d[j - 1]; best_i[j] = best_i[j - 1]; best_s[j] = best_s[j - 1];
                }
                best_d[k] = d; best_i[k] = i; best_s[k] = sc;
                break;
            }
        }
    }

    int count = 0;
    for (int k = 0; k < 3 && k < max_n; k++) {
        if (best_i[k] == DICT_NOIDX) break;
        out[count].word = word_at(best_i[k]);
        out[count].score = best_s[k];
        count++;
    }
    return count;
}
