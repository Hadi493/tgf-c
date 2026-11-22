#include "pages.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_hash(const char *s) {
    if (!s || strlen(s) != 32) return 0;
    for (size_t i = 0; i < 32; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    return 1;
}

static void refresh_preview(GuiSettings *p) {
    if (!p->preview_lbl) return;
    const char *hash = gtk_editable_get_text(GTK_EDITABLE(p->api_hash_entry));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "api_id", atoi(gtk_editable_get_text(GTK_EDITABLE(p->api_id_entry))));
    char masked[64] = {0};
    size_t len = hash ? strlen(hash) : 0;
    if (len > sizeof(masked) - 1) len = sizeof(masked) - 1;
    memset(masked, '*', len);
    cJSON_AddStringToObject(root, "api_hash", masked);
    if (p->cfg.dest_channel) cJSON_AddStringToObject(root, "dest_channel", p->cfg.dest_channel);
    cJSON *sources = cJSON_CreateArray();
    for (int i = 0; i < p->cfg.num_sources; i++) cJSON_AddItemToArray(sources, cJSON_CreateString(p->cfg.source_channels[i]));
    cJSON_AddItemToObject(root, "source_channels", sources);
    if (p->cfg.num_disabled > 0) {
        cJSON *disabled = cJSON_CreateArray();
        for (int i = 0; i < p->cfg.num_disabled; i++) cJSON_AddItemToArray(disabled, cJSON_CreateString(p->cfg.disabled_channels[i]));
        cJSON_AddItemToObject(root, "disabled_channels", disabled);
    }
    cJSON_AddStringToObject(root, "history_file", p->cfg.history_file ? p->cfg.history_file : "history.txt");
    cJSON_AddNumberToObject(root, "forward_delay_sec", p->delay);
    cJSON_AddNumberToObject(root, "history_window_hours", p->window_hours);
    cJSON_AddBoolToObject(root, "enable_sequential_forwarding", gtk_switch_get_active(p->seq_switch));
    cJSON_AddBoolToObject(root, "enable_ordering", p->cfg.enable_ordering);
    cJSON_AddStringToObject(root, "sequence_direction",
        gtk_toggle_button_get_active(p->order_btn[1]) ? "desc" : "asc");
    char *text = cJSON_Print(root);
    gtk_label_set_text(p->preview_lbl, text ? text : "{}");
    free(text);
    cJSON_Delete(root);
}

static void refresh_from_config(GuiSettings *p) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", p->cfg.api_id);
    gtk_editable_set_text(GTK_EDITABLE(p->api_id_entry), buf);
    gtk_editable_set_text(GTK_EDITABLE(p->api_hash_entry), p->cfg.api_hash ? p->cfg.api_hash : "");
    p->delay = p->cfg.forward_delay_sec;
    p->window_hours = p->cfg.history_window_hours;
    snprintf(buf, sizeof(buf), "%d s", p->delay);
    gtk_label_set_text(p->delay_value, buf);
    gtk_switch_set_active(p->seq_switch, p->cfg.enable_sequential_forwarding);
    gtk_toggle_button_set_active(p->order_btn[p->cfg.sequence_direction == 1 ? 1 : 0], TRUE);
    int window_idx = 3;
    if (p->window_hours == 6) window_idx = 0;
    else if (p->window_hours == 24) window_idx = 1;
    else if (p->window_hours == 72) window_idx = 2;
    gtk_toggle_button_set_active(p->window_btn[window_idx], TRUE);
    refresh_preview(p);
}

static void load_config(GuiSettings *p) {
    tgf_config_free(&p->cfg);
    if (tgf_config_load("config.json", &p->cfg) != TGF_OK) {
        memset(&p->cfg, 0, sizeof(p->cfg));
        p->cfg.forward_delay_sec = 1;
        p->cfg.history_window_hours = 24;
    }
}

static void on_delay_clicked(GtkButton *btn, gpointer user_data) {
    GuiSettings *p = (GuiSettings *)user_data;
    int delta = (int)(intptr_t)g_object_get_data(G_OBJECT(btn), "delta");
    p->delay += delta;
    if (p->delay < 1) p->delay = 1;
    if (p->delay > 3600) p->delay = 3600;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d s", p->delay);
    gtk_label_set_text(p->delay_value, buf);
    refresh_preview(p);
}

