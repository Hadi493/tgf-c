#include "tgf.h"
#include "config.h"
#include "loop.h"

#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <threads.h>
#include <unistd.h>

/* ---- context ----------------------------------------------------------- */

enum { CMD_PHONE, CMD_CODE, CMD_PW, CMD_PAUSE, CMD_RESUME,
       CMD_ADD_SOURCE, CMD_REMOVE_SOURCE, CMD_SET_ENABLED, CMD_RESCAN,
       CMD_APPLY_CONFIG, CMD_LOGOUT };

typedef struct { int kind; char *s; int64_t id; int enabled; int has_cfg; tgf_config cfg; } tgf_cmd;

struct tgf_ctx {
    Runtime rt;
    const tgf_transport *tp;
    atomic_bool running;
    atomic_bool paused;
    thrd_t thread;
    bool started;

    mtx_t ev_mtx;
    tgf_event **ev;
    int ev_head, ev_tail, ev_cap;
    int ev_pipe[2];

    mtx_t cmd_mtx;
    tgf_cmd *cmd;
    int cmd_head, cmd_tail, cmd_cap;

    tgf_config cfg;
};

/* ---- events ------------------------------------------------------------ */

static void config_copy(tgf_config *dst, const tgf_config *src);
static void runtime_config_load(Runtime *rt, const tgf_config *cfg);

static tgf_event *ev_new(tgf_event_kind kind) {
    tgf_event *e = calloc(1, sizeof(*e));
    if (e) e->kind = kind;
    return e;
}

static void ev_emit(tgf_ctx *c, tgf_event *e) {
    if (!e) return;
    mtx_lock(&c->ev_mtx);
    if (c->ev_tail - c->ev_head == c->ev_cap) {
        int ncap = c->ev_cap * 2, cnt = c->ev_tail - c->ev_head;
        tgf_event **n = calloc(ncap, sizeof(*n));
        if (!n) { mtx_unlock(&c->ev_mtx); return; }
        for (int i = 0; i < cnt; i++) n[i] = c->ev[(c->ev_head + i) % c->ev_cap];
        free(c->ev);
        c->ev = n; c->ev_cap = ncap; c->ev_head = 0; c->ev_tail = cnt;
    }
    c->ev[c->ev_tail % c->ev_cap] = e;
    c->ev_tail++;
    mtx_unlock(&c->ev_mtx);
    char b = 1;
    ssize_t r = write(c->ev_pipe[1], &b, 1);
    (void)r;
}

static void core_emit(Runtime *rt, const tgf_event *ev) {
    if (!rt->core) return;
    tgf_event *e = malloc(sizeof(*e));
    if (!e) return;
    *e = *ev;
    if (ev->text)   e->text = strdup(ev->text);
    if (ev->source) e->source = strdup(ev->source);
    if (ev->dest)   e->dest = strdup(ev->dest);
    ev_emit(rt->core, e);
}

/* v1: one context per process (shortcut: multiple contexts would need a
 * registry; the CLI and GUI are single-instance). */
static tgf_ctx *s_ctx = NULL;

void ui_status(const char *msg) {
    if (!s_ctx) return;
    snprintf(s_ctx->rt.status_msg, sizeof(s_ctx->rt.status_msg), "%s", msg);
    tgf_event *e = ev_new(TGF_EV_LOG);
    if (!e) return;
    e->text = strdup(msg);
    e->level = strncmp(msg, "ERROR", 5) == 0 ? TGF_LOG_ERROR
              : strncmp(msg, "WARNING", 7) == 0 ? TGF_LOG_WARN : TGF_LOG_INFO;
    ev_emit(s_ctx, e);
}

void ui_oom(const char *msg) { ui_status(msg); }

void ui_forwarded(const char *source, const char *dest) {
    if (!s_ctx) return;
    tgf_event *e = ev_new(TGF_EV_FORWARDED);
    if (!e) return;
    e->source = strdup(source);
    e->dest = strdup(dest);
    ev_emit(s_ctx, e);
}

void ui_refresh(const char *dest, const char *source, int src_count, int delay) {
    (void)dest; (void)source; (void)src_count; (void)delay;
}

