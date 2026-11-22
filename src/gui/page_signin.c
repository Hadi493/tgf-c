#include "pages.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <stdio.h>
#include <string.h>

static void show_session(GuiSignin *p, gboolean active) {
    gtk_widget_set_visible(p->flow_box, !active);
    gtk_widget_set_visible(p->session_box, active);
}

static void signin_show_step(GuiSignin *p, int step) {
    p->step = step;
    gtk_stack_set_visible_child(p->stack, p->pages[step]);
    show_session(p, FALSE);
    for (int i = 0; i < 3; i++)
        gtk_widget_add_css_class(p->segment[i], i <= step ? "segment-on" : "segment-off");
    const char *titles[3] = {"Sign in", "Verification code", "Two-factor password"};
    const char *subs[3] = {"Enter your Telegram phone number to begin.",
                           "Check your Telegram app on another device.",
                           "Sent to Telegram only. tgf never stores it."};
    const char *labels[3] = {"Step 1 of 3 · Phone number", "Step 2 of 3 · Verification code",
                             "Step 3 of 3 · Password"};
    gtk_label_set_text(p->title_lbl, titles[step]);
    gtk_label_set_text(p->sub_lbl, subs[step]);
    gtk_label_set_text(p->step_lbl, labels[step]);
    gtk_widget_set_sensitive(GTK_WIDGET(p->back_btn), step > 0);
    gtk_button_set_label(p->next_btn, step == 2 ? "Sign in" : "Next");
    gtk_label_set_text(p->error_lbl, "");
}

static void on_back_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GuiSignin *p = (GuiSignin *)user_data;
    if (p->step > 0) signin_show_step(p, p->step - 1);
}

static void on_next_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GuiSignin *p = (GuiSignin *)user_data;
    GtkWidget *field = p->field[p->step];
    const char *text = gtk_editable_get_text(GTK_EDITABLE(field));
    if (!text || !text[0]) {
        gtk_label_set_text(p->error_lbl, "Enter a value to continue");
        return;
    }
    gui_bridge_clear_auth_error(p->bridge);
    gtk_label_set_text(p->error_lbl, "");
    gtk_editable_set_text(GTK_EDITABLE(field), "");
    if (p->step == 0) tgf_submit_phone(p->bridge->ctx, text);
    else if (p->step == 1) tgf_submit_code(p->bridge->ctx, text);
    else tgf_submit_password(p->bridge->ctx, text);
}

static void on_logout_response(AdwAlertDialog *dialog, const char *response, gpointer user_data) {
    GuiSignin *p = (GuiSignin *)user_data;
    if (strcmp(response, "logout") == 0 && p->bridge->ctx) tgf_logout(p->bridge->ctx);
    adw_dialog_close(ADW_DIALOG(dialog));
}

static void on_logout_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GuiSignin *p = (GuiSignin *)user_data;
    AdwDialog *dialog = adw_alert_dialog_new("Log out of Telegram?",
        "This clears the cached session in tdlib_db/. You will need your phone number to sign in again.");
    AdwAlertDialog *adialog = ADW_ALERT_DIALOG(dialog);
    adw_alert_dialog_add_response(adialog, "cancel", "Cancel");
    adw_alert_dialog_add_response(adialog, "logout", "Log out");
    adw_alert_dialog_set_default_response(adialog, "cancel");
    adw_alert_dialog_set_close_response(adialog, "cancel");
    g_signal_connect(adialog, "response", G_CALLBACK(on_logout_response), p);
    adw_dialog_present(dialog, GTK_WIDGET(p->bridge->window));
}

static void on_open_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GuiSignin *p = (GuiSignin *)user_data;
    if (p->bridge->owner) gui_win_set_page((GuiWindow *)p->bridge->owner, 0, NULL);
}

static GtkWidget *flow_page(const char *caption, const char *placeholder,
                            const char *hint, gboolean password, GtkWidget **out) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *lbl = gtk_label_new(caption);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_widget_add_css_class(lbl, "card-title");
    GtkWidget *entry = password ? gtk_password_entry_new() : gtk_entry_new();
    gtk_widget_set_size_request(entry, -1, 48);
    gtk_widget_add_css_class(entry, "mono");
    gtk_widget_add_css_class(entry, "signin-entry");
    if (!password) gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    GtkWidget *hint_lbl = gtk_label_new(hint);
    gtk_label_set_xalign(GTK_LABEL(hint_lbl), 0);
    gtk_widget_add_css_class(hint_lbl, "muted");
    gtk_box_append(GTK_BOX(page), lbl);
    gtk_box_append(GTK_BOX(page), entry);
    gtk_box_append(GTK_BOX(page), hint_lbl);
    *out = entry;
    return page;
}

