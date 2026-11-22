#include "pages.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <stdio.h>
#include <string.h>

static const char *state_text(int state) {
    if (state == TGF_SRC_LIVE) return "Live";
    if (state == TGF_SRC_DISABLED) return "Disabled";
    if (state == TGF_SRC_NO_ACCESS) return "No access";
    return "Scanning";
}

static void refresh_detail(GuiSources *p) {
    GuiState *s = &p->bridge->state;
    GuiSourceRow *r = NULL;
    for (int i = 0; i < s->source_count; i++)
        if (strcmp(s->sources[i].handle, p->selected) == 0) { r = &s->sources[i]; break; }
    char buf[64], idbuf[32];
    gtk_label_set_text(p->sel_handle, r ? r->handle : "No source selected");
    gtk_label_set_text(p->sel_state, r ? state_text(r->state) : "—");
    if (r && r->last_id > 0) snprintf(idbuf, sizeof(idbuf), "%lld", r->last_id);
    else snprintf(idbuf, sizeof(idbuf), "—");
    gtk_label_set_text(p->sel_last, r ? idbuf : "—");
    snprintf(buf, sizeof(buf), "%d", r ? r->forwarded : 0);
    gtk_label_set_text(p->sel_fwd, buf);
    snprintf(buf, sizeof(buf), "%d", r ? r->skipped : 0);
    gtk_label_set_text(p->sel_skip, buf);
    if (r && r->state != TGF_SRC_DISABLED) snprintf(buf, sizeof(buf), "%d%%", (int)r->progress);
    else snprintf(buf, sizeof(buf), "—");
    gtk_label_set_text(p->sel_backfill, buf);
    if (r) {
        if (s->history_window_hours == 0) snprintf(buf, sizeof(buf), "All");
        else snprintf(buf, sizeof(buf), "%d h", s->history_window_hours);
    } else snprintf(buf, sizeof(buf), "—");
    gtk_label_set_text(p->sel_window, buf);
    gtk_label_set_text(p->sel_activity, r && r->last_activity[0] ? r->last_activity : "—");
    const char *note = r && r->state == TGF_SRC_NO_ACCESS
        ? "This account cannot read the channel. Join it in Telegram, then rescan from the window start."
        : "Rescan clears this source's backfill cursor. Already-forwarded messages stay in history and are not sent again.";
    gtk_label_set_text(p->sel_note, note);
    gtk_widget_set_sensitive(p->rescan_btn, r != NULL);
    gtk_widget_set_sensitive(p->remove_btn, r != NULL);
}

static void on_selected(GtkListBox *list, GtkListBoxRow *row, gpointer user_data) {
    (void)list;
    GuiSources *p = (GuiSources *)user_data;
    const char *handle = row ? (const char *)g_object_get_data(G_OBJECT(row), "handle") : NULL;
    snprintf(p->selected, sizeof(p->selected), "%s", handle ? handle : "");
    refresh_detail(p);
}

static void on_add_clicked(GtkButton *btn, gpointer user_data) {
    GuiSources *p = (GuiSources *)user_data;
    (void)btn;
    const char *text = gtk_editable_get_text(GTK_EDITABLE(p->add_entry));
    if (!text || !text[0]) return;
    for (int i = 0; i < p->bridge->state.source_count; i++)
        if (strcmp(p->bridge->state.sources[i].handle, text) == 0) {
            gtk_label_set_text(p->add_error, "Source already added");
            return;
        }
    gtk_label_set_text(p->add_error, "");
    gui_bridge_source_add(p->bridge, text);
    gtk_editable_set_text(GTK_EDITABLE(p->add_entry), "");
}

static gboolean on_switch_state_set(GtkSwitch *sw, gboolean state, gpointer user_data) {
    GuiSources *p = (GuiSources *)user_data;
    int idx = (int)(intptr_t)g_object_get_data(G_OBJECT(sw), "row-index");
    gui_bridge_source_toggle(p->bridge, p->handles[idx], state);
    return FALSE;
}