bool tgf_next_event(tgf_ctx *c, tgf_event *out) {
    if (!c) return false;
    mtx_lock(&c->ev_mtx);
    if (c->ev_head == c->ev_tail) { mtx_unlock(&c->ev_mtx); return false; }
    *out = *c->ev[c->ev_head % c->ev_cap];
    free(c->ev[c->ev_head % c->ev_cap]);
    c->ev_head++;
    mtx_unlock(&c->ev_mtx);
    /* drain the wake byte */
    char b;
    while (read(c->ev_pipe[0], &b, 1) > 0) { }
    return true;
}

int tgf_event_fd(tgf_ctx *c) { return c ? c->ev_pipe[0] : -1; }

void tgf_event_free(tgf_event *ev) {
    if (!ev) return;
    free(ev->text);
    free(ev->source);
    free(ev->dest);
    memset(ev, 0, sizeof(*ev));
}

/* ---- commands ---------------------------------------------------------- */

static tgf_status cmd_push(tgf_ctx *c, int kind, const char *s, int64_t id, int enabled) {
    mtx_lock(&c->cmd_mtx);
    if (c->cmd_tail - c->cmd_head == c->cmd_cap) { mtx_unlock(&c->cmd_mtx); return TGF_ERR_BUSY; }
    tgf_cmd *k = &c->cmd[c->cmd_tail % c->cmd_cap];
    k->kind = kind;
    k->s = s ? strdup(s) : NULL;
    k->id = id;
    k->enabled = enabled;
    c->cmd_tail++;
    mtx_unlock(&c->cmd_mtx);
    return TGF_OK;
}

static void exec_cmd(tgf_ctx *c, void *client, const tgf_cmd *k) {
    Runtime *rt = &c->rt;
    switch (k->kind) {
    case CMD_PHONE: auth_submit(rt, client, TGF_AUTH_PHONE, k->s); break;
    case CMD_CODE:  auth_submit(rt, client, TGF_AUTH_CODE, k->s); break;
    case CMD_PW:    auth_submit(rt, client, TGF_AUTH_PASSWORD, k->s); break;
    case CMD_PAUSE:
        atomic_store(&c->paused, true);
        { tgf_event *e = ev_new(TGF_EV_PIPELINE_STATE); if (e) { e->pipe = TGF_PIPE_PAUSED; ev_emit(c, e); } }
        break;
    case CMD_RESUME:
        atomic_store(&c->paused, false);
        { tgf_event *e = ev_new(TGF_EV_PIPELINE_STATE); if (e) { e->pipe = TGF_PIPE_RUNNING; ev_emit(c, e); } }
        break;
    case CMD_ADD_SOURCE: {
        int n = rt->cfg.num_sources + 1;
        char **ns = realloc(rt->cfg.source_channels, n * sizeof(char *));
        if (ns) {
            rt->cfg.source_channels = ns;
            rt->cfg.source_channels[rt->cfg.num_sources] = strdup(k->s);
            rt->cfg.num_sources = n;
            rt->source_chat_ids = realloc(rt->source_chat_ids, n * sizeof(long long));
            rt->source_chat_ids[n - 1] = 0;
            rt->source_count = 0;
            rt->resolve_index = 0;
            rt->resolve_active = 0;
            if (rt->dest_resolved) resolve_next(rt, client);
        }
        break;
    }
    case CMD_REMOVE_SOURCE:
        for (int i = 0; i < rt->cfg.num_sources; i++)
            if (rt->source_chat_ids[i] == k->id) rt->source_chat_ids[i] = 0;
        break;
    case CMD_SET_ENABLED:
        if (k->enabled) {
            rt->resolve_index = 0;
            rt->source_count = 0;
            rt->resolve_active = 0;
            if (rt->dest_resolved) resolve_next(rt, client);
        } else {
            for (int i = 0; i < rt->cfg.num_sources; i++)
                if (rt->source_chat_ids[i] == k->id) rt->source_chat_ids[i] = 0;
        }
        break;
    case CMD_RESCAN:
        tracker_store_set_backfill_done(&rt->tracker, k->id, 0);
        tracker_store_set_scan_from(&rt->tracker, k->id, 0);
        tracker_store_save(&rt->tracker);
        break;
    case CMD_APPLY_CONFIG: {
        Runtime *rt2 = &c->rt;
        for (int i = 0; i < rt2->cfg.num_sources; i++) free(rt2->cfg.source_channels[i]);
        free(rt2->cfg.source_channels);
        free(rt2->cfg.api_hash); free(rt2->cfg.dest_channel); free(rt2->cfg.history_file);
        runtime_config_load(rt2, &k->cfg);
        config_copy(&c->cfg, &k->cfg);
        rt2->source_count = 0; rt2->resolve_index = 0; rt2->resolve_active = 0;
        if (rt2->dest_resolved) resolve_next(rt2, client);
        break;
    }
    case CMD_LOGOUT:
        transport_send(rt, client, "logOut", "", (RequestId){REQ_NONE, 0, 0});
        break;
    }
}

