#ifndef TGF_LOOP_H
#define TGF_LOOP_H

#include "tgf.h"

/* Consumer hook invoked once per tick at the point the dashboard used to be
 * refreshed. The CLI passes a ncurses/stderr callback; the core passes NULL
 * (it emits events instead). */
typedef void (*tgf_tick_cb)(Runtime *rt, void *ud);

typedef struct {
    time_t last_poll;
    time_t last_prune;
    int    paused;
} LoopState;

void loop_tick(Runtime *rt, void *client, LoopState *ls, tgf_tick_cb cb, void *ud);

#endif
