/*
    * LLM Daemon — System tray app to manage a local LLM server process
    *
    * Features:
    * - Start/stop server with dynamic menu sensitivity
    * - HTTP readiness probe with timeout and notifications
    * - Configurable via ~/.config/llm-daemon/config.ini
    *
    * Build (Linux):
    *   gcc llm-daemon.c -o llm-daemon `pkg-config --cflags --libs gtk+-3.0 appindicator3 libnotify` -lcurl
    *
    * Usage:
    *   ./llm-daemon
    *
    * Notes:
    * - Designed for Linux with AppIndicator; may require adjustments for other platforms.
    * - Ensure the specified server binary and model path are correct in the config.
*/

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <libnotify/notify.h>
#include <unistd.h>

/* ── Default configuration (overridden by config.ini) ──────────────────────── */

#define DEFAULT_BIN_DIR "/home/USERNAME/Projects/LLAMA-CPP/bin" 

#define DEFAULT_SERVER   DEFAULT_BIN_DIR "/llama-server"

// Enter model name if you wish to specify a default, or leave as-is to require user configuration
#define DEFAULT_MODEL \
    DEFAULT_BIN_DIR "/models/" \
    "MODEL-NAME.gguf"

#define DEFAULT_HOST          "127.0.0.1" // Change to 0.0.0.0 if you want external access
#define DEFAULT_PORT          8080
#define DEFAULT_CTX_SIZE      "0"

#define POLL_INTERVAL_MS      2000   /* ms between HTTP readiness probes  */
#define POLL_MAX_CHECKS       50     /* 50 × 2 s ≈ 100 s total timeout   */

#define APP_NAME              "LLM Daemon"
#define CONFIG_GROUP          "server"

/* ── Runtime config (populated from GKeyFile at startup) ──────────────────── */

typedef struct {
    gchar   *bin_dir;
    gchar   *server_bin;
    gchar   *model_path;
    gchar   *host;
    gint     port;
    gchar   *ctx_size;
} AppConfig;

static AppConfig cfg;

/* ── App state ──────────────────────────────────────────────────────────────── */

/* Service lifecycle state — drives menu sensitivity */
typedef enum {
    STATE_STOPPED  = 0,   /* nothing running                  */
    STATE_STARTING,       /* spawned, HTTP probe not yet OK   */
    STATE_RUNNING,        /* HTTP probe returned 200          */
    STATE_STOPPING,       /* SIGTERM sent, waiting for exit   */
} ServiceState;

static GMainLoop    *main_loop    = NULL;
static AppIndicator *indicator    = NULL;
static GPid          child_pid    = 0;
static guint         poll_timer   = 0;
static gint          poll_count   = 0;
static ServiceState  svc_state    = STATE_STOPPED;

/* Weak references to menu items so we can toggle sensitivity */
static GtkWidget    *mi_start     = NULL;
static GtkWidget    *mi_stop      = NULL;

/* ── Forward declarations ───────────────────────────────────────────────────── */

static void set_state(ServiceState new_state);
static void rebuild_menu(GtkWidget *menu, gpointer data);

/* ══════════════════════════════════════════════════════════════════════════════
 * 2. CONFIG  — GKeyFile, ~/.config/llm-daemon/config.ini
 * ════════════════════════════════════════════════════════════════════════════ */

static gchar *config_path(void)
{
    return g_build_filename(g_get_home_dir(), ".config", "llm-daemon", "config.ini", NULL);
}

static void config_load(void)
{
    GKeyFile *kf   = g_key_file_new();
    gchar    *path = config_path();
    GError   *err  = NULL;

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_printerr("Config load warning: %s\n", err->message);
        g_clear_error(&err);
    }

#define GETSTR(key, def) \
    g_key_file_has_key(kf, CONFIG_GROUP, key, NULL) \
        ? g_key_file_get_string(kf, CONFIG_GROUP, key, NULL) \
        : g_strdup(def)

    cfg.bin_dir    = GETSTR("bin_dir",    DEFAULT_BIN_DIR);
    cfg.server_bin = GETSTR("server_bin", DEFAULT_SERVER);
    cfg.model_path = GETSTR("model_path", DEFAULT_MODEL);
    cfg.host       = GETSTR("host",       DEFAULT_HOST);
    cfg.ctx_size   = GETSTR("ctx_size",   DEFAULT_CTX_SIZE);
    cfg.port       = g_key_file_has_key(kf, CONFIG_GROUP, "port", NULL)
                     ? g_key_file_get_integer(kf, CONFIG_GROUP, "port", NULL)
                     : DEFAULT_PORT;
