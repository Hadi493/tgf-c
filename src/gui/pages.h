#ifndef TGF_GUI_PAGES_H
#define TGF_GUI_PAGES_H

#include "gui.h"

typedef void (*GuiOpenSourcesFn)(const char *handle, gpointer user_data);

typedef struct {
    GuiBridge *bridge;
    GtkWidget *content;
    GtkButton *pause_btn;
    GtkLabel *status_lbl;
    GtkLabel *sources_lbl;
    GtkLabel *dest_lbl;
    GtkLabel *fwd_lbl;
    GtkLabel *queued_lbl;
    GtkLabel *skipped_lbl;
    GtkLabel *next_lbl;
    GtkListBox *source_list;
    GtkListBox *queue_list;
    GtkLabel *order_lbl;
    GtkLabel *log_lbl;
    GtkToggleButton *filter_btn[4];
    GuiOpenSourcesFn on_open_sources;
    gpointer on_open_data;
    int filter;
    int paused;
} GuiDashboard;

void gui_page_dashboard_init(GuiDashboard *p, GuiBridge *b);
void gui_page_dashboard_event(GuiDashboard *p, const tgf_event *ev);
void gui_page_dashboard_set_paused(GuiDashboard *p, int paused);

typedef struct {
    GuiBridge *bridge;
    GtkWidget *content;
    GtkLabel *dest_lbl;
    GtkEntry *add_entry;
    GtkLabel *add_error;
    GtkListBox *source_list;
    GtkLabel *sel_handle;
    GtkLabel *sel_state;
    GtkLabel *sel_last;
    GtkLabel *sel_fwd;
    GtkLabel *sel_skip;
    GtkLabel *sel_backfill;
    GtkLabel *sel_window;
    GtkLabel *sel_activity;
    GtkLabel *sel_note;
    GtkWidget *rescan_btn;
    GtkWidget *remove_btn;
    char handles[64][256];
    int n_handles;
    char pending_handle[256];
    char selected[256];
} GuiSources;

void gui_page_sources_init(GuiSources *p, GuiBridge *b);
void gui_page_sources_event(GuiSources *p, const tgf_event *ev);
void gui_page_sources_select(GuiSources *p, const char *handle);

typedef struct {
    GuiBridge *bridge;
    GtkWidget *content;
    tgf_config cfg;
    GtkEntry *api_id_entry;
    GtkEntry *api_hash_entry;
    GtkWidget *delay_minus;
    GtkWidget *delay_plus;
    GtkLabel *delay_value;
    GtkSwitch *seq_switch;
    GtkToggleButton *order_btn[2];
    GtkToggleButton *window_btn[4];
    GtkLabel *preview_lbl;
    GtkLabel *error_lbl;
    int delay;
    int window_hours;
} GuiSettings;

void gui_page_settings_init(GuiSettings *p, GuiBridge *b);
void gui_page_settings_event(GuiSettings *p, const tgf_event *ev);

typedef struct {
    GuiBridge *bridge;
    GtkWidget *content;
    GtkWidget *flow_box;
    GtkWidget *session_box;
    GtkStack *stack;
    GtkWidget *pages[3];
    GtkWidget *segment[3];
    GtkLabel *title_lbl;
    GtkLabel *sub_lbl;
    GtkLabel *step_lbl;
    GtkWidget *field[3];
    GtkLabel *error_lbl;
    GtkButton *back_btn;
    GtkButton *next_btn;
    GtkWidget *open_btn;
    GtkWidget *logout_btn;
    int step;
} GuiSignin;

void gui_page_signin_init(GuiSignin *p, GuiBridge *b);
void gui_page_signin_event(GuiSignin *p, const tgf_event *ev);
void gui_page_signin_set_step(GuiSignin *p, int step);

#endif
