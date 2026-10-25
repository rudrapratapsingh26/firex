/*
 * Node configuration: every tunable number in one table, stored in NVS on the
 * node (main/settings_nvs.c). Pure C.
 *
 * All values are unsigned integers in stated units (scores in hundredths), so
 * a change can be logged as a config_change entry without floats. A change is
 * refused if it leaves the set inconsistent: an exit threshold at or above the
 * entry threshold, a dwell shorter than the DHT22's 2 s sampling floor, a heat
 * or flame range that is empty.
 */
#ifndef FX_SETTINGS_H
#define FX_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "flame_score.h"
#include "fusion.h"
#include "heat_score.h"
#include "mq_score.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FX_S_ENTER = 0,
    FX_S_EXIT,
    FX_S_ELEVATED_MS,
    FX_S_CRITICAL_MS,
    FX_S_FLAME_ALONE,
    FX_S_FLAME_SCORE,
    FX_S_FLAME_MS,
    FX_S_WARMUP_S,
    FX_S_WARM_S,
    FX_S_RH_CONTEXT,
    FX_S_HEAT_QUIET,
    FX_S_GAS_SUPPRESS,
    FX_S_MOS_PAIR, /* NVS stores settings by key, so a new id may sit anywhere */
    FX_S_MQ2_R0,
    FX_S_MQ135_R0,
    FX_S_MQ2_FULL,
    FX_S_MQ135_FULL,
    FX_S_HEAT_START_C,
    FX_S_HEAT_FULL_C,
    FX_S_ROR_START,
    FX_S_ROR_FULL,
    FX_S_FLAME_QUIET,
    FX_S_FLAME_FULL,
    FX_S_SILENCE_S,
    FX_S_FAN_DUTY,
    FX_S_CAM_FRAMES,
    FX_S_SELFTEST_H,
    FX_S_RING_BRIGHT,
    FX_SETTINGS_COUNT
} fx_setting_id_t;

typedef struct {
    const char *key; /* also the NVS key: at most 15 characters */
    uint32_t def;
    uint32_t min;
    uint32_t max;
    const char *unit;
    const char *help;
} fx_setting_def_t;

extern const fx_setting_def_t FX_SETTING_DEFS[FX_SETTINGS_COUNT];

typedef struct {
    uint32_t v[FX_SETTINGS_COUNT];
} fx_settings_t;

typedef enum {
    FX_SET_OK = 0,
    FX_SET_UNKNOWN,
    FX_SET_RANGE,
    FX_SET_INCONSISTENT,
    FX_SET_LOCKED
} fx_set_status_t;

void fx_settings_defaults(fx_settings_t *s);
int fx_settings_find(const char *key); /* -1 if unknown */
/* Privacy locks (M6). A production build defines FX_CAM_FRAMES_LOCKED to the number of
 * frames per CRITICAL. cam_frames then has that value as its default, minimum and maximum,
 * so a value an earlier image stored in NVS is ignored at load, and fx_settings_set refuses
 * any change with FX_SET_LOCKED. */
bool fx_settings_locked(int id);
bool fx_settings_consistent(const fx_settings_t *s, const char **why);
/* On any failure the settings are left unchanged. */
fx_set_status_t fx_settings_set(fx_settings_t *s, const char *key, uint32_t value, uint32_t *old_value,
                                const char **why);

void fx_settings_fusion(const fx_settings_t *s, fx_config_t *cfg);
void fx_settings_heat(const fx_settings_t *s, fx_heat_params_t *p);
void fx_settings_flame(const fx_settings_t *s, fx_flame_params_t *p);
/* Scoring curves only; R0 is read with fx_settings_r0. */
void fx_settings_mq(const fx_settings_t *s, fx_mq_params_t *mq2, fx_mq_params_t *mq135);
float fx_settings_r0(const fx_settings_t *s, fx_modality_t modality);

#ifdef __cplusplus
}
#endif
#endif /* FX_SETTINGS_H */
