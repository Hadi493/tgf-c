#include "gui.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static AdwApplication *g_app = NULL;
static GuiWindow *g_win = NULL;
static GuiBridge g_bridge;
static tgf_ctx *g_core_ctx = NULL;
static guint g_idle_id = 0;

static void install_css(void) {
    static const char css[] =
        ":root { --accent-bg-color: #6cb6ff; --accent-color: #6cb6ff; --accent-fg-color: #0b1520; "
        "--tgf-canvas: #14171b; --tgf-sidebar: #101316; --tgf-card: #1b1f24; --tgf-log: #0f1215; "
        "--tgf-raised: #181c21; --tgf-input: #14171b; --tgf-border: #262b31; --tgf-border-strong: #343b43; "
        "--tgf-text: #e3e6e8; --tgf-secondary: #c4cacf; --tgf-muted: #9aa3ab; --tgf-log-text: #d6dadd; "
        "--tgf-accent: #6cb6ff; --tgf-green: #7fd18b; --tgf-green-bg: #1c2b22; "
        "--tgf-amber: #e5b567; --tgf-amber-bg: #2e2619; --tgf-red: #f08a8a; --tgf-red-border: #5a3436; "
        "--tgf-nav: #232a31; }"
        "window { background-color: var(--tgf-canvas); color: var(--tgf-text); "
        "font-family: \"Inter\", system-ui, sans-serif; font-size: 14px; }"
        ".sidebar { background-color: var(--tgf-sidebar); border-right: 1px solid var(--tgf-border); "
        "padding: 20px 14px; min-width: 232px; }"
        ".brand-logo { margin: 0; }"
        ".brand { font-weight: 700; font-size: 18px; color: var(--tgf-text); }"
        ".version, .muted { color: var(--tgf-muted); font-size: 13px; }"
        ".nav-button { min-height: 44px; padding: 0 12px; border-radius: 9px; color: var(--tgf-muted); }"
        ".nav-button.active { background-color: var(--tgf-nav); color: var(--tgf-text); }"
        ".card, .connection-card { background-color: var(--tgf-card); border: 1px solid var(--tgf-border); "
        "border-radius: 12px; padding: 16px; }"
        ".connection-card { background-color: var(--tgf-raised); border-radius: 10px; padding: 12px 14px; }"
        ".log-card { background-color: var(--tgf-log); }"
        ".card-title { font-weight: 600; font-size: 14px; color: var(--tgf-text); }"
        ".page-title { font-weight: 600; font-size: 20px; color: var(--tgf-text); }"
        ".window-title { font-weight: 600; font-size: 18px; color: var(--tgf-text); }"
        ".metric-value { font-family: \"JetBrains Mono\", monospace; font-size: 28px; "
        "font-weight: 500; color: var(--tgf-text); }"
        ".mono, .log, entry, .detail-handle { font-family: \"JetBrains Mono\", monospace; }"
        ".detail-handle { font-size: 20px; font-weight: 700; color: var(--tgf-text); }"
        ".status-pill { background-color: var(--tgf-green-bg); color: var(--tgf-green); "
        "border-radius: 14px; padding: 4px 12px; font-size: 13px; font-weight: 500; }"
        ".status-pill.paused { background-color: var(--tgf-amber-bg); color: var(--tgf-amber); }"
        ".status-pill.idle { background-color: var(--tgf-nav); color: var(--tgf-secondary); }"
        ".dot-live { color: var(--tgf-green); }"
        ".dot-idle { color: var(--tgf-muted); }"
        ".table-head { color: var(--tgf-muted); font-size: 12px; }"
        ".hint { color: var(--tgf-secondary); font-size: 13px; }"
        ".error { color: var(--tgf-red); font-size: 13px; }"
        ".filter-chip { font-size: 13px; border-radius: 8px; border: 1px solid var(--tgf-border-strong); "
        "color: var(--tgf-secondary); }"
        ".filter-chip:checked, .choice:checked { background-color: var(--tgf-accent); "
        "color: var(--tgf-ink, #0b1520); }"
        ".choice, .kind-chip { font-size: 13px; border-radius: 9px; color: var(--tgf-secondary); "
        "border: 1px solid var(--tgf-border-strong); }"
        ".choice { padding: 0 14px; }"
        ".kind-chip { border-radius: 6px; }"
        ".segment-on { background-color: var(--tgf-accent); border-radius: 2px; }"
        ".segment-off { background-color: var(--tgf-border); border-radius: 2px; }"
        ".segment-group { background-color: var(--tgf-input); border: 1px solid var(--tgf-border-strong); "
        "border-radius: 11px; padding: 3px; }"
        ".segment { background-color: transparent; color: var(--tgf-secondary); border: 0; "
        "border-radius: 8px; padding: 0 16px; font-size: 14px; font-weight: 500; }"
        ".segment:checked { background-color: var(--tgf-accent); color: #0b1520; }"
        ".success-mark { color: var(--tgf-green); font-size: 32px; }"
        "button.suggested-action { background-color: var(--tgf-accent); color: #0b1520; "
        "font-weight: 600; border-radius: 9px; }"
        "button.destructive-action { color: var(--tgf-red); border: 1px solid var(--tgf-red-border); "
        "background-color: transparent; border-radius: 9px; }"
        ".link-button { background-color: transparent; border-width: 0; box-shadow: none; "
        "color: var(--tgf-accent); font-size: 13px; padding: 0; }"
        ".link-button:hover { color: color-mix(in srgb, var(--tgf-accent) 80%, white); }"
        "progressbar.backfill-bar trough { background-color: #2a3037; border-radius: 3px; min-height: 6px; }"
        "progressbar.backfill-bar progress { background-color: var(--tgf-accent); border-radius: 3px; min-height: 6px; }"
        ".backfill-pct { font-family: \"JetBrains Mono\", monospace; font-size: 12px; color: var(--tgf-muted); }"
        ".setting-row { border-bottom: 1px solid var(--tgf-border); }"
        ".form-actions { background-color: var(--tgf-raised); border-top: 1px solid var(--tgf-border); }"
        ".preview-card { background-color: var(--tgf-log); }"
        ".delay-value { font-size: 16px; }"
        ".signin-title { font-size: 24px; font-weight: 600; }"
        ".signin-entry { font-size: 16px; }"
        ".privacy-note { border-top: 1px solid var(--tgf-border); padding-top: 4px; }"
        ".btn-action { padding: 0 18px; }"
        ".btn-primary { padding: 0 22px; }"
        ".btn-full { padding: 0 14px; }"
        "entry { background-color: var(--tgf-input); color: var(--tgf-text); "
        "border: 1px solid var(--tgf-border-strong); border-radius: 9px; font-size: 14px; }"
        "list, scrolledwindow, viewport, row { background-color: transparent; }"
        "switch:checked { background-color: #6cb6ff; }"
        "switch:checked slider { background-color: #0b1520; }"
        "button:focus-visible, entry:focus-visible { outline: 2px solid var(--tgf-accent); outline-offset: 2px; }"
        "@media (prefers-reduced-motion: no-preference) { button { transition: background-color 150ms ease; } }";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static gboolean dispatch_cb(gpointer user_data) {
    GuiBridge *b = (GuiBridge *)user_data;
    tgf_event ev;
    while (tgf_next_event(b->ctx, &ev)) gui_bridge_event(b, &ev);
    return G_SOURCE_CONTINUE;
}

static void stop_core(void) {
    if (g_idle_id) { g_source_remove(g_idle_id); g_idle_id = 0; }
    if (g_core_ctx) { tgf_stop(g_core_ctx); tgf_destroy(g_core_ctx); g_core_ctx = NULL; }
    g_bridge.ctx = NULL;
}

static void on_window_destroy(GtkWindow *win, gpointer user_data) {
    (void)win;
    (void)user_data;
    stop_core();
    g_application_quit(G_APPLICATION(g_app));
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)app;
    (void)user_data;
    if (g_win) { gui_win_free(g_win); g_win = NULL; }
    stop_core();
    install_css();

    tgf_config cfg;
    if (tgf_config_load("config.json", &cfg) == TGF_OK) {
        snprintf(g_bridge.state.dest, sizeof(g_bridge.state.dest), "%s",
                 cfg.dest_channel ? cfg.dest_channel : "@me");
        g_bridge.state.sequence_direction = cfg.sequence_direction;
        g_bridge.state.history_window_hours = cfg.history_window_hours;
        g_core_ctx = tgf_create(&cfg, &transport_tdlib);
        tgf_config_free(&cfg);
    }
    if (!g_core_ctx) {
        const char *err = tgf_config_error();
        char text[512];
        snprintf(text, sizeof(text), "%s", err ? err : "Cannot load config.json");
        g_win = gui_win_new_error(app, &g_bridge, text);
        g_signal_connect(G_OBJECT(gui_win_get_window(g_win)), "destroy", G_CALLBACK(on_window_destroy), NULL);
        return;
    }
    if (tgf_start(g_core_ctx) != TGF_OK) { tgf_destroy(g_core_ctx); g_core_ctx = NULL; return; }

    g_bridge.ctx = g_core_ctx;
    g_bridge.window = NULL;
    g_bridge.page_event = NULL;
    g_win = gui_win_new(app, &g_bridge);
    g_bridge.owner = g_win;
    g_bridge.window = GTK_WINDOW(gui_win_get_window(g_win));
    g_signal_connect(G_OBJECT(g_bridge.window), "destroy", G_CALLBACK(on_window_destroy), NULL);
    gui_bridge_set_page_event(&g_bridge, gui_win_page_event);
    gtk_window_present(GTK_WINDOW(g_bridge.window));
    g_idle_id = g_timeout_add(50, (GSourceFunc)dispatch_cb, &g_bridge);
}

int main(int argc, char **argv) {
    g_setenv("GSK_RENDERER", "cairo", FALSE);
    AdwApplication *app = adw_application_new("io.github.hadi493.tgf", G_APPLICATION_DEFAULT_FLAGS);
    g_app = app;
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    g_application_run(G_APPLICATION(app), argc, argv);
    if (g_win) gui_win_free(g_win);
    stop_core();
    g_object_unref(app);
    return 0;
}
