#include "pages.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

struct GuiWindow {
    GtkWidget *window;
    GuiBridge *bridge;
    GuiDashboard dash;
    GuiSources srcs;
    GuiSettings settings;
    GuiSignin signin;
    GtkWidget *content;
    GtkStack *stack;
    GtkWidget *nav_btn[4];
};

static void set_nav_active(GuiWindow *w, int page) {
    for (int i = 0; i < 4; i++) {
        if (!w->nav_btn[i]) continue;
        if (i == page) gtk_widget_add_css_class(w->nav_btn[i], "active");
        else gtk_widget_remove_css_class(w->nav_btn[i], "active");
    }
}

static void on_nav_clicked(GtkButton *btn, gpointer user_data) {
    GuiWindow *w = (GuiWindow *)user_data;
    (void)btn;
    int page = (int)(intptr_t)g_object_get_data(G_OBJECT(btn), "page");
    gui_win_set_page(w, page, NULL);
}

static void open_sources_cb(const char *handle, gpointer user_data) {
    gui_win_set_page((GuiWindow *)user_data, 1, handle);
}

static GtkWidget *append_nav(GtkWidget *nav, const char *label, int page, GuiWindow *w) {
    static const char *icons[4] = {"view-grid-symbolic", "view-list-symbolic",
                                   "preferences-system-symbolic", "avatar-default-symbolic"};
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "nav-button");
    gtk_widget_set_size_request(btn, -1, 44);
    gtk_widget_set_halign(btn, GTK_ALIGN_FILL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(box, GTK_ALIGN_START);
    GtkWidget *icon = gtk_image_new_from_icon_name(icons[page]);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
    GtkWidget *text = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), text);
    gtk_button_set_child(GTK_BUTTON(btn), box);
    g_object_set_data(G_OBJECT(btn), "page", GINT_TO_POINTER(page));
    g_signal_connect(G_OBJECT(btn), "clicked", G_CALLBACK(on_nav_clicked), w);
    gtk_box_append(GTK_BOX(nav), btn);
    return btn;
}

