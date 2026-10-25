/*
 * FireX ESP32-CAM (AI-Thinker, OV2640), M2 image.
 *
 * Two serial ports:
 *
 *   node link  UART1 (GPIO 14 TX, GPIO 13 RX by default), wired to the node.
 *              PING, SNAP and CHECK. The node sends SNAP only on its CRITICAL
 *              transition, and only a fixed number of times per incident.
 *   console    UART0, the USB adapter. PING and CHECK. SNAP is refused unless the
 *              image was built with CONFIG_FIREX_CAM_BENCH_SHUTTER, which is off by
 *              default and makes every banner say "bench".
 *
 * Replies:
 *   FXPONG firex-cam <version> <bench|link-only>
 *   FXJPEG 1 <length> <sha256 hex> <width> <height>\n <length bytes> \nFXEND\n
 *   FXCHECK ok <length> <width> <height>        a frame was captured and discarded
 *   FXCHECK fail <reason>
 *   FXERR <reason>
 *
 * CHECK exists so the node's daily self-test proves the camera works without
 * any image leaving it.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "sdkconfig.h"

#define CONSOLE_PORT UART_NUM_0
#define LINK_PORT UART_NUM_1
#define CMD_LINE_MAX 64
#define FLASH_LED_GPIO 4
#define SETTLE_FRAMES 2

#ifdef CONFIG_FIREX_CAM_BENCH_SHUTTER
#define IMAGE_KIND "bench"
#else
#define IMAGE_KIND "link-only"
#endif

#ifdef CONFIG_FIREX_CAM_PRODUCTION
/* The console refusal of SNAP is a privacy lock only if nobody can flash it away. */
#if defined(CONFIG_FIREX_CAM_BENCH_SHUTTER) || !defined(CONFIG_SECURE_BOOT_V2_ENABLED) || \
    !defined(CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE) || !defined(CONFIG_SECURE_DISABLE_ROM_DL_MODE)
#error "a production camera image needs secure boot v2, release flash encryption, no ROM loader and no bench shutter"
#endif
#endif

static const char *TAG = "cam";

typedef struct {
    const char *name;
    framesize_t size;
} size_name_t;

static const size_name_t SIZES[] = {
    {"qvga", FRAMESIZE_QVGA}, {"vga", FRAMESIZE_VGA}, {"svga", FRAMESIZE_SVGA},
    {"xga", FRAMESIZE_XGA},   {"hd", FRAMESIZE_HD},   {"uxga", FRAMESIZE_UXGA},
};

static bool s_camera_up;
static framesize_t s_size = FRAMESIZE_VGA;
static int s_quality = 12;
static SemaphoreHandle_t s_camera_lock;

static void reply(uart_port_t port, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }
    uart_write_bytes(port, buf, (size_t)n);
}

static esp_err_t camera_start(void)
{
    /* AI-Thinker ESP32-CAM pin map. */
    const camera_config_t cfg = {
        .pin_pwdn = 32,
        .pin_reset = -1,
        .pin_xclk = 0,
        .pin_sccb_sda = 26,
        .pin_sccb_scl = 27,
        .pin_d7 = 35,
        .pin_d6 = 34,
        .pin_d5 = 39,
        .pin_d4 = 36,
        .pin_d3 = 21,
        .pin_d2 = 19,
        .pin_d1 = 18,
        .pin_d0 = 5,
        .pin_vsync = 25,
        .pin_href = 23,
        .pin_pclk = 22,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&cfg);
}

static bool parse_size(const char *word, framesize_t *out)
{
    for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
        if (strcasecmp(word, SIZES[i].name) == 0) {
            *out = SIZES[i].size;
            return true;
        }
    }
    return false;
}

/* Capture one frame after applying size and quality. Caller holds the lock and
 * returns the buffer. NULL with *reason set on failure. */
