#include "tgf_core.h"
#include "tgf.h"
#include "transport_fake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int checks = 0, failures = 0;
static void push_auth(const char *state);
#define CHECK_LL(a,e) do { long long a_=(long long)(a), e_=(long long)(e); checks++; \
    if (a_!=e_) { failures++; fprintf(stderr,"FAIL %s:%d: %s==%lld expected %lld\n",__FILE__,__LINE__,#a,a_,e_); } } while(0)
#define CHECK_STR(a,e) do { const char *a_=(a); checks++; \
    if (!a_ || strcmp(a_,e)) { failures++; fprintf(stderr,"FAIL %s:%d: %s=='%s' expected '%s'\n",__FILE__,__LINE__,#a,a_?a_:"(null)",e); } } while(0)

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputs(text, f);
    fclose(f);
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    b[n] = 0; fclose(f);
    return b;
}

static tgf_config make_cfg(void) {
    tgf_config c;
    memset(&c, 0, sizeof(c));
    c.api_id = 12345;
    c.api_hash = strdup("hashhashhash");
    c.dest_channel = strdup("@me");
    c.history_file = strdup("history.txt");
    c.forward_delay_sec = 0;
    c.history_window_hours = 24;
    c.enable_sequential_forwarding = 0;
    c.enable_ordering = 0;
    c.sequence_direction = 0;
    c.num_sources = 1;
    c.source_channels = calloc(1, sizeof(char *));
    c.source_channels[0] = strdup("telegram");
    return c;
}

static tgf_config make_cfg_n(int n) {
    tgf_config c = make_cfg();
    tgf_config_free(&c);
    memset(&c, 0, sizeof(c));
    c.api_id = 12345;
    c.api_hash = strdup("hashhashhash");
    c.dest_channel = strdup("@me");
    c.history_file = strdup("history.txt");
    c.forward_delay_sec = 0;
    c.history_window_hours = 24;
    c.num_sources = n;
    c.source_channels = calloc(n, sizeof(char *));
    for (int i = 0; i < n; i++) {
        char b[32];
        snprintf(b, sizeof(b), "chan%d", i);
        c.source_channels[i] = strdup(b);
    }
    return c;
}

static void push_chat(const char *extra, long long id) {
    char b[256];
    snprintf(b, sizeof(b), "{\"@type\":\"chat\",\"id\":%lld,\"@extra\":\"%s\"}", id, extra);
    fake_push(b);
}

static void push_error(const char *extra, int code, const char *msg) {
    char b[256];
    snprintf(b, sizeof(b), "{\"@type\":\"error\",\"code\":%d,\"message\":\"%s\",\"@extra\":\"%s\"}",
             code, msg, extra);
    fake_push(b);
}

static void script_auth_ready(int sources) {
    push_auth("authorizationStateReady");
    fake_push("{\"@type\":\"user\",\"id\":555,\"@extra\":\"getme\"}");
    push_chat("resolve_dest", -100999);
    for (int i = 0; i < sources; i++) {
        char extra[32];
        snprintf(extra, sizeof(extra), "resolve_%d", i);
        push_chat(extra, -100100 - i);
    }
}

static int wait_event(tgf_ctx *c, tgf_event_kind k, int timeout_s) {
    for (int i = 0; i < timeout_s * 10; i++) {
        tgf_event ev;
        while (tgf_next_event(c, &ev)) {
            int hit = (ev.kind == k);
            tgf_event_free(&ev);
            if (hit) return 1;
        }
        usleep(100000);
    }
    return 0;
}

static void push_auth(const char *state) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"@type\":\"updateAuthorizationState\",\"@extra\":\"\",\"authorizationState\":{\"@type\":\"%s\"}}", state);
    fake_push(buf);
}

static int wait_source_state(tgf_ctx *c, tgf_source_state st, const char *source,
                             long long *id_out, int timeout_s) {
    for (int i = 0; i < timeout_s * 10; i++) {
        tgf_event ev;
        while (tgf_next_event(c, &ev)) {
            int hit = ev.kind == TGF_EV_SOURCE_STATE && ev.src_state == st &&
                      ((!source && !ev.source) ||
                       (source && ev.source && strcmp(ev.source, source) == 0));
            if (hit && id_out) *id_out = ev.id;
            tgf_event_free(&ev);
            if (hit) return 1;
        }
        usleep(100000);
    }
    return 0;
}

static int wait_error(tgf_ctx *c, char *buf, size_t n, int timeout_s) {
    for (int i = 0; i < timeout_s * 10; i++) {
        tgf_event ev;
        while (tgf_next_event(c, &ev)) {
            int hit = ev.kind == TGF_EV_ERROR;
            if (hit && buf && n) snprintf(buf, n, "%s", ev.text ? ev.text : "");
            tgf_event_free(&ev);
            if (hit) return 1;
        }
        usleep(100000);
    }
    return 0;
}

static int sent_contains(const char *needle) {
    for (int i = 0; i < fake_sent_count(); i++)
        if (fake_sent(i) && strstr(fake_sent(i), needle)) return 1;
    return 0;
}

static void test_config_roundtrip(void) {
    write_file("cfg_in.json",
        "{\"api_id\":12345,\"api_hash\":\"h\",\"dest_channel\":\"@me\","
        "\"source_channels\":[\"telegram\"],\"custom_key\":\"keepme\",\"forward_delay_sec\":7}");
    tgf_config c;
    CHECK_LL(tgf_config_load("cfg_in.json", &c), TGF_OK);
    CHECK_LL(c.forward_delay_sec, 7);
    CHECK_LL(c.num_sources, 1);
    CHECK_STR(c.source_channels[0], "telegram");
    CHECK_LL(tgf_config_save("cfg_in.json", &c), TGF_OK);
    char *out = read_text("cfg_in.json");
    CHECK_LL(out != NULL, 1);
    if (out) {
        CHECK_LL(strstr(out, "keepme") != NULL, 1);          /* unknown key preserved */
        CHECK_LL(strstr(out, "\"forward_delay_sec\":7") != NULL, 1);
    }
    free(out);
    struct stat st;
    CHECK_LL(stat("cfg_in.json", &st), 0);
    CHECK_LL(st.st_mode & 0777, 0600);
    tgf_config_free(&c);
    remove("cfg_in.json"); remove("cfg_in.json.tmp");
}

static void test_config_validation(void) {
    tgf_config c;
    remove("cfg_missing.json");
    CHECK_LL(tgf_config_load("cfg_missing.json", &c), TGF_ERR_IO);
    CHECK_STR(tgf_config_error(), "Cannot open cfg_missing.json");

    write_file("cfg_invalid.json", "{not json");
    CHECK_LL(tgf_config_load("cfg_invalid.json", &c), TGF_ERR_PARSE);
    CHECK_STR(tgf_config_error(), "Invalid JSON in cfg_invalid.json");

    write_file("cfg_noid.json",
        "{\"api_hash\":\"h\",\"dest_channel\":\"@me\",\"source_channels\":[]}");
    CHECK_LL(tgf_config_load("cfg_noid.json", &c), TGF_ERR_PARSE);
    CHECK_STR(tgf_config_error(), "Missing api_id");

    write_file("cfg_nosrc.json", "{\"api_id\":1,\"api_hash\":\"h\",\"dest_channel\":\"@me\"}");
    CHECK_LL(tgf_config_load("cfg_nosrc.json", &c), TGF_ERR_PARSE);
    CHECK_STR(tgf_config_error(), "Missing source_channels array");

    write_file("cfg_empty.json",
        "{\"api_id\":1,\"api_hash\":\"h\",\"dest_channel\":\"@me\","
        "\"source_channels\":[],\"disabled_channels\":[]}");
    CHECK_LL(tgf_config_load("cfg_empty.json", &c), TGF_OK);
    CHECK_LL(c.num_sources, 0);
    CHECK_LL(c.num_disabled, 0);
    CHECK_LL(tgf_config_error() == NULL, 1);
    tgf_config_free(&c);

    write_file("cfg_bad.json", "not json");
    tgf_config good = make_cfg();
    CHECK_LL(tgf_config_save("cfg_bad.json", &good), TGF_ERR_PARSE);
    char *txt = read_text("cfg_bad.json");
    CHECK_STR(txt, "not json");
    free(txt);
    tgf_config_free(&good);

    remove("cfg_invalid.json"); remove("cfg_noid.json");
    remove("cfg_nosrc.json"); remove("cfg_empty.json"); remove("cfg_bad.json");
}

static void test_disabled_and_source_ops(void) {
    fake_reset();
    tgf_config c = make_cfg_n(3);
    c.disabled_channels = calloc(1, sizeof(char *));
    c.disabled_channels[0] = strdup("@chan1");
    c.num_disabled = 1;
    tgf_ctx *ctx = tgf_create(&c, &transport_fake);
    tgf_config_free(&c);
    CHECK_LL(ctx != NULL, 1);
    CHECK_LL(tgf_start(ctx), TGF_OK);

    push_auth("authorizationStateReady");
    fake_push("{\"@type\":\"user\",\"id\":555,\"@extra\":\"getme\"}");
    push_chat("resolve_dest", -100999);
    push_chat("resolve_0", -100100);
    push_chat("resolve_2", -100102);

    long long id0 = 0, id2 = 0;
    CHECK_LL(wait_source_state(ctx, TGF_SRC_LIVE, "chan0", &id0, 3), 1);
    CHECK_LL(id0, -100100);
    CHECK_LL(wait_source_state(ctx, TGF_SRC_DISABLED, "chan1", NULL, 3), 1);
    CHECK_LL(wait_source_state(ctx, TGF_SRC_LIVE, "chan2", &id2, 3), 1);
    CHECK_LL(id2, -100102);
    CHECK_LL(wait_event(ctx, TGF_EV_PIPELINE_STATE, 3), 1);
    CHECK_LL(sent_contains("\"username\":\"chan1\""), 0);   /* disabled: never resolved */

    CHECK_LL(tgf_set_source_enabled(ctx, id0, false), TGF_OK);
    CHECK_LL(wait_source_state(ctx, TGF_SRC_DISABLED, "chan0", NULL, 3), 1);

    push_chat("resolve_0", -100200);
    CHECK_LL(tgf_set_source_enabled_by_handle(ctx, "@chan0", true), TGF_OK);
    long long new_id = 0;
    CHECK_LL(wait_source_state(ctx, TGF_SRC_LIVE, "chan0", &new_id, 3), 1);
    CHECK_LL(new_id, -100200);

    CHECK_LL(tgf_remove_source_by_handle(ctx, "t.me/chan1"), TGF_OK);
    push_chat("resolve_2", -100101);
    CHECK_LL(tgf_add_source(ctx, "@chan1"), TGF_OK);
    long long id1 = 0;
    CHECK_LL(wait_source_state(ctx, TGF_SRC_LIVE, "@chan1", &id1, 3), 1);
    CHECK_LL(id1, -100101);

    CHECK_LL(wait_error(ctx, NULL, 0, 1), 0);
    tgf_stop(ctx);
    tgf_destroy(ctx);
    fake_reset();
}

static void test_apply_config_growth(void) {
    fake_reset();
    tgf_config c = make_cfg_n(1);
    tgf_ctx *ctx = tgf_create(&c, &transport_fake);
    tgf_config_free(&c);
    CHECK_LL(ctx != NULL, 1);
    CHECK_LL(tgf_start(ctx), TGF_OK);
    script_auth_ready(1);
    CHECK_LL(wait_event(ctx, TGF_EV_PIPELINE_STATE, 3), 1);

    tgf_config big = make_cfg_n(4);
    CHECK_LL(tgf_apply_config(ctx, &big), TGF_OK);
    tgf_config_free(&big);
    push_chat("resolve_dest", -100999);
    for (int i = 0; i < 4; i++) {
        char extra[32];
        snprintf(extra, sizeof(extra), "resolve_%d", i);
        push_chat(extra, -100200 - i);
    }
    int live = 0;
    int running = 0;
    for (int i = 0; i < 30 && live < 4; i++) {
        tgf_event ev;
        while (tgf_next_event(ctx, &ev)) {
            if (ev.kind == TGF_EV_SOURCE_STATE && ev.src_state == TGF_SRC_LIVE) live++;
            if (ev.kind == TGF_EV_PIPELINE_STATE && ev.pipe == TGF_PIPE_RUNNING) running = 1;
            tgf_event_free(&ev);
        }
        usleep(100000);
    }
    CHECK_LL(live, 4);
    CHECK_LL(running, 1);

    tgf_config small = make_cfg_n(1);
    CHECK_LL(tgf_apply_config(ctx, &small), TGF_OK);
    tgf_config_free(&small);
    push_chat("resolve_dest", -100999);
    push_chat("resolve_0", -100300);
    CHECK_LL(wait_event(ctx, TGF_EV_PIPELINE_STATE, 3), 1);
    CHECK_LL(wait_error(ctx, NULL, 0, 1), 0);

    tgf_stop(ctx);
    tgf_destroy(ctx);
    fake_reset();
}

static void test_fatal_resolution(void) {
    fake_reset();
    tgf_config c = make_cfg_n(1);
    tgf_ctx *ctx = tgf_create(&c, &transport_fake);
    tgf_config_free(&c);
    CHECK_LL(ctx != NULL, 1);
    CHECK_LL(tgf_start(ctx), TGF_OK);

    push_auth("authorizationStateReady");
    fake_push("{\"@type\":\"user\",\"id\":555,\"@extra\":\"getme\"}");
    push_chat("resolve_dest", -100999);
    push_error("resolve_0", 400, "USERNAME_NOT_OCCUPIED");

    char msg[256] = "";
    CHECK_LL(wait_error(ctx, msg, sizeof(msg), 3), 1);
    CHECK_STR(msg, "ERROR: No source channels resolved");
    CHECK_LL(wait_event(ctx, TGF_EV_PIPELINE_STATE, 1), 0);

    tgf_stop(ctx);
    tgf_destroy(ctx);
    fake_reset();
}

static void test_auth_events_and_commands(void) {
    fake_reset();
    tgf_config c = make_cfg();
    tgf_ctx *ctx = tgf_create(&c, &transport_fake);
    CHECK_LL(ctx != NULL, 1);
    tgf_config_free(&c);
    CHECK_LL(tgf_start(ctx), TGF_OK);
    CHECK_LL(tgf_start(ctx), TGF_OK);   /* idempotent */

    push_auth("authorizationStateWaitTdlibParameters");
    push_auth("authorizationStateWaitPhoneNumber");
    CHECK_LL(wait_event(ctx, TGF_EV_AUTH_STATE, 3), 1);
    /* drain to the phone state */
    tgf_event ev;
    int got_phone = 0;
    for (int i = 0; i < 30; i++) {
        while (tgf_next_event(ctx, &ev)) {
            if (ev.kind == TGF_EV_AUTH_STATE && ev.auth == TGF_AUTH_PHONE) got_phone = 1;
            tgf_event_free(&ev);
        }
        if (got_phone) break;
        usleep(100000);
    }
    CHECK_LL(got_phone, 1);
    CHECK_LL(tgf_submit_phone(ctx, "+123456789"), TGF_OK);

    push_auth("authorizationStateWaitCode");
    int got_code = 0;
    for (int i = 0; i < 30; i++) {
        while (tgf_next_event(ctx, &ev)) {
            if (ev.kind == TGF_EV_AUTH_STATE && ev.auth == TGF_AUTH_CODE) got_code = 1;
            tgf_event_free(&ev);
        }
        if (got_code) break;
        usleep(100000);
    }
    CHECK_LL(got_code, 1);
    CHECK_LL(tgf_submit_code(ctx, "12345"), TGF_OK);

    push_auth("authorizationStateReady");
    int got_ready = 0;
    for (int i = 0; i < 30; i++) {
        while (tgf_next_event(ctx, &ev)) {
            if (ev.kind == TGF_EV_AUTH_STATE && ev.auth == TGF_AUTH_READY) got_ready = 1;
            tgf_event_free(&ev);
        }
        if (got_ready) break;
        usleep(100000);
    }
    CHECK_LL(got_ready, 1);

    CHECK_LL(tgf_pause(ctx), TGF_OK);
    CHECK_LL(wait_event(ctx, TGF_EV_PIPELINE_STATE, 3), 1);
    CHECK_LL(tgf_resume(ctx), TGF_OK);
    CHECK_LL(wait_event(ctx, TGF_EV_PIPELINE_STATE, 3), 1);

    tgf_stop(ctx);
    tgf_stop(ctx);   /* idempotent */
    tgf_destroy(ctx);
    fake_reset();
}

int main(void) {
    char tmpl[] = "/tmp/tgf-core-XXXXXX";
    char dir[256];
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
    snprintf(dir, sizeof(dir), "%s", tmpl);
    if (chdir(dir) != 0) { perror("chdir"); return 1; }

    int before = failures; test_config_roundtrip();
    printf("%s config_roundtrip\n", failures == before ? "ok  " : "FAIL");
    before = failures; test_config_validation();
    printf("%s config_validation\n", failures == before ? "ok  " : "FAIL");
    before = failures; test_auth_events_and_commands();
    printf("%s auth_events_and_commands\n", failures == before ? "ok  " : "FAIL");
    before = failures; test_disabled_and_source_ops();
    printf("%s disabled_and_source_ops\n", failures == before ? "ok  " : "FAIL");
    before = failures; test_apply_config_growth();
    printf("%s apply_config_growth\n", failures == before ? "ok  " : "FAIL");
    before = failures; test_fatal_resolution();
    printf("%s fatal_resolution\n", failures == before ? "ok  " : "FAIL");

    chdir("/");
    rmdir(dir);
    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
