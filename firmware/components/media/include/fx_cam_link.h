/*
 * UART link from the node to the ESP32-CAM. Requests are single lines; replies
 * are parsed by fx_frame_rx.c. Only the camera task calls these, and the
 * camera task runs at low priority: a slow or silent camera costs frames,
 * never alarm time.
 */
#ifndef FX_CAM_LINK_H
#define FX_CAM_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fx_frame_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart;
    bool ready;
    fx_frame_rx_t rx;
} fx_cam_link_t;

typedef struct {
    fx_frame_status_t status; /* FX_FRAME_PENDING here means the camera timed out */
    uint8_t sha256[32];
    uint32_t length;
    uint32_t width;
    uint32_t height;
    char error[64];
} fx_cam_result_t;

esp_err_t fx_cam_link_init(fx_cam_link_t *link, int uart_num, int tx_gpio, int rx_gpio, int baud);

/* One JPEG. Bytes go to `sink` as they arrive; the result carries the hash the
 * node computed, which equals the camera's when status is FX_FRAME_JPEG. */
fx_cam_result_t fx_cam_snap(fx_cam_link_t *link, const char *size, int quality, fx_frame_sink_t sink, void *sink_ctx,
                            uint32_t timeout_ms);

/* The self-test capture: the camera takes a frame and reports only its size. */
fx_cam_result_t fx_cam_check(fx_cam_link_t *link, uint32_t timeout_ms);

bool fx_cam_ping(fx_cam_link_t *link, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* FX_CAM_LINK_H */
