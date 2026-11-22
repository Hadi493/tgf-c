#ifndef TGF_CORE_PRIV_H
#define TGF_CORE_PRIV_H

#include <stdatomic.h>
#include <threads.h>

#include "tgf.h"

enum {
    CMD_PHONE,
    CMD_CODE,
    CMD_PW,
    CMD_PAUSE,
    CMD_RESUME,
    CMD_ADD_SOURCE,
    CMD_REMOVE_SOURCE,
    CMD_SET_ENABLED,
    CMD_RESCAN,
    CMD_APPLY_CONFIG,
    CMD_LOGOUT
};

typedef struct {
    int kind;
    char *s;
    int64_t id;
    int enabled;
    int has_cfg;
    tgf_config cfg;
} tgf_cmd;

struct tgf_ctx {
    Runtime rt;
    const tgf_transport *tp;
    atomic_bool running;
    atomic_bool paused;
    thrd_t thread;
    bool started;
    bool loaded;

    mtx_t ev_mtx;
    tgf_event **ev;
    int ev_head, ev_tail, ev_cap;
    int ev_pipe[2];

    mtx_t cmd_mtx;
    tgf_cmd *cmd;
    int cmd_head, cmd_tail, cmd_cap;

    tgf_config cfg;
    int last_progress[MAX_SEQ_TRACKER];
};

tgf_event *ev_new(tgf_event_kind kind);
void ev_emit(tgf_ctx *c, tgf_event *e);
void drain_cmds(tgf_ctx *c, void *client);
void config_copy(tgf_config *dst, const tgf_config *src);
void runtime_config_load(Runtime *rt, const tgf_config *cfg);
int handle_matches(const char *a, const char *b);
int source_disabled(const Config *cfg, const char *handle);
int source_index_by_handle(const Config *cfg, const char *handle);

#endif
