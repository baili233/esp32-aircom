/*
 * WiFi manager: AP / STA / APSTA modes, async scan, STA auto-reconnect
 * with exponential backoff. All heavy operations (stop/start/scan) run in
 * the wifi task; httpd handlers only post commands.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "wifi_manager.h"

static const char *TAG = "wifi";

#define SCAN_MAX_APS      20
#define RECONNECT_MAX_MS  (30 * 1000)

typedef enum {
    WIFI_CMD_APPLY = 1,
    WIFI_CMD_SCAN  = 2,
} wifi_cmd_type_t;

typedef struct {
    uint8_t       type;
    app_wifi_cfg_t cfg;
} wifi_cmd_t;

typedef enum {
    SCAN_IDLE = 0,
    SCAN_RUNNING,
    SCAN_DONE,
} scan_state_t;

static app_wifi_cfg_t s_cfg;
static wifi_status_t  s_status;
static SemaphoreHandle_t s_lock;      /* guards s_status + scan results */
static QueueHandle_t  s_cmd_queue;
static esp_timer_handle_t s_reconnect_timer;
static int            s_retry_count;
static esp_netif_t   *s_ap_netif;

static wifi_ap_record_t s_scan_aps[SCAN_MAX_APS];
static int    s_scan_count;
static scan_state_t s_scan_state = SCAN_IDLE;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static wifi_mode_t map_mode(uint8_t m) {
    switch (m) {
        case APP_WIFI_STA_ONLY: return WIFI_MODE_STA;
        case APP_WIFI_APSTA:    return WIFI_MODE_APSTA;
        default:                return WIFI_MODE_AP;
    }
}

/* ---- reconnect with exponential backoff ---- */

static void reconnect_timer_cb(void *arg) {
    ESP_LOGI(TAG, "reconnecting STA (retry #%d)...", s_retry_count);
    esp_wifi_connect();
}

static void schedule_reconnect(void) {
    uint32_t delay_ms = 500U << (s_retry_count > 6 ? 6 : s_retry_count);
    if (delay_ms > RECONNECT_MAX_MS) {
        delay_ms = RECONNECT_MAX_MS;
    }
    s_retry_count++;
    ESP_LOGW(TAG, "STA disconnected, reconnect in %lu ms", (unsigned long)delay_ms);
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, delay_ms * 1000U);
}

/* ---- event handlers (run in esp_event task, keep them quick) ---- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                if (s_cfg.sta_ssid[0]) {
                    esp_wifi_connect();
                }
                break;
            case WIFI_EVENT_STA_CONNECTED:
                s_retry_count = 0;
                lock();
                s_status.sta_fail_reason = 0;
                unlock();
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *d = data;
                lock();
                s_status.sta_connected = false;
                s_status.sta_ip[0] = '\0';
                s_status.sta_fail_reason = d->reason;
                unlock();
                if (s_cfg.sta_ssid[0]) {
                    schedule_reconnect();
                }
                break;
            }
            case WIFI_EVENT_SCAN_DONE: {
                uint16_t n = SCAN_MAX_APS;
                if (esp_wifi_scan_get_ap_records(&n, s_scan_aps) == ESP_OK) {
                    s_scan_count = n;
                } else {
                    s_scan_count = 0;
                }
                lock();
                s_status.scanning = false;
                s_scan_state = SCAN_DONE;
                unlock();
                ESP_LOGI(TAG, "scan done, %d APs", s_scan_count);
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *e = data;
            char ip[16];
            esp_ip4addr_ntoa(&e->ip_info.ip, ip, sizeof(ip));
            lock();
            s_status.sta_connected = true;
            strlcpy(s_status.sta_ip, ip, sizeof(s_status.sta_ip));
            unlock();
            s_retry_count = 0;
            ESP_LOGI(TAG, "STA got IP: %s", ip);
        } else if (id == IP_EVENT_STA_LOST_IP) {
            lock();
            s_status.sta_connected = false;
            s_status.sta_ip[0] = '\0';
            unlock();
        }
    }
}

/* ---- apply / scan (run in wifi task) ---- */

