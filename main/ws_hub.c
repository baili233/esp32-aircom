/*
 * WebSocket hub: fans UART data out to every connected WS client.
 *
 * Sends never happen from the caller's task: every frame is wrapped in a
 * heap message, queued, and executed inside the httpd work queue via
 * httpd_queue_work + httpd_ws_send_frame_async (the only sanctioned way
 * to send outside a handler).
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "ws_hub.h"

static const char *TAG = "ws_hub";

#define TX_QUEUE_LEN 32
#define MAX_CLIENTS  16

typedef struct {
    int             fd;
    httpd_ws_type_t type;
    size_t          len;
    uint8_t         data[];
} ws_msg_t;

static httpd_handle_t s_server;
static QueueHandle_t  s_tx_queue;
static int            s_client_count;
static uint32_t       s_drop_count;

/* Runs in httpd context. */
static void ws_send_work(void *arg) {
    ws_msg_t *m = arg;
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = m->type,
        .payload = m->data,
        .len = m->len,
    };
    httpd_ws_send_frame_async(s_server, m->fd, &frame);
    free(m);
}

static void ws_tx_task(void *arg) {
    ws_msg_t *m;
    for (;;) {
        xQueueReceive(s_tx_queue, &m, portMAX_DELAY);
        if (httpd_queue_work(s_server, ws_send_work, m) != ESP_OK) {
            free(m);   /* server gone/queue full */
        }
    }
}

void ws_hub_init(void *server) {
    s_server = (httpd_handle_t)server;
    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(ws_msg_t *));
    xTaskCreatePinnedToCore(ws_tx_task, "ws_tx", 4096, NULL, 8, NULL, tskNO_AFFINITY);
}

/* Snapshot of currently connected WS fds. Returns count. */
static int collect_clients(int *fds) {
    if (!s_server) {
        return 0;
    }
    int raw[MAX_CLIENTS];
    size_t count = MAX_CLIENTS;
    if (httpd_get_client_list(s_server, &count, raw) != ESP_OK) {
        return 0;
    }
    int n = 0;
    for (size_t i = 0; i < count && n < MAX_CLIENTS; i++) {
        if (httpd_ws_get_fd_info(s_server, raw[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            fds[n++] = raw[i];
        }
    }
    return n;
}

static void send_to(int fd, httpd_ws_type_t type, const uint8_t *data, size_t len) {
    ws_msg_t *m = malloc(sizeof(ws_msg_t) + len);
    if (!m) {
        return;
    }
    m->fd = fd;
    m->type = type;
    m->len = len;
    if (len) {
        memcpy(m->data, data, len);
    }
    if (xQueueSend(s_tx_queue, &m, 0) != pdTRUE) {
        /* Slow consumer: drop rather than stall the data source. */
        s_drop_count++;
        free(m);
    }
}

void ws_hub_broadcast(const uint8_t *data, size_t len) {
    int fds[MAX_CLIENTS];
    int n = collect_clients(fds);
    for (int i = 0; i < n; i++) {
        send_to(fds[i], HTTPD_WS_TYPE_BINARY, data, len);
    }
}

void ws_hub_broadcast_text(const char *json) {
    int fds[MAX_CLIENTS];
    int n = collect_clients(fds);
    for (int i = 0; i < n; i++) {
        send_to(fds[i], HTTPD_WS_TYPE_TEXT, (const uint8_t *)json, strlen(json));
    }
}

int ws_hub_client_count(void) {
    int fds[MAX_CLIENTS];
    return collect_clients(fds);
}

uint32_t ws_hub_drop_count(void) {
    return s_drop_count;
}