void gui_page_signin_init(GuiSignin *p, GuiBridge *b) {
    memset(p, 0, sizeof(*p));
    p->bridge = b;
    p->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_set_size_request(card, 460, -1);
    gtk_widget_set_halign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(card, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(card, 36);
    gtk_widget_set_margin_bottom(card, 36);

    p->flow_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(p->flow_box, 20);
    gtk_widget_set_margin_end(p->flow_box, 20);
    gtk_widget_set_margin_top(p->flow_box, 20);
    gtk_widget_set_margin_bottom(p->flow_box, 20);
    p->title_lbl = GTK_LABEL(gtk_label_new("Sign in"));
    gtk_widget_add_css_class(GTK_WIDGET(p->title_lbl), "window-title");
    gtk_widget_add_css_class(GTK_WIDGET(p->title_lbl), "signin-title");
    gtk_label_set_xalign(GTK_LABEL(p->title_lbl), 0);
    p->sub_lbl = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(p->sub_lbl), "muted");
    gtk_label_set_xalign(GTK_LABEL(p->sub_lbl), 0);

    GtkWidget *progress = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (int i = 0; i < 3; i++) {
        p->segment[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(p->segment[i], -1, 4);
        gtk_widget_set_hexpand(p->segment[i], TRUE);
        gtk_box_append(GTK_BOX(progress), p->segment[i]);
    }
    p->step_lbl = GTK_LABEL(gtk_label_new("Step 1 of 3 · Phone number"));
    gtk_label_set_xalign(GTK_LABEL(p->step_lbl), 0);
    gtk_widget_add_css_class(GTK_WIDGET(p->step_lbl), "muted");

    p->stack = GTK_STACK(gtk_stack_new());
    p->pages[0] = flow_page("Phone number", "+1 555 0100", "Include the country code.", FALSE, &p->field[0]);
    p->pages[1] = flow_page("Verification code", "00000", "Check your Telegram app on another device.", FALSE, &p->field[1]);
    p->pages[2] = flow_page("Password", "", "Sent to Telegram only. tgf never stores it.", TRUE, &p->field[2]);
    gtk_stack_add_named(p->stack, p->pages[0], "phone");
    gtk_stack_add_named(p->stack, p->pages[1], "code");
    gtk_stack_add_named(p->stack, p->pages[2], "password");

    p->error_lbl = GTK_LABEL(gtk_label_new(""));
    gtk_widget_add_css_class(GTK_WIDGET(p->error_lbl), "error");
    gtk_label_set_xalign(GTK_LABEL(p->error_lbl), 0);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    p->back_btn = GTK_BUTTON(gtk_button_new_with_label("Back"));
    gtk_widget_add_css_class(GTK_WIDGET(p->back_btn), "btn-action");
    gtk_widget_set_size_request(GTK_WIDGET(p->back_btn), -1, 48);
    g_signal_connect(G_OBJECT(p->back_btn), "clicked", G_CALLBACK(on_back_clicked), p);
    p->next_btn = GTK_BUTTON(gtk_button_new_with_label("Next"));
    gtk_widget_add_css_class(GTK_WIDGET(p->next_btn), "suggested-action");
    gtk_widget_add_css_class(GTK_WIDGET(p->next_btn), "btn-action");
    gtk_widget_set_size_request(GTK_WIDGET(p->next_btn), -1, 48);
    gtk_widget_set_hexpand(GTK_WIDGET(p->next_btn), TRUE);
    g_signal_connect(G_OBJECT(p->next_btn), "clicked", G_CALLBACK(on_next_clicked), p);
    gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(p->back_btn));
    gtk_box_append(GTK_BOX(buttons), GTK_WIDGET(p->next_btn));

    gtk_box_append(GTK_BOX(p->flow_box), GTK_WIDGET(p->title_lbl));
    gtk_box_append(GTK_BOX(p->flow_box), GTK_WIDGET(p->sub_lbl));
    gtk_box_append(GTK_BOX(p->flow_box), progress);
    gtk_box_append(GTK_BOX(p->flow_box), GTK_WIDGET(p->step_lbl));
    gtk_box_append(GTK_BOX(p->flow_box), GTK_WIDGET(p->stack));
    gtk_box_append(GTK_BOX(p->flow_box), GTK_WIDGET(p->error_lbl));
    gtk_box_append(GTK_BOX(p->flow_box), buttons);
    GtkWidget *flow_privacy = gtk_label_new(
        "tgf talks to Telegram through TDLib, bundled in the binary. Your number and codes go straight to Telegram.");
    gtk_label_set_wrap(GTK_LABEL(flow_privacy), TRUE);
    gtk_label_set_xalign(GTK_LABEL(flow_privacy), 0);
    gtk_widget_add_css_class(flow_privacy, "muted");
    gtk_widget_add_css_class(flow_privacy, "privacy-note");
    gtk_box_append(GTK_BOX(p->flow_box), flow_privacy);

    p->session_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(p->session_box, 20);
    gtk_widget_set_margin_end(p->session_box, 20);
    gtk_widget_set_margin_top(p->session_box, 20);
    gtk_widget_set_margin_bottom(p->session_box, 20);
    GtkWidget *check = gtk_label_new("✓");
    gtk_widget_add_css_class(check, "success-mark");
    GtkWidget *session_title = gtk_label_new("Session active");
    gtk_widget_add_css_class(session_title, "window-title");
    gtk_label_set_xalign(GTK_LABEL(session_title), 0);
    GtkWidget *db = gtk_label_new("tdlib_db/");
    gtk_widget_add_css_class(db, "mono");
    gtk_label_set_xalign(GTK_LABEL(db), 0);
    GtkWidget *note = gtk_label_new("Session saved on this machine");
    gtk_widget_add_css_class(note, "muted");
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    GtkWidget *session_actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    p->open_btn = gtk_button_new_with_label("Open dashboard");
    gtk_widget_add_css_class(p->open_btn, "suggested-action");
    gtk_widget_add_css_class(p->open_btn, "btn-full");
    gtk_widget_set_hexpand(p->open_btn, TRUE);
    gtk_widget_set_size_request(p->open_btn, -1, 48);
    g_signal_connect(G_OBJECT(p->open_btn), "clicked", G_CALLBACK(on_open_clicked), p);
    p->logout_btn = gtk_button_new_with_label("Log out");
    gtk_widget_add_css_class(p->logout_btn, "destructive-action");
    gtk_widget_add_css_class(p->logout_btn, "btn-action");
    gtk_widget_set_size_request(p->logout_btn, -1, 48);
    g_signal_connect(G_OBJECT(p->logout_btn), "clicked", G_CALLBACK(on_logout_clicked), p);
    gtk_box_append(GTK_BOX(session_actions), p->open_btn);
    gtk_box_append(GTK_BOX(session_actions), p->logout_btn);
    GtkWidget *privacy = gtk_label_new(
        "tgf talks to Telegram through TDLib, bundled in the binary. Your number and codes go straight to Telegram.");
    gtk_label_set_wrap(GTK_LABEL(privacy), TRUE);
    gtk_label_set_xalign(GTK_LABEL(privacy), 0);
    gtk_widget_add_css_class(privacy, "muted");
    gtk_widget_add_css_class(privacy, "privacy-note");
    gtk_box_append(GTK_BOX(p->session_box), check);
    gtk_box_append(GTK_BOX(p->session_box), session_title);
    gtk_box_append(GTK_BOX(p->session_box), db);
    gtk_box_append(GTK_BOX(p->session_box), note);
    gtk_box_append(GTK_BOX(p->session_box), session_actions);
    gtk_box_append(GTK_BOX(p->session_box), privacy);

    gtk_box_append(GTK_BOX(card), p->flow_box);
    gtk_box_append(GTK_BOX(card), p->session_box);
    gtk_box_append(GTK_BOX(p->content), card);
    signin_show_step(p, 0);
}

