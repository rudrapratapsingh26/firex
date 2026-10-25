/*
 * Store and forward on the node (docs/transport.md sections 2 and 3). Pure C.
 *
 * The ledger on flash is the outbound queue. This tracks what the gateway has
 * acknowledged and what to send next, from the gateway's commands:
 *
 *   {"cmd":"sync","have_seq":N}      after every hello: send from N+1
 *   {"cmd":"ack","seq":N}            everything up to N is stored
 *   {"cmd":"resend","from_seq":N}    the gateway saw a gap
 *   {"cmd":"media_ack","sha256":"…"} an image is stored; delete the node's copy
 *   {"cmd":"time","unix_ms":N}       the gateway's clock, for the ledger (M5)
 *   {"cmd":"ota","version":"…","size":N,"sha256":"…","url":"https://…"}
 *                                    a signed image to install (M5, main/ota_task.c)
 *   {"cmd":"trace_ack","sha256":"…"} a trace file is stored; delete the node's copy (M5)
 *
 * At most `window` entries are sent beyond the last acknowledgement, so a
 * reconnect after a long outage does not flood the broker or the node's RAM.
 */
#ifndef FX_SYNC_H
#define FX_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FX_CMD_UNKNOWN = 0,
    FX_CMD_SYNC,
    FX_CMD_ACK,
    FX_CMD_RESEND,
    FX_CMD_MEDIA_ACK,
    FX_CMD_TIME,
    FX_CMD_OTA,
    FX_CMD_TRACE_ACK
} fx_cmd_kind_t;

#define FX_CMD_VERSION_MAX 32u
#define FX_CMD_URL_MAX 160u

typedef struct {
    fx_cmd_kind_t kind;
    int64_t number;                   /* have_seq, seq, from_seq, or unix_ms for time */
    char sha256[65];                  /* media_ack, trace_ack, ota */
    char version[FX_CMD_VERSION_MAX]; /* ota */
    uint32_t size;                    /* ota: image bytes */
    char url[FX_CMD_URL_MAX];         /* ota */
} fx_cmd_t;

/* Parse one gateway command. False for malformed input or missing fields. */
bool fx_cmd_parse(const char *json, size_t len, fx_cmd_t *out);

typedef struct {
    bool synced;    /* a sync arrived on this connection */
    int64_t acked;  /* highest seq the gateway holds, -1 for none */
    int64_t next;   /* next seq to publish */
    uint32_t window;
} fx_sync_t;

void fx_sync_init(fx_sync_t *s, uint32_t window);
/* A new connection: nothing is sent until the gateway answers hello with sync. */
void fx_sync_disconnected(fx_sync_t *s);
void fx_sync_on_cmd(fx_sync_t *s, const fx_cmd_t *cmd, int64_t oldest_seq);
/* The seq to publish now, or -1. `last_seq` is the newest entry on flash (-1 if none). */
int64_t fx_sync_next(const fx_sync_t *s, int64_t oldest_seq, int64_t last_seq);
void fx_sync_sent(fx_sync_t *s, int64_t seq);

#ifdef __cplusplus
}
#endif
#endif /* FX_SYNC_H */
