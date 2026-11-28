#include "transport_fake.h"
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>

static char **script; static int script_n, script_i, script_cap;
static char **sent; static int sent_n, sent_cap;
static mtx_t script_mtx;
static mtx_t sent_mtx;
static int mtx_ready;

static void mtx_init_once(void) {
    if (mtx_ready) return;
    mtx_init(&script_mtx, mtx_plain);
    mtx_init(&sent_mtx, mtx_plain);
    mtx_ready = 1;
}

void fake_reset(void) {
    mtx_init_once();
    mtx_lock(&script_mtx);
    for (int i = 0; i < script_n; i++) free(script[i]);
    free(script); script = NULL; script_n = script_i = script_cap = 0;
    mtx_unlock(&script_mtx);
    mtx_lock(&sent_mtx);
    for (int i = 0; i < sent_n; i++) free(sent[i]);
    free(sent); sent = NULL; sent_n = sent_cap = 0;
    mtx_unlock(&sent_mtx);
}

void fake_push(const char *json) {
    mtx_init_once();
    mtx_lock(&script_mtx);
    if (script_n == script_cap) { script_cap = script_cap ? script_cap * 2 : 16; script = realloc(script, script_cap * sizeof(char *)); }
    script[script_n++] = strdup(json);
    mtx_unlock(&script_mtx);
}

int fake_sent_count(void) {
    mtx_init_once();
    mtx_lock(&sent_mtx);
    int n = sent_n;
    mtx_unlock(&sent_mtx);
    return n;
}

const char *fake_sent(int i) {
    mtx_init_once();
    mtx_lock(&sent_mtx);
    const char *s = sent[i];
    mtx_unlock(&sent_mtx);
    return s;
}

static void *fc(void) { static int dummy; return &dummy; }
static void fs(void *c, const char *j) {
    (void)c;
    mtx_init_once();
    mtx_lock(&sent_mtx);
    if (sent_n == sent_cap) { sent_cap = sent_cap ? sent_cap * 2 : 16; sent = realloc(sent, sent_cap * sizeof(char *)); }
    sent[sent_n++] = strdup(j);
    mtx_unlock(&sent_mtx);
}
static const char *fr(void *c, double t) {
    (void)c; (void)t;
    mtx_init_once();
    mtx_lock(&script_mtx);
    const char *r;
    if (script_i < script_n) { r = script[script_i++]; }
    else { r = NULL; }
    mtx_unlock(&script_mtx);
    if (!r) usleep(50000);   /* emulate the receive timeout; the real client blocks ~1 s */
    return r;
}
static const char *fe(void *c, const char *j) { (void)c; (void)j; return NULL; }
static void fd(void *c) { (void)c; }

const tgf_transport transport_fake = { fc, fs, fr, fe, fd };
void transport_destroy(void *c) { (void)c; }