static void drain_cmds(tgf_ctx *c, void *client) {
    for (;;) {
        mtx_lock(&c->cmd_mtx);
        if (c->cmd_head == c->cmd_tail) { mtx_unlock(&c->cmd_mtx); break; }
        tgf_cmd k = c->cmd[c->cmd_head % c->cmd_cap];
        c->cmd_head++;
        mtx_unlock(&c->cmd_mtx);
        exec_cmd(c, client, &k);
        if (k.has_cfg) tgf_config_free(&k.cfg);
        if (k.s) { memset(k.s, 0, strlen(k.s)); free(k.s); }
    }
}

static void emit_queue(tgf_ctx *c) {
    Runtime *rt = &c->rt;
    tgf_event *e = ev_new(TGF_EV_QUEUE);
    if (e) { e->total = rt->fwd_queue_count; ev_emit(c, e); }
    tgf_event *n = ev_new(TGF_EV_NEXT_FORWARD_IN);
    if (n) {
        double now = (double)time(NULL);
        int64_t ms = 0;
        if (rt->fwd_queue_count > 0 && !atomic_load(&c->paused)) {
            double t = rt->fwd_flood_until > now ? rt->fwd_flood_until
                                                 : rt->fwd_last_time + rt->cfg.forward_delay_sec;
            ms = (int64_t)((t - now) * 1000.0);
            if (ms < 0) ms = 0;
        }
        n->ms = ms;
        ev_emit(c, n);
    }
}

static int core_thread(void *arg) {
    tgf_ctx *c = arg;
    Runtime *rt = &c->rt;
    LoopState ls = {0};
    void *client = c->tp->create();
    if (!client) {
        tgf_event *e = ev_new(TGF_EV_ERROR);
        if (e) { e->scope = TGF_ERR_SCOPE_PIPELINE; e->text = strdup("Failed to create TDLib client"); ev_emit(c, e); }
        return 0;
    }
    transport_send(rt, client, "setLogVerbosityLevel", "\"new_verbosity_level\":0", (RequestId){REQ_NONE, 0, 0});
    transport_send(rt, client, "getAuthorizationState", "", (RequestId){REQ_NONE, 0, 0});
    while (atomic_load(&c->running)) {
        drain_cmds(c, client);
        ls.paused = atomic_load(&c->paused);
        loop_tick(rt, client, &ls, NULL, NULL);
        emit_queue(c);
    }
    c->tp->destroy(client);
    return 0;
}

/* ---- config ------------------------------------------------------------ */

static char *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (!buf) { fclose(f); return NULL; }
    long got = (long)fread(buf, 1, n, f);
    fclose(f);
    if (got < 0) { free(buf); return NULL; }
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

static void set_str(char **dst, const char *v) { free(*dst); *dst = v ? strdup(v) : NULL; }

