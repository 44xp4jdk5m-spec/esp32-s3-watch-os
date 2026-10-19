/**
 * @file watch_face_registry.c
 * @brief Watch-face registry — manages the list of faces and the current
 *        face's screen lifecycle.
 */
#include "watch_face.h"
#include "esp_log.h"

static const char *TAG = "WF_REG";

static const watch_face_t *s_faces[WATCH_FACE_MAX];
static uint8_t s_count      = 0;
static uint8_t s_current_idx = 0;
static lv_obj_t *s_screen   = NULL;

/* ── Built-in faces (implemented in their own modules) ─────────────── */
extern const watch_face_t watch_face_classic;
extern const watch_face_t watch_face_minimal;
extern const watch_face_t watch_face_flux;
extern const watch_face_t watch_face_parallax;
extern const watch_face_t watch_face_hands;
extern const watch_face_t watch_face_california;

void watch_face_register(const watch_face_t *face)
{
    if (!face || s_count >= WATCH_FACE_MAX) return;
    s_faces[s_count++] = face;
}

void watch_face_registry_init(void)
{
    s_count = 0;
    s_current_idx = 0;
    s_screen = NULL;
    watch_face_register(&watch_face_classic);
    watch_face_register(&watch_face_minimal);
    watch_face_register(&watch_face_flux);
    watch_face_register(&watch_face_parallax);
    watch_face_register(&watch_face_hands);
    watch_face_register(&watch_face_california);
    ESP_LOGI(TAG, "%u watch faces registered", s_count);
}

uint8_t watch_face_count(void)
{
    return s_count;
}

const watch_face_t *watch_face_get(uint8_t index)
{
    return (index < s_count) ? s_faces[index] : NULL;
}

const watch_face_t *watch_face_get_by_id(uint8_t id)
{
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_faces[i]->id == id) return s_faces[i];
    }
    return NULL;
}

const watch_face_t *watch_face_current(void)
{
    return (s_current_idx < s_count) ? s_faces[s_current_idx] : NULL;
}

lv_obj_t *watch_face_current_screen(void)
{
    return s_screen;
}

uint8_t watch_face_current_id(void)
{
    const watch_face_t *f = watch_face_current();
    return f ? f->id : 0;
}

static uint8_t index_of_id(uint8_t id)
{
    for (uint8_t i = 0; i < s_count; i++) {
        if (s_faces[i]->id == id) return i;
    }
    return 0;   /* fallback to the first face */
}

/* Create the screen for the face at @p idx. The screen is registered with
 * the display (created with NULL parent) but NOT loaded. */
static bool create_face(uint8_t id)
{
    uint8_t idx = index_of_id(id);
    const watch_face_t *face = s_faces[idx];
    if (!face) return false;

    /* Destroy previous face screen (if any) */
    if (s_screen) {
        if (s_faces[s_current_idx]->destroy) s_faces[s_current_idx]->destroy();
        s_screen = NULL;
    }

    lv_obj_t *scr = face->create();
    if (!scr) {
        ESP_LOGE(TAG, "face '%s' (id=%d) create failed", face->name, face->id);
        return false;
    }
    s_current_idx = idx;
    s_screen = scr;
    ESP_LOGI(TAG, "face '%s' (id=%d) screen ready", face->name, face->id);
    return true;
}

bool watch_face_create_for_boot(uint8_t id)
{
    return create_face(id);
}

bool watch_face_activate(uint8_t id)
{
    if (!create_face(id)) return false;
    lv_screen_load(s_screen);
    ESP_LOGI(TAG, "activated face '%s' (id=%d)",
             s_faces[s_current_idx]->name, s_faces[s_current_idx]->id);
    return true;
}

void watch_face_update_current_time(const char *time_str, const char *date_str)
{
    const watch_face_t *f = watch_face_current();
    if (f && f->update_time) f->update_time(time_str, date_str);
}

void watch_face_update_current_battery(int pct, bool charging, bool vbus_present)
{
    const watch_face_t *f = watch_face_current();
    if (f && f->update_battery) f->update_battery(pct, charging, vbus_present);
}

void watch_face_aod_pixel_shift(int dx, int dy)
{
    const watch_face_t *f = watch_face_current();
    if (f && f->aod_pixel_shift) f->aod_pixel_shift(dx, dy);
}
