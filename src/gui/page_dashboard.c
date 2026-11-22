#include "pages.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <stdio.h>
#include <string.h>

static void on_pause_clicked(GtkButton *btn, gpointer user_data) {
    GuiDashboard *p = (GuiDashboard *)user_data;
    (void)btn;
    if (p->paused) gui_bridge_resume(p->bridge);
    else gui_bridge_pause(p->bridge);
}

static void open_sources(GuiDashboard *p, const char *handle) {
    if (p->on_open_sources) p->on_open_sources(handle, p->on_open_data);
}

static void on_sources_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    open_sources((GuiDashboard *)user_data, NULL);
}

static void on_source_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data) {
    (void)list;
    if (!row) return;
    const char *handle = (const char *)g_object_get_data(G_OBJECT(row), "handle");
    if (handle) open_sources((GuiDashboard *)user_data, handle);
}

static void refresh_log(GuiDashboard *p) {
    static const char *tags[4] = {"INFO", "FWD ", "SKIP", "WARN"};
    static const char *colors[4] = {"#9aa3ab", "#7fd18b", "#e5b567", "#f08a8a"};
    GString *text = g_string_new("");
    GuiState *s = &p->bridge->state;
    int n = 0;
    for (int i = s->log_len - 1; i >= 0 && i >= s->log_len - TGF_GUI_MAX_LOG && n < 200; i--) {
        GuiLogLine *line = &s->log[i % TGF_GUI_MAX_LOG];
        if (p->filter != TGF_LOG_ALL && line->category != p->filter) continue;
        char *msg = g_markup_escape_text(line->text, -1);
        g_string_append_printf(text,
            "<span foreground=\"#9aa3ab\">%s</span>  "
            "<span foreground=\"%s\" weight=\"700\">%s</span>  "
            "<span foreground=\"#d6dadd\">%s</span>\n",
            line->time, colors[line->category], tags[line->category], msg ? msg : "");
        g_free(msg);
        n++;
    }
    if (n == 0) g_string_assign(text, "Waiting for messages");
    gtk_label_set_markup(p->log_lbl, text->str);
    g_string_free(text, TRUE);
}

static void on_filter_toggled(GtkToggleButton *btn, gpointer user_data) {
    GuiDashboard *p = (GuiDashboard *)user_data;
    if (!gtk_toggle_button_get_active(btn)) return;
    p->filter = (int)(intptr_t)g_object_get_data(G_OBJECT(btn), "filter");
    for (int i = 0; i < 4; i++)
        gtk_toggle_button_set_active(p->filter_btn[i], i == p->filter);
    refresh_log(p);
}

static void refresh_stats(GuiDashboard *p) {
    GuiState *s = &p->bridge->state;
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", s->metrics.forwarded);
    gtk_label_set_text(p->fwd_lbl, buf);
    snprintf(buf, sizeof(buf), "%d", s->metrics.queued);
    gtk_label_set_text(p->queued_lbl, buf);
    snprintf(buf, sizeof(buf), "%d", s->metrics.skipped);
    gtk_label_set_text(p->skipped_lbl, buf);
    if (s->metrics.paused) snprintf(buf, sizeof(buf), "paused");
    else snprintf(buf, sizeof(buf), "%ds", s->metrics.next_in_ms / 1000);
    gtk_label_set_text(p->next_lbl, buf);
    const char *status = s->metrics.paused ? "Paused" : (s->metrics.running ? "Forwarding" : "Idle");
    gtk_label_set_text(p->status_lbl, status);
    if (s->metrics.paused) gtk_widget_add_css_class(GTK_WIDGET(p->status_lbl), "paused");
    else gtk_widget_remove_css_class(GTK_WIDGET(p->status_lbl), "paused");
    if (!s->metrics.paused && !s->metrics.running) gtk_widget_add_css_class(GTK_WIDGET(p->status_lbl), "idle");
    else gtk_widget_remove_css_class(GTK_WIDGET(p->status_lbl), "idle");
    gtk_button_set_label(p->pause_btn, s->metrics.paused ? "Resume" : "Pause");
    snprintf(buf, sizeof(buf), "%d sources →", s->source_count);
    gtk_label_set_text(p->sources_lbl, buf);
    gtk_label_set_text(p->dest_lbl, s->dest[0] ? s->dest : "@me");
    gtk_label_set_text(p->order_lbl, s->sequence_direction == 1 ? "new → old" : "old → new");
}