static void do_apply(const app_wifi_cfg_t *cfg) {
    s_cfg = *cfg;
    s_retry_count = 0;
    esp_timer_stop(s_reconnect_timer);

    ESP_LOGI(TAG, "applying wifi config: mode=%u sta=\"%s\" ap=\"%s\"",
             cfg->mode, cfg->sta_ssid, cfg->ap_ssid);

    esp_wifi_stop();

    wifi_config_t sta = { 0 };
    strlcpy((char *)sta.sta.ssid, cfg->sta_ssid, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, cfg->sta_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* accept any security */

    wifi_config_t ap = { 0 };
    strlcpy((char *)ap.ap.ssid, cfg->ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, cfg->ap_pass, sizeof(ap.ap.password));
    ap.ap.ssid_len = strnlen(cfg->ap_ssid, sizeof(cfg->ap_ssid));
    ap.ap.channel = 0;                 /* auto: follows STA in APSTA mode */
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    wifi_mode_t mode = map_mode(cfg->mode);
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    }
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    }
    ESP_ERROR_CHECK(esp_wifi_start());

    lock();
    s_status.mode = cfg->mode;
    s_status.sta_connected = false;
    s_status.sta_ip[0] = '\0';
    s_status.sta_fail_reason = 0;
    s_status.ap_ip[0] = '\0';
    unlock();
    /* STA connect happens on WIFI_EVENT_STA_START; AP IP is filled on
     * the first status query after AP start. */
}

static void do_scan(void) {
    lock();
    s_scan_state = SCAN_RUNNING;
    s_status.scanning = true;
    s_scan_count = 0;
    unlock();

    wifi_scan_config_t sc = { 0 };
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        lock();
        s_scan_state = SCAN_DONE;
        s_scan_count = 0;
        s_status.scanning = false;
        unlock();
    } else {
        ESP_LOGI(TAG, "scan started");
    }
}

static void wifi_task(void *arg) {
    wifi_cmd_t cmd;
    for (;;) {
        xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY);
        switch (cmd.type) {
            case WIFI_CMD_APPLY:
                do_apply(&cmd.cfg);
                break;
            case WIFI_CMD_SCAN:
                do_scan();
                break;
            default:
                break;
        }
    }
}

/* ---- public API ---- */

esp_err_t wifi_manager_init(const app_wifi_cfg_t *cfg) {
    memset(&s_status, 0, sizeof(s_status));
    s_cfg = *cfg;

    s_lock = xSemaphoreCreateMutex();
    s_cmd_queue = xQueueCreate(4, sizeof(wifi_cmd_t));

    const esp_timer_create_args_t targs = {
        .name = "sta_recon",
        .callback = reconnect_timer_cb,
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    /* netifs: create both once; mode switching reuses them */
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));

    if (xTaskCreatePinnedToCore(wifi_task, "wifi_mgr", 4096, NULL, 3, NULL,
                                tskNO_AFFINITY) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* Initial bring-up goes through the same apply path */
    wifi_cmd_t cmd = { .type = WIFI_CMD_APPLY, .cfg = *cfg };
    xQueueSend(s_cmd_queue, &cmd, portMAX_DELAY);

    /* Wait briefly so early /api/status calls see real state */
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

void wifi_manager_apply(const app_wifi_cfg_t *cfg) {
    wifi_cmd_t cmd = { .type = WIFI_CMD_APPLY };
    cmd.cfg = *cfg;
    xQueueSend(s_cmd_queue, &cmd, 0);
}

void wifi_manager_request_scan(void) {
    /* Ignore request if one is already running */
    lock();
    scan_state_t st = s_scan_state;
    unlock();
    if (st == SCAN_RUNNING) {
        return;
    }
    wifi_cmd_t cmd = { .type = WIFI_CMD_SCAN };
    xQueueSend(s_cmd_queue, &cmd, 0);
}

int wifi_manager_scan_results(wifi_ap_record_t *out_aps, int max, int *out_count) {
    lock();
    scan_state_t st = s_scan_state;
    int n = s_scan_count;
    if (st == SCAN_DONE && out_aps) {
        int c = n < max ? n : max;
        memcpy(out_aps, s_scan_aps, c * sizeof(wifi_ap_record_t));
        n = c;
    }
    unlock();
    if (out_count) {
        *out_count = n;
    }
    return st == SCAN_RUNNING ? 1 : 0;
}

void wifi_manager_get_status(wifi_status_t *out) {
    lock();
    *out = s_status;
    unlock();

    out->mode = s_cfg.mode;

    /* live AP info */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_STA) {
        wifi_sta_list_t stas;
        if (esp_wifi_ap_get_sta_list(&stas) == ESP_OK) {
            out->ap_clients = stas.num;
        }
        if (s_ap_netif) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK &&
                ip.ip.addr != 0) {
                esp_ip4addr_ntoa(&ip.ip, out->ap_ip, sizeof(out->ap_ip));
            }
        }
    } else {
        out->ap_clients = 0;
    }

    /* live STA info */
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_AP) {
        wifi_ap_record_t apinfo;
        if (esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK) {
            out->sta_started = true;
            out->sta_rssi = apinfo.rssi;
        } else {
            out->sta_started = s_cfg.sta_ssid[0] != '\0';
            out->sta_rssi = 0;
        }
        out->sta_connected = out->sta_ip[0] != '\0';
    } else {
        out->sta_started = false;
        out->sta_rssi = 0;
    }
}
