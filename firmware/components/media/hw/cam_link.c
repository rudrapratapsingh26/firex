#include "fx_cam_link.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_timer.h"

#define RX_BUFFER 16384
#define CHUNK 1024

static const char *TAG = "cam_link";

esp_err_t fx_cam_link_init(fx_cam_link_t *link, int uart_num, int tx_gpio, int rx_gpio, int baud)
{
    ESP_RETURN_ON_FALSE(link && tx_gpio >= 0 && rx_gpio >= 0, ESP_ERR_INVALID_ARG, TAG, "pins required");
    memset(link, 0, sizeof(*link));
    link->uart = uart_num;
    const uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(uart_num, &cfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(uart_num, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG,
                        "uart pins");
    ESP_RETURN_ON_ERROR(uart_driver_install(uart_num, RX_BUFFER, 0, 0, NULL, 0), TAG, "uart driver");
    link->ready = true;
    return ESP_OK;
}

static fx_cam_result_t transact(fx_cam_link_t *link, const char *command, fx_frame_sink_t sink, void *sink_ctx,
                                uint32_t timeout_ms)
{
    fx_cam_result_t res;
    memset(&res, 0, sizeof(res));
    res.status = FX_FRAME_PENDING;
    if (!link || !link->ready) {
        res.status = FX_FRAME_ERROR;
        snprintf(res.error, sizeof(res.error), "link not initialised");
        return res;
    }

    fx_frame_rx_init(&link->rx, sink, sink_ctx);
    uart_flush_input(link->uart);
    uart_write_bytes(link->uart, command, strlen(command));

    uint8_t buf[CHUNK];
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (res.status == FX_FRAME_PENDING && esp_timer_get_time() < deadline) {
        const int n = uart_read_bytes(link->uart, buf, sizeof(buf), pdMS_TO_TICKS(50));
        size_t at = 0;
        while (n > 0 && at < (size_t)n && res.status == FX_FRAME_PENDING) {
            size_t used = 0;
            res.status = fx_frame_rx_feed(&link->rx, buf + at, (size_t)n - at, &used);
            at += used;
        }
    }

    if (res.status == FX_FRAME_PENDING) {
        snprintf(res.error, sizeof(res.error), "timeout after %lu ms", (unsigned long)timeout_ms);
    } else if (res.status == FX_FRAME_ERROR || (res.status == FX_FRAME_CHECK && !link->rx.check_ok)) {
        snprintf(res.error, sizeof(res.error), "%s", link->rx.error);
    }
    memcpy(res.sha256, link->rx.actual, 32);
    res.length = link->rx.length;
    res.width = link->rx.width;
    res.height = link->rx.height;
    return res;
}

fx_cam_result_t fx_cam_snap(fx_cam_link_t *link, const char *size, int quality, fx_frame_sink_t sink, void *sink_ctx,
                            uint32_t timeout_ms)
{
    char command[48];
    snprintf(command, sizeof(command), "SNAP %s %d\n", size ? size : "vga", quality);
    return transact(link, command, sink, sink_ctx, timeout_ms);
}

fx_cam_result_t fx_cam_check(fx_cam_link_t *link, uint32_t timeout_ms)
{
    return transact(link, "CHECK\n", NULL, NULL, timeout_ms);
}

bool fx_cam_ping(fx_cam_link_t *link, uint32_t timeout_ms)
{
    return transact(link, "PING\n", NULL, NULL, timeout_ms).status == FX_FRAME_PONG;
}
