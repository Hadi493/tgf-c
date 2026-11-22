#ifndef TGF_H_
#define TGF_H_

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "config.h"
#include "../core/tgf_core.h"

#define POLL_INTERVAL 5
#define MSG_LIMIT 10000

#define MAX_SEQ_TRACKER 128

typedef struct {
    long long id, album_id, reply_to_msg_id, date;
} PendingMsg;

typedef struct {
    long long chat_id;
    long long last_msg_id;
    long long last_date;
    long long scan_from_id;
    int backfill_done;
    PendingMsg *pending;
    int pending_count;
    int pending_cap;
} SeqEntry;

#define MAX_FWD_IDS 256
#define MAX_FWD_ATTEMPTS 3
#define FWD_TIMEOUT_SEC 60

typedef struct {
    long long src_chat_id;
    long long *ids;
    int count;
    int retries;
} ForwardJob;

typedef struct {
    char **set;
    int set_size;
    int count;
    time_t *time;
} HistoryStore;

typedef struct {
    SeqEntry entries[MAX_SEQ_TRACKER];
    int count;
} TrackerStore;

typedef struct {
    long long ids[MAX_FWD_IDS];
    int count;
} GroupJob;

typedef struct {
    long long id;
    long long date;
} GroupMark;

typedef struct {
    GroupJob *jobs;
    int job_count;
    GroupMark *marks;
    int mark_count;
} GroupResult;

typedef enum {
    REQ_NONE,
    REQ_AUTH_PARAMS,
    REQ_AUTH_PHONE,
    REQ_AUTH_CODE,
    REQ_AUTH_PW,
    REQ_GETME,
    REQ_RESOLVE_DEST,
    REQ_RESOLVE_SOURCE,
    REQ_HISTORY,
    REQ_FORWARD_ONE,
    REQ_FORWARD_BATCH
} RequestKind;

typedef struct {
    RequestKind kind;
    long long chat_id;
    long long msg_id;
} RequestId;

static inline const char *request_extra(RequestId id, char *buf, size_t size) {
    switch (id.kind) {
    case REQ_AUTH_PARAMS:
        return "auth_params";
    case REQ_AUTH_PHONE:
        return "auth_phone";
    case REQ_AUTH_CODE:
        return "auth_code";
    case REQ_AUTH_PW:
        return "auth_pw";
    case REQ_GETME:
        return "getme";
    case REQ_RESOLVE_DEST:
        return "resolve_dest";
    case REQ_NONE:
        return NULL;
    case REQ_RESOLVE_SOURCE:
        snprintf(buf, size, "resolve_%lld", id.chat_id);
        return buf;
    case REQ_HISTORY:
        snprintf(buf, size, "history_%lld", id.chat_id);
        return buf;
    case REQ_FORWARD_ONE:
        snprintf(buf, size, "fwd_%lld_%lld", id.chat_id, id.msg_id);
        return buf;
    case REQ_FORWARD_BATCH:
        snprintf(buf, size, "fwd_%lld_batch", id.chat_id);
        return buf;
    }
    return NULL;
}

static inline long long cJSON_GetInt64(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (!v) return 0;
    if (cJSON_IsNumber(v)) return (long long)v->valuedouble;
    if (cJSON_IsString(v)) return atoll(v->valuestring);
    return 0;
}

static inline const char *cJSON_GetStr(const cJSON *obj, const char *key) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return v && cJSON_IsString(v) ? v->valuestring : NULL;
}

typedef struct Runtime {
    Config cfg;
    const tgf_transport *tp;

    int authorized;
    int dest_resolved;
    long long dest_chat_id;
    long long *source_chat_ids;
    char *src_name;
    int source_count;
    int resolve_index;
    int resolve_active;
    int debug;

    void (*emit)(struct Runtime *rt, const tgf_event *ev); /* core event sink (may be NULL) */
    tgf_ctx *core;                                         /* owning core (may be NULL) */

    HistoryStore history;
    TrackerStore tracker;

    ForwardJob *fwd_queue;
    int fwd_queue_count;
    int fwd_queue_cap;
    int fwd_queue_head;
    double fwd_last_time;
    ForwardJob fwd_inflight;
    bool fwd_inflight_active;
    double fwd_inflight_time;
    double fwd_flood_until;

    volatile bool keep_running;
    bool dashboard_active;
    int fatal;
    char fatal_msg[256];
    char status_msg[256];
} Runtime;

#define APPEND_ID(ids, pos, size, val)                                                             \
    do {                                                                                           \
        int n_ = snprintf((ids) + (pos), (size) - (pos), "%s%lld", (pos) == 0 ? "" : ",",          \
                          (long long)(val));                                                       \
        if (n_ > 0) (pos) += n_;                                                                   \
    } while (0)

// --- functions ---

void ui_status(const char *msg);
void ui_oom(const char *msg);
void ui_forwarded(const char *source, const char *dest);
void ui_refresh(const char *dest, const char *source, int src_count, int delay);
void ui_dashboard_begin(const char *dest, int delay);
void ui_dashboard_event(const tgf_event *ev);
void ui_dashboard_draw(void);
void ui_dashboard_end(void);

void transport_send(Runtime *rt, void *client, const char *type, const char *payload, RequestId id);
void transport_destroy(void *client);

void auth_handle(Runtime *rt, void *client, const char *json);
void auth_submit(Runtime *rt, void *client, int kind, const char *value);
void resolve_next(Runtime *rt, void *client);
void resolve_destination(Runtime *rt, void *client);
void resolve_source_failed(Runtime *rt, void *client, int idx, int code);
void getme_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]);
void resolve_dest_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]);
void resolve_source_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]);
void poll_channels(Runtime *rt, void *client);
void history_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]);
void history_response(Runtime *rt, void *client, cJSON *root, long long src_chat_id);
void forward_handle(Runtime *rt, void *client, cJSON *root, RequestId id, char *argv[]);
void forward_error_handle(Runtime *rt, RequestId id, int code, const char *msg);

void history_store_load(HistoryStore *hs, const Config *cfg);
int history_store_has(const HistoryStore *hs, const Config *cfg, long long chat_id,
                      long long msg_id);
void history_store_add(HistoryStore *hs, const Config *cfg, long long chat_id, long long msg_id);
void history_store_prune(HistoryStore *hs, const Config *cfg);

void tracker_store_load(TrackerStore *ts);
void tracker_store_save(const TrackerStore *ts);
void tracker_store_set(TrackerStore *ts, long long chat_id, long long msg_id, long long date);
long long tracker_store_get_scan_from(const TrackerStore *ts, long long chat_id);
void tracker_store_set_scan_from(TrackerStore *ts, long long chat_id, long long msg_id);
int tracker_store_is_backfill_done(const TrackerStore *ts, long long chat_id);
void tracker_store_set_backfill_done(TrackerStore *ts, long long chat_id, int done);
void tracker_store_pending_append(TrackerStore *ts, long long chat_id, PendingMsg msg);
void seq_pending_load(Runtime *rt);
void flush_pending(Runtime *rt, long long src_chat_id);

void fwd_queue_push(Runtime *rt, long long src_chat_id, const long long *ids, int count);
void fwd_queue_save(Runtime *rt);
void fwd_queue_load(Runtime *rt);
int process_fwd_queue(Runtime *rt, void *client);
void process_msgs(Runtime *rt, long long src_chat_id, PendingMsg *msgs, int count);
GroupResult group_batch(const PendingMsg *msgs, int count, int order);
void group_result_free(GroupResult *r);

#endif
