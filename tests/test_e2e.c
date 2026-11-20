#include "tgf.h"
#include "config.h"
#include "loop.h"
#include "transport_fake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static Runtime rt;
static int checks = 0, failures = 0;

#define CHECK_LL(actual, expected) do { \
    long long a_ = (long long)(actual), e_ = (long long)(expected); \
    checks++; \
    if (a_ != e_) { failures++; fprintf(stderr, "FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual, a_, e_); } \
} while (0)

void ui_status(const char *msg) { snprintf(rt.status_msg, sizeof(rt.status_msg), "%s", msg); }
void ui_oom(const char *msg) { ui_status(msg); }
void ui_forwarded(const char *s, const char *d) { (void)s; (void)d; }

static cJSON *msg_obj(long long id, long long date, long long album, long long reply, long long chat) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "id", (double)id);
    cJSON_AddNumberToObject(m, "date", (double)date);
    cJSON_AddNumberToObject(m, "chat_id", (double)chat);
    if (album) cJSON_AddNumberToObject(m, "media_album_id", (double)album);
    if (reply) { cJSON *r = cJSON_CreateObject(); cJSON_AddNumberToObject(r, "message_id", (double)reply); cJSON_AddItemToObject(m, "reply_to", r); }
    return m;
}

static void push_history(long long src, cJSON *arr) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, "messages", arr);
    cJSON_AddStringToObject(r, "@type", "messages");
    char ex[64]; snprintf(ex, sizeof(ex), "history_%lld", src);
    cJSON_AddStringToObject(r, "@extra", ex);
    char *s = cJSON_PrintUnformatted(r); fake_push(s); free(s); cJSON_Delete(r);
}

static void push_ok(const char *extra) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "@type", "ok");
    cJSON_AddStringToObject(r, "@extra", extra);
    char *s = cJSON_PrintUnformatted(r); fake_push(s); free(s); cJSON_Delete(r);
}

static void reset_state(void) {
    if (rt.history.set) for (int i = 0; i < rt.history.set_size; i++) free(rt.history.set[i]);
    free(rt.history.set); free(rt.history.time);
    rt.history.set = NULL; rt.history.time = NULL; rt.history.set_size = 0; rt.history.count = 0;
    for (int i = 0; i < rt.tracker.count; i++) free(rt.tracker.entries[i].pending);
    memset(rt.tracker.entries, 0, sizeof(rt.tracker.entries)); rt.tracker.count = 0;
    for (int i = 0; i < rt.fwd_queue_count; i++) { int idx = (rt.fwd_queue_head + i) % rt.fwd_queue_cap; free(rt.fwd_queue[idx].ids); }
    free(rt.fwd_queue); rt.fwd_queue = NULL; rt.fwd_queue_count = rt.fwd_queue_cap = rt.fwd_queue_head = 0;
    rt.fwd_last_time = 0; free(rt.fwd_inflight.ids); rt.fwd_inflight.ids = NULL; rt.fwd_inflight.retries = 0;
    rt.fwd_inflight_active = false; rt.fwd_inflight_time = 0; rt.fwd_flood_until = 0;
    free(rt.source_chat_ids); rt.source_chat_ids = NULL;
    if (rt.cfg.source_channels) { for (int i = 0; i < rt.cfg.num_sources; i++) free(rt.cfg.source_channels[i]); free(rt.cfg.source_channels); }
    free(rt.cfg.api_hash); free(rt.cfg.dest_channel); free(rt.cfg.history_file);
    memset(&rt.cfg, 0, sizeof(rt.cfg));
    rt.authorized = rt.dest_resolved = rt.source_count = rt.resolve_index = rt.resolve_active = 0;
    rt.src_name = "unknown"; rt.status_msg[0] = 0;
    fake_reset();
    remove("history.txt"); remove("seq_tracker.txt"); remove("seq_pending.txt");
    remove("fwd_queue.txt"); remove("fwd_queue.txt.tmp"); remove("config.json");
}

static void setup(int sequential, int ordering, int direction, int delay) {
    reset_state();
    rt.tp = &transport_fake;
    rt.cfg.history_window_hours = 0;
    rt.cfg.enable_sequential_forwarding = sequential;
    rt.cfg.enable_ordering = ordering;
    rt.cfg.sequence_direction = direction;
    rt.cfg.forward_delay_sec = delay;
    rt.cfg.num_sources = 1;
    rt.cfg.source_channels = calloc(1, sizeof(char *));
    rt.cfg.source_channels[0] = strdup("src");
    rt.cfg.dest_channel = strdup("@me");
    rt.cfg.history_file = strdup("history.txt");
    rt.authorized = 1; rt.dest_resolved = 1; rt.dest_chat_id = -100777;
    rt.source_chat_ids = calloc(1, sizeof(long long));
    rt.source_chat_ids[0] = -100123;
    rt.source_count = 1; rt.resolve_active = 0;
}

static void run_ticks(int n) {
    LoopState ls = {0};
    ls.last_poll = time(NULL) + 100000;
    ls.last_prune = time(NULL) + 100000;
    void *client = transport_fake.create();
    for (int i = 0; i < n; i++) loop_tick(&rt, client, &ls, NULL, NULL);
}

