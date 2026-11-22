/* TGF-C core API — the library boundary between the pipeline and its frontends.
 *
 * Self-contained: includes only the C standard library. No tgf.h, no cJSON,
 * no ncurses, no GTK. A frontend compiles against this header alone.
 *
 * Thread-safety: tgf_create/tgf_destroy are NOT thread-safe (call before/after
 * the core thread runs). All other functions are thread-safe. The core owns
 * one worker thread; frontends talk to it only through tgf_*_event and the
 * submit_* / pause / resume / ... commands.
 *
 * Ownership: tgf_next_event() returns an event whose strings are owned by the
 * core; the consumer MUST call tgf_event_free() on it. tgf_config_free()
 * releases a config. tgf_destroy() stops and frees the context. */
#ifndef TGF_CORE_H
#define TGF_CORE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TGF_API_VERSION 1

typedef struct tgf_ctx tgf_ctx;

typedef enum {
    TGF_OK = 0,
    TGF_ERR_INVALID,   /* bad argument */
    TGF_ERR_IO,        /* file/socket IO failure */
    TGF_ERR_OOM,       /* out of memory */
    TGF_ERR_STATE,     /* not in a state that allows the call */
    TGF_ERR_LOCKED,    /* another instance holds the session lock */
    TGF_ERR_PARSE,     /* config parse failure */
    TGF_ERR_BUSY       /* command queue full */
} tgf_status;

/* ---- transport (TDLib JSON seam) -------------------------------------- */

/* Immutable vtable. The real implementation wraps td_json_client_*; a fake
 * replays scripted responses and records sends. */
typedef struct {
    void *(*create)(void);
    void (*send)(void *client, const char *json);
    const char *(*receive)(void *client, double timeout);
    /* Synchronous request. Result owned by the transport, valid until the next
     * execute() on the same client; the caller must not free it. */
    const char *(*execute)(void *client, const char *json);
    void (*destroy)(void *client);
} tgf_transport;

/* Real TDLib-backed vtable, provided by libtgf. */
extern const tgf_transport transport_tdlib;

/* ---- config ------------------------------------------------------------ */

/* Same fields as config.json (05 §2). Strings are owned by the struct. */
typedef struct {
    int    api_id;
    char  *api_hash;
    char  *dest_channel;
    char **source_channels;
    int    num_sources;
    char **disabled_channels;   /* optional; 05 §2 */
    int    num_disabled;
    char  *history_file;
    int    forward_delay_sec;
    int    history_window_hours;
    int    enable_sequential_forwarding;
    int    enable_ordering;
    int    sequence_direction;   /* 0 asc, 1 desc */
} tgf_config;

tgf_status tgf_config_load(const char *path, tgf_config *out);
tgf_status tgf_config_save(const char *path, const tgf_config *cfg);
/* Human-readable reason for the last tgf_config_load failure. Startup-only;
 * static storage, not thread-safe. NULL when the last load succeeded. */
const char *tgf_config_error(void);
void       tgf_config_free(tgf_config *cfg);

/* ---- events (core -> frontend) ---------------------------------------- */

typedef enum {
    TGF_EV_AUTH_STATE, TGF_EV_CONNECTION_STATE, TGF_EV_SOURCE_STATE,
    TGF_EV_FORWARDED, TGF_EV_SKIPPED, TGF_EV_QUEUE,
    TGF_EV_NEXT_FORWARD_IN, TGF_EV_PIPELINE_STATE, TGF_EV_LOG, TGF_EV_ERROR
} tgf_event_kind;

typedef enum { TGF_AUTH_PARAMS, TGF_AUTH_PHONE, TGF_AUTH_CODE, TGF_AUTH_PASSWORD,
               TGF_AUTH_READY, TGF_AUTH_CLOSING, TGF_AUTH_LOGGING_OUT } tgf_auth_state;
