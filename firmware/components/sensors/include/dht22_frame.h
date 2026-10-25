/*
 * DHT22 (AM2302) frame decoding. Pure C.
 *
 * The hardware layer captures the single-wire line as alternating level and
 * duration pulses (the RMT peripheral on the ESP32-S3); everything from pulses
 * to degrees happens here, so it is tested without a sensor.
 *
 * Frame: 40 bits, MSB first. Each bit is ~50 us low, then high for 26-28 us (0)
 * or ~70 us (1). Bytes: RH high, RH low, T high (bit 7 = sign), T low, checksum.
 * Both values are tenths.
 */
#ifndef FX_DHT22_FRAME_H
#define FX_DHT22_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The part cannot be read faster than this. Fusion dwell timers must be at
 * least this long, which fx_config_check enforces. */
#define FX_DHT22_FLOOR_MS 2000u

typedef struct {
    uint8_t level; /* 0 low, 1 high */
    uint16_t us;
} fx_pulse_t;

typedef enum {
    FX_DHT22_OK = 0,
    FX_DHT22_SHORT_FRAME, /* fewer than 40 data bits captured */
    FX_DHT22_BAD_PULSE,   /* a bit's high time is outside anything a DHT22 sends */
    FX_DHT22_CHECKSUM,
    FX_DHT22_ZERO_FRAME,  /* five zero bytes pass the checksum but are a line fault */
    FX_DHT22_RANGE        /* decoded, but outside the part's rated range */
} fx_dht22_status_t;

typedef struct {
    float temp_c;
    float rh_pct;
    uint8_t bytes[5];
} fx_dht22_reading_t;

fx_dht22_status_t fx_dht22_decode_bytes(const uint8_t bytes[5], fx_dht22_reading_t *out);

/* Uses the last 40 high pulses that sit between two low pulses, so the host
 * start signal and the sensor's 80 us response pair before the data are
 * ignored however much of them the capture caught. */
fx_dht22_status_t fx_dht22_decode_pulses(const fx_pulse_t *pulses, size_t n, fx_dht22_reading_t *out);

/* True when a new read is allowed. */
bool fx_dht22_due(bool have_last, uint32_t last_ms, uint32_t now_ms);

const char *fx_dht22_status_name(fx_dht22_status_t s);

#ifdef __cplusplus
}
#endif
#endif /* FX_DHT22_FRAME_H */
