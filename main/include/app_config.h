#pragma once
/*
 * AirCom global configuration: types, defaults and NVS persistence.
 */
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_VERSION "1.0.0"

/* UART1 on GPIO17(TX) / GPIO18(RX) */
#define APP_UART_NUM    1
#define APP_UART_TX_PIN 17
#define APP_UART_RX_PIN 18

/* WiFi operation modes (persisted, web-switchable) */
typedef enum {
    APP_WIFI_AP_ONLY  = 0,   /* device emits its own hotspot            */
    APP_WIFI_STA_ONLY = 1,   /* device joins an existing network        */
    APP_WIFI_APSTA    = 2,   /* hotspot + station (rescue access point) */
} app_wifi_mode_t;

#define APP_WIFI_SSID_MAX 33   /* 32 chars + NUL */
#define APP_WIFI_PASS_MAX 65   /* 64 chars + NUL */

typedef struct {
    uint8_t mode;                                  /* app_wifi_mode_t */
    char    sta_ssid[APP_WIFI_SSID_MAX];
    char    sta_pass[APP_WIFI_PASS_MAX];
    char    ap_ssid[APP_WIFI_SSID_MAX];
    char    ap_pass[APP_WIFI_PASS_MAX];
} app_wifi_cfg_t;

/* parity encoding: 0=none 1=odd 2=even (mapped to uart_parity_t in uart_bridge) */
#define APP_PARITY_NONE 0
#define APP_PARITY_ODD  1
#define APP_PARITY_EVEN 2

/* stop bits encoding matches uart_stop_bits_t: 1=1bit 2=1.5bits 3=2bits */
#define APP_STOP_1   1
#define APP_STOP_15  2
#define APP_STOP_2   3

typedef struct {
    uint32_t baud;
    uint8_t  data_bits;   /* 5..8 */
    uint8_t  parity;      /* APP_PARITY_x */
    uint8_t  stop_bits;   /* APP_STOP_x */
} app_uart_cfg_t;

typedef struct {
    app_wifi_cfg_t wifi;
    app_uart_cfg_t uart;
} app_cfg_t;

/* ---- quick-send snippets (stored under their own NVS key) ----
 * Common commands/phrases the user keeps around and fills into the
 * terminal input. Lengths are UTF-8 bytes including the NUL. */
#define APP_SNIPPET_MAX        12
#define APP_SNIPPET_LABEL_MAX  48    /* UTF-8 bytes incl. NUL */
#define APP_SNIPPET_TEXT_MAX   320   /* UTF-8 bytes incl. NUL */

typedef struct {
    char label[APP_SNIPPET_LABEL_MAX];  /* optional short name        */
    char text[APP_SNIPPET_TEXT_MAX];    /* payload sent to the UART   */
} app_snippet_t;

typedef struct {
    uint8_t count;
    app_snippet_t items[APP_SNIPPET_MAX];
} app_snippets_t;

/* Fill factory defaults (AP SSID gets a MAC-derived suffix). */
void app_cfg_defaults(app_cfg_t *cfg);

/* Load from NVS; falls back to defaults (and persists them) if absent/corrupt. */
esp_err_t app_cfg_load(app_cfg_t *cfg);

/* Snapshot accessors; setters update NVS atomically. */
void        app_cfg_get(app_cfg_t *out);
esp_err_t   app_cfg_set_uart(const app_uart_cfg_t *uart);
esp_err_t   app_cfg_set_wifi(const app_wifi_cfg_t *wifi);

/* Quick-send snippets; empty list by default.
 *
 * The store is ~4.4 KB, so it lives in a heap-allocated singleton. Callers
 * only ever touch it through the returned pointer and must never take a
 * by-value copy, or they will blow the (8 KB) httpd task stack. */
app_snippets_t *app_cfg_snippets(void);
esp_err_t       app_cfg_set_snippets(const app_snippets_t *snips);

#ifdef __cplusplus
}
#endif