static void on_remove_response(AdwAlertDialog *dialog, const char *response, gpointer user_data) {
    GuiSources *p = (GuiSources *)user_data;
    if (strcmp(response, "remove") == 0 && p->pending_handle[0])
        gui_bridge_source_remove(p->bridge, p->pending_handle);
    p->pending_handle[0] = 0;
    adw_dialog_close(ADW_DIALOG(dialog));
}

static void on_remove_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GuiSources *p = (GuiSources *)user_data;
    snprintf(p->pending_handle, sizeof(p->pending_handle), "%s", p->selected);
    AdwDialog *dialog = adw_alert_dialog_new("Remove source",
        "Remove this source from sources? History entries are kept.");
    AdwAlertDialog *adialog = ADW_ALERT_DIALOG(dialog);
    adw_alert_dialog_add_response(adialog, "cancel", "Cancel");
    adw_alert_dialog_add_response(adialog, "remove", "Remove");
    adw_alert_dialog_set_default_response(adialog, "cancel");
    adw_alert_dialog_set_close_response(adialog, "cancel");
    g_signal_connect(adialog, "response", G_CALLBACK(on_remove_response), p);
    adw_dialog_present(dialog, GTK_WIDGET(p->bridge->window));
}

static void on_rescan_response(AdwAlertDialog *dialog, const char *response, gpointer user_data) {
    GuiSources *p = (GuiSources *)user_data;
    if (strcmp(response, "rescan") == 0 && p->pending_handle[0])
        gui_bridge_source_rescan(p->bridge, p->pending_handle);
    p->pending_handle[0] = 0;
    adw_dialog_close(ADW_DIALOG(dialog));
}

static void on_rescan_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GuiSources *p = (GuiSources *)user_data;
    snprintf(p->pending_handle, sizeof(p->pending_handle), "%s", p->selected);
    AdwDialog *dialog = adw_alert_dialog_new("Rescan from window start",
        "This clears the backfill cursor and can re-queue many messages. Already-forwarded messages stay in history.");
    AdwAlertDialog *adialog = ADW_ALERT_DIALOG(dialog);
    adw_alert_dialog_add_response(adialog, "cancel", "Cancel");
    adw_alert_dialog_add_response(adialog, "rescan", "Rescan");
    adw_alert_dialog_set_default_response(adialog, "cancel");
    adw_alert_dialog_set_close_response(adialog, "cancel");
    g_signal_connect(adialog, "response", G_CALLBACK(on_rescan_response), p);
    adw_dialog_present(dialog, GTK_WIDGET(p->bridge->window));
}

