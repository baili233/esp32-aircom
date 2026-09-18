#pragma once
/*
 * WiFi manager: three switchable modes (AP / STA / APSTA), non-blocking
 * scan, STA reconnect with backoff.
 */
#include "esp_err.h"
#include "esp_wifi.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t mode;               /* active app_wifi_mode_t                */
    bool    sta_started;        /* STA interface up                      */
    bool    sta_connected;
    char    sta_ip[16];         /* "" when no IP                         */
    int8_t  sta_rssi;
    int     sta_fail_reason;    /* last disconnect reason, 0 = none      */
    char    ap_ip[16];          /* "" when AP inactive                   */
    uint8_t ap_clients;         /* stations connected to our AP          */
    bool    scanning;
} wifi_status_t;

/* Bring up WiFi according to cfg (called once from app_main). */
esp_err_t wifi_manager_init(const app_wifi_cfg_t *cfg);

/* Re-apply a new WiFi config asynchronously (safe to call from httpd). */
void wifi_manager_apply(const app_wifi_cfg_t *cfg);

/* Trigger a non-blocking scan; results fetched via wifi_manager_scan_results. */
void wifi_manager_request_scan(void);

/* Returns 1 while scanning, 0 with results ready. out_aps may be NULL. */
int wifi_manager_scan_results(wifi_ap_record_t *out_aps, int max, int *out_count);

void wifi_manager_get_status(wifi_status_t *out);

#ifdef __cplusplus
}
#endif
