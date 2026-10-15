/*
 * AirCom configuration persistence: one versioned blob in NVS.
 */
#include <string.h>
#include <stdlib.h>
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
#define NVS_KEY_SNIPS "snips"
#define CFG_MAGIC     0xAC0F1990U
#define CFG_VERSION   1
#define SNIP_MAGIC    0xAC0F5A11U
#define SNIP_VERSION  1

/* On-flash wrapper: magic + version guard so corrupt/foreign blobs fall
 * back to defaults instead of producing garbage SSIDs. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_size;
    app_cfg_t cfg;
} cfg_blob_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t payload_size;
    app_snippets_t snips;
} snip_blob_t;

static app_cfg_t s_cfg;
static app_snippets_t *s_snips;   /* heap: ~4.4 KB, never copied by value */
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

/* Drop empty entries and clamp the count, compacting in place so we never
 * need a second ~4.4 KB copy. */
static void snippets_normalize(app_snippets_t *s) {
    if (s->count > APP_SNIPPET_MAX) {
        s->count = APP_SNIPPET_MAX;
    }

    uint8_t out = 0;
    for (uint8_t i = 0; i < s->count; i++) {
        if (s->items[i].text[0] == '\0') {
            continue;
        }
        if (out != i) {
            s->items[out] = s->items[i];
        }
        out++;
    }
    if (out < s->count) {
        memset(&s->items[out], 0, sizeof(s->items[0]) * (s->count - out));
    }
    s->count = out;
}

static void snippets_load(void) {
    if (!s_snips) {
        return;
    }
    memset(s_snips, 0, sizeof(*s_snips));

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    /* Read into a heap wrapper: the blob is ~4.4 KB, far too big for the stack. */
    snip_blob_t *blob = malloc(sizeof(*blob));
    if (!blob) {
        nvs_close(h);
        ESP_LOGW(TAG, "no memory to load snippets");
        return;
    }
    size_t len = sizeof(*blob);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_SNIPS, blob, &len);
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(*blob) &&
        blob->magic == SNIP_MAGIC && blob->version == SNIP_VERSION &&
        blob->payload_size == sizeof(app_snippets_t)) {
        *s_snips = blob->snips;
        snippets_normalize(s_snips);
        ESP_LOGI(TAG, "loaded %u quick-send snippet(s)", (unsigned)s_snips->count);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "stored snippets invalid, ignoring");
    }
    free(blob);
}

static esp_err_t snippets_save(const app_snippets_t *s) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    /* Build the wrapper on the heap as well. */
    snip_blob_t *blob = malloc(sizeof(*blob));
    if (!blob) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    blob->magic = SNIP_MAGIC;
    blob->version = SNIP_VERSION;
    blob->payload_size = sizeof(app_snippets_t);
    memcpy(&blob->snips, s, sizeof(*s));

    err = nvs_set_blob(h, NVS_KEY_SNIPS, blob, sizeof(*blob));
    free(blob);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
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
    if (!s_snips) {
        s_snips = calloc(1, sizeof(*s_snips));
        if (!s_snips) {
            ESP_LOGE(TAG, "no memory for snippet store");
        }
    }
    snippets_load();
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

app_snippets_t *app_cfg_snippets(void) {
    if (!s_snips) {
        s_snips = calloc(1, sizeof(*s_snips));
    }
    return s_snips;
}

/* Normalize and persist the caller's (heap-allocated) set; only publish it
 * to the in-RAM singleton once NVS has accepted it. */
esp_err_t app_cfg_set_snippets(const app_snippets_t *snips) {
    if (!s_snips) {
        return ESP_ERR_INVALID_STATE;
    }
    app_snippets_t *clean = malloc(sizeof(*clean));
    if (!clean) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(clean, snips, sizeof(*clean));
    snippets_normalize(clean);

    lock();
    esp_err_t err = snippets_save(clean);
    if (err == ESP_OK) {
        memcpy(s_snips, clean, sizeof(*s_snips));
        ESP_LOGI(TAG, "stored %u quick-send snippet(s)", (unsigned)s_snips->count);
    }
    unlock();
    free(clean);
    return err;
}
