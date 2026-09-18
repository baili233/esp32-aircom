/*
 * HTTP server: embedded terminal page, REST API and /ws endpoint.
 *
 * esp_http_server is single-threaded: every handler validates input,
 * posts work to the responsible module, and returns. Nothing here may
 * block (no scans, no wifi restarts, no long UART writes in-place).
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "uart_bridge.h"
#include "ws_hub.h"
#include "http_server.h"

static const char *TAG = "http";

/* EMBED_TXTFILES symbols: friendly name + objcopy-compatible name */
extern const unsigned char index_html[] asm("_binary_index_html_start");
extern const unsigned int  index_html_length asm("index_html_length");

#define WS_FRAME_MAX  (16 * 1024)
#define CTRL_TEXT_MAX 512

static httpd_handle_t s_server;

/* ---------------- helpers ---------------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *json) {
    char *s = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "ok", true);
    return send_json(req, j);
}

static esp_err_t send_err(httpd_req_t *req, const char *msg) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* Read and fully drain the request body; parse as JSON. */
static cJSON *read_body_json(httpd_req_t *req, size_t maxlen) {
    char hl[16] = { 0 };
    if (httpd_req_get_hdr_value_str(req, "Content-Length", hl, sizeof(hl)) != ESP_OK) {
        return NULL;
    }
    int len = atoi(hl);
    if (len < 0) {
        return NULL;
    }

    char *buf = NULL;
    cJSON *json = NULL;
    if ((size_t)len <= maxlen) {
        buf = malloc(len + 1);
    }
    if (buf) {
        int got = 0;
        while (got < len) {
            int r = httpd_req_recv(req, buf + got, len - got);
            if (r <= 0) {
                break;
            }
            got += r;
        }
        buf[got] = '\0';
        json = cJSON_Parse(buf);
    } else {
        /* body too large (or OOM): drain and reject */
        char sink[256];
        int remaining = len;
        while (remaining > 0) {
            int r = httpd_req_recv(req, sink, remaining > (int)sizeof(sink) ? (int)sizeof(sink) : remaining);
            if (r <= 0) {
                break;
            }
            remaining -= r;
        }
    }
    free(buf);
    return json;
}

static double stop_bits_to_real(uint8_t s) {
    if (s == APP_STOP_15) return 1.5;
    if (s == APP_STOP_2)  return 2.0;
    return 1.0;
}

static bool real_to_stop_bits(double v, uint8_t *out) {
    if (v > 0.99 && v < 1.01) {
        *out = APP_STOP_1;
        return true;
    }
    if (v > 1.49 && v < 1.51) {
        *out = APP_STOP_15;
        return true;
    }
    if (v > 1.99 && v < 2.01) {
        *out = APP_STOP_2;
        return true;
    }
    return false;
}

static void json_add_uart(cJSON *parent, const app_uart_cfg_t *u) {
    cJSON *j = cJSON_AddObjectToObject(parent, "uart");
    cJSON_AddNumberToObject(j, "baud", u->baud);
    cJSON_AddNumberToObject(j, "data_bits", u->data_bits);
    cJSON_AddNumberToObject(j, "parity", u->parity);
    cJSON_AddNumberToObject(j, "stop_bits", stop_bits_to_real(u->stop_bits));
}

static const char *mode_str(uint8_t m) {
    switch (m) {
    case APP_WIFI_STA_ONLY: return "STA";
    case APP_WIFI_APSTA:    return "AP+STA";
    default:                return "AP";
    }
}

/* ---------------- static page ---------------- */

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)index_html, index_html_length);
}

static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ---------------- REST API ---------------- */

