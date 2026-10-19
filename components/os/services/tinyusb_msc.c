/**
 * @file tinyusb_msc.c — TinyUSB MSC: expose SD card via USB OTG (GPIO19/20).
 *
 * All callbacks required by TinyUSB v0.15 are implemented.
 * Config descriptor built as raw bytes to avoid packed-struct pitfalls.
 */

#include "tinyusb_msc.h"
#include "esp_log.h"
#include "esp_err.h"
#include "tusb.h"
#include "bsp/esp32_s3_touch_amoled_2_06.h"
#include "sdmmc_cmd.h"
#include "esp_private/usb_phy.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "USB_MSC";
static bool s_active = false;
static usb_phy_handle_t s_phy_handle = NULL;

/* ── USB Device Descriptor ──────────────────────────────────────────── */
#define USBD_VID 0x303A
#define USBD_PID 0x4002
#define USBD_LANGID_STRING 0x0409
enum { STRID_LANGID = 0, STRID_MANUFACTURER, STRID_PRODUCT, STRID_SERIAL };

static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USBD_VID,
    .idProduct          = USBD_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

/* ── Config Descriptor (built as raw bytes) ─────────────────────────── */
#define MSC_EP_IN   0x81
#define MSC_EP_OUT  0x01

static uint8_t s_desc_cfg[32];
static bool s_cfg_built = false;

static void build_config_desc(void)
{
    uint8_t *p = s_desc_cfg;
    /* Config header (9 bytes) */
    *p++ = 9; *p++ = TUSB_DESC_CONFIGURATION;
    *p++ = 32; *p++ = 0;  /* wTotalLength */
    *p++ = 1;  *p++ = 1;  *p++ = 0;
    *p++ = 0x80; *p++ = 100;
    /* MSC Interface (9 bytes) */
    *p++ = 9; *p++ = TUSB_DESC_INTERFACE;
    *p++ = 0; *p++ = 0; *p++ = 2;
    *p++ = TUSB_CLASS_MSC; *p++ = 0x06; *p++ = 0x50; *p++ = 0;
    /* EP IN (7 bytes) — Full Speed bulk = max 64 bytes */
    *p++ = 7; *p++ = TUSB_DESC_ENDPOINT;
    *p++ = MSC_EP_IN; *p++ = 0x02;
    *p++ = 64; *p++ = 0x00; *p++ = 0;
    /* EP OUT (7 bytes) — Full Speed bulk = max 64 bytes */
    *p++ = 7; *p++ = TUSB_DESC_ENDPOINT;
    *p++ = MSC_EP_OUT; *p++ = 0x02;
    *p++ = 64; *p++ = 0x00; *p++ = 0;
    s_cfg_built = true;
}

