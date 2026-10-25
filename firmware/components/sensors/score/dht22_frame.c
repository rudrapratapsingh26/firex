#include "dht22_frame.h"

#include <string.h>

#define BIT_ONE_MIN_US 50u
#define HIGH_MIN_US 10u
#define HIGH_MAX_US 110u

fx_dht22_status_t fx_dht22_decode_bytes(const uint8_t bytes[5], fx_dht22_reading_t *out)
{
    if (!bytes || !out) {
        return FX_DHT22_SHORT_FRAME;
    }
    memcpy(out->bytes, bytes, 5);
    const uint8_t sum = (uint8_t)(bytes[0] + bytes[1] + bytes[2] + bytes[3]);
    if (sum != bytes[4]) {
        return FX_DHT22_CHECKSUM;
    }
    if ((bytes[0] | bytes[1] | bytes[2] | bytes[3] | bytes[4]) == 0u) {
        return FX_DHT22_ZERO_FRAME;
    }

    const uint16_t rh = (uint16_t)((bytes[0] << 8) | bytes[1]);
    const uint16_t t = (uint16_t)(((bytes[2] & 0x7Fu) << 8) | bytes[3]);
    out->rh_pct = (float)rh / 10.0f;
    out->temp_c = (float)t / 10.0f;
    if (bytes[2] & 0x80u) {
        out->temp_c = -out->temp_c;
    }
    if (out->rh_pct > 100.0f || out->temp_c < -40.0f || out->temp_c > 80.0f) {
        return FX_DHT22_RANGE;
    }
    return FX_DHT22_OK;
}

fx_dht22_status_t fx_dht22_decode_pulses(const fx_pulse_t *pulses, size_t n, fx_dht22_reading_t *out)
{
    if (!pulses || !out || n < 3u) {
        return FX_DHT22_SHORT_FRAME;
    }

    /* Walk backwards collecting data-bit high pulses: a high between two lows. */
    uint16_t highs[40];
    size_t found = 0u;
    for (size_t i = n - 2u; i >= 1u && found < 40u; i--) {
        if (pulses[i].level == 1u && pulses[i - 1u].level == 0u && pulses[i + 1u].level == 0u) {
            highs[39u - found] = pulses[i].us;
            found++;
        }
    }
    if (found < 40u) {
        return FX_DHT22_SHORT_FRAME;
    }

    uint8_t bytes[5] = {0};
    for (size_t bit = 0; bit < 40u; bit++) {
        const uint16_t us = highs[bit];
        if (us < HIGH_MIN_US || us > HIGH_MAX_US) {
            return FX_DHT22_BAD_PULSE;
        }
        if (us >= BIT_ONE_MIN_US) {
            bytes[bit / 8u] |= (uint8_t)(0x80u >> (bit % 8u));
        }
    }
    return fx_dht22_decode_bytes(bytes, out);
}

bool fx_dht22_due(bool have_last, uint32_t last_ms, uint32_t now_ms)
{
    return !have_last || (uint32_t)(now_ms - last_ms) >= FX_DHT22_FLOOR_MS;
}

const char *fx_dht22_status_name(fx_dht22_status_t s)
{
    switch (s) {
    case FX_DHT22_OK: return "ok";
    case FX_DHT22_SHORT_FRAME: return "short_frame";
    case FX_DHT22_BAD_PULSE: return "bad_pulse";
    case FX_DHT22_CHECKSUM: return "checksum";
    case FX_DHT22_ZERO_FRAME: return "zero_frame";
    case FX_DHT22_RANGE: return "range";
    default: return "?";
    }
}