static esp_err_t status_handler(httpd_req_t *req) {
    app_cfg_t cfg;
    app_cfg_get(&cfg);

    wifi_status_t w;
    wifi_manager_get_status(&w);

    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "version", APP_VERSION);

    cJSON *jw = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddStringToObject(jw, "mode_str", mode_str(cfg.wifi.mode));
    cJSON_AddNumberToObject(jw, "mode", cfg.wifi.mode);
    cJSON_AddStringToObject(jw, "sta_ssid", cfg.wifi.sta_ssid);
    cJSON_AddBoolToObject(jw, "sta_started", w.sta_started);
    cJSON_AddBoolToObject(jw, "sta_connected", w.sta_connected);
    cJSON_AddStringToObject(jw, "sta_ip", w.sta_ip);
    cJSON_AddNumberToObject(jw, "sta_rssi", w.sta_rssi);
    cJSON_AddNumberToObject(jw, "fail_reason", w.sta_fail_reason);
    cJSON_AddStringToObject(jw, "ap_ssid", cfg.wifi.ap_ssid);
    cJSON_AddStringToObject(jw, "ap_ip", w.ap_ip);
    cJSON_AddNumberToObject(jw, "ap_clients", w.ap_clients);
    cJSON_AddBoolToObject(jw, "scanning", w.scanning);

    json_add_uart(j, &cfg.uart);

    cJSON *js = cJSON_AddObjectToObject(j, "stats");
    cJSON_AddNumberToObject(js, "ws_clients", ws_hub_client_count());
    cJSON_AddNumberToObject(js, "ws_drops", ws_hub_drop_count());
    cJSON_AddNumberToObject(js, "uart_rx", uart_bridge_rx_count());
    cJSON_AddNumberToObject(js, "uart_tx", uart_bridge_tx_count());
    cJSON_AddNumberToObject(js, "uart_ovf", uart_bridge_ovf_count());
    cJSON_AddNumberToObject(js, "heap_free", esp_get_free_heap_size());
    cJSON_AddNumberToObject(js, "uptime", (double)(esp_timer_get_time() / 1000000ULL));

    return send_json(req, j);
}

static esp_err_t config_get_handler(httpd_req_t *req) {
    app_cfg_t cfg;
    app_cfg_get(&cfg);

    cJSON *j = cJSON_CreateObject();
    cJSON *jw = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddNumberToObject(jw, "mode", cfg.wifi.mode);
    cJSON_AddStringToObject(jw, "sta_ssid", cfg.wifi.sta_ssid);
    cJSON_AddStringToObject(jw, "sta_pass", cfg.wifi.sta_pass[0] ? "***" : "");
    cJSON_AddStringToObject(jw, "ap_ssid", cfg.wifi.ap_ssid);
    cJSON_AddStringToObject(jw, "ap_pass", cfg.wifi.ap_pass[0] ? "***" : "");

    json_add_uart(j, &cfg.uart);
    return send_json(req, j);
}

