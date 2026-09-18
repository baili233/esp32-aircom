/*
 * UART bridge: owns UART1. A single task performs
 *   - aggregated RX reads (10 ms timeout, <=1024 B per chunk) -> ws_hub
 *   - draining of a TX queue (fed by the WS handler) -> uart driver
 * so no other task in the system ever blocks on the UART driver.
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "uart_bridge.h"
#include "ws_hub.h"

static const char *TAG = "uart";

#define RX_CHUNK_MAX   1024
#define RX_RINGBUF     (8 * 1024)
#define TX_RINGBUF     (2 * 1024)
#define EVT_QUEUE_LEN  20
#define TX_QUEUE_LEN   32
#define UART_PRIO      10

typedef struct {
    size_t len;
    uint8_t data[];
} tx_msg_t;

typedef struct {
    app_uart_cfg_t cfg;
} uart_cmd_t;

static app_uart_cfg_t s_cfg;
static QueueHandle_t s_uart_evt_queue;
static QueueHandle_t s_tx_queue;
static QueueHandle_t s_cmd_queue;   /* reconfig requests from httpd */
static uint32_t s_rx_count, s_tx_count, s_ovf_count;

static uart_parity_t map_parity(uint8_t p) {
    switch (p) {
        case APP_PARITY_ODD:  return UART_PARITY_ODD;
        case APP_PARITY_EVEN: return UART_PARITY_EVEN;
        default:              return UART_PARITY_DISABLE;
    }
}

static uart_word_length_t map_data_bits(uint8_t b){
    switch (b) {
        case 5: return UART_DATA_5_BITS;
        case 6: return UART_DATA_6_BITS;
        case 7: return UART_DATA_7_BITS;
        default: return UART_DATA_8_BITS;
    }
}

static uart_stop_bits_t map_stop_bits(uint8_t s) {
    switch (s) {
        case APP_STOP_15: return UART_STOP_BITS_1_5;
        case APP_STOP_2:  return UART_STOP_BITS_2;
        default:          return UART_STOP_BITS_1;
    }
}

static void apply_params(const app_uart_cfg_t *c) {
    uart_wait_tx_idle_polling(APP_UART_NUM);
    uart_set_baudrate(APP_UART_NUM, c->baud);
    uart_set_word_length(APP_UART_NUM, map_data_bits(c->data_bits));
    uart_set_parity(APP_UART_NUM, map_parity(c->parity));
    uart_set_stop_bits(APP_UART_NUM, map_stop_bits(c->stop_bits));
}

static void drain_tx(void) {
    tx_msg_t *m;
    while (xQueueReceive(s_tx_queue, &m, 0) == pdTRUE) {
        int written = uart_write_bytes(APP_UART_NUM, m->data, m->len);
        if (written > 0) {
            s_tx_count += written;
        }
        free(m);
    }
}

static void uart_task(void *arg) {
    uint8_t buf[RX_CHUNK_MAX];
    for (;;) {
        /* pending reconfig: flush TX queue first, wait for the wire to
         * drain, then swap parameters (never blocks the httpd caller) */
        uart_cmd_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            drain_tx();
            apply_params(&cmd.cfg);
            s_cfg = cmd.cfg;
            ESP_LOGI(TAG, "reconfigured: %lu baud", (unsigned long)cmd.cfg.baud);
        }

        int n = uart_read_bytes(APP_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (n > 0) {
            s_rx_count += n;
            ws_hub_broadcast(buf, n);
        }
        drain_tx();

        /* Drain driver events: recover from FIFO/ringbuf overflow instead
         * of silently stalling. */
        uart_event_t evt;
        while (xQueueReceive(s_uart_evt_queue, &evt, 0) == pdTRUE) {
            if (evt.type == UART_FIFO_OVF || evt.type == UART_BUFFER_FULL) {
                ESP_LOGW(TAG, "uart overflow (evt=%d), flushing input", evt.type);
                uart_flush_input(APP_UART_NUM);
                s_ovf_count++;
            }
        }
    }
}

esp_err_t uart_bridge_init(const app_uart_cfg_t *cfg) {
    s_cfg = *cfg;

    uart_config_t uc = {
        .baud_rate = cfg->baud,
        .data_bits = map_data_bits(cfg->data_bits),
        .parity = map_parity(cfg->parity),
        .stop_bits = map_stop_bits(cfg->stop_bits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(APP_UART_NUM, &uc);
    if (err != ESP_OK) return err;
    err = uart_set_pin(APP_UART_NUM, APP_UART_TX_PIN, APP_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(APP_UART_NUM, RX_RINGBUF, TX_RINGBUF,
                              EVT_QUEUE_LEN, &s_uart_evt_queue, 0);
    if (err != ESP_OK) return err;

    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_msg_t *));
    s_cmd_queue = xQueueCreate(4, sizeof(uart_cmd_t));

    if (xTaskCreatePinnedToCore(uart_task, "uart_bridge", 4096, NULL,
                                UART_PRIO, NULL, tskNO_AFFINITY) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UART1 ready: %lu %u%c%u on TX=%d RX=%d",
             (unsigned long)cfg->baud, cfg->data_bits,
             cfg->parity == APP_PARITY_NONE ? 'N' : (cfg->parity == APP_PARITY_ODD ? 'O' : 'E'),
             cfg->stop_bits, APP_UART_TX_PIN, APP_UART_RX_PIN);
    return ESP_OK;
}

esp_err_t uart_bridge_apply(const app_uart_cfg_t *cfg) {
    uart_cmd_t cmd = { .cfg = *cfg };
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

size_t uart_bridge_tx(const uint8_t *data, size_t len) {
    tx_msg_t *m = malloc(sizeof(tx_msg_t) + len);
    if (!m) {
        return 0;
    }
    m->len = len;
    memcpy(m->data, data, len);
    if (xQueueSend(s_tx_queue, &m, 0) != pdTRUE) {
        free(m);
        return 0;
    }
    return len;
}

const app_uart_cfg_t *uart_bridge_cfg(void) {
    return &s_cfg;
}

uint32_t uart_bridge_rx_count(void) { return s_rx_count; }
uint32_t uart_bridge_tx_count(void) { return s_tx_count; }
uint32_t uart_bridge_ovf_count(void) { return s_ovf_count; }
