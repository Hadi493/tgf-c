#include "tgf.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef TGF_NOGUI
#include <curses.h>
#endif

static Runtime *ui_rt = NULL;
static bool ui_debug = false;

static void status_stderr(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void forwarded_stderr(const char *source, const char *dest) {
    if (ui_debug) printf(" FORWARDED from %s → %s\n", source, dest);
}

static void redraw_stderr(const char *dest, const char *source, int src_count, int delay) {
    (void)dest; (void)source; (void)src_count; (void)delay;
}

static Ui ui_stderr = { status_stderr, forwarded_stderr, redraw_stderr };

#ifndef TGF_NOGUI
static bool ncurses_ready = false;
static int  action_msg_row = 0;

static void ncurses_init(void) {
    initscr();
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    curs_set(0);
    ncurses_ready = true;
    ui_rt->dashboard_active = true;
}

static void ncurses_cleanup(void) {
    if(ncurses_ready) endwin();
    ui_rt->dashboard_active = false;
}

static void print_dashboard(const char *dest, const char *source, int src_count, int delay) {
    if (!ncurses_ready) return;
    int row = 1;
    clear();
    mvprintw(row++, 0, "                                                      ");
    mvprintw(row++, 0, "                                                      ");
    mvprintw(row++, 0, "           -->TGF - TELEGRAM FEED MONITOR-->          ");
    mvprintw(row++, 0, "       ---------------------------------------------  ");
    mvprintw(row++, 0, "       Target Channel ........ %s ", dest              );
    mvprintw(row++, 0, "       Source Channels ....... %d ", src_count         );
    mvprintw(row++, 0, "       Forward Delay ......... %dsec ", delay          );
    mvprintw(row++, 0, "       Status ................ %s ", "ACTIVE"          );
    mvprintw(row++, 0, "       ---------------------------------------------  ");
    action_msg_row = row;
    mvprintw(row++, 0, "       FORWARDED from %s -> %s", source, dest        );
    mvprintw(row++, 0, "       ---------------------------------------------  ");
    if (ui_rt->status_msg[0]) {
        mvprintw(row++, 0, "       %s", ui_rt->status_msg);
        ui_rt->status_msg[0] = '\0';
    }
    mvprintw(row++, 0, "       [TGF] Running (PID: %d)", getpid());
    mvprintw(row++, 0, "       [TGF] Stop with: Ctrl+C | kill %d | pkill tgf  ", getpid());
    refresh();
}

static void update_action_msg(const char *source, const char *dest) {
    if (!ncurses_ready) return;
    mvprintw(action_msg_row, 0, "       FORWARDED from %s -> %s", source, dest);
    refresh();
}

static void status_ncurses(const char *msg) {
    if (!ui_rt->dashboard_active) fprintf(stderr, "%s\n", msg);
}

static void redraw_ncurses(const char *dest, const char *source, int src_count, int delay) {
    print_dashboard(dest, source, src_count, delay);
    update_action_msg(source, dest);
}

static Ui ui_ncurses = { status_ncurses, NULL, redraw_ncurses };
#endif

static Ui *ui = &ui_stderr;

void ui_init(Runtime *rt, bool dashboard, bool debug) {
    ui_rt = rt;
    ui_debug = debug;
#ifdef TGF_NOGUI
    (void)dashboard;
    ui = &ui_stderr;
#else
    ui = dashboard ? &ui_ncurses : &ui_stderr;
#endif
}

void ui_status(const char *msg)
{
    snprintf(ui_rt->status_msg, sizeof(ui_rt->status_msg), "%s", msg);
    ui->status(msg);
}

void ui_oom(const char *msg)
{
    ui_status(msg);
}

void ui_forwarded(const char *source, const char *dest)
{
    if (ui->forwarded) ui->forwarded(source, dest);
}

void ui_refresh(const char *dest, const char *source, int src_count, int delay)
{
    ui->redraw(dest, source, src_count, delay);
}

#ifndef TGF_NOGUI
void ui_ncurses_init(void) {
    ncurses_init();
    atexit(ncurses_cleanup);
}
#endif
