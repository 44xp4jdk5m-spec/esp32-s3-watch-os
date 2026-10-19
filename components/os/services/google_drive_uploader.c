/**
 * @file google_drive_uploader.c
 * @brief Upload images to Google Drive via REST API v3.
 *
 * Uses ESP-HTTP client with multipart/form-data upload.
 * All temporary buffers are allocated from PSRAM.
 *
 * API endpoint: POST https://www.googleapis.com/upload/drive/v3/files
 *               ?uploadType=multipart
 * Headers:      Authorization: Bearer {token}
 *               Content-Type: multipart/form-data; boundary={boundary}
 */

#include "google_drive_uploader.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "GDRIVE";

#define GDRIVE_API_BASE   "https://www.googleapis.com/upload/drive/v3/files"
#define GDRIVE_UPLOAD_TIMEOUT_MS  30000

/* ── MIME types ─────────────────────────────────────────────────────── */

static const char *_mime_for_ext(const char *file_name)
{
    const char *ext = strrchr(file_name, '.');
    if (!ext) return "application/octet-stream";
    if (strcasecmp(ext, ".bmp") == 0)  return "image/bmp";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, ".png") == 0)  return "image/png";
    return "application/octet-stream";
}

/* ── HTTP upload ─────────────────────────────────────────────────────── */

gdrive_result_t google_drive_upload_file(const char *file_name,
                                         const uint8_t *data,
                                         uint32_t data_size,
                                         const char *mime_type,
                                         const char *folder_id,
                                         const char *access_token)
{
    if (!file_name || !data || !data_size || !access_token || !*access_token) {
        return GDRIVE_ERR_CONFIG;
    }

    if (!mime_type || !*mime_type) {
        mime_type = _mime_for_ext(file_name);
    }

    /* Build the multipart body in PSRAM --------------------------------- */

    /*
     * Google Drive multipart upload structure:
     *
     * --boundary\r\n
     * Content-Type: application/json; charset=UTF-8\r\n
     * \r\n
     * {"name":"filename.bmp", "parents":["folderId"]}\r\n
     * --boundary\r\n
     * Content-Type: image/bmp\r\n
     * \r\n
     * <binary data>\r\n
     * --boundary--\r\n
     */

    const char boundary[] = "watch_screenshot_boundary";
    /* Estimate total body size */
    char meta_json[256];
    int meta_len;
    if (folder_id && *folder_id) {
        meta_len = snprintf(meta_json, sizeof(meta_json),
                            "{\"name\":\"%s\",\"parents\":[\"%s\"]}",
                            file_name, folder_id);
    } else {
        meta_len = snprintf(meta_json, sizeof(meta_json),
                            "{\"name\":\"%s\"}", file_name);
    }

    /* --boundary\r\n */
    size_t part1_len = strlen(boundary) + 6; /* "--" + boundary + "\r\n" */
    size_t part2_len = strlen(boundary) + 8; /* "\r\n--" + boundary + "--\r\n" */
    size_t json_hdr_len = (size_t)(64 + strlen(mime_type) + meta_len);

    size_t body_size = part1_len + json_hdr_len + part2_len + data_size;
    uint8_t *body = (uint8_t *)heap_caps_malloc(body_size,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        ESP_LOGE(TAG, "OOM: cannot allocate %zu byte body", body_size);
        return GDRIVE_ERR_OOM;
    }

    size_t pos = 0;

    /* Metadata part */
    pos += (size_t)snprintf((char *)body + pos, body_size - pos,
        "--%s\r\n"
        "Content-Type: application/json; charset=UTF-8\r\n"
        "\r\n"
        "%s\r\n",
        boundary, meta_json);

    /* Binary data part */
    pos += (size_t)snprintf((char *)body + pos, body_size - pos,
        "--%s\r\n"
        "Content-Type: %s\r\n"
        "\r\n",
        boundary, mime_type);

    memcpy(body + pos, data, data_size);
    pos += data_size;

    pos += (size_t)snprintf((char *)body + pos, body_size - pos,
        "\r\n--%s--\r\n", boundary);

    /* Build the URL ----------------------------------------------------- */
    char url[512];
    snprintf(url, sizeof(url),
             "%s?uploadType=multipart", GDRIVE_API_BASE);

    /* HTTP request ------------------------------------------------------ */
    esp_http_client_config_t cfg = {
        .url                   = url,
        .method                = HTTP_METHOD_POST,
        .timeout_ms            = GDRIVE_UPLOAD_TIMEOUT_MS,
        .keep_alive_enable     = false,
        .buffer_size           = 4096,
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        heap_caps_free(body);
        return GDRIVE_ERR_OOM;
    }

    /* Headers */
    char auth_hdr[GDRIVE_TOKEN_MAX + 32];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", access_token);
    esp_http_client_set_header(client, "Authorization", auth_hdr);

    char content_type[128];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_http_client_set_post_field(client, (char *)body, (int)body_size);

    /* Perform the request ----------------------------------------------- */
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        heap_caps_free(body);
        if (err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_HTTP_CONNECTING) {
            return GDRIVE_ERR_TIMEOUT;
        }
        return GDRIVE_ERR_HTTP;
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status: %d", status);

    gdrive_result_t result;
    if (status == 200) {
        result = GDRIVE_OK;
    } else if (status == 401) {
        result = GDRIVE_ERR_AUTH;
        /* Try to read response body for more info */
        char resp_buf[256] = {0};
        int read_len = esp_http_client_read(client, resp_buf, sizeof(resp_buf) - 1);
        if (read_len > 0) {
            resp_buf[read_len] = 0;
            ESP_LOGW(TAG, "Auth error response: %s", resp_buf);
        }
    } else if (status >= 500) {
        result = GDRIVE_ERR_SERVER;
    } else {
        result = GDRIVE_ERR_RESPONSE;
        char resp_buf[256] = {0};
        int read_len = esp_http_client_read(client, resp_buf, sizeof(resp_buf) - 1);
        if (read_len > 0) {
            resp_buf[read_len] = 0;
            ESP_LOGW(TAG, "Unexpected response (%d): %s", status, resp_buf);
        }
    }

    esp_http_client_cleanup(client);
    heap_caps_free(body);

    ESP_LOGI(TAG, "Upload result: %s", google_drive_result_str(result));
    return result;
}

/* ── Error string ────────────────────────────────────────────────────── */

const char *google_drive_result_str(gdrive_result_t res)
{
    switch (res) {
    case GDRIVE_OK:           return "OK";
    case GDRIVE_ERR_CONFIG:   return "Missing/invalid config";
    case GDRIVE_ERR_NO_WIFI:  return "No WiFi connection";
    case GDRIVE_ERR_HTTP:     return "HTTP error";
    case GDRIVE_ERR_AUTH:     return "Auth failed (token expired?)";
    case GDRIVE_ERR_SERVER:   return "Server error";
    case GDRIVE_ERR_RESPONSE: return "Unexpected response";
    case GDRIVE_ERR_OOM:      return "Out of memory";
    case GDRIVE_ERR_TIMEOUT:  return "Connection timeout";
    default:                  return "Unknown error";
    }
}
