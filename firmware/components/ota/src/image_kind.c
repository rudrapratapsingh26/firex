#include <string.h>

#include "fx_ota.h"

const char *fx_ota_kind_refusal(const fx_ota_image_kind_t *candidate, const char *running)
{
    if (!candidate || memcmp(candidate->magic, FX_OTA_KIND_MAGIC, sizeof(candidate->magic)) != 0) {
        return "the image carries no image kind: it was built before M6 or is not a FireX node image";
    }
    const char *end = memchr(candidate->kind, '\0', sizeof(candidate->kind));
    if (!end || end == candidate->kind) {
        return "the image's kind is malformed";
    }
    if (!running || strcmp(candidate->kind, running) != 0) {
        return "the image is for another kind of unit (dev kit, EVT or EVT production)";
    }
    return NULL;
}
