#ifndef TGF_GUI_H
#define TGF_GUI_H

#include <glib.h>
#include <gtk/gtk.h>
#include <adwaita.h>
#include "tgf.h"
#include "tgf_core.h"

#define TGF_GUI_MAX_LOG 256
#define TGF_GUI_MAX_QUEUE 5

#define TGF_LOG_ALL 0
#define TGF_LOG_FORWARDED 1
#define TGF_LOG_SKIPPED 2
#define TGF_LOG_WARNINGS 3

typedef struct GuiBridge GuiBridge;

typedef struct {
    int forwarded;
    int skipped;
    int queued;
    int errors;
    int running;
    int paused;
    int next_in_ms;
    int conn_state;
    int auth_state;
    char auth_error[256];
    char fatal[256];
} GuiMetrics;

typedef struct {
    char text[512];
    char time[16];
    int category;
} GuiLogLine;

typedef struct {
    char handle[256];
    long long chat_id;
    long long last_id;
    double progress;
    char last_activity[16];
    int forwarded;
    int skipped;
    int state;
} GuiSourceRow;

typedef struct {
    GuiMetrics metrics;
    GuiLogLine log[TGF_GUI_MAX_LOG];
    int log_len;
    GuiSourceRow sources[64];
    int source_count;
    char dest[256];
    int sequence_direction;
    int history_window_hours;
    int queue_len;
    long long queue_ids[TGF_GUI_MAX_QUEUE];
    char queue_src[TGF_GUI_MAX_QUEUE][256];
} GuiState;

struct GuiBridge {
    tgf_ctx *ctx;
    GuiState state;
    GMutex lock;
    void (*page_event)(GuiBridge *b, const tgf_event *ev);
    void *owner;
    GtkWindow *window;
};

typedef struct GuiWindow GuiWindow;

GuiWindow *gui_win_new(GtkApplication *app, GuiBridge *b);
GuiWindow *gui_win_new_error(GtkApplication *app, GuiBridge *b, const char *message);
void gui_win_free(GuiWindow *w);
void gui_win_set_page(GuiWindow *w, int page, const char *source_handle);
GtkWidget *gui_win_get_content(GuiWindow *w);
GtkWidget *gui_win_get_window(GuiWindow *w);
void gui_win_page_event(GuiBridge *b, const tgf_event *ev);

void gui_state_init(GuiState *s, const char *dest);
void gui_bridge_init(GuiBridge *b, tgf_ctx *ctx);
void gui_bridge_destroy(GuiBridge *b);
void gui_bridge_event(GuiBridge *b, tgf_event *ev);
void gui_bridge_shutdown(GuiBridge *b);
void gui_bridge_source_toggle(GuiBridge *b, const char *handle, gboolean enable);
void gui_bridge_source_remove(GuiBridge *b, const char *handle);
void gui_bridge_source_add(GuiBridge *b, const char *handle);
void gui_bridge_source_rescan(GuiBridge *b, const char *handle);
void gui_bridge_pause(GuiBridge *b);
void gui_bridge_resume(GuiBridge *b);
void gui_bridge_clear_auth_error(GuiBridge *b);
void gui_bridge_set_page_event(GuiBridge *b, void (*fn)(GuiBridge *b, const tgf_event *ev));

#endif