GuiWindow *gui_win_new(GtkApplication *app, GuiBridge *b) {
    GuiWindow *w = calloc(1, sizeof(*w));
    w->bridge = b;
    w->content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(sidebar, "sidebar");
    gtk_widget_set_size_request(sidebar, 232, -1);
    GtkWidget *identity = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *logo = g_file_test("tgf.png", G_FILE_TEST_EXISTS)
        ? gtk_image_new_from_file("tgf.png")
        : gtk_image_new_from_icon_name("image-missing-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 48);
    gtk_widget_add_css_class(logo, "brand-logo");
    gtk_widget_set_halign(logo, GTK_ALIGN_START);
    GtkWidget *wordmark = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *brand = gtk_label_new("tgf");
    gtk_label_set_xalign(GTK_LABEL(brand), 0);
    gtk_widget_add_css_class(brand, "brand");
    gtk_box_append(GTK_BOX(wordmark), brand);
    GtkWidget *version = gtk_label_new("v1.3.1");
    gtk_label_set_xalign(GTK_LABEL(version), 0);
    gtk_widget_add_css_class(version, "version");
    gtk_box_append(GTK_BOX(wordmark), version);
    gtk_box_append(GTK_BOX(identity), logo);
    gtk_box_append(GTK_BOX(identity), wordmark);
    gtk_box_append(GTK_BOX(sidebar), identity);
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_vexpand(nav, TRUE);
    gtk_box_append(GTK_BOX(sidebar), nav);
    GtkWidget *connection = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(connection, "connection-card");
    GtkWidget *connected = gtk_label_new("TDLib connected");
    gtk_label_set_xalign(GTK_LABEL(connected), 0);
    GtkWidget *session = gtk_label_new("session: tdlib_db/");
    gtk_label_set_xalign(GTK_LABEL(session), 0);
    gtk_widget_add_css_class(session, "muted");
    gtk_box_append(GTK_BOX(connection), connected);
    gtk_box_append(GTK_BOX(connection), session);
    gtk_box_append(GTK_BOX(sidebar), connection);
    gtk_box_append(GTK_BOX(w->content), sidebar);
    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(main, TRUE);
    gtk_box_append(GTK_BOX(w->content), main);
    w->stack = GTK_STACK(gtk_stack_new());
    gtk_widget_set_vexpand(GTK_WIDGET(w->stack), TRUE);
    gtk_box_append(GTK_BOX(main), GTK_WIDGET(w->stack));
    gui_page_dashboard_init(&w->dash, b);
    w->dash.on_open_sources = open_sources_cb;
    w->dash.on_open_data = w;
    gui_page_sources_init(&w->srcs, b);
    gui_page_settings_init(&w->settings, b);
    gui_page_signin_init(&w->signin, b);
    gtk_stack_add_named(w->stack, w->dash.content, "Dashboard");
    gtk_stack_add_named(w->stack, w->srcs.content, "Sources");
    gtk_stack_add_named(w->stack, w->settings.content, "Settings");
    gtk_stack_add_named(w->stack, w->signin.content, "Session");
    w->nav_btn[0] = append_nav(nav, "Dashboard", 0, w);
    w->nav_btn[1] = append_nav(nav, "Sources", 1, w);
    w->nav_btn[2] = append_nav(nav, "Settings", 2, w);
    w->nav_btn[3] = append_nav(nav, "Session", 3, w);
    set_nav_active(w, 0);

    GtkWidget *win = adw_application_window_new(app);
    adw_application_window_set_content((AdwApplicationWindow *)win, w->content);
    gtk_window_set_default_size(GTK_WINDOW(win), 1280, 800);
    gtk_window_present(GTK_WINDOW(win));
    w->window = win;
    return w;
}

GuiWindow *gui_win_new_error(GtkApplication *app, GuiBridge *b, const char *message) {
    GuiWindow *w = calloc(1, sizeof(*w));
    w->bridge = b;
    w->content = gtk_label_new(message ? message : "Cannot load config.json");
    GtkWidget *win = adw_application_window_new(app);
    adw_application_window_set_content((AdwApplicationWindow *)win, w->content);
    gtk_window_set_default_size(GTK_WINDOW(win), 960, 640);
    gtk_window_present(GTK_WINDOW(win));
    w->window = win;
    return w;
}

void gui_win_free(GuiWindow *w) {
    if (!w) return;
    tgf_config_free(&w->settings.cfg);
    if (w->window) gtk_window_destroy(GTK_WINDOW(w->window));
    free(w);
}

void gui_win_set_page(GuiWindow *w, int page, const char *source_handle) {
    if (!w) return;
    switch (page) {
    case 0: gtk_stack_set_visible_child(w->stack, w->dash.content); break;
    case 1: gtk_stack_set_visible_child(w->stack, w->srcs.content); break;
    case 2: gtk_stack_set_visible_child(w->stack, w->settings.content); break;
    case 3: gtk_stack_set_visible_child(w->stack, w->signin.content); break;
    default: return;
    }
    set_nav_active(w, page);
    if (page == 1 && source_handle) gui_page_sources_select(&w->srcs, source_handle);
}

GtkWidget *gui_win_get_content(GuiWindow *w) { return w ? w->content : NULL; }

GtkWidget *gui_win_get_window(GuiWindow *w) { return w ? w->window : NULL; }

void gui_win_page_event(GuiBridge *b, const tgf_event *ev) {
    GuiWindow *w = (GuiWindow *)b->owner;
    if (!w) return;
    switch (ev->kind) {
    case TGF_EV_AUTH_STATE:
        gui_page_signin_event(&w->signin, ev);
        break;
    case TGF_EV_ERROR:
        gui_page_signin_event(&w->signin, ev);
        gui_page_dashboard_event(&w->dash, ev);
        break;
    case TGF_EV_SOURCE_STATE:
    case TGF_EV_FORWARDED:
    case TGF_EV_SKIPPED:
        gui_page_sources_event(&w->srcs, ev);
        gui_page_dashboard_event(&w->dash, ev);
        break;
    case TGF_EV_LOG:
    case TGF_EV_QUEUE:
    case TGF_EV_NEXT_FORWARD_IN:
    case TGF_EV_PIPELINE_STATE:
    case TGF_EV_CONNECTION_STATE:
        gui_page_dashboard_event(&w->dash, ev);
        break;
    default:
        break;
    }
}
