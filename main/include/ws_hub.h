#pragma once
/*
 * WebSocket hub: broadcast UART data to every connected WS client.
 * Sends are funneled through a TX queue + httpd work queue, never from
 * the UART task directly.
 */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the hub to a running httpd instance. */
void ws_hub_init(void *server);

/* Binary payload -> every WS client. Copies data; safe from any task. */
void ws_hub_broadcast(const uint8_t *data, size_t len);

/* JSON text frame -> every WS client (control messages). */
void ws_hub_broadcast_text(const char *json);

int     ws_hub_client_count(void);
uint32_t ws_hub_drop_count(void);

#ifdef __cplusplus
}
#endif
