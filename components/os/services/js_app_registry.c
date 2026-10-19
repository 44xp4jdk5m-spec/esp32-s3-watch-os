/*
 * Watch OS — JavaScript app registry implementation (see header).
 */
#include "js_app_registry.h"

#include "app_image_loader.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "JS_REG";

#define JS_APPS_ROOT "/sdcard/watchos/usr/app"
#define MAX_APPS     16

static js_app_entry_t s_entries[MAX_APPS];
static int            s_count = 0;

/* Free a single entry (keeps the slot, just clears it). */
static void js_app_entry_clear(js_app_entry_t *e)
{
    if (e->name)  { heap_caps_free(e->name);  e->name = NULL; }
    if (e->dir)   { heap_caps_free(e->dir);   e->dir = NULL; }
    if (e->entry) { heap_caps_free(e->entry); e->entry = NULL; }
    if (e->author)       { heap_caps_free(e->author);       e->author = NULL; }
    if (e->version)      { heap_caps_free(e->version);      e->version = NULL; }
    if (e->description)  { heap_caps_free(e->description);  e->description = NULL; }
    if (e->icon)  { app_img_free(e->icon);    e->icon = NULL; }
    e->icon_w = e->icon_h = 0;
}

static void js_app_registry_clear(void)
{
    for (int i = 0; i < s_count; i++)
        js_app_entry_clear(&s_entries[i]);
    s_count = 0;
}

static char *js_strdup_psram(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) memcpy(p, s, n);
    return p;
}

/* Read a whole (small) file into a PSRAM buffer. */
static char *js_read_file(const char *path, size_t max_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || sz > (long)max_size) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { heap_caps_free(buf); return NULL; }
    buf[sz] = '\0';
    return buf;
}

static void js_register_app(const char *dir)
{
    if (s_count >= MAX_APPS) return;

    char mpath[128];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", dir);
    char *json = js_read_file(mpath, 8192);
    if (!json) {
        ESP_LOGW(TAG, "no manifest at %s", mpath);
        return;
    }

    cJSON *root = cJSON_Parse(json);
    heap_caps_free(json);
    if (!root) return;

    const cJSON *nm = cJSON_GetObjectItem(root, "name");
    const cJSON *ent = cJSON_GetObjectItem(root, "entry");
    const cJSON *ic = cJSON_GetObjectItem(root, "icon");
    const cJSON *iw = cJSON_GetObjectItem(root, "icon_w");
    const cJSON *ih = cJSON_GetObjectItem(root, "icon_h");
    const cJSON *cf = cJSON_GetObjectItem(root, "icon_cf");
    const cJSON *au  = cJSON_GetObjectItem(root, "author");
    const cJSON *ver = cJSON_GetObjectItem(root, "version");
    const cJSON *desc = cJSON_GetObjectItem(root, "description");

    js_app_entry_t *e = &s_entries[s_count];
    memset(e, 0, sizeof(*e));
    e->name  = js_strdup_psram(cJSON_IsString(nm) ? nm->valuestring : "App");
    e->entry = js_strdup_psram(cJSON_IsString(ent) ? ent->valuestring : "main.js");
    e->dir   = js_strdup_psram(dir);
    e->author      = js_strdup_psram(cJSON_IsString(au)  ? au->valuestring  : NULL);
    e->version     = js_strdup_psram(cJSON_IsString(ver) ? ver->valuestring : NULL);
    e->description = js_strdup_psram(cJSON_IsString(desc)? desc->valuestring: NULL);
    e->icon_w = cJSON_IsNumber(iw) ? (uint32_t)iw->valuedouble : 96;
    e->icon_h = cJSON_IsNumber(ih) ? (uint32_t)ih->valuedouble : 96;

    lv_color_format_t cformat = LV_COLOR_FORMAT_RGB565;
    if (cJSON_IsString(cf)) {
        if (strcmp(cf->valuestring, "RGB565A8") == 0)
            cformat = LV_COLOR_FORMAT_RGB565A8;
    }

    /* Decode the app icon into PSRAM. */
    if (cJSON_IsString(ic)) {
        char ipath[128];
        snprintf(ipath, sizeof(ipath), "%s/%s", dir, ic->valuestring);
        e->icon = app_img_load_raw(ipath, e->icon_w, e->icon_h, cformat);
        if (!e->icon)
            ESP_LOGW(TAG, "failed to load icon %s", ipath);
    }

    cJSON_Delete(root);
    s_count++;
    ESP_LOGI(TAG, "registered app '%s' (%s)", e->name, e->dir);
}