static camera_fb_t *capture(char *args, const char **reason)
{
    framesize_t size = s_size;
    int quality = s_quality;
    for (char *save = NULL, *tok = strtok_r(args, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        if (parse_size(tok, &size)) {
            continue;
        }
        const long q = strtol(tok, NULL, 10);
        if (q < 4 || q > 63) {
            *reason = "bad_argument";
            return NULL;
        }
        quality = (int)q;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (!s_camera_up || !sensor) {
        *reason = "camera_not_initialised";
        return NULL;
    }
    if (size != s_size) {
        sensor->set_framesize(sensor, size);
        s_size = size;
    }
    if (quality != s_quality) {
        sensor->set_quality(sensor, quality);
        s_quality = quality;
    }
    for (int i = 0; i < SETTLE_FRAMES; i++) {
        camera_fb_t *old = esp_camera_fb_get();
        if (old) {
            esp_camera_fb_return(old);
        }
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        *reason = "capture_failed";
        return NULL;
    }
    if (fb->format != PIXFORMAT_JPEG) {
        esp_camera_fb_return(fb);
        *reason = "not_jpeg";
        return NULL;
    }
    return fb;
}

static void snap(uart_port_t port, char *args)
{
    xSemaphoreTake(s_camera_lock, portMAX_DELAY);
    const char *reason = NULL;
    camera_fb_t *fb = capture(args, &reason);
    if (!fb) {
        xSemaphoreGive(s_camera_lock);
        reply(port, "FXERR %s\n", reason);
        return;
    }
    unsigned char digest[32];
    if (mbedtls_sha256(fb->buf, fb->len, digest, 0) != 0) {
        esp_camera_fb_return(fb);
        xSemaphoreGive(s_camera_lock);
        reply(port, "FXERR hash_failed\n");
        return;
    }
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    }
    reply(port, "FXJPEG 1 %u %s %u %u\n", (unsigned)fb->len, hex, (unsigned)fb->width, (unsigned)fb->height);
    uart_write_bytes(port, (const char *)fb->buf, fb->len);
    reply(port, "\nFXEND\n");
    uart_wait_tx_done(port, pdMS_TO_TICKS(30000));
    esp_camera_fb_return(fb);
    xSemaphoreGive(s_camera_lock);
}

static void check(uart_port_t port)
{
    xSemaphoreTake(s_camera_lock, portMAX_DELAY);
    const char *reason = NULL;
    char no_args[1] = "";
    camera_fb_t *fb = capture(no_args, &reason);
    if (!fb) {
        xSemaphoreGive(s_camera_lock);
        reply(port, "FXCHECK fail %s\n", reason);
        return;
    }
    const unsigned len = (unsigned)fb->len, w = (unsigned)fb->width, h = (unsigned)fb->height;
    esp_camera_fb_return(fb); /* discarded: a self-test sees the size, never the picture */
    xSemaphoreGive(s_camera_lock);
    reply(port, "FXCHECK ok %u %u %u\n", len, w, h);
}

static void handle(uart_port_t port, char *line)
{
    char *args = strchr(line, ' ');
    if (args) {
        *args++ = '\0';
    } else {
        args = line + strlen(line);
    }
    if (strcasecmp(line, "PING") == 0) {
        reply(port, "FXPONG firex-cam %s %s\n", esp_app_get_description()->version, IMAGE_KIND);
    } else if (strcasecmp(line, "CHECK") == 0) {
        check(port);
    } else if (strcasecmp(line, "SNAP") == 0) {
#ifndef CONFIG_FIREX_CAM_BENCH_SHUTTER
        if (port == CONSOLE_PORT) {
            reply(port, "FXERR not_permitted_on_console\n");
            return;
        }
#endif
        snap(port, args);
    } else if (line[0] != '\0') {
        reply(port, "FXERR unknown_command %s\n", line);
    }
}

static void port_task(void *arg)
{
    const uart_port_t port = (uart_port_t)(intptr_t)arg;
    char line[CMD_LINE_MAX];
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(port, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[n] = '\0';
            handle(port, line);
            n = 0;
            continue;
        }
        if (n < sizeof(line) - 1) {
            line[n++] = (char)c;
        }
    }
}

void app_main(void)
{
    /* The flash LED floats on at boot on some boards; hold it off. */
    gpio_reset_pin(FLASH_LED_GPIO);
    gpio_set_direction(FLASH_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(FLASH_LED_GPIO, 0);

    s_camera_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_PORT, 1024, 0, 0, NULL, 0));

    const uart_config_t link_cfg = {
        .baud_rate = CONFIG_FIREX_CAM_LINK_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(LINK_PORT, &link_cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_PORT, CONFIG_FIREX_CAM_LINK_TX_GPIO, CONFIG_FIREX_CAM_LINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(LINK_PORT, 1024, 0, 0, NULL, 0));

    const esp_err_t err = camera_start();
    s_camera_up = err == ESP_OK;
    if (!s_camera_up) {
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
    }
    esp_log_level_set("*", ESP_LOG_ERROR);

    const char *version = esp_app_get_description()->version;
    reply(CONSOLE_PORT, "\nFXREADY firex-cam %s %s camera=%s\n", version, IMAGE_KIND, s_camera_up ? "ok" : "failed");
    reply(LINK_PORT, "\nFXREADY firex-cam %s %s camera=%s\n", version, IMAGE_KIND, s_camera_up ? "ok" : "failed");

    xTaskCreate(port_task, "link", 4096, (void *)(intptr_t)LINK_PORT, 5, NULL);
    xTaskCreate(port_task, "console", 4096, (void *)(intptr_t)CONSOLE_PORT, 4, NULL);
}