static void refresh_sources(GuiSources *p) {
    GuiState *s = &p->bridge->state;
    gtk_label_set_text(p->dest_lbl, s->dest[0] ? s->dest : "@me");
    gtk_list_box_remove_all(p->source_list);
    p->n_handles = 0;
    for (int i = 0; i < s->source_count; i++) {
        GuiSourceRow *r = &s->sources[i];
        int idx = p->n_handles;
        if (idx >= 64) break;
        snprintf(p->handles[idx], 256, "%s", r->handle);
        p->n_handles++;
        GtkWidget *row = gtk_list_box_row_new();
        gtk_widget_set_size_request(row, -1, 56);
        g_object_set_data_full(G_OBJECT(row), "handle", g_strdup(r->handle), g_free);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(box, 16);
        gtk_widget_set_margin_end(box, 16);
        GtkWidget *handle = gtk_label_new(r->handle);
        gtk_widget_set_hexpand(handle, TRUE);
        gtk_label_set_xalign(GTK_LABEL(handle), 0);
        gtk_widget_add_css_class(handle, "mono");
        GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_size_request(status, 110, -1);
        GtkWidget *dot = gtk_label_new("•");
        gtk_widget_add_css_class(dot, r->state == TGF_SRC_LIVE ? "dot-live" : "dot-idle");
        GtkWidget *state_lbl = gtk_label_new(state_text(r->state));
        gtk_label_set_xalign(GTK_LABEL(state_lbl), 0);
        gtk_box_append(GTK_BOX(status), dot);
        gtk_box_append(GTK_BOX(status), state_lbl);
        char counter[16];
        snprintf(counter, sizeof(counter), "%d", r->forwarded);
        GtkWidget *fwd_lbl = gtk_label_new(counter);
        gtk_widget_set_size_request(fwd_lbl, 100, -1);
        gtk_label_set_xalign(GTK_LABEL(fwd_lbl), 0);
        GtkWidget *toggle_cell = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(toggle_cell, 60, -1);
        GtkWidget *toggle = gtk_switch_new();
        gtk_switch_set_active(GTK_SWITCH(toggle), r->state != TGF_SRC_DISABLED);
        gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(toggle, GTK_ALIGN_START);
        gtk_accessible_update_property(GTK_ACCESSIBLE(toggle), GTK_ACCESSIBLE_PROPERTY_LABEL, r->handle, -1);
        g_signal_connect(G_OBJECT(toggle), "state-set", G_CALLBACK(on_switch_state_set), p);
        g_object_set_data(G_OBJECT(toggle), "row-index", GINT_TO_POINTER(idx));
        gtk_box_append(GTK_BOX(toggle_cell), toggle);
        gtk_box_append(GTK_BOX(box), handle);
        gtk_box_append(GTK_BOX(box), status);
        gtk_box_append(GTK_BOX(box), fwd_lbl);
        gtk_box_append(GTK_BOX(box), toggle_cell);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(p->source_list, row);
    }
    gui_page_sources_select(p, p->selected);
    refresh_detail(p);
}

void gui_page_sources_select(GuiSources *p, const char *handle) {
    for (int i = 0; ; i++) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(p->source_list, i);
        if (!row) break;
        const char *h = (const char *)g_object_get_data(G_OBJECT(row), "handle");
        if (h && strcmp(h, handle) == 0) {
            gtk_list_box_select_row(p->source_list, row);
            return;
        }
    }
}

void gui_page_sources_event(GuiSources *p, const tgf_event *ev) {
    if (ev->kind == TGF_EV_SOURCE_STATE || ev->kind == TGF_EV_FORWARDED || ev->kind == TGF_EV_SKIPPED)
        refresh_sources(p);
}

static GtkWidget *field_row(const char *label, const char *value, GtkLabel **out) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_add_css_class(lbl, "muted");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_width_chars(GTK_LABEL(lbl), 24);
    *out = GTK_LABEL(gtk_label_new(value));
    gtk_label_set_xalign(*out, 0);
    gtk_widget_add_css_class(GTK_WIDGET(*out), "mono");
    gtk_box_append(GTK_BOX(row), lbl);
    gtk_box_append(GTK_BOX(row), GTK_WIDGET(*out));
    return row;
}

