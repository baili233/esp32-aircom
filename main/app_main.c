/*
 * AirCom - wireless serial terminal for ESP32-S3.
 *
 * UART1 (GPIO17/18) <-> WebSocket terminal served by the on-chip HTTP
 * server. Configuration (WiFi mode, serial parameters) persists in NVS
 * and is editable from the web terminal itself.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "uart_bridge.h"
#include "http_server.h"

static const char *TAG = "aircom";

void app_main(void) {
    /* NVS: erase and retry when partition layout changed */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    app_cfg_t cfg;
    ESP_ERROR_CHECK(app_cfg_load(&cfg));

    ESP_LOGI(TAG, "AirCom v%s starting", APP_VERSION);

    ESP_ERROR_CHECK(wifi_manager_init(&cfg.wifi));
    ESP_ERROR_CHECK(uart_bridge_init(&cfg.uart));
    ESP_ERROR_CHECK(http_server_start());

    ESP_LOGI(TAG, "ready - connect to AP \"%s\" (pass \"%s\"), "
             "then open the terminal in a browser",
             cfg.wifi.ap_ssid, cfg.wifi.ap_pass);
}
