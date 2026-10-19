/**
 * @file wifi_file_server.c
 * @brief Simple HTTP file server for SD card browsing.
 *
 * Serves files from /sdcard/ over HTTP on port 8080.
 * Directory listing with links for easy browsing.
 *
 * Uses ESP-IDF's esp_http_server.
 * All file read buffers use PSRAM (MALLOC_CAP_SPIRAM).
 */

#include "wifi_file_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "WIFI_FS";
static httpd_handle_t s_server = NULL;

/* ── Helpers ────────────────────────────────────────────────────────── */

static const char *_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css") == 0)  return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".js") == 0)   return "application/javascript";
    if (strcasecmp(ext, ".json") == 0)  return "application/json";
    if (strcasecmp(ext, ".png") == 0)  return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, ".bmp") == 0)  return "image/bmp";
    if (strcasecmp(ext, ".gif") == 0)  return "image/gif";
    if (strcasecmp(ext, ".txt") == 0)  return "text/plain; charset=utf-8";
    if (strcasecmp(ext, ".csv") == 0)  return "text/csv; charset=utf-8";
    if (strcasecmp(ext, ".zip") == 0)  return "application/zip";
    if (strcasecmp(ext, ".pdf") == 0)  return "application/pdf";
    return "application/octet-stream";
}

/* ── GET handler: serve file or directory listing ──────────────────── */

/* Strip trailing slash for consistent path building */
static char *_strip_trailing_slash(const char *path, char *buf, size_t size)
{
    size_t len = strlen(path);
    if (len > 1 && path[len - 1] == '/') {
        snprintf(buf, size, "%.*s", (int)(len - 1), path);
        return buf;
    }
    return (char *)path;
}

static esp_err_t _get_handler(httpd_req_t *req)
{
    char uri[512];
    if (httpd_req_get_url_query_str(req, uri, sizeof(uri)) == ESP_OK) {
        /* Ignore query params */
    }

    /* Decode URI — use raw path from req->uri */
    const char *path = req->uri;
    if (!path || strcmp(path, "/") == 0) path = "/sdcard";

    /* Build filesystem path: map / → /sdcard */
    char fs_path[256];
    if (strncmp(path, "/sdcard", 7) == 0) {
        snprintf(fs_path, sizeof(fs_path), "%s", path);
    } else if (strcmp(path, "/") == 0) {
        snprintf(fs_path, sizeof(fs_path), "/sdcard");
    } else {
        snprintf(fs_path, sizeof(fs_path), "/sdcard%s", path);
    }

    struct stat st;
    if (stat(fs_path, &st) != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "<html><body><h1>404 Not Found</h1></body></html>");
        return ESP_OK;
    }

    if (S_ISDIR(st.st_mode)) {
        /* ── Directory listing ── */
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr_chunk(req, "<html><head><meta charset='utf-8'/>");
        httpd_resp_sendstr_chunk(req, "<style>body{font-family:sans-serif;margin:20px}");
        httpd_resp_sendstr_chunk(req, "a{display:block;padding:4px}");
        httpd_resp_sendstr_chunk(req, "</style></head><body>");
        httpd_resp_sendstr_chunk(req, "<h2>SD Card — ");
        httpd_resp_sendstr_chunk(req, path);
        httpd_resp_sendstr_chunk(req, "</h2><hr>");

        /* Parent link */
        if (strcmp(fs_path, "/sdcard") != 0) {
            httpd_resp_sendstr_chunk(req, "<a href='..'>[..]</a>");
        }

        DIR *dir = opendir(fs_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.') continue;
                char subpath[320];
                const char *base = (strcmp(path, "/") == 0) ? "" : path;
                snprintf(subpath, sizeof(subpath), "%s/%s", base, entry->d_name);
                httpd_resp_sendstr_chunk(req, "<a href='");
                httpd_resp_sendstr_chunk(req, subpath);
                httpd_resp_sendstr_chunk(req, "'>");
                httpd_resp_sendstr_chunk(req, entry->d_name);
                if (entry->d_type == DT_DIR) {
                    httpd_resp_sendstr_chunk(req, "/");
                }
                httpd_resp_sendstr_chunk(req, "</a>");
            }
            closedir(dir);
        }
        httpd_resp_sendstr_chunk(req, "</body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    /* ── Serve file ── */
    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        httpd_resp_set_status(req, "500 Internal Error");
        httpd_resp_sendstr(req, "Cannot open file");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    httpd_resp_set_type(req, _mime_type(fs_path));
    httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

    /* Read and send in chunks using PSRAM buffer */
    uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        httpd_resp_set_status(req, "500 OOM");
        httpd_resp_sendstr(req, "Out of memory");
        return ESP_OK;
    }

    long remaining = fsize;
    while (remaining > 0) {
        long to_read = (remaining > 4096) ? 4096 : remaining;
        size_t nread = fread(buf, 1, (size_t)to_read, f);
        if (nread > 0) {
            if (httpd_resp_send_chunk(req, (char *)buf, nread) != ESP_OK) {
                break;
            }
            remaining -= (long)nread;
        } else {
            break;
        }
    }
    httpd_resp_send_chunk(req, NULL, 0);

    heap_caps_free(buf);
    fclose(f);
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t wifi_file_server_start(void)
{
    if (s_server) {
        return ESP_OK; /* already running */
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = 8080;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    httpd_uri_t uri_get = {
        .uri      = "/*",
        .method   = HTTP_GET,
        .handler  = _get_handler,
    };
    httpd_register_uri_handler(s_server, &uri_get);

    /* Get IP */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        ESP_LOGI(TAG, "File server running at http://" IPSTR ":8080",
                 IP2STR(&ip.ip));
    } else {
        ESP_LOGI(TAG, "File server running on port 8080 (IP unknown)");
    }

    return ESP_OK;
}

void wifi_file_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "File server stopped");
    }
}

bool wifi_file_server_is_running(void)
{
    return (s_server != NULL);
}

const char *wifi_file_server_get_ip(char *buf, size_t size)
{
    if (!s_server || !buf || size < 16) return NULL;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(buf, size, IPSTR, IP2STR(&ip.ip));
        return buf;
    }
    return NULL;
}