#undef GETSTR

    g_key_file_free(kf);
    g_free(path);
}

static void config_save(void)
{
    GKeyFile *kf   = g_key_file_new();
    gchar    *dir  = g_build_filename(g_get_home_dir(), ".config", "llm-daemon", NULL);
    gchar    *path = config_path();

    g_key_file_set_string (kf, CONFIG_GROUP, "bin_dir",    cfg.bin_dir);
    g_key_file_set_string (kf, CONFIG_GROUP, "server_bin", cfg.server_bin);
    g_key_file_set_string (kf, CONFIG_GROUP, "model_path", cfg.model_path);
    g_key_file_set_string (kf, CONFIG_GROUP, "host",       cfg.host);
    g_key_file_set_integer(kf, CONFIG_GROUP, "port",       cfg.port);
    g_key_file_set_string (kf, CONFIG_GROUP, "ctx_size",   cfg.ctx_size);

    g_mkdir_with_parents(dir, 0755);
    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, path, &err)) {
        g_printerr("Config save failed: %s\n", err->message);
        g_clear_error(&err);
    }

    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

/* ── Settings dialog ──────────────────────────────────────────────────────── */

static void on_settings_response(GtkDialog *dlg, gint resp, gpointer data)
{
    (void)data;
    if (resp == GTK_RESPONSE_ACCEPT) {
        g_free(cfg.bin_dir);
        g_free(cfg.server_bin);
        g_free(cfg.model_path);
        g_free(cfg.host);
        g_free(cfg.ctx_size);

        cfg.bin_dir    = g_strdup(gtk_entry_get_text(
                            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "bin_dir"))));
        cfg.server_bin = g_strdup(gtk_entry_get_text(
                            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "server_bin"))));
        cfg.model_path = g_strdup(gtk_entry_get_text(
                            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "model_path"))));
        cfg.host       = g_strdup(gtk_entry_get_text(
                            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "host"))));
        cfg.ctx_size   = g_strdup(gtk_entry_get_text(
                            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "ctx_size"))));
        cfg.port       = (gint)gtk_spin_button_get_value(
                            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "port")));
        config_save();
    }
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

static GtkWidget *add_entry_row(GtkWidget *grid, int row,
                                const gchar *label, const gchar *value,
                                const gchar *key, GtkWidget *dlg)
{
    GtkWidget *lbl = gtk_label_new(label);
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), value ? value : "");
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 55);
    gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
    g_object_set_data(G_OBJECT(dlg), key, entry);
    return entry;
}

static void on_settings_activate(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "LLM Daemon — Settings", NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Save",   GTK_RESPONSE_ACCEPT,
        NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing   (GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

    add_entry_row(grid, 0, "Library dir:",   cfg.bin_dir,    "bin_dir",    dlg);
    add_entry_row(grid, 1, "Server binary:", cfg.server_bin, "server_bin", dlg);
    add_entry_row(grid, 2, "Model path:",    cfg.model_path, "model_path", dlg);
    add_entry_row(grid, 3, "Host:",          cfg.host,       "host",       dlg);
    add_entry_row(grid, 4, "Context size:",  cfg.ctx_size,   "ctx_size",   dlg);

    /* Port spinner */
    GtkWidget *port_lbl = gtk_label_new("Port:");
    gtk_widget_set_halign(port_lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), port_lbl, 0, 5, 1, 1);
    GtkWidget *spin = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), cfg.port);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, 5, 1, 1);
    g_object_set_data(G_OBJECT(dlg), "port", spin);

    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                       grid, TRUE, TRUE, 0);
    g_signal_connect(dlg, "response", G_CALLBACK(on_settings_response), NULL);
    gtk_widget_show_all(dlg);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 4. NOTIFICATIONS  — libnotify
 * ════════════════════════════════════════════════════════════════════════════ */

