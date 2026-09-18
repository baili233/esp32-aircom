/*
 * AirCom configuration persistence: one versioned blob in NVS.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "app_config.h"

static const char *TAG = "config";

#define NVS_NAMESPACE "aircom"
#define NVS_KEY_CFG   "cfg"
#define CFG_MAGIC     0xAC0F1990U
#define CFG_VERSION   1

/* On-flash wrapper: magic + version guard so corrupt/foreign blobs fall
 * back to defaults instead of producing garbage SSIDs. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_size;
    app_cfg_t cfg;
} cfg_blob_t;

static app_cfg_t s_cfg;
static SemaphoreHandle_t s_lock;

static void lock(void)   { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_lock); }

static app_uart_cfg_t default_uart(void) {
    app_uart_cfg_t u = {
        .baud = 115200,
        .data_bits = 8,
        .parity = APP_PARITY_NONE,
        .stop_bits = APP_STOP_1,
    };
    return u;
}

void app_cfg_defaults(app_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    cfg->uart = default_uart();

    cfg->wifi.mode = APP_WIFI_AP_ONLY;
    cfg->wifi.sta_ssid[0] = '\0';
    cfg->wifi.sta_pass[0] = '\0';

    /* AP SSID derives from the radio MAC: "AirCom-A1B2" */
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(cfg->wifi.ap_ssid, sizeof(cfg->wifi.ap_ssid),
             "AirCom-%02X%02X", mac[4], mac[5]);
    strlcpy(cfg->wifi.ap_pass, "aircom123", sizeof(cfg->wifi.ap_pass));
}

static esp_err_t save_blob(const app_cfg_t *cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    cfg_blob_t blob = {
        .magic = CFG_MAGIC,
        .version = CFG_VERSION,
        .payload_size = sizeof(*cfg),
        .cfg = *cfg,
    };
    err = nvs_set_blob(h, NVS_KEY_CFG, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_cfg_load(app_cfg_t *cfg) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateRecursiveMutex();
    }
    app_cfg_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no stored config (%s), using defaults", esp_err_to_name(err));
        goto use_defaults;
    }

    cfg_blob_t blob;
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, NVS_KEY_CFG, &blob, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(blob) ||
        blob.magic != CFG_MAGIC || blob.version != CFG_VERSION ||
        blob.payload_size != sizeof(*cfg)) {
        ESP_LOGW(TAG, "stored config invalid, using defaults");
        goto use_defaults;
    }
    *cfg = blob.cfg;
    ESP_LOGI(TAG, "config loaded (mode=%u baud=%lu ap_ssid=\"%s\")",
             cfg->wifi.mode, (unsigned long)cfg->uart.baud, cfg->wifi.ap_ssid);
    s_cfg = *cfg;
    return ESP_OK;

use_defaults:
    s_cfg = *cfg;
    esp_err_t serr = save_blob(&s_cfg);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "failed persisting defaults: %s", esp_err_to_name(serr));
    }
    return ESP_OK;
}

void app_cfg_get(app_cfg_t *out) {
    lock();
    *out = s_cfg;
    unlock();
}

esp_err_t app_cfg_set_uart(const app_uart_cfg_t *uart) {
    lock();
    s_cfg.uart = *uart;
    esp_err_t err = save_blob(&s_cfg);
    unlock();
    return err;
}

esp_err_t app_cfg_set_wifi(const app_wifi_cfg_t *wifi) {
    lock();
    s_cfg.wifi = *wifi;
    esp_err_t err = save_blob(&s_cfg);
    unlock();
    return err;
}