static void on_seq_toggled(GtkSwitch *sw, GParamSpec *pspec, gpointer user_data) {
    (void)sw;
    (void)pspec;
    GuiSettings *p = (GuiSettings *)user_data;
    gboolean seq = gtk_switch_get_active(p->seq_switch);
    for (int i = 0; i < 2; i++) gtk_widget_set_sensitive(GTK_WIDGET(p->order_btn[i]), seq);
    refresh_preview(p);
}

static void on_order_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)btn;
    refresh_preview((GuiSettings *)user_data);
}

static void on_window_toggled(GtkToggleButton *btn, gpointer user_data) {
    GuiSettings *p = (GuiSettings *)user_data;
    if (!gtk_toggle_button_get_active(btn)) return;
    int idx = (int)(intptr_t)g_object_get_data(G_OBJECT(btn), "window-index");
    p->window_hours = idx == 0 ? 6 : idx == 1 ? 24 : idx == 2 ? 72 : 0;
    refresh_preview(p);
}

static void on_save_clicked(GtkButton *btn, gpointer user_data) {
    GuiSettings *p = (GuiSettings *)user_data;
    (void)btn;
    const char *id = gtk_editable_get_text(GTK_EDITABLE(p->api_id_entry));
    const char *hash = gtk_editable_get_text(GTK_EDITABLE(p->api_hash_entry));
    if (atoi(id) <= 0) { gtk_label_set_text(p->error_lbl, "api_id must be a positive integer"); return; }
    if (!hex_hash(hash)) { gtk_label_set_text(p->error_lbl, "api_hash must be 32 hexadecimal characters"); return; }
    free(p->cfg.api_hash);
    p->cfg.api_hash = strdup(hash);
    p->cfg.api_id = atoi(id);
    p->cfg.forward_delay_sec = p->delay;
    p->cfg.history_window_hours = p->window_hours;
    p->cfg.enable_sequential_forwarding = gtk_switch_get_active(p->seq_switch);
    p->cfg.sequence_direction = gtk_toggle_button_get_active(p->order_btn[1]) ? 1 : 0;
    if (tgf_config_save("config.json", &p->cfg) != TGF_OK) {
        gtk_label_set_text(p->error_lbl, "Could not save config.json");
        return;
    }
    if (tgf_apply_config(p->bridge->ctx, &p->cfg) != TGF_OK) {
        gtk_label_set_text(p->error_lbl, "Config was saved but could not be applied");
        return;
    }
    snprintf(p->bridge->state.dest, sizeof(p->bridge->state.dest), "%s",
             p->cfg.dest_channel ? p->cfg.dest_channel : "@me");
    p->bridge->state.sequence_direction = p->cfg.sequence_direction;
    p->bridge->state.history_window_hours = p->cfg.history_window_hours;
    gtk_label_set_text(p->error_lbl, "Saved");
    refresh_preview(p);
}

static void on_revert_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GuiSettings *p = (GuiSettings *)user_data;
    load_config(p);
    refresh_from_config(p);
    gtk_label_set_text(p->error_lbl, "");
}

static GtkWidget *setting_row(const char *title, const char *desc, GtkWidget *control) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_add_css_class(row, "setting-row");
    gtk_widget_set_size_request(row, -1, 80);
    gtk_widget_set_margin_start(row, 4);
    gtk_widget_set_margin_end(row, 4);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_size_request(col, 200, -1);
    GtkWidget *lbl = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_widget_add_css_class(lbl, "card-title");
    GtkWidget *sub = gtk_label_new(desc);
    gtk_label_set_xalign(GTK_LABEL(sub), 0);
    gtk_label_set_wrap(GTK_LABEL(sub), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(sub), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(sub, "muted");
    gtk_box_append(GTK_BOX(col), lbl);
    gtk_box_append(GTK_BOX(col), sub);
    gtk_widget_set_hexpand(col, TRUE);
    gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), col);
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), control);
    return row;
}