static void config_from_cjson(tgf_config *out, const cJSON *root) {
    memset(out, 0, sizeof(*out));
    out->forward_delay_sec = 1;
    out->history_window_hours = 24;
    const cJSON *v;
    v = cJSON_GetObjectItem(root, "api_id");
    if (v && cJSON_IsNumber(v)) out->api_id = (int)v->valuedouble;
    v = cJSON_GetObjectItem(root, "api_hash");
    if (v && cJSON_IsString(v)) set_str(&out->api_hash, v->valuestring);
    v = cJSON_GetObjectItem(root, "dest_channel");
    if (v && cJSON_IsString(v)) set_str(&out->dest_channel, v->valuestring);
    v = cJSON_GetObjectItem(root, "history_file");
    set_str(&out->history_file, v && cJSON_IsString(v) ? v->valuestring : "history.txt");
    v = cJSON_GetObjectItem(root, "forward_delay_sec");
    if (v && cJSON_IsNumber(v)) out->forward_delay_sec = (int)v->valuedouble;
    v = cJSON_GetObjectItem(root, "history_window_hours");
    if (v && cJSON_IsNumber(v)) out->history_window_hours = (int)v->valuedouble;
    v = cJSON_GetObjectItem(root, "enable_sequential_forwarding");
    if (v && cJSON_IsBool(v)) out->enable_sequential_forwarding = v->valueint;
    v = cJSON_GetObjectItem(root, "enable_ordering");
    if (v && cJSON_IsBool(v)) out->enable_ordering = v->valueint;
    v = cJSON_GetObjectItem(root, "sequence_direction");
    out->sequence_direction = (v && cJSON_IsString(v) && strcmp(v->valuestring, "desc") == 0) ? 1 : 0;
    v = cJSON_GetObjectItem(root, "source_channels");
    if (v && cJSON_IsArray(v)) {
        out->num_sources = cJSON_GetArraySize(v);
        out->source_channels = calloc(out->num_sources, sizeof(char *));
        for (int i = 0; i < out->num_sources; i++) {
            const cJSON *el = cJSON_GetArrayItem(v, i);
            out->source_channels[i] = strdup(el && cJSON_IsString(el) ? el->valuestring : "");
        }
    }
}

tgf_status tgf_config_load(const char *path, tgf_config *out) {
    long len;
    char *buf = read_file(path, &len);
    if (!buf) return TGF_ERR_IO;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return TGF_ERR_PARSE;
    config_from_cjson(out, root);
    cJSON_Delete(root);
    return TGF_OK;
}

static cJSON *config_to_json(const tgf_config *cfg) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "api_id", cfg->api_id);
    cJSON_AddStringToObject(r, "api_hash", cfg->api_hash ? cfg->api_hash : "");
    cJSON_AddStringToObject(r, "dest_channel", cfg->dest_channel ? cfg->dest_channel : "");
    cJSON_AddStringToObject(r, "history_file", cfg->history_file ? cfg->history_file : "history.txt");
    cJSON_AddNumberToObject(r, "forward_delay_sec", cfg->forward_delay_sec);
    cJSON_AddNumberToObject(r, "history_window_hours", cfg->history_window_hours);
    cJSON_AddBoolToObject(r, "enable_sequential_forwarding", cfg->enable_sequential_forwarding);
    cJSON_AddBoolToObject(r, "enable_ordering", cfg->enable_ordering);
    cJSON_AddStringToObject(r, "sequence_direction", cfg->sequence_direction ? "desc" : "asc");
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < cfg->num_sources; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(cfg->source_channels[i]));
    cJSON_AddItemToObject(r, "source_channels", arr);
    return r;
}

tgf_status tgf_config_save(const char *path, const tgf_config *cfg) {
    /* Preserve unknown keys: start from the existing file's JSON if it parses. */
    cJSON *root = NULL;
    long len;
    char *existing = read_file(path, &len);
    if (existing) {
        root = cJSON_Parse(existing);
        free(existing);
        if (!root) return TGF_ERR_PARSE;   /* never overwrite an unparseable file */
    }
    if (!root) root = cJSON_CreateObject();

    const cJSON *keep = cJSON_GetObjectItem(root, "api_hash"); (void)keep;
    /* remove known keys, then re-add from cfg */
    const char *keys[] = {"api_id","api_hash","dest_channel","history_file","forward_delay_sec",
                          "history_window_hours","enable_sequential_forwarding","enable_ordering",
                          "sequence_direction","source_channels"};
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) cJSON_DeleteItemFromObject(root, keys[i]);
    cJSON *fresh = config_to_json(cfg);
    cJSON *child;
    cJSON_ArrayForEach(child, fresh) { cJSON_AddItemToObject(root, child->string, cJSON_Duplicate(child, 1)); }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(fresh);
    cJSON_Delete(root);
    if (!text) return TGF_ERR_OOM;

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(text); return TGF_ERR_IO; }
    size_t n = strlen(text);
    size_t w = fwrite(text, 1, n, f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
    fclose(f);
    free(text);
    if (w != n) { remove(tmp); return TGF_ERR_IO; }
    if (chmod(tmp, 0600) != 0) { /* mode is best-effort */ }
    if (rename(tmp, path) != 0) { remove(tmp); return TGF_ERR_IO; }
    return TGF_OK;
}

