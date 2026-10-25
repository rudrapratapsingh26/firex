#include <dirent.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "fx_ledger_hw.h"

static const char *TAG = "fs_store";

esp_err_t fx_fs_mount(const char *label, const char *base_path, bool *formatted)
{
    if (formatted) {
        *formatted = false;
    }
    esp_vfs_littlefs_conf_t conf = {
        .base_path = base_path,
        .partition_label = label,
        .format_if_mount_failed = false,
    };
    if (esp_vfs_littlefs_register(&conf) == ESP_OK) {
        return ESP_OK;
    }

    /* Format only a partition that has never held anything. */
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    ESP_RETURN_ON_FALSE(part, ESP_ERR_NOT_FOUND, TAG, "no partition labelled %s", label);
    uint8_t block[256];
    bool blank = true;
    for (size_t off = 0; off < 8192 && blank; off += sizeof(block)) {
        if (esp_partition_read(part, off, block, sizeof(block)) != ESP_OK) {
            blank = false;
            break;
        }
        for (size_t i = 0; i < sizeof(block); i++) {
            if (block[i] != 0xFF) {
                blank = false;
                break;
            }
        }
    }
    if (!blank) {
        ESP_LOGE(TAG, "%s holds data but does not mount; not formatting it", label);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(esp_littlefs_format(label), TAG, "format %s", label);
    if (formatted) {
        *formatted = true;
    }
    return esp_vfs_littlefs_register(&conf);
}

esp_err_t fx_fs_usage(const char *label, size_t *total_bytes, size_t *used_bytes)
{
    return esp_littlefs_info(label, total_bytes, used_bytes);
}

static void seg_path(const fx_fs_store_t *s, uint32_t seg, char *out, size_t cap)
{
    snprintf(out, cap, "%s/seg-%08" PRIu32 ".bin", s->base, seg);
}

static void drop_reader(fx_fs_store_t *s)
{
    if (s->reader) {
        fclose(s->reader);
        s->reader = NULL;
    }
}

static int fs_append(void *ctx, uint32_t seg, const uint8_t *data, size_t len)
{
    fx_fs_store_t *s = (fx_fs_store_t *)ctx;
    if (s->reader && s->reader_seg == seg) {
        drop_reader(s); /* a handle opened before this write would not see it */
    }
    char path[64];
    seg_path(s, seg, path, sizeof(path));
    FILE *f = fopen(path, "ab");
    if (!f) {
        return -1;
    }
    int rc = fwrite(data, 1, len, f) == len ? 0 : -1;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        rc = -1;
    }
    if (fclose(f) != 0) {
        rc = -1;
    }
    return rc;
}

static int fs_read(void *ctx, uint32_t seg, uint32_t offset, uint8_t *buf, size_t len, size_t *got)
{
    fx_fs_store_t *s = (fx_fs_store_t *)ctx;
    if (!s->reader || s->reader_seg != seg) {
        drop_reader(s);
        char path[64];
        seg_path(s, seg, path, sizeof(path));
        s->reader = fopen(path, "rb");
        if (!s->reader) {
            return -1;
        }
        s->reader_seg = seg;
    }
    *got = 0;
    if (fseek(s->reader, (long)offset, SEEK_SET) != 0) {
        return 0;
    }
    *got = fread(buf, 1, len, s->reader);
    clearerr(s->reader);
    return 0;
}

static long fs_size(void *ctx, uint32_t seg)
{
    fx_fs_store_t *s = (fx_fs_store_t *)ctx;
    char path[64];
    seg_path(s, seg, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int fs_remove(void *ctx, uint32_t seg)
{
    fx_fs_store_t *s = (fx_fs_store_t *)ctx;
    if (s->reader && s->reader_seg == seg) {
        drop_reader(s);
    }
    char path[64];
    seg_path(s, seg, path, sizeof(path));
    return unlink(path) == 0 ? 0 : -1;
}

static bool parse_seg_name(const char *name, uint32_t *seg)
{
    if (strncmp(name, "seg-", 4) != 0) {
        return false;
    }
    char *end = NULL;
    const unsigned long v = strtoul(name + 4, &end, 10);
    if (end == name + 4 || strcmp(end, ".bin") != 0 || v > 0xFFFFFFFFul) {
        return false;
    }
    *seg = (uint32_t)v;
    return true;
}

static int fs_range(void *ctx, uint32_t *first, uint32_t *last)
{
    fx_fs_store_t *s = (fx_fs_store_t *)ctx;
    DIR *dir = opendir(s->base);
    if (!dir) {
        return -1;
    }
    bool any = false;
    uint32_t lo = 0, hi = 0, seg = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (!parse_seg_name(e->d_name, &seg)) {
            continue;
        }
        if (!any || seg < lo) lo = seg;
        if (!any || seg > hi) hi = seg;
        any = true;
    }
    closedir(dir);
    if (!any) {
        return 1;
    }
    *first = lo;
    *last = hi;
    return 0;
}

void fx_fs_store_init(fx_fs_store_t *s, const char *base_path, fx_store_ops_t *ops)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->base, sizeof(s->base), "%s", base_path);
    ops->append = fs_append;
    ops->read = fs_read;
    ops->size = fs_size;
    ops->remove = fs_remove;
    ops->range = fs_range;
    ops->ctx = s;
}

int fx_fs_store_wipe(fx_fs_store_t *s)
{
    drop_reader(s);
    DIR *dir = opendir(s->base);
    if (!dir) {
        return -1;
    }
    uint32_t segs[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < 256) {
        if (parse_seg_name(e->d_name, &segs[n])) {
            n++;
        }
    }
    closedir(dir);
    int removed = 0;
    for (int i = 0; i < n; i++) {
        char path[64];
        seg_path(s, segs[i], path, sizeof(path)); /* rebuilt from the number, never copied from the directory */
        removed += unlink(path) == 0 ? 1 : 0;
    }
    return removed;
}
