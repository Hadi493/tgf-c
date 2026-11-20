#include "transport_fake.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char **script; static int script_n, script_i, script_cap;
static char **sent; static int sent_n, sent_cap;

void fake_reset(void) {
    for (int i = 0; i < script_n; i++) free(script[i]);
    free(script); script = NULL; script_n = script_i = script_cap = 0;
    for (int i = 0; i < sent_n; i++) free(sent[i]);
    free(sent); sent = NULL; sent_n = sent_cap = 0;
}

void fake_push(const char *json) {
    if (script_n == script_cap) { script_cap = script_cap ? script_cap * 2 : 16; script = realloc(script, script_cap * sizeof(char *)); }
    script[script_n++] = strdup(json);
}

int fake_sent_count(void) { return sent_n; }
const char *fake_sent(int i) { return sent[i]; }

static void *fc(void) { static int dummy; return &dummy; }
static void fs(void *c, const char *j) {
    (void)c;
    if (sent_n == sent_cap) { sent_cap = sent_cap ? sent_cap * 2 : 16; sent = realloc(sent, sent_cap * sizeof(char *)); }
    sent[sent_n++] = strdup(j);
}
static const char *fr(void *c, double t) {
    (void)c; (void)t;
    if (script_i < script_n) return script[script_i++];
    usleep(50000);   /* emulate the receive timeout; the real client blocks ~1 s */
    return NULL;
}
static const char *fe(void *c, const char *j) { (void)c; (void)j; return NULL; }
static void fd(void *c) { (void)c; }

const tgf_transport transport_fake = { fc, fs, fr, fe, fd };
void transport_destroy(void *c) { (void)c; }