void tgf_config_free(tgf_config *cfg) {
    if (!cfg) return;
    free(cfg->api_hash);
    free(cfg->dest_channel);
    free(cfg->history_file);
    if (cfg->source_channels) {
        for (int i = 0; i < cfg->num_sources; i++) free(cfg->source_channels[i]);
        free(cfg->source_channels);
    }
    memset(cfg, 0, sizeof(*cfg));
}

/* ---- lifecycle --------------------------------------------------------- */

static void config_copy(tgf_config *dst, const tgf_config *src) {
    *dst = *src;
    dst->api_hash = src->api_hash ? strdup(src->api_hash) : NULL;
    dst->dest_channel = src->dest_channel ? strdup(src->dest_channel) : NULL;
    dst->history_file = src->history_file ? strdup(src->history_file) : NULL;
    dst->source_channels = NULL;
    if (src->num_sources > 0) {
        dst->source_channels = calloc(src->num_sources, sizeof(char *));
        for (int i = 0; i < src->num_sources; i++)
            dst->source_channels[i] = strdup(src->source_channels[i]);
    }
}

static void runtime_config_load(Runtime *rt, const tgf_config *cfg) {
    rt->cfg.api_id = cfg->api_id;
    set_str(&rt->cfg.api_hash, cfg->api_hash);
    set_str(&rt->cfg.dest_channel, cfg->dest_channel);
    set_str(&rt->cfg.history_file, cfg->history_file ? cfg->history_file : "history.txt");
    rt->cfg.forward_delay_sec = cfg->forward_delay_sec;
    rt->cfg.history_window_hours = cfg->history_window_hours;
    rt->cfg.enable_sequential_forwarding = cfg->enable_sequential_forwarding;
    rt->cfg.enable_ordering = cfg->enable_ordering;
    rt->cfg.sequence_direction = cfg->sequence_direction;
    rt->cfg.num_sources = cfg->num_sources;
    rt->cfg.source_channels = calloc(cfg->num_sources, sizeof(char *));
    for (int i = 0; i < cfg->num_sources; i++)
        rt->cfg.source_channels[i] = strdup(cfg->source_channels[i]);
}

tgf_ctx *tgf_create(const tgf_config *cfg, const tgf_transport *tp) {
    if (!cfg || !tp) return NULL;
    tgf_ctx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->tp = tp;
    atomic_init(&c->running, false);
    atomic_init(&c->paused, false);
    config_copy(&c->cfg, cfg);
    runtime_config_load(&c->rt, cfg);
    c->rt.tp = tp;
    c->rt.core = c;
    c->rt.emit = core_emit;
    c->rt.keep_running = true;
    c->rt.src_name = "unknown";
    mtx_init(&c->ev_mtx, mtx_plain);
    mtx_init(&c->cmd_mtx, mtx_plain);
    c->ev_cap = 64;
    c->ev = calloc(c->ev_cap, sizeof(*c->ev));
    c->cmd_cap = 32;
    c->cmd = calloc(c->cmd_cap, sizeof(*c->cmd));
    if (pipe(c->ev_pipe) != 0) { free(c->ev); free(c->cmd); free(c); return NULL; }
    /* non-blocking so tgf_next_event's wake-byte drain never blocks */
    for (int i = 0; i < 2; i++) {
        int fl = fcntl(c->ev_pipe[i], F_GETFL, 0);
        if (fl >= 0) fcntl(c->ev_pipe[i], F_SETFL, fl | O_NONBLOCK);
    }
    s_ctx = c;
    return c;
}

tgf_status tgf_start(tgf_ctx *c) {
    if (!c) return TGF_ERR_INVALID;
    if (c->started) return TGF_OK;
    Runtime *rt = &c->rt;
    rt->source_chat_ids = calloc(rt->cfg.num_sources, sizeof(long long));
    if (!rt->source_chat_ids) return TGF_ERR_OOM;
    history_store_load(&rt->history, &rt->cfg);
    fwd_queue_load(rt);
    if (rt->cfg.enable_sequential_forwarding) { tracker_store_load(&rt->tracker); seq_pending_load(rt); }
    atomic_store(&c->running, true);
    if (thrd_create(&c->thread, core_thread, c) != thrd_success) {
        atomic_store(&c->running, false);
        return TGF_ERR_STATE;
    }
    c->started = true;
    return TGF_OK;
}

void tgf_stop(tgf_ctx *c) {
    if (!c || !c->started) return;
    atomic_store(&c->running, false);
    thrd_join(c->thread, NULL);
    c->started = false;
}