static void refresh_sources(GuiDashboard *p) {
    GuiState *s = &p->bridge->state;
    gtk_list_box_remove_all(p->source_list);
    for (int i = 0; i < s->source_count; i++) {
        GuiSourceRow *r = &s->sources[i];
        GtkWidget *row = gtk_list_box_row_new();
        gtk_widget_set_size_request(row, -1, 38);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
        g_object_set_data_full(G_OBJECT(row), "handle", g_strdup(r->handle), g_free);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *handle = gtk_label_new(r->handle);
        gtk_label_set_xalign(GTK_LABEL(handle), 0);
        gtk_widget_set_size_request(handle, 360, -1);
        gtk_widget_add_css_class(handle, "mono");

        GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_size_request(status, 118, -1);
        GtkWidget *dot = gtk_label_new("•");
        gtk_widget_add_css_class(dot, r->state == TGF_SRC_LIVE ? "dot-live" : "dot-idle");
        GtkWidget *state_lbl = gtk_label_new(r->state == TGF_SRC_LIVE ? "Live"
            : r->state == TGF_SRC_DISABLED ? "Disabled"
            : r->state == TGF_SRC_NO_ACCESS ? "No access" : "Scanning");
        gtk_label_set_xalign(GTK_LABEL(state_lbl), 0);
        gtk_box_append(GTK_BOX(status), dot);
        gtk_box_append(GTK_BOX(status), state_lbl);

        GtkWidget *backfill = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_hexpand(backfill, TRUE);
        GtkWidget *bar = gtk_progress_bar_new();
        gtk_widget_add_css_class(bar, "backfill-bar");
        gtk_widget_set_size_request(bar, -1, 6);
        gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(bar, TRUE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar),
            r->state == TGF_SRC_DISABLED ? 0.0 : (r->progress > 100 ? 100.0 : r->progress) / 100.0);
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", (int)r->progress);
        GtkWidget *pct_lbl = gtk_label_new(r->state == TGF_SRC_DISABLED ? "—" : pct);
        gtk_widget_add_css_class(pct_lbl, "backfill-pct");
        gtk_widget_set_size_request(pct_lbl, 34, -1);
        gtk_label_set_xalign(GTK_LABEL(pct_lbl), 1);
        gtk_box_append(GTK_BOX(backfill), bar);
        gtk_box_append(GTK_BOX(backfill), pct_lbl);

        char last[32];
        snprintf(last, sizeof(last), "%lld", r->last_id);
        GtkWidget *last_lbl = gtk_label_new(last);
        gtk_widget_set_size_request(last_lbl, 120, -1);
        gtk_label_set_xalign(GTK_LABEL(last_lbl), 0);
        gtk_widget_add_css_class(last_lbl, "muted");

        gtk_box_append(GTK_BOX(box), handle);
        gtk_box_append(GTK_BOX(box), status);
        gtk_box_append(GTK_BOX(box), backfill);
        gtk_box_append(GTK_BOX(box), last_lbl);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(p->source_list, row);
    }
}

static void refresh_queue(GuiDashboard *p) {
    GuiState *s = &p->bridge->state;
    gtk_list_box_remove_all(p->queue_list);
    if (s->queue_len == 0) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new("Nothing queued");
        gtk_widget_add_css_class(lbl, "muted");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        gtk_list_box_append(p->queue_list, row);
        return;
    }
    for (int i = 0; i < s->queue_len; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        char idx[16], id[32], ref[320];
        snprintf(idx, sizeof(idx), "#%d", i + 1);
        snprintf(id, sizeof(id), "#%lld", s->queue_ids[i]);
        snprintf(ref, sizeof(ref), "%s", s->queue_src[i][0] ? s->queue_src[i] : "source");
        GtkWidget *idx_lbl = gtk_label_new(idx);
        GtkWidget *ref_lbl = gtk_label_new(ref);
        GtkWidget *id_lbl = gtk_label_new(id);
        GtkWidget *kind = gtk_label_new("msg");
        gtk_widget_add_css_class(idx_lbl, "mono");
        gtk_widget_add_css_class(ref_lbl, "mono");
        gtk_widget_add_css_class(id_lbl, "mono");
        gtk_widget_add_css_class(kind, "kind-chip");
        gtk_widget_set_size_request(kind, -1, 22);
        gtk_label_set_xalign(GTK_LABEL(ref_lbl), 0);
        gtk_widget_set_hexpand(ref_lbl, TRUE);
        gtk_box_append(GTK_BOX(box), idx_lbl);
        gtk_box_append(GTK_BOX(box), ref_lbl);
        gtk_box_append(GTK_BOX(box), id_lbl);
        gtk_box_append(GTK_BOX(box), kind);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(p->queue_list, row);
    }
}

static GtkWidget *card(const char *title) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "card");
    GtkWidget *lbl = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_widget_add_css_class(lbl, "card-title");
    gtk_box_append(GTK_BOX(box), lbl);
    return box;
}

