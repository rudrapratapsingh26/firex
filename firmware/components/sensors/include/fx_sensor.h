/*
 * The common score API. One driver per physical sensor, each producing the
 * fx_sample_t values the fusion engine consumes for the modalities it owns:
 *
 *   MQ-2    -> gas          MQ-135 -> smoke/AQ
 *   KY-026  -> flame        DHT22  -> heat + humidity
 *
 * Pure C: the struct and the collector compile on a laptop; the drivers behind
 * the function pointers live in hw/ and are ESP-IDF code. The M6 PCB swaps the
 * drivers and keeps this interface.
 */
#ifndef FX_SENSOR_H
#define FX_SENSOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fusion.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FX_MOD_BIT(m) ((uint8_t)(1u << (m)))

typedef struct fx_sensor fx_sensor_t;

struct fx_sensor {
    const char *name;       /* "mq2", "mq135", "ky026", "dht22" */
    uint8_t modalities;     /* FX_MOD_BIT of every modality this driver writes */
    uint32_t period_ms;     /* poll cadence; the DHT22 cannot go below FX_DHT22_FLOOR_MS */
    const char *raw_header; /* comma-separated raw column names for serial capture */

    /* 0 on success. A driver whose init failed is never polled and scores invalid. */
    int (*init)(fx_sensor_t *s);
    /* Do the I/O. Runs in the driver's own sampling task at period_ms. */
    void (*poll)(fx_sensor_t *s, uint32_t now_ms);
    /* Write samples[m] for each modality in `modalities`. Never blocks on I/O. */
    void (*score)(fx_sensor_t *s, uint32_t now_ms, fx_sample_t samples[FX_MODALITY_COUNT]);
    /* Raw values matching raw_header, no trailing newline. Returns chars written. */
    int (*raw)(fx_sensor_t *s, char *buf, size_t len);

    bool ready; /* set by the owner once init succeeded */
    void *ctx;
};

/* Every modality invalid with score 0. */
void fx_samples_invalid(fx_sample_t samples[FX_MODALITY_COUNT]);

/* Collect one sample set from a list of drivers. A modality with no working
 * driver stays invalid, and a driver cannot write a modality it does not own.
 * Returns the bitmask of modalities that some ready driver covered. */
uint8_t fx_sensors_collect(fx_sensor_t *const sensors[], size_t count, uint32_t now_ms,
                           fx_sample_t out[FX_MODALITY_COUNT]);

#ifdef __cplusplus
}
#endif
#endif /* FX_SENSOR_H */