void tgf_destroy(tgf_ctx *c) {
    if (!c) return;
    tgf_stop(c);
    /* free any queued events */
    for (int i = c->ev_head; i < c->ev_tail; i++) {
        tgf_event *e = c->ev[i % c->ev_cap];
        if (e) { tgf_event_free(e); free(e); }
    }
    free(c->ev);
    for (int i = c->cmd_head; i < c->cmd_tail; i++) free(c->cmd[i % c->cmd_cap].s);
    free(c->cmd);
    close(c->ev_pipe[0]);
    close(c->ev_pipe[1]);
    mtx_destroy(&c->ev_mtx);
    mtx_destroy(&c->cmd_mtx);
    Runtime *rt = &c->rt;
    for (int i = 0; i < rt->history.set_size; i++) free(rt->history.set[i]);
    free(rt->history.set); free(rt->history.time);
    for (int i = 0; i < rt->fwd_queue_count; i++) { int idx = (rt->fwd_queue_head + i) % rt->fwd_queue_cap; free(rt->fwd_queue[idx].ids); }
    free(rt->fwd_queue); free(rt->fwd_inflight.ids); free(rt->source_chat_ids);
    for (int i = 0; i < rt->cfg.num_sources; i++) free(rt->cfg.source_channels[i]);
    free(rt->cfg.source_channels);
    free(rt->cfg.api_hash); free(rt->cfg.dest_channel); free(rt->cfg.history_file);
    tgf_config_free(&c->cfg);
    free(c);
}

/* ---- command entry points --------------------------------------------- */

tgf_status tgf_submit_phone(tgf_ctx *c, const char *p)    { return c && p ? cmd_push(c, CMD_PHONE, p, 0, 0) : TGF_ERR_INVALID; }
tgf_status tgf_submit_code(tgf_ctx *c, const char *v)     { return c && v ? cmd_push(c, CMD_CODE, v, 0, 0) : TGF_ERR_INVALID; }
tgf_status tgf_submit_password(tgf_ctx *c, const char *p) { return c && p ? cmd_push(c, CMD_PW, p, 0, 0) : TGF_ERR_INVALID; }
tgf_status tgf_pause(tgf_ctx *c)                          { return c ? cmd_push(c, CMD_PAUSE, NULL, 0, 0) : TGF_ERR_INVALID; }
tgf_status tgf_resume(tgf_ctx *c)                         { return c ? cmd_push(c, CMD_RESUME, NULL, 0, 0) : TGF_ERR_INVALID; }
tgf_status tgf_add_source(tgf_ctx *c, const char *h)      { return c && h ? cmd_push(c, CMD_ADD_SOURCE, h, 0, 0) : TGF_ERR_INVALID; }
tgf_status tgf_remove_source(tgf_ctx *c, int64_t id)      { return c ? cmd_push(c, CMD_REMOVE_SOURCE, NULL, id, 0) : TGF_ERR_INVALID; }
tgf_status tgf_set_source_enabled(tgf_ctx *c, int64_t id, bool e) { return c ? cmd_push(c, CMD_SET_ENABLED, NULL, id, e) : TGF_ERR_INVALID; }
tgf_status tgf_rescan_source(tgf_ctx *c, int64_t id)      { return c ? cmd_push(c, CMD_RESCAN, NULL, id, 0) : TGF_ERR_INVALID; }
tgf_status tgf_apply_config(tgf_ctx *c, const tgf_config *cfg) {
    if (!c || !cfg) return TGF_ERR_INVALID;
    mtx_lock(&c->cmd_mtx);
    if (c->cmd_tail - c->cmd_head == c->cmd_cap) { mtx_unlock(&c->cmd_mtx); return TGF_ERR_BUSY; }
    tgf_cmd *k = &c->cmd[c->cmd_tail % c->cmd_cap];
    k->kind = CMD_APPLY_CONFIG; k->s = NULL; k->id = 0; k->enabled = 0;
    config_copy(&k->cfg, cfg); k->has_cfg = 1;
    c->cmd_tail++;
    mtx_unlock(&c->cmd_mtx);
    return TGF_OK;
}
tgf_status tgf_logout(tgf_ctx *c)                         { return c ? cmd_push(c, CMD_LOGOUT, NULL, 0, 0) : TGF_ERR_INVALID; }
