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
    before = failures; test_auth_events_and_commands();
    printf("%s auth_events_and_commands\n", failures == before ? "ok  " : "FAIL");

    chdir("/");
    rmdir(dir);
    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
