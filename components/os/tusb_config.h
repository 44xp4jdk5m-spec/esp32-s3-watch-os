/**
 * @file tusb_config.h — TinyUSB config for ESP32-S3 MSC only.
 */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_MCU               OPT_MCU_ESP32S3
#define CFG_TUSB_OS                OPT_OS_FREERTOS
#define CFG_TUSB_RHPORT0_MODE      OPT_MODE_DEVICE
#define CFG_TUSB_DEBUG             0
#define CFG_TUD_ENABLED            1
#define CFG_TUD_MAX_SPEED          OPT_SPEED_FULL

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN         __attribute__((aligned(4)))
#define CFG_TUD_ATTR               __attribute__((aligned(4)))

#define CFG_TUD_CDC                0
#define CFG_TUD_MSC                1
#define CFG_TUD_HID                0
#define CFG_TUD_MIDI               0
#define CFG_TUD_VENDOR             0

/* Class buffer sized to a full SD sector so each READ10/WRITE10 callback
 * gets one aligned 512 B block (kept in sync with bsp_sdcard->csd.sector_size).
 * Bulk EP max packet stays 64 B at Full Speed; TinyUSB chunks the transfer. */
#define CFG_TUD_MSC_EP_BUFSIZE     512
#define CFG_TUD_MSC_MAX_LUN        1
#define CFG_TUD_DESC_STR_LEN       32
#define CFG_TUD_EP0_BUF_SIZE       64
#define CFG_TUD_ENDPOINT0_SIZE     64

#ifdef __cplusplus
}
#endif