/* ── String Descriptors ─────────────────────────────────────────────── */
static uint16_t _ds[33];
static uint16_t *_mkstr(const char *s) {
    uint8_t len = (uint8_t)strlen(s); if (len > 32) len = 32;
    _ds[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    for (uint8_t i = 0; i < len; i++) _ds[1 + i] = s[i];
    return _ds;
}

/* ═══════════════════════════════════════════════════════════════════════
 * USB Descriptor Callbacks
 * ═══════════════════════════════════════════════════════════════════════ */

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&s_desc_device;
}
uint8_t const *tud_descriptor_configuration_cb(uint8_t idx) {
    (void)idx; if (!s_cfg_built) build_config_desc();
    return s_desc_cfg;
}
uint16_t const *tud_descriptor_string_cb(uint8_t idx, uint16_t langid) {
    (void)langid;
    switch (idx) {
    case STRID_LANGID: _ds[0]=(TUSB_DESC_STRING<<8)|4; _ds[1]=USBD_LANGID_STRING; return _ds;
    case STRID_MANUFACTURER: return _mkstr("Espressif");
    case STRID_PRODUCT: return _mkstr("ESP32-S3 SD Reader");
    case STRID_SERIAL: return _mkstr("000001");
    default: return NULL;
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * MSC Callbacks
 * ═══════════════════════════════════════════════════════════════════════ */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vid[8], uint8_t pid[16], uint8_t rev[4]) {
    (void)lun;
    memset(vid,' ',8); memcpy(vid,"ESP32-S3",8);
    memset(pid,' ',16); memcpy(pid,"SD Reader       ",16);
    memset(rev,' ',4); memcpy(rev,"1.0",3);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) { (void)lun; return bsp_sdcard != NULL; }
void tud_msc_capacity_cb(uint8_t lun, uint32_t *cnt, uint16_t *sz) {
    (void)lun;
    if (bsp_sdcard) { *cnt = bsp_sdcard->csd.capacity; *sz = bsp_sdcard->csd.sector_size; }
    else { *cnt = 0; *sz = 512; }
}

/* Scratch buffer for sub-sector (unaligned) accesses. 512 B == SD sector size. */
static uint8_t s_secbuf[512];

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t off, void *buf, uint32_t bufsize) {
    (void)lun;
    if (!bsp_sdcard || bufsize == 0) return -1;
    size_t sec = bsp_sdcard->csd.sector_size;
    if (sec == 0 || sec > sizeof(s_secbuf)) return -1;

    /* Fast path: aligned, whole-sector read */
    if (off == 0 && (bufsize % sec) == 0) {
        esp_err_t e = sdmmc_read_sectors(bsp_sdcard, buf, lba, bufsize / sec);
        return (e == ESP_OK) ? (int32_t)bufsize : -1;
    }

    /* General path: TinyUSB hands us <= CFG_TUD_MSC_EP_BUFSIZE bytes at a time
     * with a byte offset inside the block (e.g. 64 B chunks of a 512 B sector).
     * Read the covering sector(s) and copy the requested window. */
    uint8_t *dst = (uint8_t *)buf;
    uint32_t cur_lba = lba;
    uint32_t cur_off = off;
    uint32_t remain = bufsize;
    while (remain > 0) {
        esp_err_t e = sdmmc_read_sectors(bsp_sdcard, s_secbuf, cur_lba, 1);
        if (e != ESP_OK) return -1;
        uint32_t n = (uint32_t)(sec - cur_off);
        if (n > remain) n = remain;
        memcpy(dst, s_secbuf + cur_off, n);
        dst += n; remain -= n; cur_lba++; cur_off = 0;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t off, uint8_t *buf, uint32_t bufsize) {
    (void)lun;
    if (!bsp_sdcard || bufsize == 0) return -1;
    size_t sec = bsp_sdcard->csd.sector_size;
    if (sec == 0 || sec > sizeof(s_secbuf)) return -1;

    /* Fast path: aligned, whole-sector write */
    if (off == 0 && (bufsize % sec) == 0) {
        esp_err_t e = sdmmc_write_sectors(bsp_sdcard, buf, lba, bufsize / sec);
        return (e == ESP_OK) ? (int32_t)bufsize : -1;
    }

    /* General path: read-modify-write for partial-sector writes */
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t cur_lba = lba;
    uint32_t cur_off = off;
    uint32_t remain = bufsize;
    while (remain > 0) {
        esp_err_t e = sdmmc_read_sectors(bsp_sdcard, s_secbuf, cur_lba, 1);
        if (e != ESP_OK) return -1;
        uint32_t n = (uint32_t)(sec - cur_off);
        if (n > remain) n = remain;
        memcpy(s_secbuf + cur_off, src, n);
        e = sdmmc_write_sectors(bsp_sdcard, s_secbuf, cur_lba, 1);
        if (e != ESP_OK) return -1;
        src += n; remain -= n; cur_lba++; cur_off = 0;
    }
    return (int32_t)bufsize;
}
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const cmd[16], void *buf, uint16_t bufsize) {
    (void)lun; (void)cmd; (void)buf; (void)bufsize; return -1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Init / Deinit
 * ═══════════════════════════════════════════════════════════════════════ */

static void tusb_task(void *arg) {
    (void)arg; while (1) { tud_task(); vTaskDelay(pdMS_TO_TICKS(1)); }
}

esp_err_t tinyusb_msc_init(void) {
    if (s_active) return ESP_OK;
    if (!bsp_sdcard) { ESP_LOGE(TAG,"SD card not mounted"); return ESP_ERR_INVALID_STATE; }

    ESP_LOGI(TAG, "Starting TinyUSB MSC...");

    /* Claim the internal FSLS PHY for USB OTG.
     * This swaps the USB D+/D- pads from USB Serial/JTAG → USB OTG. */
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target     = USB_PHY_TARGET_INT,
        .otg_mode   = USB_OTG_MODE_DEVICE,
        .otg_speed  = USB_PHY_SPEED_FULL,
        .otg_io_conf = NULL,
        .ext_io_conf = NULL,
    };
    esp_err_t phy_err = usb_new_phy(&phy_cfg, &s_phy_handle);
    if (phy_err != ESP_OK) {
        ESP_LOGE(TAG, "USB PHY claim failed: %s", esp_err_to_name(phy_err));
        return phy_err;
    }

    tusb_init();

    TaskHandle_t t;
    if (xTaskCreatePinnedToCore(tusb_task, "tusb_msc", 3072, NULL, tskIDLE_PRIORITY+3, &t, 1) != pdPASS) {
        ESP_LOGE(TAG, "Task failed"); return ESP_ERR_NO_MEM;
    }
    s_active = true;
    ESP_LOGI(TAG, "TinyUSB MSC ready — connect to PC via USB (GPIO19/20)");
    return ESP_OK;
}

bool tinyusb_msc_is_active(void) { return s_active; }
void tinyusb_msc_deinit(void) {
    s_active = false;
    if (s_phy_handle) {
        usb_del_phy(s_phy_handle);
        s_phy_handle = NULL;
    }
}