static GtkWidget *toggle_pairs(GtkToggleButton **out, const char **names, int count,
                               GCallback callback, GuiSettings *p, int window_mode) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, window_mode ? 8 : 3);
    if (!window_mode) {
        gtk_widget_add_css_class(box, "segment-group");
        gtk_widget_set_size_request(box, -1, 44);
    }
    for (int i = 0; i < count; i++) {
        GtkWidget *btn = gtk_toggle_button_new_with_label(names[i]);
        if (window_mode) {
            gtk_widget_add_css_class(btn, "choice");
            gtk_widget_set_size_request(btn, 64, 44);
        } else {
            gtk_widget_add_css_class(btn, "segment");
            gtk_widget_set_size_request(btn, -1, 38);
        }
        g_object_set_data(G_OBJECT(btn), window_mode ? "window-index" : "order-index", GINT_TO_POINTER(i));
        g_signal_connect(G_OBJECT(btn), "clicked", callback, p);
        out[i] = GTK_TOGGLE_BUTTON(btn);
        if (i > 0) gtk_toggle_button_set_group(out[i], out[0]);
        gtk_box_append(GTK_BOX(box), btn);
    }
    return box;
}

void gui_page_settings_init(GuiSettings *p, GuiBridge *b) {
    memset(p, 0, sizeof(*p));
    p->bridge = b;
    load_config(p);
    p->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_size_request(header, -1, 64);
    gtk_widget_set_margin_start(header, 24);
    gtk_widget_set_margin_end(header, 24);
    GtkWidget *title = gtk_label_new("Settings");
    gtk_widget_add_css_class(title, "page-title");
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    GtkWidget *sub = gtk_label_new("Everything here maps 1:1 to a field in config.json");
    gtk_widget_add_css_class(sub, "muted");
    gtk_widget_set_hexpand(sub, TRUE);
    gtk_box_append(GTK_BOX(header), title);
    gtk_box_append(GTK_BOX(header), sub);
    gtk_box_append(GTK_BOX(p->content), header);

    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_margin_start(main, 24);
    gtk_widget_set_margin_end(main, 24);
    gtk_widget_set_margin_bottom(main, 24);
    gtk_widget_set_vexpand(main, TRUE);
    gtk_box_append(GTK_BOX(p->content), main);

    GtkWidget *form = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(form, "card");
    gtk_widget_set_hexpand(form, TRUE);
    GtkWidget *api_id = gtk_entry_new();
    gtk_widget_set_size_request(api_id, 200, 44);
    gtk_widget_add_css_class(api_id, "mono");
    p->api_id_entry = GTK_ENTRY(api_id);
    gtk_box_append(GTK_BOX(form), setting_row("API ID", "Telegram application ID from my.telegram.org/apps", api_id));

    GtkWidget *hash = gtk_password_entry_new();
    gtk_widget_set_size_request(hash, 200, 44);
    gtk_widget_add_css_class(hash, "mono");
    p->api_hash_entry = GTK_ENTRY(hash);
    gtk_box_append(GTK_BOX(form), setting_row("API hash", "Stays on this machine, never shown in the preview", hash));

    GtkWidget *delay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    p->delay_minus = gtk_button_new_with_label("−");
    p->delay_plus = gtk_button_new_with_label("+");
    gtk_widget_set_size_request(p->delay_minus, 44, 44);
    gtk_widget_set_size_request(p->delay_plus, 44, 44);
    g_object_set_data(G_OBJECT(p->delay_minus), "delta", GINT_TO_POINTER(-1));
    g_object_set_data(G_OBJECT(p->delay_plus), "delta", GINT_TO_POINTER(1));
    g_signal_connect(G_OBJECT(p->delay_minus), "clicked", G_CALLBACK(on_delay_clicked), p);
    g_signal_connect(G_OBJECT(p->delay_plus), "clicked", G_CALLBACK(on_delay_clicked), p);
    p->delay_value = GTK_LABEL(gtk_label_new("1 s"));
    gtk_widget_set_size_request(GTK_WIDGET(p->delay_value), 72, 44);
    gtk_widget_add_css_class(GTK_WIDGET(p->delay_value), "mono");
    gtk_widget_add_css_class(GTK_WIDGET(p->delay_value), "delay-value");
    gtk_box_append(GTK_BOX(delay), p->delay_minus);
    gtk_box_append(GTK_BOX(delay), GTK_WIDGET(p->delay_value));
    gtk_box_append(GTK_BOX(delay), p->delay_plus);
    gtk_box_append(GTK_BOX(form), setting_row("Delay between forwards", "Keeps you under Telegram rate limits. Default 1 s.", delay));

    GtkWidget *seq = gtk_switch_new();
    gtk_accessible_update_property(GTK_ACCESSIBLE(seq), GTK_ACCESSIBLE_PROPERTY_LABEL, "Sequential forwarding", -1);
    p->seq_switch = GTK_SWITCH(seq);
    g_signal_connect(G_OBJECT(seq), "notify::active", G_CALLBACK(on_seq_toggled), p);
    gtk_box_append(GTK_BOX(form), setting_row("Sequential forwarding", "Collect all history first, then forward in order", seq));

    const char *orders[2] = {"Old → new", "New → old"};
    GtkWidget *order = toggle_pairs(p->order_btn, orders, 2, G_CALLBACK(on_order_toggled), p, 0);
    gtk_box_append(GTK_BOX(form), setting_row("Order", "Only applies when sequential forwarding is on", order));

    const char *windows[4] = {"6 h", "24 h", "72 h", "All"};
    GtkWidget *window = toggle_pairs(p->window_btn, windows, 4, G_CALLBACK(on_window_toggled), p, 1);
    gtk_box_append(GTK_BOX(form), setting_row("History window", "Only forward messages newer than this", window));

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(actions, "form-actions");
    gtk_widget_set_margin_start(actions, 4);
    gtk_widget_set_margin_bottom(actions, 4);
    GtkWidget *revert = gtk_button_new_with_label("Revert");
    gtk_widget_add_css_class(revert, "btn-action");
    gtk_widget_set_size_request(revert, -1, 44);
    g_signal_connect(G_OBJECT(revert), "clicked", G_CALLBACK(on_revert_clicked), p);
    GtkWidget *save = gtk_button_new_with_label("Save to config.json");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_add_css_class(save, "btn-primary");
    gtk_widget_set_size_request(save, -1, 44);
    g_signal_connect(G_OBJECT(save), "clicked", G_CALLBACK(on_save_clicked), p);
    gtk_box_append(GTK_BOX(actions), revert);
    gtk_box_append(GTK_BOX(actions), save);
    p->error_lbl = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(p->error_lbl), "error");
    gtk_widget_set_hexpand(GTK_WIDGET(p->error_lbl), TRUE);
    gtk_box_append(GTK_BOX(actions), GTK_WIDGET(p->error_lbl));
    gtk_box_append(GTK_BOX(form), actions);
    gtk_box_append(GTK_BOX(main), form);

    GtkWidget *preview = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(preview, "card");
    gtk_widget_add_css_class(preview, "preview-card");
    gtk_widget_set_size_request(preview, 360, -1);
    gtk_widget_set_hexpand(preview, FALSE);
    GtkWidget *ph = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_size_request(ph, -1, 46);
    GtkWidget *pname = gtk_label_new("config.json");
    gtk_widget_add_css_class(pname, "mono");
    gtk_widget_add_css_class(pname, "card-title");
    GtkWidget *plive = gtk_label_new("live preview");
    gtk_widget_add_css_class(plive, "muted");
    gtk_widget_set_hexpand(pname, TRUE);
    gtk_box_append(GTK_BOX(ph), pname);
    gtk_box_append(GTK_BOX(ph), plive);
    p->preview_lbl = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(GTK_LABEL(p->preview_lbl), 0);
    gtk_label_set_wrap(GTK_LABEL(p->preview_lbl), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(p->preview_lbl), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(GTK_WIDGET(p->preview_lbl), "mono");
    gtk_box_append(GTK_BOX(preview), ph);
    gtk_box_append(GTK_BOX(preview), GTK_WIDGET(p->preview_lbl));
    gtk_box_append(GTK_BOX(main), preview);

    refresh_from_config(p);
}

void gui_page_settings_event(GuiSettings *p, const tgf_event *ev) { (void)p; (void)ev; }