static esp_err_t uart_post_handler(httpd_req_t *req) {
    app_cfg_t cfg;
    app_cfg_get(&cfg);
    app_uart_cfg_t u = cfg.uart;

    cJSON *j = read_body_json(req, 512);
    if (!j) {
        return send_err(req, "invalid json");
    }

    cJSON *jb = cJSON_GetObjectItem(j, "baud");
    cJSON *jd = cJSON_GetObjectItem(j, "data_bits");
    cJSON *jp = cJSON_GetObjectItem(j, "parity");
    cJSON *js = cJSON_GetObjectItem(j, "stop_bits");

    bool ok = true;
    if (cJSON_IsNumber(jb)) {
        int b = (int)jb->valuedouble;
        if (b < 300 || b > 2000000) {
            ok = false;
        } else {
            u.baud = (uint32_t)b;
        }
    }
    if (ok && cJSON_IsNumber(jd)) {
        int d = (int)jd->valuedouble;
        if (d < 5 || d > 8) {
            ok = false;
        } else {
            u.data_bits = (uint8_t)d;
        }
    }
    if (ok && cJSON_IsNumber(jp)) {
        int p = (int)jp->valuedouble;
        if (p < 0 || p > 2) {
            ok = false;
        } else {
            u.parity = (uint8_t)p;
        }
    }
    if (ok && cJSON_IsNumber(js)) {
        if (!real_to_stop_bits(js->valuedouble, &u.stop_bits)) {
            ok = false;
        }
    }
    cJSON_Delete(j);

    if (!ok) {
        return send_err(req, "uart parameter out of range");
    }

    uart_bridge_apply(&u);
    esp_err_t err = app_cfg_set_uart(&u);
    if (err != ESP_OK) {
        return send_err(req, "nvs save failed");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    json_add_uart(resp, &u);
    return send_json(req, resp);
}

static bool valid_ssid(const char *s) {
    size_t n = strnlen(s, APP_WIFI_SSID_MAX);
    return n > 0 && n < APP_WIFI_SSID_MAX;
}

static esp_err_t wifi_post_handler(httpd_req_t *req) {
    app_cfg_t cfg;
    app_cfg_get(&cfg);
    app_wifi_cfg_t w = cfg.wifi;

    cJSON *j = read_body_json(req, 1024);
    if (!j) {
        return send_err(req, "invalid json");
    }

    bool ok = true;
    char reason[64] = "bad parameter";

    cJSON *jm = cJSON_GetObjectItem(j, "mode");
    if (cJSON_IsNumber(jm)) {
        int m = (int)jm->valuedouble;
        if (m < 0 || m > 2) {
            ok = false;
            snprintf(reason, sizeof(reason), "mode must be 0..2");
        } else {
            w.mode = (uint8_t)m;
        }
    }

    if (ok) {
        cJSON *item;
        if ((item = cJSON_GetObjectItem(j, "sta_ssid")) && cJSON_IsString(item)) {
            strlcpy(w.sta_ssid, item->valuestring, sizeof(w.sta_ssid));
        }
        if ((item = cJSON_GetObjectItem(j, "sta_pass")) && cJSON_IsString(item)) {
            strlcpy(w.sta_pass, item->valuestring, sizeof(w.sta_pass));
        }
        if ((item = cJSON_GetObjectItem(j, "ap_ssid")) && cJSON_IsString(item)) {
            strlcpy(w.ap_ssid, item->valuestring, sizeof(w.ap_ssid));
        }
        if ((item = cJSON_GetObjectItem(j, "ap_pass")) && cJSON_IsString(item)) {
            if (strcmp(item->valuestring, "***") != 0) {  /* untouched masked field */
                strlcpy(w.ap_pass, item->valuestring, sizeof(w.ap_pass));
            }
        }

        if (w.mode != APP_WIFI_STA_ONLY) {
            size_t pl = strnlen(w.ap_pass, APP_WIFI_PASS_MAX);
            if (pl != 0 && pl < 8) {
                ok = false;
                snprintf(reason, sizeof(reason), "AP password must be >= 8 chars or empty");
            }
            if (!valid_ssid(w.ap_ssid)) {
                ok = false;
                snprintf(reason, sizeof(reason), "AP SSID required");
            }
        }
        if (w.mode != APP_WIFI_AP_ONLY && w.sta_ssid[0] &&
            strnlen(w.sta_ssid, APP_WIFI_SSID_MAX) >= APP_WIFI_SSID_MAX) {
            ok = false;
            snprintf(reason, sizeof(reason), "STA SSID too long");
        }
    }

    cJSON_Delete(j);
    if (!ok) {
        return send_err(req, reason);
    }

    /* mask echo: never store "***" as a password */
    if (strcmp(w.sta_pass, "***") == 0) {
        app_cfg_t cur;
        app_cfg_get(&cur);
        strlcpy(w.sta_pass, cur.wifi.sta_pass, sizeof(w.sta_pass));
    }

    esp_err_t err = app_cfg_set_wifi(&w);
    if (err != ESP_OK) {
        return send_err(req, "nvs save failed");
    }
    wifi_manager_apply(&w);
    return send_ok(req);
}

static esp_err_t scan_post_handler(httpd_req_t *req) {
    wifi_manager_request_scan();
    return send_ok(req);
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    wifi_ap_record_t aps[10];
    int count = 0;
    int running = wifi_manager_scan_results(aps, 10, &count);

    cJSON *j = cJSON_CreateObject();
    if (running) {
        cJSON_AddStringToObject(j, "state", "scanning");
    } else {
        cJSON_AddStringToObject(j, "state", "done");
        cJSON *arr = cJSON_AddArrayToObject(j, "aps");
        for (int i = 0; i < count; i++) {
            cJSON *a = cJSON_CreateObject();
            char ssid[33];
            memcpy(ssid, aps[i].ssid, 32);
            ssid[32] = '\0';
            cJSON_AddStringToObject(a, "ssid", ssid);
            cJSON_AddNumberToObject(a, "rssi", aps[i].rssi);
            cJSON_AddNumberToObject(a, "chan", aps[i].primary);
            cJSON_AddNumberToObject(a, "auth", aps[i].authmode);
            cJSON_AddItemToArray(arr, a);
        }
    }
    return send_json(req, j);
}

static void reboot_timer_cb(void *arg) {
    esp_restart();
}

static esp_err_t reboot_post_handler(httpd_req_t *req) {
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t args = {
            .name = "reboot",
            .callback = reboot_timer_cb,
        };
        esp_timer_create(&args, &t);
    }
    esp_timer_stop(t);
    esp_timer_start_once(t, 500 * 1000);
    ESP_LOGW(TAG, "reboot requested via web");
    return send_ok(req);
}

