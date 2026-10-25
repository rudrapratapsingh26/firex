#include "fx_sensor.h"

void fx_samples_invalid(fx_sample_t samples[FX_MODALITY_COUNT])
{
    if (!samples) {
        return;
    }
    for (int i = 0; i < FX_MODALITY_COUNT; i++) {
        samples[i].score = 0.0f;
        samples[i].valid = false;
    }
}

uint8_t fx_sensors_collect(fx_sensor_t *const sensors[], size_t count, uint32_t now_ms,
                           fx_sample_t out[FX_MODALITY_COUNT])
{
    fx_samples_invalid(out);
    if (!sensors || !out) {
        return 0u;
    }
    uint8_t covered = 0u;
    for (size_t k = 0; k < count; k++) {
        fx_sensor_t *s = sensors[k];
        if (!s || !s->ready || !s->score) {
            continue;
        }
        fx_sample_t scratch[FX_MODALITY_COUNT];
        fx_samples_invalid(scratch);
        s->score(s, now_ms, scratch);
        for (int m = 0; m < FX_MODALITY_COUNT; m++) {
            if (s->modalities & FX_MOD_BIT(m)) {
                out[m] = scratch[m];
                covered |= FX_MOD_BIT(m);
            }
        }
    }
    return covered;
}
