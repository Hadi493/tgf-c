#include "tgf.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef TGF_NOGUI
#include <curses.h>
#endif

/* Event-driven dashboard (spec 09 GUI-010: the TUI reads only events, never
 * core internals). main() feeds it with ui_dashboard_event/draw. */

#ifndef TGF_NOGUI
static bool ncurses_ready = false;
static int action_msg_row = 0;

static void ncurses_init(void) {
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    curs_set(0);
    ncurses_ready = true;
}

static void ncurses_cleanup(void) {
    if (ncurses_ready) endwin();
}

static void print_dashboard(const char *dest, const char *source, int src_count, int delay,
                            const char *status) {
    if (!ncurses_ready) return;
    int row = 1;
    clear();
    mvprintw(row++, 0, "                                                      ");
    mvprintw(row++, 0, "                                                      ");
    mvprintw(row++, 0, "           -->TGF - TELEGRAM FEED MONITOR-->          ");
    mvprintw(row++, 0, "       ---------------------------------------------  ");
    mvprintw(row++, 0, "       Target Channel ........ %s ", dest);
    mvprintw(row++, 0, "       Source Channels ....... %d ", src_count);
    mvprintw(row++, 0, "       Forward Delay ......... %dsec ", delay);
    mvprintw(row++, 0, "       Status ................ %s ",
             (status && status[0]) ? status : "ACTIVE");
    mvprintw(row++, 0, "       ---------------------------------------------  ");
    action_msg_row = row;
    mvprintw(row++, 0, "       FORWARDED from %s -> %s", source, dest);
    mvprintw(row++, 0, "       ---------------------------------------------  ");
    mvprintw(row++, 0, "       [TGF] Running (PID: %d)", getpid());
    mvprintw(row++, 0, "       [TGF] Stop with: Ctrl+C | kill %d | pkill tgf  ", getpid());
    refresh();
}

static void update_action_msg(const char *source, const char *dest) {
    if (!ncurses_ready) return;
    mvprintw(action_msg_row, 0, "       FORWARDED from %s -> %s", source, dest);
    refresh();
}
#endif

#ifndef TGF_NOGUI
static char d_dest[128] = "", d_src[128] = "unknown", d_status[256] = "";
static int d_sources = 0, d_delay = 0;
static bool dash_active = false;
static time_t last_draw = 0;
#endif

void ui_dashboard_begin(const char *dest, int delay) {
#ifndef TGF_NOGUI
    snprintf(d_dest, sizeof(d_dest), "%s", dest ? dest : "");
    d_delay = delay;
    if (!dash_active) {
        ncurses_init();
        dash_active = true;
    }
#else
    (void)dest;
    (void)delay;
#endif
}

void ui_dashboard_event(const tgf_event *ev) {
#ifndef TGF_NOGUI
    if (!ev) return;
    if (ev->kind == TGF_EV_LOG && ev->text) {
        snprintf(d_status, sizeof(d_status), "%s", ev->text);
    } else if (ev->kind == TGF_EV_FORWARDED && ev->source) {
        snprintf(d_src, sizeof(d_src), "%s", ev->source);
    } else if (ev->kind == TGF_EV_SOURCE_STATE && ev->total > 0) {
        d_sources = (int)ev->total;
    }
#else
    (void)ev;
#endif
}

void ui_dashboard_draw(void) {
#ifndef TGF_NOGUI
    if (!dash_active) return;
    time_t now = time(NULL);
    if (now - last_draw < 1) return;
    last_draw = now;
    print_dashboard(d_dest, d_src, d_sources, d_delay, d_status);
    update_action_msg(d_src, d_dest);
#endif
}

void ui_dashboard_end(void) {
#ifndef TGF_NOGUI
    if (dash_active) {
        ncurses_cleanup();
        dash_active = false;
    }
#endif
}