typedef enum { TGF_CONN_CONNECTING, TGF_CONN_READY, TGF_CONN_LOST } tgf_conn_state;
typedef enum { TGF_SRC_LIVE, TGF_SRC_SCANNING, TGF_SRC_NO_ACCESS, TGF_SRC_DISABLED } tgf_source_state;
typedef enum { TGF_SKIP_DUPLICATE, TGF_SKIP_WINDOW } tgf_skip_reason;
typedef enum { TGF_PIPE_RUNNING, TGF_PIPE_PAUSED } tgf_pipeline_state;
typedef enum { TGF_LOG_INFO, TGF_LOG_WARN, TGF_LOG_ERROR } tgf_log_level;
typedef enum { TGF_ERR_SCOPE_SOURCE, TGF_ERR_SCOPE_AUTH, TGF_ERR_SCOPE_PIPELINE } tgf_error_scope;

typedef struct tgf_event {
    tgf_event_kind kind;
    /* generic payload; the meaningful fields depend on kind */
    int64_t id;                 /* message id (FORWARDED/SKIPPED) or source id */
    int64_t last_id;            /* source last-seen id */
    int64_t total;              /* queue total length */
    int64_t ms;                 /* next-forward countdown in ms */
    double  progress;           /* backfill progress 0..100 */
    int     code;               /* error code */
    char   *text;               /* log/error text (owned) */
    char   *source;             /* source name (owned) */
    char   *dest;               /* destination name (owned) */
    tgf_auth_state     auth;
    tgf_conn_state     conn;
    tgf_source_state   src_state;
    tgf_skip_reason    skip;
    tgf_pipeline_state pipe;
    tgf_log_level      level;
    tgf_error_scope    scope;
} tgf_event;

/* ---- lifecycle --------------------------------------------------------- */

/* cfg and tp are copied/borrowed: tp must outlive the context; cfg is copied.
 * Returns NULL on allocation failure. */
tgf_ctx   *tgf_create(const tgf_config *cfg, const tgf_transport *tp);
tgf_status tgf_start(tgf_ctx *ctx);      /* spawns the core thread; idempotent */
void       tgf_stop(tgf_ctx *ctx);       /* idempotent; signals and joins */
void       tgf_destroy(tgf_ctx *ctx);    /* stop + free; NULL-safe */

/* ---- event delivery ---------------------------------------------------- */

/* Pop one event (non-blocking). Returns false when the queue is empty.
 * On true, the consumer MUST call tgf_event_free(out). */
bool tgf_next_event(tgf_ctx *ctx, tgf_event *out);
/* Pollable read end; readable when an event is queued (pipe). Integrates with
 * poll()/select()/a GLib GSource without busy-waiting. */
int  tgf_event_fd(tgf_ctx *ctx);
void tgf_event_free(tgf_event *ev);

/* ---- commands (frontend -> core) -------------------------------------- */

tgf_status tgf_submit_phone(tgf_ctx *ctx, const char *phone);
tgf_status tgf_submit_code(tgf_ctx *ctx, const char *code);
tgf_status tgf_submit_password(tgf_ctx *ctx, const char *password);
tgf_status tgf_pause(tgf_ctx *ctx);
tgf_status tgf_resume(tgf_ctx *ctx);
tgf_status tgf_add_source(tgf_ctx *ctx, const char *handle);
tgf_status tgf_remove_source(tgf_ctx *ctx, int64_t id);
tgf_status tgf_set_source_enabled(tgf_ctx *ctx, int64_t id, bool enabled);
tgf_status tgf_rescan_source(tgf_ctx *ctx, int64_t id);
/* Handle-addressed variants: a source disabled in config is never resolved and
 * has no chat id, so frontends address it by its configured handle. */
tgf_status tgf_remove_source_by_handle(tgf_ctx *ctx, const char *handle);
tgf_status tgf_set_source_enabled_by_handle(tgf_ctx *ctx, const char *handle, bool enabled);
tgf_status tgf_rescan_source_by_handle(tgf_ctx *ctx, const char *handle);
tgf_status tgf_apply_config(tgf_ctx *ctx, const tgf_config *cfg);
tgf_status tgf_logout(tgf_ctx *ctx);

#endif