/* ---------------- WebSocket ---------------- */

static esp_err_t ws_send_text_req(httpd_req_t *req, const char *text) {
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    return httpd_ws_send_frame(req, &frame);
}

static void ws_handle_ctrl(httpd_req_t *req, const char *text) {
    cJSON *j = cJSON_Parse(text);
    if (!j) {
        return;
    }
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(j, "t"));
    if (t && strcmp(t, "ping") == 0) {
        ws_send_text_req(req, "{\"t\":\"pong\"}");
    } else if (t && strcmp(t, "hello") == 0) {
        app_cfg_t cfg;
        app_cfg_get(&cfg);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "t", "hello");
        cJSON_AddStringToObject(r, "version", APP_VERSION);
        json_add_uart(r, &cfg.uart);
        cJSON_AddNumberToObject(r, "clients", ws_hub_client_count());
        char *s = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        if (s) {
            ws_send_text_req(req, s);
            free(s);
        }
    }
    cJSON_Delete(j);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        /* initial handshake */
        ESP_LOGI(TAG, "ws client connected (fd=%d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);   /* probe length */
    if (ret != ESP_OK) {
        return ret;
    }
    if (pkt.len > WS_FRAME_MAX) {
        ESP_LOGW(TAG, "ws frame too large (%u), dropping client", (unsigned)pkt.len);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = NULL;
    if (pkt.len) {
        buf = malloc(pkt.len);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
        pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
    }

    if (pkt.type == HTTPD_WS_TYPE_BINARY && pkt.len) {
        uart_bridge_tx(pkt.payload, pkt.len);
    } else if (pkt.type == HTTPD_WS_TYPE_TEXT && pkt.len) {
        if (pkt.len <= CTRL_TEXT_MAX) {
            /* control frames must be NUL-terminated for cJSON */
            char *text = malloc(pkt.len + 1);
            if (text) {
                memcpy(text, pkt.payload, pkt.len);
                text[pkt.len] = '\0';
                ws_handle_ctrl(req, text);
                free(text);
            }
        }
    }
    /* control frames (PING/PONG/CLOSE) never reach this handler */

    free(buf);
    return ESP_OK;
}

/* ---------------- server lifecycle ---------------- */

static const httpd_uri_t uri_index   = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
static const httpd_uri_t uri_favicon = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler };
static const httpd_uri_t uri_status  = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
static const httpd_uri_t uri_cfg_get = { .uri = "/api/config", .method = HTTP_GET, .handler = config_get_handler };
static const httpd_uri_t uri_uart    = { .uri = "/api/uart", .method = HTTP_POST, .handler = uart_post_handler };
static const httpd_uri_t uri_wifi    = { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler };
static const httpd_uri_t uri_scan_g  = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = scan_get_handler };
static const httpd_uri_t uri_scan_p  = { .uri = "/api/wifi/scan", .method = HTTP_POST, .handler = scan_post_handler };
static const httpd_uri_t uri_reboot  = { .uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler };
static const httpd_uri_t uri_ws      = { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true };

esp_err_t http_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_open_sockets = 8;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 30;
    config.keep_alive_interval = 10;
    config.keep_alive_count = 3;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        return err;
    }

    httpd_register_uri_handler(s_server, &uri_index);
    httpd_register_uri_handler(s_server, &uri_favicon);
    httpd_register_uri_handler(s_server, &uri_status);
    httpd_register_uri_handler(s_server, &uri_cfg_get);
    httpd_register_uri_handler(s_server, &uri_uart);
    httpd_register_uri_handler(s_server, &uri_wifi);
    httpd_register_uri_handler(s_server, &uri_scan_g);
    httpd_register_uri_handler(s_server, &uri_scan_p);
    httpd_register_uri_handler(s_server, &uri_reboot);
    httpd_register_uri_handler(s_server, &uri_ws);

    ws_hub_init(s_server);
    ESP_LOGI(TAG, "http server on port 80, terminal at http://<device-ip>/");
    return ESP_OK;
}

void http_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
