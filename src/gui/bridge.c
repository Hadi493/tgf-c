#include "gui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void now_time(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, len, "%H:%M:%S", &tm);
}

static void log_push(GuiState *s, const char *text, int category) {
    GuiLogLine *line = &s->log[s->log_len % TGF_GUI_MAX_LOG];
    snprintf(line->text, sizeof(line->text), "%s", text ? text : "");
    line->category = category;
    now_time(line->time, sizeof(line->time));
    s->log_len++;
}

static void source_touch(GuiSourceRow *r) {
    now_time(r->last_activity, sizeof(r->last_activity));
}

static void source_upsert(GuiState *s, const tgf_event *ev) {
    const char *handle = ev->source ? ev->source : "";
    for (int i = 0; i < s->source_count; i++) {
        if (strcmp(s->sources[i].handle, handle) == 0) {
            snprintf(s->sources[i].handle, sizeof(s->sources[i].handle), "%s", handle);
            s->sources[i].chat_id = ev->id;
            s->sources[i].last_id = ev->last_id;
            s->sources[i].progress = ev->progress;
            s->sources[i].state = ev->src_state;
            source_touch(&s->sources[i]);
            return;
        }
    }
    if (s->source_count >= 64) return;
    GuiSourceRow *r = &s->sources[s->source_count++];
    snprintf(r->handle, sizeof(r->handle), "%s", handle);
    r->chat_id = ev->id;
    r->last_id = ev->last_id;
    r->progress = ev->progress;
    r->state = ev->src_state;
    source_touch(r);
}

static void source_counter(GuiState *s, const char *handle, int forwarded) {
    for (int i = 0; i < s->source_count; i++) {
        if (strcmp(s->sources[i].handle, handle ? handle : "") != 0) continue;
        if (forwarded) s->sources[i].forwarded++;
        else s->sources[i].skipped++;
        source_touch(&s->sources[i]);
        return;
    }
}

void gui_state_init(GuiState *s, const char *dest) {
    (void)dest;
    memset(s, 0, sizeof(*s));
}

void gui_bridge_init(GuiBridge *b, tgf_ctx *ctx) {
    memset(b, 0, sizeof(*b));
    b->ctx = ctx;
    b->page_event = NULL;
}

void gui_bridge_destroy(GuiBridge *b) {
    b->ctx = NULL;
    b->page_event = NULL;
}

void gui_bridge_shutdown(GuiBridge *b) { (void)b; }

void gui_bridge_source_toggle(GuiBridge *b, const char *handle, gboolean enable) {
    if (!b->ctx) return;
    tgf_status st = enable ? tgf_set_source_enabled_by_handle(b->ctx, handle, TRUE)
                            : tgf_set_source_enabled_by_handle(b->ctx, handle, FALSE);
    (void)st;
}

void gui_bridge_source_remove(GuiBridge *b, const char *handle) {
    if (!b->ctx) return;
    tgf_status st = tgf_remove_source_by_handle(b->ctx, handle);
    (void)st;
}

void gui_bridge_source_add(GuiBridge *b, const char *handle) {
    if (!b->ctx) return;
    tgf_status st = tgf_add_source(b->ctx, handle);
    (void)st;
}

void gui_bridge_source_rescan(GuiBridge *b, const char *handle) {
    if (!b->ctx) return;
    tgf_status st = tgf_rescan_source_by_handle(b->ctx, handle);
    (void)st;
}

void gui_bridge_pause(GuiBridge *b) { if (b->ctx) tgf_pause(b->ctx); }
void gui_bridge_resume(GuiBridge *b) { if (b->ctx) tgf_resume(b->ctx); }
void gui_bridge_clear_auth_error(GuiBridge *b) { b->state.metrics.auth_error[0] = 0; }

void gui_bridge_set_page_event(GuiBridge *b, void (*fn)(GuiBridge *b, const tgf_event *ev)) {
    b->page_event = fn;
}

void gui_bridge_event(GuiBridge *b, tgf_event *ev) {
    GuiState *s = &b->state;
    char buf[512];
    switch (ev->kind) {
    case TGF_EV_AUTH_STATE:
        s->metrics.auth_state = (int)ev->auth;
        break;
    case TGF_EV_CONNECTION_STATE:
        s->metrics.conn_state = (int)ev->conn;
        break;
    case TGF_EV_SOURCE_STATE:
        source_upsert(s, ev);
        break;
    case TGF_EV_FORWARDED:
        s->metrics.forwarded++;
        source_counter(s, ev->source, 1);
        snprintf(buf, sizeof(buf), "Forwarded %s #%lld", ev->source ? ev->source : "?", (long long)ev->id);
        log_push(s, buf, TGF_LOG_FORWARDED);
        break;
    case TGF_EV_SKIPPED:
        s->metrics.skipped++;
        source_counter(s, ev->source, 0);
        snprintf(buf, sizeof(buf), "Skipped %s #%lld (%s)", ev->source ? ev->source : "?",
                 (long long)ev->id, ev->skip == TGF_SKIP_WINDOW ? "outside window" : "duplicate");
        log_push(s, buf, TGF_LOG_SKIPPED);
        break;
    case TGF_EV_QUEUE:
        if (ev->id > 0) {
            if (s->queue_len < TGF_GUI_MAX_QUEUE) {
                s->queue_ids[s->queue_len] = ev->id;
                snprintf(s->queue_src[s->queue_len], sizeof(s->queue_src[0]), "%s", ev->source ? ev->source : "");
                s->queue_len++;
            }
        } else {
            s->queue_len = 0;
            s->metrics.queued = (int)ev->total;
        }
        break;
    case TGF_EV_NEXT_FORWARD_IN:
        s->metrics.next_in_ms = (int)ev->ms;
        break;
    case TGF_EV_PIPELINE_STATE:
        s->metrics.paused = ev->pipe == TGF_PIPE_PAUSED;
        s->metrics.running = ev->pipe == TGF_PIPE_RUNNING;
        break;
    case TGF_EV_LOG:
        if (ev->text) log_push(s, ev->text, ev->level == TGF_LOG_INFO ? TGF_LOG_ALL : TGF_LOG_WARNINGS);
        break;
    case TGF_EV_ERROR:
        s->metrics.errors++;
        snprintf(s->metrics.fatal, sizeof(s->metrics.fatal), "%s", ev->text ? ev->text : "Fatal core error");
        if (ev->scope == TGF_ERR_SCOPE_AUTH)
            snprintf(s->metrics.auth_error, sizeof(s->metrics.auth_error), "%s", s->metrics.fatal);
        log_push(s, s->metrics.fatal, TGF_LOG_WARNINGS);
        break;
    default:
        break;
    }
    if (b->page_event) b->page_event(b, ev);
    tgf_event_free(ev);
}
