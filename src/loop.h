#ifndef TGF_LOOP_H
#define TGF_LOOP_H

#include "tgf.h"

/* Per-loop clock state (was static locals in main). One tick = one receive
 * (<=1 s) plus the periodic tasks gated by time(NULL) deltas. */
typedef struct {
    time_t last_poll;
    time_t last_dashboard;
    time_t last_prune;
    int    ncurses_inited;
} LoopState;

void loop_tick(Runtime *rt, void *client, LoopState *ls, char *argv[]);

#endif