static void notify_send(const gchar *summary, const gchar *body,
                        NotifyUrgency urgency)
{
    NotifyNotification *n = notify_notification_new(summary, body, NULL);
    notify_notification_set_urgency(n, urgency);
    notify_notification_set_timeout(n, 4000);
    GError *err = NULL;
    if (!notify_notification_show(n, &err)) {
        g_printerr("Notification error: %s\n", err->message);
        g_clear_error(&err);
    }
    g_object_unref(n);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Icon & state machine
 * ════════════════════════════════════════════════════════════════════════════ */

static void update_icon(void)
{
    if (!indicator) return;
    const gchar *icon;
    switch (svc_state) {
        case STATE_RUNNING:  icon = "emblem-ok";              break;
        case STATE_STARTING: /* fall-through */
        case STATE_STOPPING: icon = "emblem-synchronizing";   break;
        default:             icon = "emblem-readonly";        break;
    }
    app_indicator_set_icon(indicator, icon);
}

/* ── 3. Dynamic menu sensitivity ─────────────────────────────────────────── */

static void update_menu_sensitivity(void)
{
    if (!mi_start || !mi_stop) return;
    gtk_widget_set_sensitive(mi_start, svc_state == STATE_STOPPED);
    gtk_widget_set_sensitive(mi_stop,
        svc_state == STATE_RUNNING || svc_state == STATE_STARTING);
}

static void set_state(ServiceState new_state)
{
    svc_state = new_state;
    update_icon();
    update_menu_sensitivity();
}

/* ── Signal handler (SIGINT/SIGTERM to the tray app itself) ─────────────── */

static void on_unix_signal(int signum)
{
    (void)signum;
    if (main_loop) g_main_loop_quit(main_loop);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * 1. CHILD WATCH  — g_child_watch_add()
 * ════════════════════════════════════════════════════════════════════════════ */

static void on_child_exit(GPid pid, gint status, gpointer data)
{
    (void)data;

    if (poll_timer) {
        g_source_remove(poll_timer);
        poll_timer = 0;
    }

    gboolean was_running = (svc_state == STATE_RUNNING);

    g_spawn_close_pid(pid);   /* reap zombie, free GPid resources */
    child_pid  = 0;
    poll_count = 0;
    set_state(STATE_STOPPED);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        g_print("  Child exited with code %d\n", code);
        if (code != 0 && was_running)
            notify_send("LLM Server Crashed",
                        "The server exited unexpectedly. Check logs.",
                        NOTIFY_URGENCY_CRITICAL);
    } else if (WIFSIGNALED(status)) {
        g_print("  Child killed by signal %d\n", WTERMSIG(status));
        if (was_running)
            notify_send("LLM Server Stopped",
                        "The server was terminated by a signal.",
                        NOTIFY_URGENCY_NORMAL);
    }
}

/* ── stop_child: sends SIGTERM; on_child_exit handles cleanup ─────────────── */

static void stop_child(void)
{
    if (poll_timer) {
        g_source_remove(poll_timer);
        poll_timer = 0;
    }
    if (child_pid == 0) return;

    set_state(STATE_STOPPING);
    g_print("  Sending SIGTERM to PID %d\n", child_pid);

    if (kill(child_pid, SIGTERM) != 0) {
        g_printerr("  SIGTERM failed: %s — force-killing\n", strerror(errno));
        kill(child_pid, SIGKILL);
        g_spawn_close_pid(child_pid);
        child_pid = 0;
        set_state(STATE_STOPPED);
    }
    /* Normal path: on_child_exit() fires asynchronously and does the rest */
}

/* ══════════════════════════════════════════════════════════════════════════════
 * HTTP readiness poll
 * ════════════════════════════════════════════════════════════════════════════ */

static gboolean check_readiness(gpointer user_data)
{
    (void)user_data;

    gchar *url = g_strdup_printf("http://%s:%d/v1/models", cfg.host, cfg.port);

    CURL *curl = curl_easy_init();
    CURLcode rc = CURLE_FAILED_INIT;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL,            url);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,        1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);
        curl_easy_setopt(curl, CURLOPT_NOBODY,         1L); /* HEAD */
        rc = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    g_free(url);

    if (rc == CURLE_OK) {
        g_print("  ✅ Service ready\n");
        poll_timer = 0;
        poll_count = 0;
        set_state(STATE_RUNNING);
        /* 4. Ready notification */
        notify_send("LLM Server Ready",
                    "The server is up and accepting requests.",
                    NOTIFY_URGENCY_LOW);
        return G_SOURCE_REMOVE;
    }

    if (++poll_count >= POLL_MAX_CHECKS) {
        g_printerr("  ❌ Timed out waiting for service\n");
        notify_send("LLM Server Timeout",
                    "Server did not become ready within the timeout.",
                    NOTIFY_URGENCY_CRITICAL);
        stop_child();
        poll_timer = 0;
        return G_SOURCE_REMOVE;
    }

    g_print("  ⏳ Waiting for service… (%d/%d)\n", poll_count, POLL_MAX_CHECKS);
    return G_SOURCE_CONTINUE;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Menu callbacks
 * ════════════════════════════════════════════════════════════════════════════ */

