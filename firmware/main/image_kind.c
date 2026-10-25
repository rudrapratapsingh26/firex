/*
 * This image's kind (M6), in ESP-IDF's custom app description: a fixed place in the
 * signed image, so ota_task.c can read a candidate's kind from its first bytes and refuse
 * one built for another kind of unit (components/ota, fx_ota_kind_refusal).
 */
#include "fx_ota.h"
#include "node.h"
#include "sdkconfig.h"

#if CONFIG_FIREX_PRODUCTION
#define KIND "evt-production"
#elif CONFIG_FIREX_BOARD_EVT
#define KIND "evt"
#else
#define KIND "devkit"
#endif

const fx_ota_image_kind_t fx_image_kind __attribute__((section(".rodata_custom_desc"))) = {
    .magic = FX_OTA_KIND_MAGIC,
    .kind = KIND,
};
