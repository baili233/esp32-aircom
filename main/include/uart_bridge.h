#pragma once
/*
 * UART bridge: UART1 <-> ws_hub. Both directions are funneled through the
 * uart task so no other task ever blocks on the UART driver.
 */
#include "esp_err.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t uart_bridge_init(const app_uart_cfg_t *cfg);

/* Reconfigure at runtime (baud/format), takes effect after TX drains. */
esp_err_t uart_bridge_apply(const app_uart_cfg_t *cfg);

/* Queue bytes for TX (called from httpd WS handler). Returns queued length. */
size_t uart_bridge_tx(const uint8_t *data, size_t len);

/* Runtime reconfig callback type used by http_server. */
const app_uart_cfg_t *uart_bridge_cfg(void);

uint32_t uart_bridge_rx_count(void);
uint32_t uart_bridge_tx_count(void);
uint32_t uart_bridge_ovf_count(void);

#ifdef __cplusplus
}
#endif