static int forwards(long long *out, int max) {
    int n = 0;
    for (int i = 0; i < fake_sent_count(); i++) {
        cJSON *r = cJSON_Parse(fake_sent(i));
        if (!r) continue;
        const char *t = cJSON_GetStr(r, "@type");
        if (t && !strcmp(t, "forwardMessages")) {
            cJSON *ids = cJSON_GetObjectItem(r, "message_ids");
            if (ids && cJSON_IsArray(ids) && cJSON_GetArraySize(ids) > 0 && n < max)
                out[n++] = (long long)cJSON_GetArrayItem(ids, 0)->valuedouble;
        }
        cJSON_Delete(r);
    }
    return n;
}

static void e2e_asc(void) {
    setup(0, 1, 0, 0);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(30, 300, 0, 0, -100123));
    cJSON_AddItemToArray(arr, msg_obj(10, 100, 0, 0, -100123));
    cJSON_AddItemToArray(arr, msg_obj(20, 200, 0, 0, -100123));
    push_history(-100123, arr);
    push_ok("fwd_-100123_10"); push_ok("fwd_-100123_20"); push_ok("fwd_-100123_30");
    run_ticks(5);
    long long f[8]; int n = forwards(f, 8);
    CHECK_LL(n, 3); CHECK_LL(f[0], 10); CHECK_LL(f[1], 20); CHECK_LL(f[2], 30);
}

static void e2e_desc(void) {
    setup(0, 1, 1, 0);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(10, 100, 0, 0, -100123));
    cJSON_AddItemToArray(arr, msg_obj(30, 300, 0, 0, -100123));
    cJSON_AddItemToArray(arr, msg_obj(20, 200, 0, 0, -100123));
    push_history(-100123, arr);
    push_ok("fwd_-100123_30"); push_ok("fwd_-100123_20"); push_ok("fwd_-100123_10");
    run_ticks(5);
    long long f[8]; int n = forwards(f, 8);
    CHECK_LL(n, 3); CHECK_LL(f[0], 30); CHECK_LL(f[1], 20); CHECK_LL(f[2], 10);
}

static void e2e_dedupe_across_polls(void) {
    setup(0, 1, 0, 0);
    cJSON *a1 = cJSON_CreateArray();
    cJSON_AddItemToArray(a1, msg_obj(10, 100, 0, 0, -100123));
    cJSON_AddItemToArray(a1, msg_obj(20, 200, 0, 0, -100123));
    push_history(-100123, a1);
    push_ok("fwd_-100123_10"); push_ok("fwd_-100123_20");
    cJSON *a2 = cJSON_CreateArray();
    cJSON_AddItemToArray(a2, msg_obj(10, 100, 0, 0, -100123));
    cJSON_AddItemToArray(a2, msg_obj(20, 200, 0, 0, -100123));
    push_history(-100123, a2);
    run_ticks(6);
    long long f[8]; int n = forwards(f, 8);
    CHECK_LL(n, 2); CHECK_LL(f[0], 10); CHECK_LL(f[1], 20);
}

static void e2e_delay_gate(void) {
    setup(0, 1, 0, 3600);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(10, 100, 0, 0, -100123));
    cJSON_AddItemToArray(arr, msg_obj(20, 200, 0, 0, -100123));
    cJSON_AddItemToArray(arr, msg_obj(30, 300, 0, 0, -100123));
    push_history(-100123, arr);
    push_ok("fwd_-100123_10"); push_ok("fwd_-100123_20"); push_ok("fwd_-100123_30");
    run_ticks(5);
    long long f[8]; int n = forwards(f, 8);
    CHECK_LL(n, 1); CHECK_LL(f[0], 10);
}

static void e2e_backfill_asc(void) {
    setup(1, 0, 0, 0);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, msg_obj(30, 300, 0, 0, -100123));
    cJSON_AddItemToArray(arr, msg_obj(10, 100, 0, 0, -100123));
    cJSON_AddItemToArray(arr, msg_obj(20, 200, 0, 0, -100123));
    push_history(-100123, arr);
    push_history(-100123, cJSON_CreateArray());
    push_ok("fwd_-100123_10"); push_ok("fwd_-100123_20"); push_ok("fwd_-100123_30");
    run_ticks(7);
    long long f[8]; int n = forwards(f, 8);
    CHECK_LL(n, 3); CHECK_LL(f[0], 10); CHECK_LL(f[1], 20); CHECK_LL(f[2], 30);
}

typedef void (*test_fn)(void);
static void run_test(const char *name, test_fn fn) {
    int before = failures;
    fn();
    printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

int main(void) {
    char tmpl[] = "/tmp/tgf-e2e-XXXXXX";
    char dir[256];
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
    snprintf(dir, sizeof(dir), "%s", tmpl);
    if (chdir(dir) != 0) { perror("chdir"); return 1; }

    run_test("e2e_asc", e2e_asc);
    run_test("e2e_desc", e2e_desc);
    run_test("e2e_dedupe_across_polls", e2e_dedupe_across_polls);
    run_test("e2e_delay_gate", e2e_delay_gate);
    run_test("e2e_backfill_asc", e2e_backfill_asc);
    reset_state();
    chdir("/");
    rmdir(dir);
    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