int js_app_registry_scan(void)
{
    js_app_registry_clear();

    struct stat st;
    if (stat(JS_APPS_ROOT, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "no app folder: %s", JS_APPS_ROOT);
        return 0;
    }

    DIR *d = opendir(JS_APPS_ROOT);
    if (!d) return 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && s_count < MAX_APPS) {
        if (de->d_name[0] == '.') continue;
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/%s", JS_APPS_ROOT, de->d_name);
        struct stat dst;
        if (stat(dir, &dst) == 0 && S_ISDIR(dst.st_mode))
            js_register_app(dir);
    }
    closedir(d);
    return s_count;
}

int js_app_registry_count(void)
{
    return s_count;
}

const js_app_entry_t *js_app_registry_get(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    return &s_entries[index];
}

const char *js_app_registry_entry_path(int index)
{
    const js_app_entry_t *e = js_app_registry_get(index);
    if (!e) return NULL;
    static char buf[128];
    snprintf(buf, sizeof(buf), "%s/%s", e->dir, e->entry);
    return buf;
}

/* Recursively delete a directory (FAT/VFS can only remove empty dirs). */
static void js_fs_remove_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char p[128];
            snprintf(p, sizeof(p), "%s/%s", path, de->d_name);
            struct stat st;
            if (stat(p, &st) == 0) {
                if (S_ISDIR(st.st_mode)) js_fs_remove_recursive(p);
                else remove(p);
            }
        }
        closedir(d);
    }
    rmdir(path);
}

/* True when @p path is a directory holding at least one entry. */
static bool js_dir_has_entries(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    DIR *d = opendir(path);
    if (!d) return false;
    bool has = false;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        has = true;
        break;
    }
    closedir(d);
    return has;
}

int js_app_registry_delete(int index)
{
    const js_app_entry_t *e = js_app_registry_get(index);
    if (!e || !e->dir) return -1;

    ESP_LOGI(TAG, "Deleting app '%s' (%s)", e->name, e->dir);
    js_fs_remove_recursive(e->dir);

    /* Drop the entry and shift the rest down. */
    js_app_entry_clear(&s_entries[index]);
    for (int i = index; i < s_count - 1; i++)
        s_entries[i] = s_entries[i + 1];
    memset(&s_entries[s_count - 1], 0, sizeof(s_entries[0]));
    s_count--;
    ESP_LOGI(TAG, "Deleted (remaining %d)", s_count);
    return 0;
}

int js_app_registry_clear_data(int index)
{
    const js_app_entry_t *e = js_app_registry_get(index);
    if (!e || !e->dir) return -1;

    char data_path[160];
    snprintf(data_path, sizeof(data_path), "%s/app_data", e->dir);
    struct stat st;
    if (stat(data_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGW(TAG, "No app_data dir for '%s' (%s)", e->name, data_path);
        return -1;
    }

    ESP_LOGI(TAG, "Clearing data for '%s' (%s)", e->name, data_path);
    js_fs_remove_recursive(data_path);
    return 0;
}

bool js_app_registry_has_data(int index)
{
    const js_app_entry_t *e = js_app_registry_get(index);
    if (!e || !e->dir) return false;

    char data_path[160];
    snprintf(data_path, sizeof(data_path), "%s/app_data", e->dir);
    return js_dir_has_entries(data_path);
}