void gui_page_sources_init(GuiSources *p, GuiBridge *b) {
    memset(p, 0, sizeof(*p));
    p->bridge = b;
    p->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_size_request(header, -1, 64);
    gtk_widget_set_margin_start(header, 24);
    gtk_widget_set_margin_end(header, 24);
    GtkWidget *title = gtk_label_new("Sources");
    gtk_widget_add_css_class(title, "page-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    GtkWidget *sub = gtk_label_new("Many channels in, one channel out");
    gtk_widget_add_css_class(sub, "muted");
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(sub), 48);
    GtkWidget *note = gtk_label_new("edits are written to config.json");
    gtk_widget_add_css_class(note, "mono");
    gtk_widget_add_css_class(note, "muted");
    gtk_widget_set_hexpand(sub, TRUE);
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), sub);
    gtk_box_append(GTK_BOX(header), note);
    gtk_box_append(GTK_BOX(p->content), header);

    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_set_margin_start(main, 24);
    gtk_widget_set_margin_end(main, 24);
    gtk_widget_set_margin_bottom(main, 24);
    gtk_widget_set_vexpand(main, TRUE);
    gtk_box_append(GTK_BOX(p->content), main);

    GtkWidget *dest = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_add_css_class(dest, "card");
    gtk_widget_set_size_request(dest, -1, 84);
    gtk_widget_set_margin_start(dest, 2);
    gtk_widget_set_margin_end(dest, 2);
    GtkWidget *dest_lbl_col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_size_request(dest_lbl_col, 200, -1);
    GtkWidget *dest_title = gtk_label_new("Destination");
    gtk_label_set_xalign(GTK_LABEL(dest_title), 0);
    gtk_widget_add_css_class(dest_title, "card-title");
    GtkWidget *dest_desc = gtk_label_new("Where messages are forwarded");
    gtk_widget_add_css_class(dest_desc, "muted");
    gtk_label_set_xalign(GTK_LABEL(dest_desc), 0);
    gtk_box_append(GTK_BOX(dest_lbl_col), dest_title);
    gtk_box_append(GTK_BOX(dest_lbl_col), dest_desc);
    p->dest_lbl = GTK_LABEL(gtk_label_new("@me"));
    gtk_widget_add_css_class(GTK_WIDGET(p->dest_lbl), "mono");
    gtk_widget_set_size_request(GTK_WIDGET(p->dest_lbl), 280, 44);
    GtkWidget *hint = gtk_label_new("@me is your Saved Messages. Use @name for a channel you own.");
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0);
    gtk_widget_add_css_class(hint, "muted");
    gtk_widget_set_hexpand(hint, TRUE);
    gtk_box_append(GTK_BOX(dest), dest_lbl_col);
    gtk_box_append(GTK_BOX(dest), GTK_WIDGET(p->dest_lbl));
    gtk_box_append(GTK_BOX(dest), hint);
    gtk_box_append(GTK_BOX(main), dest);

    GtkWidget *table = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(table, "card");
    gtk_widget_set_vexpand(table, TRUE);
    GtkWidget *table_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_size_request(table_head, -1, 44);
    GtkWidget *heading = gtk_label_new("Sources");
    gtk_widget_add_css_class(heading, "card-title");
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    p->add_entry = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_size_request(GTK_WIDGET(p->add_entry), 280, 44);
    gtk_entry_set_placeholder_text(p->add_entry, "@name or t.me/name");
    GtkWidget *add_btn = gtk_button_new_with_label("Add source");
    gtk_widget_add_css_class(add_btn, "suggested-action");
    gtk_widget_add_css_class(add_btn, "btn-action");
    gtk_widget_set_size_request(add_btn, -1, 44);
    g_signal_connect(G_OBJECT(add_btn), "clicked", G_CALLBACK(on_add_clicked), p);
    p->add_error = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(p->add_error), "error");
    gtk_widget_set_hexpand(GTK_WIDGET(p->add_error), TRUE);
    gtk_box_append(GTK_BOX(table_head), heading);
    gtk_box_append(GTK_BOX(table_head), GTK_WIDGET(p->add_entry));
    gtk_box_append(GTK_BOX(table_head), add_btn);
    gtk_box_append(GTK_BOX(table_head), GTK_WIDGET(p->add_error));
    gtk_box_append(GTK_BOX(table), table_head);

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_size_request(cols, -1, 32);
    gtk_widget_add_css_class(cols, "table-head");
    gtk_widget_set_margin_start(cols, 16);
    gtk_widget_set_margin_end(cols, 16);
    const char *names[4] = {"Channel", "Status", "Forwarded", "On"};
    int widths[4] = {0, 110, 100, 60};
    for (int i = 0; i < 4; i++) {
        GtkWidget *lbl = gtk_label_new(names[i]);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        if (widths[i] > 0) gtk_widget_set_size_request(lbl, widths[i], -1);
        else gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(cols), lbl);
    }
    gtk_box_append(GTK_BOX(table), cols);
    GtkWidget *sc = gtk_scrolled_window_new();
    p->source_list = GTK_LIST_BOX(gtk_list_box_new());
    g_signal_connect(G_OBJECT(p->source_list), "row-selected", G_CALLBACK(on_selected), p);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), GTK_WIDGET(p->source_list));
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_box_append(GTK_BOX(table), sc);
    gtk_box_append(GTK_BOX(main), table);

    GtkWidget *detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_add_css_class(detail, "card");
    gtk_widget_set_margin_start(detail, 4);
    gtk_widget_set_margin_end(detail, 4);
    GtkWidget *sel_tag = gtk_label_new("Selected source");
    gtk_widget_add_css_class(sel_tag, "muted");
    gtk_label_set_xalign(GTK_LABEL(sel_tag), 0);
    p->sel_handle = GTK_LABEL(gtk_label_new("No source selected"));
    gtk_widget_add_css_class(GTK_WIDGET(p->sel_handle), "detail-handle");
    gtk_label_set_xalign(GTK_LABEL(p->sel_handle), 0);
    p->sel_state = GTK_LABEL(gtk_label_new("—"));
    gtk_label_set_xalign(GTK_LABEL(p->sel_state), 0);
    gtk_box_append(GTK_BOX(detail), sel_tag);
    gtk_box_append(GTK_BOX(detail), GTK_WIDGET(p->sel_handle));
    gtk_box_append(GTK_BOX(detail), GTK_WIDGET(p->sel_state));

    GtkWidget *stats = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_append(GTK_BOX(stats), field_row("Last seen message", "—", &p->sel_last));
    gtk_box_append(GTK_BOX(stats), field_row("Forwarded", "0", &p->sel_fwd));
    gtk_box_append(GTK_BOX(stats), field_row("Skipped as duplicates", "0", &p->sel_skip));
    gtk_box_append(GTK_BOX(stats), field_row("Backfill", "0%", &p->sel_backfill));
    gtk_box_append(GTK_BOX(stats), field_row("History window", "24 h", &p->sel_window));
    gtk_box_append(GTK_BOX(stats), field_row("Last activity", "—", &p->sel_activity));
    gtk_box_append(GTK_BOX(detail), stats);

    p->sel_note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_wrap(GTK_LABEL(p->sel_note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(p->sel_note), 0);
    gtk_widget_add_css_class(GTK_WIDGET(p->sel_note), "hint");
    gtk_box_append(GTK_BOX(detail), GTK_WIDGET(p->sel_note));

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    p->rescan_btn = gtk_button_new_with_label("Rescan from window start");
    gtk_widget_add_css_class(p->rescan_btn, "btn-full");
    gtk_widget_set_hexpand(p->rescan_btn, TRUE);
    gtk_widget_set_size_request(p->rescan_btn, -1, 44);
    g_signal_connect(G_OBJECT(p->rescan_btn), "clicked", G_CALLBACK(on_rescan_clicked), p);
    p->remove_btn = gtk_button_new_with_label("Remove source");
    gtk_widget_add_css_class(p->remove_btn, "destructive-action");
    gtk_widget_add_css_class(p->remove_btn, "btn-full");
    gtk_widget_set_hexpand(p->remove_btn, TRUE);
    gtk_widget_set_size_request(p->remove_btn, -1, 44);
    g_signal_connect(G_OBJECT(p->remove_btn), "clicked", G_CALLBACK(on_remove_clicked), p);
    gtk_box_append(GTK_BOX(actions), p->rescan_btn);
    gtk_box_append(GTK_BOX(actions), p->remove_btn);
    gtk_box_append(GTK_BOX(detail), actions);
    gtk_box_append(GTK_BOX(main), detail);

    refresh_sources(p);
}