static void on_start(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    if (child_pid != 0) return;

    gchar *port_str = g_strdup_printf("%d", cfg.port);
    gchar *ld_path  = g_strdup_printf("LD_LIBRARY_PATH=%s", cfg.bin_dir);

    const gchar *argv[] = {
        cfg.server_bin,
        "-m",     cfg.model_path,
        "--host", cfg.host,
        "--port", port_str,
        "-c",     cfg.ctx_size,
        NULL
    };
    gchar *envp[] = { ld_path, NULL };

    GPid    new_pid = 0;
    GError *err     = NULL;

    gboolean ok = g_spawn_async(
        NULL, (gchar **)argv, envp,
        G_SPAWN_DO_NOT_REAP_CHILD |   /* 1. we reap via g_child_watch_add */
        G_SPAWN_STDOUT_TO_DEV_NULL |
        G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, &new_pid, &err);

    g_free(port_str);
    g_free(ld_path);

    if (!ok) {
        g_printerr("  ❌ Launch failed: %s\n", err ? err->message : "unknown");
        g_clear_error(&err);
        return;
    }

    child_pid  = new_pid;
    poll_count = 0;
    set_state(STATE_STARTING);
    g_print("  ▶ Started (PID %d) — polling http://%s:%d/v1/models\n",
            child_pid, cfg.host, cfg.port);

    /* 1. Child watch — instant notification when process exits */
    g_child_watch_add(child_pid, on_child_exit, NULL);

    /* HTTP readiness poll */
    poll_timer = g_timeout_add(POLL_INTERVAL_MS, check_readiness, NULL);
}

static void on_stop(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    if (child_pid == 0) return;
    g_print("⏹ Stop requested\n");
    stop_child();
}

static void on_quit(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    g_print("🚪 Quit\n");
    stop_child();
    g_main_loop_quit(main_loop);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Menu builder
 * ════════════════════════════════════════════════════════════════════════════ */

static void rebuild_menu(GtkWidget *menu, gpointer data)
{
    (void)data;

    mi_start = mi_stop = NULL;   /* pointers about to be destroyed */

    GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    mi_start = gtk_menu_item_new_with_label("Start");
    g_signal_connect(mi_start, "activate", G_CALLBACK(on_start), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_start);

    mi_stop = gtk_menu_item_new_with_label("Stop");
    g_signal_connect(mi_stop, "activate", G_CALLBACK(on_stop), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_stop);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *mi_settings = gtk_menu_item_new_with_label("Settings…");
    g_signal_connect(mi_settings, "activate", G_CALLBACK(on_settings_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_settings);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *mi_quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(mi_quit, "activate", G_CALLBACK(on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_quit);

    gtk_widget_show_all(menu);

    /* 3. Reflect current state immediately */
    update_menu_sensitivity();
}

/* ══════════════════════════════════════════════════════════════════════════════
 * Entry point
 * ════════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    gtk_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* 4. Init libnotify */
    notify_init(APP_NAME);

    /* 2. Load config (uses compile-time defaults if file absent) */
    config_load();

    indicator = app_indicator_new(
        "com.example.myllm-daemon",
        "dialog-information",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);

    if (!indicator) {
        g_printerr("Failed to create AppIndicator.\n");
        return EXIT_FAILURE;
    }

    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(indicator, APP_NAME);
    update_icon();

    GtkWidget *menu = gtk_menu_new();
    g_signal_connect(menu, "deactivate", G_CALLBACK(rebuild_menu), NULL);
    rebuild_menu(menu, NULL);
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    signal(SIGINT,  on_unix_signal);
    signal(SIGTERM, on_unix_signal);

    main_loop = g_main_loop_new(NULL, FALSE);
    g_print("%s active — server will run on http://%s:%d\n",
            APP_NAME, cfg.host, cfg.port);
    g_main_loop_run(main_loop);

    if (child_pid) stop_child();
    g_object_unref(indicator);
    notify_uninit();
    curl_global_cleanup();

    g_free(cfg.bin_dir);
    g_free(cfg.server_bin);
    g_free(cfg.model_path);
    g_free(cfg.host);
    g_free(cfg.ctx_size);

    return EXIT_SUCCESS;
}