#include "tgf.h"
#include "core_priv.h"
#include "config.h"
#include "loop.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

/* ---- events ------------------------------------------------------------ */

tgf_event *ev_new(tgf_event_kind kind) {
    tgf_event *e = calloc(1, sizeof(*e));
    if (e) e->kind = kind;
    return e;
}

void ev_emit(tgf_ctx *c, tgf_event *e) {
    if (!e) return;
    mtx_lock(&c->ev_mtx);
    if (c->ev_tail - c->ev_head == c->ev_cap) {
        int ncap = c->ev_cap * 2, cnt = c->ev_tail - c->ev_head;
        tgf_event **n = calloc(ncap, sizeof(*n));
        if (!n) {
            mtx_unlock(&c->ev_mtx);
            return;
        }
        for (int i = 0; i < cnt; i++) n[i] = c->ev[(c->ev_head + i) % c->ev_cap];
        free(c->ev);
        c->ev = n;
        c->ev_cap = ncap;
        c->ev_head = 0;
        c->ev_tail = cnt;
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
    if (ev->text) e->text = strdup(ev->text);
    if (ev->source) e->source = strdup(ev->source);
    if (ev->dest) e->dest = strdup(ev->dest);
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
    e->level = strncmp(msg, "ERROR", 5) == 0     ? TGF_LOG_ERROR
               : strncmp(msg, "WARNING", 7) == 0 ? TGF_LOG_WARN
                                                 : TGF_LOG_INFO;
    ev_emit(s_ctx, e);
}

void ui_oom(const char *msg) {
    ui_status(msg);
}

void ui_forwarded(const char *source, const char *dest) {
    if (!s_ctx) return;
    tgf_event *e = ev_new(TGF_EV_FORWARDED);
    if (!e) return;
    e->source = strdup(source);
    e->dest = strdup(dest);
    ev_emit(s_ctx, e);
}

void ui_refresh(const char *dest, const char *source, int src_count, int delay) {
    (void)dest;
    (void)source;
    (void)src_count;
    (void)delay;
}

bool tgf_next_event(tgf_ctx *c, tgf_event *out) {
    if (!c) return false;
    mtx_lock(&c->ev_mtx);
    if (c->ev_head == c->ev_tail) {
        mtx_unlock(&c->ev_mtx);
        return false;
    }
    *out = *c->ev[c->ev_head % c->ev_cap];
    free(c->ev[c->ev_head % c->ev_cap]);
    c->ev_head++;
    mtx_unlock(&c->ev_mtx);
    /* drain the wake byte */
    char b;
    while (read(c->ev_pipe[0], &b, 1) > 0) {
    }
    return true;
}

int tgf_event_fd(tgf_ctx *c) {
    return c ? c->ev_pipe[0] : -1;
}

void tgf_event_free(tgf_event *ev) {
    if (!ev) return;
    free(ev->text);
    free(ev->source);
    free(ev->dest);
    memset(ev, 0, sizeof(*ev));
}

static const char *source_name_for(const Runtime *rt, long long chat_id) {
    for (int i = 0; i < rt->cfg.num_sources; i++)
        if (rt->source_chat_ids[i] == chat_id) return rt->cfg.source_channels[i];
    return NULL;
}

static void emit_queue(tgf_ctx *c) {
    Runtime *rt = &c->rt;
    tgf_event *e = ev_new(TGF_EV_QUEUE);
    if (e) {
        e->total = rt->fwd_queue_count;
        ev_emit(c, e);
    }
    for (int i = 0; i < rt->fwd_queue_count && i < 5; i++) {
        int idx = (rt->fwd_queue_head + i) % rt->fwd_queue_cap;
        ForwardJob *j = &rt->fwd_queue[idx];
        tgf_event *p = ev_new(TGF_EV_QUEUE);
        if (!p) continue;
        p->total = rt->fwd_queue_count;
        p->id = j->count > 0 ? j->ids[0] : 0;
        const char *name = source_name_for(rt, j->src_chat_id);
        if (name) p->source = strdup(name);
        ev_emit(c, p);
    }
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

static int backfill_progress(const Runtime *rt, long long chat_id) {
    if (!rt->cfg.enable_sequential_forwarding) return 100;
    for (int i = 0; i < rt->tracker.count; i++) {
        const SeqEntry *entry = &rt->tracker.entries[i];
        if (entry->chat_id != chat_id) continue;
        if (entry->backfill_done) return 100;
        if (entry->last_msg_id <= 0 || entry->scan_from_id <= 0) return 0;
        long long scanned = entry->last_msg_id - entry->scan_from_id;
        if (scanned < 0) scanned = 0;
        int pct = (int)(scanned * 100 / entry->last_msg_id);
        return pct > 100 ? 100 : pct;
    }
    return 0;
}

static void emit_source_states(tgf_ctx *c) {
    Runtime *rt = &c->rt;
    if (!rt->cfg.enable_sequential_forwarding) return;
    for (int i = 0; i < rt->source_count && i < MAX_SEQ_TRACKER; i++) {
        long long chat_id = rt->source_chat_ids[i];
        if (!chat_id) continue;
        int pct = backfill_progress(rt, chat_id);
        if (pct == c->last_progress[i]) continue;
        c->last_progress[i] = pct;
        tgf_event *e = ev_new(TGF_EV_SOURCE_STATE);
        if (!e) continue;
        e->id = chat_id;
        e->total = rt->source_count;
        e->progress = pct;
        e->src_state = rt->cfg.enable_sequential_forwarding && pct < 100
                           ? TGF_SRC_SCANNING : TGF_SRC_LIVE;
        const char *name = source_name_for(rt, chat_id);
        if (name) e->source = strdup(name);
        ev_emit(c, e);
    }
}

static int core_thread(void *arg) {
    tgf_ctx *c = arg;
    Runtime *rt = &c->rt;
    LoopState ls = {0};
    void *client = c->tp->create();
    if (!client) {
        tgf_event *e = ev_new(TGF_EV_ERROR);
        if (e) {
            e->scope = TGF_ERR_SCOPE_PIPELINE;
            e->text = strdup("Failed to create TDLib client");
            ev_emit(c, e);
        }
        return 0;
    }
    transport_send(rt, client, "setLogVerbosityLevel", "\"new_verbosity_level\":0",
                   (RequestId){REQ_NONE, 0, 0});
    transport_send(rt, client, "getAuthorizationState", "", (RequestId){REQ_NONE, 0, 0});
    while (atomic_load(&c->running)) {
        drain_cmds(c, client);
        ls.paused = atomic_load(&c->paused);
        loop_tick(rt, client, &ls, NULL, NULL);
        if (rt->fatal) {
            tgf_event *e = ev_new(TGF_EV_ERROR);
            if (e) {
                e->scope = TGF_ERR_SCOPE_PIPELINE;
                e->code = 1;
                e->text = strdup(rt->fatal_msg[0] ? rt->fatal_msg : "Fatal core error");
                ev_emit(c, e);
            }
            break;
        }
        emit_queue(c);
        emit_source_states(c);
    }
    c->tp->destroy(client);
    return 0;
}

/* ---- config ------------------------------------------------------------ */

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
    if (!c->ev || !c->cmd || pipe(c->ev_pipe) != 0) {
        free(c->ev);
        free(c->cmd);
        mtx_destroy(&c->ev_mtx);
        mtx_destroy(&c->cmd_mtx);
        config_free(&c->rt.cfg);
        tgf_config_free(&c->cfg);
        free(c);
        return NULL;
    }
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
    free(rt->source_chat_ids);
    rt->source_chat_ids =
        calloc(rt->cfg.num_sources > 0 ? rt->cfg.num_sources : 1, sizeof(long long));
    if (!rt->source_chat_ids) return TGF_ERR_OOM;
    if (!c->loaded) {
        history_store_load(&rt->history, &rt->cfg);
        fwd_queue_load(rt);
        if (rt->cfg.enable_sequential_forwarding) {
            tracker_store_load(&rt->tracker);
            seq_pending_load(rt);
        }
        c->loaded = true;
    }
    rt->authorized = 0;
    rt->dest_resolved = 0;
    rt->dest_chat_id = 0;
    rt->source_count = 0;
    rt->resolve_index = 0;
    rt->resolve_active = 0;
    rt->fatal = 0;
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
    if (s_ctx == c) s_ctx = NULL;
    /* free any queued events */
    for (int i = c->ev_head; i < c->ev_tail; i++) {
        tgf_event *e = c->ev[i % c->ev_cap];
        if (e) {
            tgf_event_free(e);
            free(e);
        }
    }
    free(c->ev);
    for (int i = c->cmd_head; i < c->cmd_tail; i++) {
        tgf_cmd *k = &c->cmd[i % c->cmd_cap];
        if (k->has_cfg) tgf_config_free(&k->cfg);
        free(k->s);
    }
    free(c->cmd);
    close(c->ev_pipe[0]);
    close(c->ev_pipe[1]);
    mtx_destroy(&c->ev_mtx);
    mtx_destroy(&c->cmd_mtx);
    Runtime *rt = &c->rt;
    for (int i = 0; i < rt->history.set_size; i++) free(rt->history.set[i]);
    free(rt->history.set);
    free(rt->history.time);
    for (int i = 0; i < rt->fwd_queue_count; i++) {
        int idx = (rt->fwd_queue_head + i) % rt->fwd_queue_cap;
        free(rt->fwd_queue[idx].ids);
    }
    free(rt->fwd_queue);
    free(rt->fwd_inflight.ids);
    free(rt->source_chat_ids);
    for (int i = 0; i < rt->tracker.count; i++) free(rt->tracker.entries[i].pending);
    config_free(&rt->cfg);
    tgf_config_free(&c->cfg);
    free(c);
}