static GtkWidget *metric_card(const char *title, GtkLabel **value) {
    GtkWidget *box = card(title);
    *value = GTK_LABEL(gtk_label_new("0"));
    gtk_label_set_xalign(*value, 0);
    gtk_widget_add_css_class(GTK_WIDGET(*value), "metric-value");
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(*value));
    return box;
}

static GtkWidget *head_label(const char *text, int width) {
    GtkWidget *lbl = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_width_chars(GTK_LABEL(lbl), width);
    gtk_widget_add_css_class(lbl, "muted");
    return lbl;
}

void gui_page_dashboard_init(GuiDashboard *p, GuiBridge *b) {
    memset(p, 0, sizeof(*p));
    p->bridge = b;
    p->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(header, 24);
    gtk_widget_set_margin_end(header, 24);
    gtk_widget_set_margin_top(header, 16);
    gtk_widget_set_margin_bottom(header, 16);
    GtkWidget *title = gtk_label_new("Dashboard");
    gtk_widget_add_css_class(title, "window-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    GtkWidget *sources_btn = gtk_button_new_with_label("0 sources →");
    gtk_widget_add_css_class(sources_btn, "flat");
    p->sources_lbl = GTK_LABEL(gtk_button_get_child(GTK_BUTTON(sources_btn)));
    g_signal_connect(G_OBJECT(sources_btn), "clicked", G_CALLBACK(on_sources_clicked), p);
    p->dest_lbl = GTK_LABEL(gtk_label_new("@me"));
    gtk_widget_add_css_class(GTK_WIDGET(p->dest_lbl), "mono");
    p->status_lbl = GTK_LABEL(gtk_label_new("Idle"));
    gtk_widget_add_css_class(GTK_WIDGET(p->status_lbl), "status-pill");
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    p->pause_btn = GTK_BUTTON(gtk_button_new_with_label("Pause"));
    gtk_widget_add_css_class(GTK_WIDGET(p->pause_btn), "suggested-action");
    gtk_widget_add_css_class(GTK_WIDGET(p->pause_btn), "btn-action");
    gtk_widget_set_size_request(GTK_WIDGET(p->pause_btn), -1, 40);
    g_signal_connect(G_OBJECT(p->pause_btn), "clicked", G_CALLBACK(on_pause_clicked), p);
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), sources_btn);
    gtk_box_append(GTK_BOX(header), GTK_WIDGET(p->dest_lbl));
    gtk_box_append(GTK_BOX(header), spacer);
    gtk_box_append(GTK_BOX(header), GTK_WIDGET(p->status_lbl));
    gtk_box_append(GTK_BOX(header), GTK_WIDGET(p->pause_btn));
    gtk_box_append(GTK_BOX(p->content), header);

    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_margin_start(main, 24);
    gtk_widget_set_margin_end(main, 24);
    gtk_widget_set_margin_bottom(main, 24);
    gtk_widget_set_vexpand(main, TRUE);
    gtk_box_append(GTK_BOX(p->content), main);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_attach(GTK_GRID(grid), metric_card("Forwarded today", &p->fwd_lbl), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), metric_card("In queue (sequential)", &p->queued_lbl), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), metric_card("Skipped as duplicates", &p->skipped_lbl), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), metric_card("Next forward in", &p->next_lbl), 3, 0, 1, 1);
    gtk_box_append(GTK_BOX(main), grid);

    GtkWidget *src_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(src_box, "card");
    GtkWidget *card_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_size_request(card_head, -1, 46);
    GtkWidget *card_title = gtk_label_new("Sources");
    gtk_widget_add_css_class(card_title, "card-title");
    gtk_label_set_xalign(GTK_LABEL(card_title), 0);
    gtk_widget_set_hexpand(card_title, TRUE);
    GtkWidget *manage = gtk_button_new_with_label("Manage");
    gtk_button_set_has_frame(GTK_BUTTON(manage), FALSE);
    gtk_widget_add_css_class(manage, "link-button");
    g_signal_connect(G_OBJECT(manage), "clicked", G_CALLBACK(on_sources_clicked), p);
    gtk_box_append(GTK_BOX(card_head), card_title);
    gtk_box_append(GTK_BOX(card_head), manage);
    gtk_box_append(GTK_BOX(src_box), card_head);
    GtkWidget *src_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_size_request(src_head, -1, 28);
    gtk_widget_add_css_class(src_head, "table-head");
    GtkWidget *col_channel = head_label("Channel", 0);
    gtk_widget_set_size_request(col_channel, 360, -1);
    GtkWidget *col_status = head_label("Status", 0);
    gtk_widget_set_size_request(col_status, 118, -1);
    GtkWidget *col_backfill = head_label("Backfill", 0);
    gtk_widget_set_hexpand(col_backfill, TRUE);
    GtkWidget *col_last = head_label("Last message", 0);
    gtk_widget_set_size_request(col_last, 120, -1);
    gtk_box_append(GTK_BOX(src_head), col_channel);
    gtk_box_append(GTK_BOX(src_head), col_status);
    gtk_box_append(GTK_BOX(src_head), col_backfill);
    gtk_box_append(GTK_BOX(src_head), col_last);
    gtk_box_append(GTK_BOX(src_box), src_head);
    GtkWidget *src_sc = gtk_scrolled_window_new();
    p->source_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(p->source_list, GTK_SELECTION_NONE);
    g_signal_connect(G_OBJECT(p->source_list), "row-activated", G_CALLBACK(on_source_activated), p);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(src_sc), GTK_WIDGET(p->source_list));
    gtk_widget_set_vexpand(GTK_WIDGET(src_sc), TRUE);
    gtk_box_append(GTK_BOX(src_box), src_sc);
    gtk_widget_set_vexpand(src_box, TRUE);
    gtk_box_append(GTK_BOX(main), src_box);

    GtkWidget *queue_box = card("Up next");
    p->order_lbl = GTK_LABEL(gtk_label_new(b->state.sequence_direction == 1 ? "new → old" : "old → new"));
    gtk_widget_add_css_class(GTK_WIDGET(p->order_lbl), "muted");
    GtkWidget *queue_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(GTK_WIDGET(p->order_lbl), TRUE);
    gtk_box_append(GTK_BOX(queue_head), GTK_WIDGET(p->order_lbl));
    gtk_box_append(GTK_BOX(queue_box), queue_head);
    p->queue_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(p->queue_list, GTK_SELECTION_NONE);
    gtk_widget_set_size_request(GTK_WIDGET(p->queue_list), -1, 150);
    gtk_box_append(GTK_BOX(queue_box), GTK_WIDGET(p->queue_list));
    gtk_box_append(GTK_BOX(main), queue_box);

    GtkWidget *log_box = card("Live log");
    gtk_widget_add_css_class(log_box, "log-card");
    GtkWidget *log_sc = gtk_scrolled_window_new();
    p->log_lbl = GTK_LABEL(gtk_label_new("Waiting for messages"));
    gtk_label_set_xalign(GTK_LABEL(p->log_lbl), 0);
    gtk_widget_add_css_class(GTK_WIDGET(p->log_lbl), "mono");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(log_sc), GTK_WIDGET(p->log_lbl));
    gtk_widget_set_vexpand(GTK_WIDGET(log_sc), TRUE);

    GtkWidget *log_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const char *names[4] = {"All", "Forwarded", "Skipped", "Warnings"};
    for (int i = 0; i < 4; i++) {
        GtkWidget *btn = gtk_toggle_button_new_with_label(names[i]);
        gtk_widget_add_css_class(btn, "filter-chip");
        gtk_widget_set_size_request(btn, -1, 30);
        g_object_set_data(G_OBJECT(btn), "filter", GINT_TO_POINTER(i));
        g_signal_connect(G_OBJECT(btn), "toggled", G_CALLBACK(on_filter_toggled), p);
        if (i > 0) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), p->filter_btn[0]);
        p->filter_btn[i] = GTK_TOGGLE_BUTTON(btn);
        gtk_box_append(GTK_BOX(log_head), btn);
    }
    gtk_toggle_button_set_active(p->filter_btn[0], TRUE);
    gtk_box_append(GTK_BOX(log_box), log_head);
    gtk_box_append(GTK_BOX(log_box), log_sc);
    gtk_widget_set_vexpand(log_box, TRUE);
    gtk_box_append(GTK_BOX(main), log_box);

    refresh_stats(p);
    refresh_sources(p);
    refresh_queue(p);
}

void gui_page_dashboard_set_paused(GuiDashboard *p, int paused) {
    p->paused = paused;
    gtk_button_set_label(p->pause_btn, paused ? "Resume" : "Pause");
}

void gui_page_dashboard_event(GuiDashboard *p, const tgf_event *ev) {
    switch (ev->kind) {
    case TGF_EV_PIPELINE_STATE:
        p->paused = ev->pipe == TGF_PIPE_PAUSED;
        refresh_stats(p);
        break;
    case TGF_EV_SOURCE_STATE:
        refresh_sources(p);
        refresh_stats(p);
        break;
    case TGF_EV_QUEUE:
        refresh_queue(p);
        refresh_stats(p);
        break;
    case TGF_EV_LOG:
    case TGF_EV_FORWARDED:
    case TGF_EV_SKIPPED:
    case TGF_EV_ERROR:
        refresh_log(p);
        refresh_stats(p);
        break;
    default:
        refresh_stats(p);
        break;
    }
}