void gui_page_signin_event(GuiSignin *p, const tgf_event *ev) {
    const char *error = p->bridge->state.metrics.auth_error;
    if (ev->kind == TGF_EV_ERROR) {
        if (ev->scope == TGF_ERR_SCOPE_AUTH && error[0]) {
            gtk_label_set_text(p->error_lbl, error);
            gtk_editable_set_text(GTK_EDITABLE(p->field[p->step]), "");
        }
        return;
    }
    if (ev->kind != TGF_EV_AUTH_STATE) return;
    if (error[0] && (ev->auth == TGF_AUTH_CODE || ev->auth == TGF_AUTH_PASSWORD || ev->auth == TGF_AUTH_PHONE)) {
        gtk_label_set_text(p->error_lbl, error);
        gtk_editable_set_text(GTK_EDITABLE(p->field[p->step]), "");
        return;
    }
    switch (ev->auth) {
    case TGF_AUTH_PHONE: signin_show_step(p, 0); break;
    case TGF_AUTH_CODE: signin_show_step(p, 1); break;
    case TGF_AUTH_PASSWORD: signin_show_step(p, 2); break;
    case TGF_AUTH_READY: show_session(p, TRUE); break;
    case TGF_AUTH_CLOSING:
    case TGF_AUTH_LOGGING_OUT: signin_show_step(p, 0); break;
    default: break;
    }
}

void gui_page_signin_set_step(GuiSignin *p, int step) {
    signin_show_step(p, step);
}
