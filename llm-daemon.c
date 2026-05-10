/*
 * llm-daemon.c — System tray manager for llama-server (v1.0)
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdatomic.h>

#include <cairo/cairo.h>
#include <curl/curl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <libnotify/notify.h>

/* Constants */
#define APP_NAME              "LLM Tray"
#define APP_ID                "com.example.llmtray"
#define LOCK_FILE             "/tmp/llmtray.lock"
#define CONFIG_DIR_NAME       "llm-daemon"
#define CONFIG_FILE_NAME      "config.ini"
#define AUTOSTART_FILE_NAME   "llm-daemon.desktop"
#define DEFAULT_PROFILE       "default"
#define CONFIG_VERSION        1

#define DEFAULT_BIN_DIR \
    "/home/gilgamesh/Projects/AI Tests/" \
    "llama-b9033-bin-ubuntu-vulkan-x64/llama-b9033/bin"
#define DEFAULT_SERVER        DEFAULT_BIN_DIR "/llama-server"
#define DEFAULT_MODEL \
    DEFAULT_BIN_DIR "/models/" \
    "Qwen3.6-35B-A3B-Uncensored-HauhauCS-Aggressive-Q6_K_P.gguf"
#define DEFAULT_HOST          "127.0.0.1"
#define DEFAULT_PORT          8080
#define DEFAULT_CTX_SIZE      "0"

#define POLL_INTERVAL_MS      2000
#define POLL_MAX_CHECKS       50
#define SIGKILL_DELAY_MS      8000
#define METRICS_INTERVAL_MS   30000
#define LOG_MAX_LINES         2000
#define LOG_MAX_BYTES         (5 * 1024 * 1024)
#define RESTART_BACKOFF_BASE  1000
#define RESTART_BACKOFF_MAX   64000
#define RESTART_STABLE_SECS   60
#define BADGE_SIZE            22

/* State machine */
typedef enum {
    STATE_STOPPED  = 0,
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPING,
    STATE_COUNT
} ServiceState;

static const gboolean VALID_TX[STATE_COUNT][STATE_COUNT] = {
    /* STOPPED  */ { FALSE,  TRUE,    FALSE,   FALSE },
    /* STARTING */ { TRUE,   FALSE,   TRUE,    TRUE  },
    /* RUNNING  */ { FALSE,  FALSE,   FALSE,   TRUE  },
    /* STOPPING */ { TRUE,   FALSE,   FALSE,   FALSE },
};

static const gchar *STATE_NAME[] = { "STOPPED","STARTING","RUNNING","STOPPING" };

/* Types */
typedef struct {
    gchar *name, *bin_dir, *server_bin, *model_path, *host, *ctx_size;
    gint   port;
} Profile;

typedef struct {
    gchar    *url;
    gboolean  is_metrics;
    guint     generation;
    gchar    *host_copy;
    gint      port_copy;
} CurlTask;

typedef struct {
    gboolean  success;
    gboolean  is_metrics;
    gdouble   tps;
    guint     generation;
} CurlResult;

/* Globals */
static GMainLoop    *main_loop       = NULL;
static AppIndicator *indicator       = NULL;
static GPid          child_pid       = 0;
static guint         poll_timer      = 0;
static guint         sigkill_timer   = 0;
static guint         metrics_timer   = 0;
static gint          poll_count      = 0;
static ServiceState  svc_state       = STATE_STOPPED;
static GDateTime    *started_at      = NULL;
static GDateTime    *last_stable_at  = NULL;
static gboolean      auto_restart    = FALSE;
static gboolean      pending_restart = FALSE;
static _Atomic guint spawn_gen       = 0;
static gint          restart_count   = 0;
static guint         restart_backoff = RESTART_BACKOFF_BASE;
static gdouble       last_tps        = -1.0;
static guint         uptime_timer    = 0;

static GList   *profiles       = NULL;
static Profile *active_profile = NULL;

static GtkWidget  *mi_status   = NULL;
static GtkWidget  *mi_start    = NULL;
static GtkWidget  *mi_stop     = NULL;
static GtkWidget  *mi_restart  = NULL;

static GtkWidget  *log_window   = NULL;
static GtkWidget  *log_textview = NULL;
static GIOChannel *log_ch_out   = NULL;
static GIOChannel *log_ch_err   = NULL;
static guint       log_wo       = 0;
static guint       log_we       = 0;
static FILE       *log_fp       = NULL;
static GMutex      log_mutex;
static _Atomic gboolean log_widget_destroyed = FALSE;
static _Atomic gboolean shutting_down = FALSE;

/* ✅ FIX: Self-pipe trick for async-signal-safety */
static int signal_pipe[2] = {-1, -1};

/* Forward declarations */
static void set_state(ServiceState s);
static void rebuild_menu(GtkWidget *menu, gpointer data);
static void do_start(void);
static gboolean do_start_wrapper(gpointer data);
static void stop_child(void);
static void on_show_log(GtkMenuItem *item, gpointer data);
static void log_append(const gchar *text);
static void safe_log_channels_close(void);
static gboolean start_metrics_idle_final(gpointer d);
static void update_menu_sensitivity(void);
static void update_icon(void);
static gboolean port_free(const gchar *host, gint port);
static gboolean on_signal_pipe_io(GIOChannel *ch, GIOCondition cond, gpointer data);
static void on_unix_signal_handler(int sig);
static gboolean log_append_main_thread(gpointer data);

/* Lock file */
static gboolean lock_acquire(void)
{
    int fd = open(LOCK_FILE, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) {
        if (errno != EEXIST) {
            g_printerr("Cannot create lock file: %s\n", strerror(errno));
            return FALSE;
        }

        FILE *f = fopen(LOCK_FILE, "r");
        if (!f) return FALSE;

        pid_t pid = 0;
        gboolean alive = (fscanf(f, "%d", &pid) == 1 &&
                          pid > 0 && kill(pid, 0) == 0);
        fclose(f);

        if (alive) {
            g_printerr("%s already running (PID %d).\n", APP_NAME, pid);
            return FALSE;
        }

        /* ✅ FIX: Remove stale lock before retrying */
        unlink(LOCK_FILE);

        fd = open(LOCK_FILE, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0) {
            g_printerr("Cannot create lock file: %s\n", strerror(errno));
            return FALSE;
        }
    }

    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        g_printerr("snprintf failed: %s\n", strerror(errno));
        close(fd);
        unlink(LOCK_FILE);
        return FALSE;
    }
    if ((size_t)n != write(fd, buf, (size_t)n)) {
        g_printerr("Failed to write PID: %s\n", strerror(errno));
        close(fd);
        unlink(LOCK_FILE);
        return FALSE;
    }
    close(fd);
    return TRUE;
}

static void lock_release(void) { unlink(LOCK_FILE); }

/* Config paths */
static gchar *cfg_dir(void)
{
    return g_build_filename(g_get_home_dir(), ".config", CONFIG_DIR_NAME, NULL);
}
static gchar *cfg_path(void)
{
    return g_build_filename(g_get_home_dir(), ".config", CONFIG_DIR_NAME,
                            CONFIG_FILE_NAME, NULL);
}

/* Schema migration */
static void config_migrate(GKeyFile *kf, gint from)
{
    if (from < 1) {
        g_key_file_set_integer(kf, "llm-daemon", "version", 1);
        g_print("Config: migrated v0→v1\n");
    }
}

/* Profile */
static Profile *profile_defaults(const gchar *name)
{
    Profile *p = g_new0(Profile, 1);
    p->name       = g_strdup(name);
    p->bin_dir    = g_strdup(DEFAULT_BIN_DIR);
    p->server_bin = g_strdup(DEFAULT_SERVER);
    p->model_path = g_strdup(DEFAULT_MODEL);
    p->host       = g_strdup(DEFAULT_HOST);
    p->port       = DEFAULT_PORT;
    p->ctx_size   = g_strdup(DEFAULT_CTX_SIZE);
    return p;
}

static void profile_free(Profile *p)
{
    if (!p) return;
    g_free(p->name); g_free(p->bin_dir); g_free(p->server_bin);
    g_free(p->model_path); g_free(p->host); g_free(p->ctx_size);
    g_free(p);
}

#define KFS(kf,g,k,d) (g_key_file_has_key(kf,g,k,NULL) \
                        ? g_key_file_get_string(kf,g,k,NULL) : g_strdup(d))

/* Profiles */
static void profiles_load(void)
{
    g_list_free_full(profiles, (GDestroyNotify)profile_free);
    profiles = NULL;

    GKeyFile *kf  = g_key_file_new();
    gchar    *path = cfg_path();
    GError   *err  = NULL;

    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err)) {
        if (!g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_printerr("Config: %s\n", err->message);
        g_clear_error(&err);
    }
    g_free(path);

    gint ver = g_key_file_has_key(kf, "llm-daemon", "version", NULL)
               ? g_key_file_get_integer(kf, "llm-daemon", "version", NULL) : 0;
    if (ver < CONFIG_VERSION) config_migrate(kf, ver);

    gsize   ng = 0;
    gchar **gs = g_key_file_get_groups(kf, &ng);
    for (gsize i = 0; i < ng; i++) {
        if (g_strcmp0(gs[i], "llm-daemon") == 0) continue;
        const gchar *g = gs[i];
        Profile *p = g_new0(Profile, 1);
        p->name       = g_strdup(g);
        p->bin_dir    = KFS(kf, g, "bin_dir",    DEFAULT_BIN_DIR);
        p->server_bin = KFS(kf, g, "server_bin", DEFAULT_SERVER);
        p->model_path = KFS(kf, g, "model_path", DEFAULT_MODEL);
        p->host       = KFS(kf, g, "host",        DEFAULT_HOST);
        p->ctx_size   = KFS(kf, g, "ctx_size",   DEFAULT_CTX_SIZE);
        p->port       = g_key_file_has_key(kf, g, "port", NULL)
                        ? g_key_file_get_integer(kf, g, "port", NULL)
                        : DEFAULT_PORT;
        profiles = g_list_append(profiles, p);
    }
    g_strfreev(gs);

    if (!profiles)
        profiles = g_list_append(profiles, profile_defaults(DEFAULT_PROFILE));

    gchar *aname = KFS(kf, "llm-daemon", "active_profile", DEFAULT_PROFILE);
    active_profile = NULL;
    for (GList *l = profiles; l; l = l->next) {
        Profile *p = l->data;
        if (g_strcmp0(p->name, aname) == 0) { active_profile = p; break; }
    }
    if (!active_profile) active_profile = profiles->data;
    g_free(aname);
    g_key_file_free(kf);
}

static void profiles_save(void)
{
    GKeyFile *kf  = g_key_file_new();
    gchar    *dir = cfg_dir();
    gchar    *path = cfg_path();

    g_key_file_set_integer(kf, "llm-daemon", "version", CONFIG_VERSION);
    if (active_profile)
        g_key_file_set_string(kf, "llm-daemon", "active_profile",
                              active_profile->name);
    for (GList *l = profiles; l; l = l->next) {
        Profile *p = l->data;
        g_key_file_set_string (kf, p->name, "bin_dir",    p->bin_dir);
        g_key_file_set_string (kf, p->name, "server_bin", p->server_bin);
        g_key_file_set_string (kf, p->name, "model_path", p->model_path);
        g_key_file_set_string (kf, p->name, "host",       p->host);
        g_key_file_set_integer(kf, p->name, "port",       p->port);
        g_key_file_set_string (kf, p->name, "ctx_size",   p->ctx_size);
    }
    g_mkdir_with_parents(dir, 0755);
    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, path, &err)) {
        g_printerr("Config save: %s\n", err->message);
        g_clear_error(&err);
    }
    g_key_file_free(kf); g_free(dir); g_free(path);
}

/* Settings dialog */
static GtkWidget *add_row(GtkWidget *grid, int row, const gchar *lbl,
                           const gchar *val, const gchar *key, GtkWidget *dlg)
{
    GtkWidget *l = gtk_label_new(lbl);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), val ? val : "");
    gtk_entry_set_width_chars(GTK_ENTRY(e), 55);
    gtk_grid_attach(GTK_GRID(grid), e, 1, row, 1, 1);
    if (key && dlg) g_object_set_data(G_OBJECT(dlg), key, e);
    return e;
}

static void on_settings_response(GtkDialog *dlg, gint resp, gpointer data)
{
    (void)data;
    if (resp == GTK_RESPONSE_ACCEPT && active_profile) {
        Profile *p = active_profile;

        g_free(p->bin_dir); g_free(p->server_bin); g_free(p->model_path);
        g_free(p->host);    g_free(p->ctx_size);

#define GE(k) gtk_entry_get_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dlg),k)))
        p->bin_dir    = g_strdup(GE("bin_dir"));
        p->server_bin = g_strdup(GE("server_bin"));
        p->model_path = g_strdup(GE("model_path"));
        p->host       = g_strdup(GE("host"));
        p->ctx_size   = g_strdup(GE("ctx_size"));

        gint port = (gint)gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "port")));
        if (port < 1 || port > 65535) {
            GtkWidget *err_dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "Invalid port: must be 1–65535");
            gtk_dialog_run(GTK_DIALOG(err_dlg));
            gtk_widget_destroy(err_dlg);

            g_free(p->host);
            g_free(p->ctx_size);
            g_object_set_data(G_OBJECT(dlg), "port", NULL);
            gtk_widget_destroy(GTK_WIDGET(dlg));
            return;
        }
        p->port = port;
#undef GE

        profiles_save();
    }
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void on_settings_activate(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    if (!active_profile) return;
    Profile *p = active_profile;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Settings", NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    add_row(grid, 0, "Profile:",       p->name,       NULL,         NULL);
    add_row(grid, 1, "Library dir:",   p->bin_dir,    "bin_dir",    dlg);
    add_row(grid, 2, "Server binary:", p->server_bin, "server_bin", dlg);
    add_row(grid, 3, "Model path:",    p->model_path, "model_path", dlg);
    add_row(grid, 4, "Host:",          p->host,       "host",       dlg);
    add_row(grid, 5, "Context size:",  p->ctx_size,   "ctx_size",   dlg);
    GtkWidget *pl = gtk_label_new("Port:");
    gtk_widget_set_halign(pl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), pl, 0, 6, 1, 1);
    GtkWidget *spin = gtk_spin_button_new_with_range(1, 65535, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), p->port);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, 6, 1, 1);
    g_object_set_data(G_OBJECT(dlg), "port", spin);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                       grid, TRUE, TRUE, 0);
    g_signal_connect(dlg, "response", G_CALLBACK(on_settings_response), NULL);
    gtk_widget_show_all(dlg);
}

/* Autostart */
static gchar *autostart_path(void)
{
    return g_build_filename(g_get_home_dir(), ".config", "autostart",
                            AUTOSTART_FILE_NAME, NULL);
}
static gboolean autostart_enabled(void)
{
    gchar *p = autostart_path();
    gboolean e = g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p); return e;
}
static void autostart_enable(void)
{
    char buf[4096] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    gchar *exe;

    if (n > 0 && buf[0] == '/') {
        exe = g_strdup(buf);
    } else {
        const gchar *prg = g_get_prgname();
        if (prg && prg[0] == '/') {
            exe = g_strdup(prg);
        } else {
            exe = g_find_program_in_path("llm-daemon");
            if (!exe) {
                g_printerr("Warning: Could not find full path to binary. Using 'llm-daemon'.\n");
                exe = g_strdup("llm-daemon");
            }
        }
    }

    /* ✅ FIX: Validate executable exists before writing .desktop */
    if (!g_file_test(exe, G_FILE_TEST_IS_EXECUTABLE)) {
        g_printerr("Autostart: Binary not found or not executable: %s\n", exe);
        g_free(exe);
        return;
    }

    gchar *dir  = g_build_filename(g_get_home_dir(), ".config", "autostart", NULL);
    gchar *path = autostart_path();
    gchar *c = g_strdup_printf(
        "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n"
        "Hidden=false\nNoDisplay=false\nX-GNOME-Autostart-enabled=true\n",
        APP_NAME, exe);
    g_mkdir_with_parents(dir, 0755);
    GError *err = NULL;
    if (!g_file_set_contents(path, c, -1, &err)) {
        g_printerr("Autostart: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    }
    g_free(c); g_free(exe); g_free(dir); g_free(path);
}

static void autostart_disable(void)
{
    gchar *p = autostart_path();
    if (unlink(p) != 0 && errno != ENOENT)
        g_printerr("Remove autostart: %s\n", strerror(errno));
    g_free(p);
}
static void on_autostart_toggled(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    gtk_check_menu_item_get_active(item) ? autostart_enable()
                                         : autostart_disable();
}

/* Notifications */
static void notify_send(const gchar *sum, const gchar *body, NotifyUrgency u)
{
    NotifyNotification *n = notify_notification_new(sum, body, NULL);
    notify_notification_set_urgency(n, u);
    notify_notification_set_timeout(n, 4000);
    GError *err = NULL;
    if (!notify_notification_show(n, &err)) {
        g_printerr("Notify: %s\n", err->message); g_clear_error(&err);
    }
    g_object_unref(n);
}

/* Persistent log */
static gchar *log_fpath(void)
{
    return g_build_filename(g_get_home_dir(), ".local", "share",
                            CONFIG_DIR_NAME, "server.log", NULL);
}

static void log_open(void)
{
    g_mutex_lock(&log_mutex);
    if (log_fp) {
        g_printerr("Log: already open\n");
        g_mutex_unlock(&log_mutex);
        return;
    }

    gchar *path = log_fpath();
    gchar *dir  = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);

    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        gchar *old = g_strdup_printf("%s.1", path);
        rename(path, old);
        g_free(old);
    }

    log_fp = fopen(path, "a");
    if (!log_fp) g_printerr("Log open: %s\n", strerror(errno));
    g_free(path);
    g_mutex_unlock(&log_mutex);
}

static void log_close(void)
{
    g_mutex_lock(&log_mutex);
    if (log_fp) { fclose(log_fp); log_fp = NULL; }
    g_mutex_unlock(&log_mutex);
}

static void log_append(const gchar *text)
{
    gchar *sanitized = NULL;
    if (!g_utf8_validate(text, -1, NULL)) {
        const gchar *end;
        gchar *tmp = g_strdup(text);
        for (gchar *p = tmp; *p; ) {
            if (g_utf8_validate(p, -1, &end)) break;
            *p = '?';
            p++;
        }
        sanitized = g_utf8_make_valid(tmp, -1);
        g_free(tmp);
    } else {
        sanitized = g_strdup(text);
    }

    g_mutex_lock(&log_mutex);
    if (log_fp) {
        fputs(sanitized, log_fp);
        fflush(log_fp);
    }
    g_mutex_unlock(&log_mutex);

    // ✅ FIX: Use atomic load with memory_order_acquire
    if (!atomic_load_explicit(&log_widget_destroyed, memory_order_acquire) && log_textview) {
        /* ✅ FIX: Unlock mutex *before* GTK calls to prevent deadlock/reentrancy */
        /* Marshaled to main thread via idle to avoid GTK thread violation */
        g_idle_add((GSourceFunc)log_append_main_thread, g_strdup(sanitized));
    }
    g_free(sanitized);
}

/* New main-thread-only helper */
static gboolean log_append_main_thread(gpointer data)
{
    gchar *text = data;
    if (!log_textview || atomic_load_explicit(&log_widget_destroyed, memory_order_acquire)) {
        g_free(text);
        return G_SOURCE_REMOVE;
    }

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_textview));
    if (!buf) {
        g_free(text);
        return G_SOURCE_REMOVE;
    }

    gint lines = gtk_text_buffer_get_line_count(buf);
    if (lines > LOG_MAX_LINES) {
        GtkTextIter s, t;
        gtk_text_buffer_get_start_iter(buf, &s);
        gtk_text_buffer_get_iter_at_line(buf, &t, lines - LOG_MAX_LINES);
        gtk_text_buffer_delete(buf, &s, &t);
    }
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, text, -1);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(log_textview),
        gtk_text_buffer_get_mark(buf, "insert"));
    g_free(text);
    return G_SOURCE_REMOVE;
}

static gboolean on_log_io(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    (void)data;

    while (TRUE) {
        gchar *line = NULL;
        gsize len = 0;
        GError *err = NULL;
        GIOStatus st = g_io_channel_read_line(ch, &line, &len, NULL, &err);

        if (st == G_IO_STATUS_NORMAL && line) {
            log_append(line);
            g_free(line);
        } else {
            g_clear_error(&err);
            if (st == G_IO_STATUS_EOF || st == G_IO_STATUS_ERROR) {
                return G_SOURCE_REMOVE;
            } else if (st == G_IO_STATUS_AGAIN) {
                return G_SOURCE_CONTINUE;
            }
            /* If G_IO_HUP (channel closed), break and remove source next time */
            break;
        }
    }
    return G_SOURCE_REMOVE;  // ✅ FIX: Only remove on EOF/error or HUP + no more lines
}

static gboolean update_uptime(gpointer data)
{
    (void)data;
    if (svc_state == STATE_RUNNING) {
        update_menu_sensitivity();
    }
    return G_SOURCE_CONTINUE;
}

static void safe_log_channels_close(void)
{
    atomic_store_explicit(&log_widget_destroyed, TRUE, memory_order_release);

    if (log_wo) {
        g_source_remove(log_wo);
        log_wo = 0;
    }
    if (log_we) {
        g_source_remove(log_we);
        log_we = 0;
    }

    if (log_ch_out) {
        g_io_channel_unref(log_ch_out);
        log_ch_out = NULL;
    }
    if (log_ch_err) {
        g_io_channel_unref(log_ch_err);
        log_ch_err = NULL;
    }
}

static void on_copy_log(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    if (!log_textview) return;

    if (atomic_load_explicit(&log_widget_destroyed, memory_order_acquire)) return;

    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_textview));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(buf, &s, &e);
    gchar *txt = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), txt, -1);
    g_free(txt);
}

static void on_log_destroy(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    log_window = log_textview = NULL;
    atomic_store_explicit(&log_widget_destroyed, TRUE, memory_order_release);
}

static void on_accel_log(GtkAccelGroup *ag, GObject *obj,
                          guint key, GdkModifierType mods, gpointer data)
{
    (void)ag; (void)obj; (void)key; (void)mods; (void)data;
    on_show_log(NULL, NULL);
}

static void on_show_log(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;
    if (log_window) { gtk_window_present(GTK_WINDOW(log_window)); return; }

    log_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(log_window), "LLM Server Log");
    gtk_window_set_default_size(GTK_WINDOW(log_window), 820, 460);
    g_signal_connect(log_window, "destroy", G_CALLBACK(on_log_destroy), NULL);

    GtkWidget *vbox   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
    log_textview = gtk_text_view_new();
    gtk_text_view_set_editable (GTK_TEXT_VIEW(log_textview), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_textview), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(log_textview), TRUE);

    gchar *fpath = log_fpath(), *existing = NULL;
    if (g_file_get_contents(fpath, &existing, NULL, NULL)) {
        gtk_text_buffer_set_text(
            gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_textview)),
            existing, -1);
        g_free(existing);
    }
    g_free(fpath);

    GtkWidget *bar  = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 4);
    GtkWidget *bcopy = gtk_button_new_with_label("Copy to Clipboard");
    g_signal_connect(bcopy, "clicked", G_CALLBACK(on_copy_log), NULL);
    gtk_box_pack_end(GTK_BOX(bar), bcopy, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(scroll), log_textview);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(vbox), bar,    FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(log_window), vbox);
    gtk_widget_show_all(log_window);
}

static const gchar *safe_icon(const gchar *want, const gchar *fallback)
{
    return gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), want)
           ? want : fallback;
}

static void render_badge(gdouble tps)
{
    if (!indicator) return;

    const gint SIZE = 48;
    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, SIZE, SIZE);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        g_printerr("Cairo: surface creation failed: %s\n",
                   cairo_status_to_string(cairo_surface_status(surf)));
        return;
    }
    cairo_t *cr = cairo_create(surf);

    cairo_arc(cr, SIZE/2.0, SIZE/2.0, SIZE/2.0 - 2, 0, 2*G_PI);
    if (tps >= 0) {
        cairo_set_source_rgb(cr, 0.2, 0.8, 0.2);
    } else {
        cairo_set_source_rgb(cr, 0.9, 0.2, 0.2);
    }
    cairo_fill_preserve(cr);

    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);

    if (tps >= 0) {
        gchar *lbl = tps >= 100 ? g_strdup_printf("%.0f", tps)
                   : tps >= 10  ? g_strdup_printf("%.0f", tps)
                                : g_strdup_printf("%.1f", tps);

        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_select_font_face(cr, "Sans",
            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, tps >= 100 ? 14.0 : 16.0);

        cairo_text_extents_t te;
        cairo_text_extents(cr, lbl, &te);
        cairo_move_to(cr,
            SIZE/2.0 - te.width/2.0  - te.x_bearing,
            SIZE/2.0 - te.height/2.0 - te.y_bearing);
        cairo_show_text(cr, lbl);
        g_free(lbl);
    } else {
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, 4.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        gdouble cx = SIZE/2.0, cy = SIZE/2.0, r = SIZE/4.0;
        cairo_move_to(cr, cx - r, cy - r);
        cairo_line_to(cr, cx + r, cy + r);
        cairo_move_to(cr, cx + r, cy - r);
        cairo_line_to(cr, cx - r, cy + r);
        cairo_stroke(cr);
    }

    cairo_destroy(cr);

    guchar *data   = cairo_image_surface_get_data(surf);
    gint    stride = cairo_image_surface_get_stride(surf);
    GdkPixbuf *pb  = gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB,
        TRUE, 8, SIZE, SIZE, stride, NULL, NULL);
    if (!pb) {
        g_printerr("Failed to create pixbuf from Cairo data\n");
        cairo_surface_destroy(surf);
        return;
    }

    gchar *tmp = g_build_filename(g_get_tmp_dir(), "llmtray_badge.png", NULL);
    GError *err = NULL;
    if (gdk_pixbuf_save(pb, tmp, "png", &err, NULL))
        app_indicator_set_icon_full(indicator, tmp, "LLM");
    else { g_printerr("Badge: %s\n", err ? err->message : "?"); g_clear_error(&err); }
    g_free(tmp);
    g_object_unref(pb);
    cairo_surface_destroy(surf);
}

static void update_icon(void)
{
    if (!indicator) return;
    if (svc_state == STATE_RUNNING) { render_badge(last_tps); return; }
    if (svc_state == STATE_STARTING || svc_state == STATE_STOPPING) {
        app_indicator_set_icon(indicator,
            safe_icon("emblem-synchronizing", "dialog-information"));
        return;
    }
    app_indicator_set_icon(indicator, safe_icon("emblem-readonly","dialog-error"));
}

static void update_menu_sensitivity(void)
{
    if (mi_start)   gtk_widget_set_sensitive(mi_start,   svc_state == STATE_STOPPED);
    if (mi_stop)    gtk_widget_set_sensitive(mi_stop,
        svc_state == STATE_RUNNING || svc_state == STATE_STARTING);
    if (mi_restart) gtk_widget_set_sensitive(mi_restart, svc_state == STATE_RUNNING);

    if (mi_status) {
        gchar *up = NULL;
        g_mutex_lock(&log_mutex);
        if (started_at) {
            GDateTime *now = g_date_time_new_now_local();
            gint64 s = g_date_time_difference(now, started_at) / G_TIME_SPAN_SECOND;
            g_date_time_unref(now);
            gint h = s/3600, m = (s%3600)/60, sec = s%60;
            up = h>0 ? g_strdup_printf("%dh %02dm %02ds",h,m,sec)
               : m>0 ? g_strdup_printf("%dm %02ds",m,sec)
                     : g_strdup_printf("%ds",(int)sec);
        }
        const gchar *dot = (svc_state==STATE_RUNNING) ? "●"
                         : (svc_state==STATE_STOPPED)  ? "✕" : "◌";
        gchar *lbl = (svc_state==STATE_RUNNING && up && child_pid)
            ? g_strdup_printf("%s Running  PID %d  up %s", dot, child_pid, up)
            : g_strdup_printf("%s %s", dot, STATE_NAME[svc_state]);
        gtk_menu_item_set_label(GTK_MENU_ITEM(mi_status), lbl);
        g_free(lbl); g_free(up);
        g_mutex_unlock(&log_mutex);
    }
}

static void set_state(ServiceState ns)
{
    if (ns == svc_state) return;
    if (!VALID_TX[svc_state][ns]) {
        g_printerr("BUG: bad transition %s→%s\n",
                   STATE_NAME[svc_state], STATE_NAME[ns]);
        g_assert_not_reached();
    }
    g_print("  State: %s → %s\n", STATE_NAME[svc_state], STATE_NAME[ns]);

    if (svc_state == STATE_RUNNING && ns != STATE_RUNNING) {
        if (uptime_timer) {
            g_source_remove(uptime_timer);
            uptime_timer = 0;
        }
    } else if (ns == STATE_RUNNING) {
        if (uptime_timer) g_source_remove(uptime_timer);
        uptime_timer = g_timeout_add(1000, update_uptime, NULL);
    }

    svc_state = ns;
    update_icon();
    update_menu_sensitivity();
}

/* ✅ FIX: Signal handler for self-pipe trick */
static void on_unix_signal_handler(int sig)
{
    static const char byte = 1;
    write(signal_pipe[1], &byte, 1);  // ✅ async-signal-safe (nonblocking write)
}

/* Self-pipe source handler */
static gboolean on_signal_pipe_io(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    (void)ch; (void)data; (void)cond;
    char buf[16];
    while (read(signal_pipe[0], buf, sizeof(buf)) > 0);
    if (main_loop) g_main_loop_quit(main_loop);
    return G_SOURCE_CONTINUE;
}

/* SIGKILL escalation */
static gboolean escalate_sigkill(gpointer data)
{
    (void)data; sigkill_timer = 0;
    if (child_pid == 0) return G_SOURCE_REMOVE;
    g_printerr("  ⚠ SIGKILL → PID %d\n", child_pid);
    kill(child_pid, SIGKILL);
    return G_SOURCE_REMOVE;
}

/* Curl handling */
static size_t curl_write(char *ptr, size_t sz, size_t nm, void *ud)
{
    g_string_append_len((GString *)ud, ptr, (gssize)(sz*nm));
    return sz * nm;
}

static gboolean curl_dispatch_fixed(gpointer data)
{
    CurlResult *res = data;

    if (res->generation != atomic_load(&spawn_gen) ||
        (svc_state != STATE_RUNNING && svc_state != STATE_STARTING)) {
        g_free(res);
        return G_SOURCE_REMOVE;
    }

    if (res->is_metrics) {
        if (res->success && res->tps >= 0) {
            g_mutex_lock(&log_mutex);
            last_tps = res->tps;
            g_mutex_unlock(&log_mutex);
            update_icon();
            gchar *tip = g_strdup_printf("%s — %.1f tok/s", APP_NAME, last_tps);
            app_indicator_set_title(indicator, tip);
            g_free(tip);
        }
    } else {
        if (res->success) {
            if (poll_timer) { g_source_remove(poll_timer); poll_timer = 0; }
            poll_count = 0;
            set_state(STATE_RUNNING);

            g_mutex_lock(&log_mutex);
            if (last_stable_at) g_date_time_unref(last_stable_at);
            last_stable_at = g_date_time_new_now_local();
            g_mutex_unlock(&log_mutex);

            notify_send("LLM Server Ready",
                        "The server is up and accepting requests.",
                        NOTIFY_URGENCY_LOW);
            g_idle_add(start_metrics_idle_final, NULL);
        } else {
            if (++poll_count >= POLL_MAX_CHECKS) {
                g_printerr("  ❌ Readiness timeout\n");
                notify_send("LLM Server Timeout",
                            "Server did not become ready in time.",
                            NOTIFY_URGENCY_CRITICAL);
                stop_child();
            } else {
                g_print("  ⏳ Waiting… (%d/%d)\n", poll_count, POLL_MAX_CHECKS);
            }
        }
    }
    g_free(res);
    return G_SOURCE_REMOVE;
}

static gpointer curl_thread_final(gpointer data)
{
    CurlTask *t   = data;
    GString  *buf = g_string_new(NULL);
    CURL     *c   = curl_easy_init();
    if (!c) {
        g_string_free(buf, TRUE);
        g_free(t->url);
        g_free(t->host_copy);
        g_free(t);
        return NULL;
    }

    curl_easy_setopt(c, CURLOPT_URL,            t->url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        3L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 2L);
    if (!t->is_metrics) curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      buf);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);

    CurlResult *res = g_new0(CurlResult, 1);
    res->is_metrics = t->is_metrics;
    res->generation = t->generation;
    res->success    = (rc == CURLE_OK);
    res->tps        = -1.0;
    if (res->success && t->is_metrics) {
        gchar **lines = g_strsplit(buf->str, "\n", -1);
        for (gchar **l = lines; *l; l++) {
            if (g_str_has_prefix(*l, "llama_tokens_per_second")) {
                gchar **p = g_strsplit(*l, " ", 2);
                if (p && p[1]) {
                    gchar *clean = g_strstrip(p[1]);
                    if (clean && *clean)
                        res->tps = g_ascii_strtod(clean, NULL);
                }
                g_strfreev(p);
                break;
            }
        }
        g_strfreev(lines);
    }
    g_string_free(buf, TRUE);
    g_free(t->url); g_free(t->host_copy); g_free(t);
    return res;
}

static gboolean dispatch_res(gpointer data)
{
    CurlResult *res = data;
    gboolean ok = g_idle_add(curl_dispatch_fixed, res);
    if (!ok) {
        g_printerr("Failed to dispatch curl result\n");
        g_free(res);
    }
    return G_SOURCE_REMOVE;
}

static gpointer curl_thread_final_wrapper(gpointer data)
{
    CurlResult *res = curl_thread_final(data);
    if (res) {
        g_idle_add(dispatch_res, res);
    }
    return NULL;
}

static void launch_curl_final(const gchar *path, gboolean is_metrics)
{
    if (!active_profile) return;

    g_mutex_lock(&log_mutex);
    Profile *p = active_profile;
    if (!p) {
        g_mutex_unlock(&log_mutex);
        return;
    }

    CurlTask *t  = g_new0(CurlTask, 1);
    t->url        = g_strdup_printf("http://%s:%d%s", p->host, p->port, path);
    t->is_metrics = is_metrics;
    t->generation = atomic_load(&spawn_gen);
    t->host_copy  = g_strdup(p->host);
    t->port_copy  = p->port;
    g_mutex_unlock(&log_mutex);

    /* ✅ FIX: Skip curl thread if shutting down */
    if (g_atomic_int_get(&shutting_down)) {
        g_free(t->url);
        g_free(t->host_copy);
        g_free(t);
        return;
    }

    GThread *thr = g_thread_new("curl", curl_thread_final_wrapper, t);
    if (!thr) {
        g_printerr("Failed to spawn curl thread\n");
        g_free(t->url);
        g_free(t->host_copy);
        g_free(t);
    } else {
        g_thread_unref(thr);
    }
}

static gboolean on_poll_tick_final(gpointer d)
{
    (void)d; launch_curl_final("/v1/models", FALSE); return G_SOURCE_CONTINUE;
}

static gboolean on_metrics_tick_final(gpointer d)
{
    (void)d;
    if (svc_state == STATE_RUNNING) launch_curl_final("/metrics", TRUE);
    return G_SOURCE_CONTINUE;
}

static gboolean start_metrics_idle_final(gpointer d)
{
    (void)d;
    if (metrics_timer) g_source_remove(metrics_timer);
    metrics_timer = g_timeout_add(METRICS_INTERVAL_MS, on_metrics_tick_final, NULL);
    return G_SOURCE_REMOVE;
}

static void on_child_exit(GPid pid, gint status, gpointer data)
{
    (void)data;
    if (poll_timer)    { g_source_remove(poll_timer);    poll_timer    = 0; }
    if (sigkill_timer) { g_source_remove(sigkill_timer); sigkill_timer = 0; }
    if (metrics_timer) { g_source_remove(metrics_timer); metrics_timer = 0; }

    gboolean was_running  = (svc_state == STATE_RUNNING);
    gboolean was_stopping = (svc_state == STATE_STOPPING);

    g_spawn_close_pid(pid);
    child_pid = 0; poll_count = 0;

    g_mutex_lock(&log_mutex);
    last_tps = -1.0;
    if (started_at) { g_date_time_unref(started_at); started_at = NULL; }
    g_mutex_unlock(&log_mutex);

    atomic_fetch_add(&spawn_gen, 1);

    if (uptime_timer) {
        g_source_remove(uptime_timer);
        uptime_timer = 0;
    }

    set_state(STATE_STOPPED);
    app_indicator_set_title(indicator, APP_NAME);

    gchar *msg = NULL;
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        msg = g_strdup_printf("\n[llm-daemon] Exited code %d\n", code);
        g_print("  Child exit: %d\n", code);
        if (code != 0 && was_running)
            notify_send("LLM Server Crashed",
                        "Exited unexpectedly — see log.", NOTIFY_URGENCY_CRITICAL);
    } else if (WIFSIGNALED(status)) {
        msg = g_strdup_printf("\n[llm-daemon] Killed signal %d\n", WTERMSIG(status));
        g_print("  Child signal: %d\n", WTERMSIG(status));
        if (was_running && !was_stopping)
            notify_send("LLM Server Stopped", "Killed by signal.",
                        NOTIFY_URGENCY_NORMAL);
    }
    if (msg) { log_append(msg); g_free(msg); }

    if (pending_restart) {
        pending_restart = FALSE;
        g_print("  Restarting…\n");
        do_start(); return;
    }

    if (auto_restart && was_running && !was_stopping) {
        g_mutex_lock(&log_mutex);
        if (last_stable_at) {
            GDateTime *now = g_date_time_new_now_local();
            gint64 up = g_date_time_difference(now, last_stable_at) / G_TIME_SPAN_SECOND;
            g_date_time_unref(now);
            if (up >= RESTART_STABLE_SECS) {
                restart_count   = 0;
                restart_backoff = RESTART_BACKOFF_BASE;
            }
        }
        g_mutex_unlock(&log_mutex);

        guint delay = restart_backoff;
        restart_count++;
        restart_backoff = MIN(restart_backoff * 2, RESTART_BACKOFF_MAX);

        gchar *bmsg = g_strdup_printf(
            "[llm-daemon] Auto-restart in %u ms (attempt %d)\n",
            delay, restart_count);
        log_append(bmsg); g_print("%s", bmsg); g_free(bmsg);
        g_timeout_add(delay, do_start_wrapper, NULL);
    }
}

static gboolean port_free(const gchar *host, gint port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return TRUE;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a = {0};
    a.sin_family      = AF_INET;
    a.sin_port        = htons((uint16_t)port);
    a.sin_addr.s_addr = inet_addr(host);
    gboolean free = (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    close(fd);
    return free;
}

static gboolean preflight(const Profile *p, gchar **errmsg)
{
    if (!g_file_test(p->server_bin, G_FILE_TEST_IS_EXECUTABLE)) {
        *errmsg = g_strdup_printf("Binary not found or not executable:\n%s",
                                  p->server_bin);
        return FALSE;
    }
    if (!g_file_test(p->model_path, G_FILE_TEST_EXISTS)) {
        *errmsg = g_strdup_printf("Model file not found:\n%s", p->model_path);
        return FALSE;
    }
    if (!port_free(p->host, p->port)) {
        *errmsg = g_strdup_printf("Port %d on %s is already in use.",
                                  p->port, p->host);
        return FALSE;
    }
    return TRUE;
}

static void show_err(const gchar *title, const gchar *msg)
{
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", msg);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void do_start(void)
{
    if (child_pid != 0 || !active_profile) return;
    if (svc_state != STATE_STOPPED) return;

    Profile *p = active_profile;
    gchar *errmsg = NULL;
    if (!preflight(p, &errmsg)) {
        g_printerr("  ❌ Preflight: %s\n", errmsg);
        show_err("Cannot start server", errmsg);
        g_free(errmsg);
        auto_restart = FALSE;
        return;
    }

    gchar *ps  = g_strdup_printf("%d", p->port);
    /* ✅ FIX: Preserve environment, only override LD_LIBRARY_PATH */
    gchar **env = g_get_environ();
    env = g_environ_setenv(env, "LD_LIBRARY_PATH", p->bin_dir, TRUE);

    const gchar *argv[] = {
        p->server_bin, "-m", p->model_path,
        "--host", p->host, "--port", ps,
        "-c", p->ctx_size, NULL
    };

    gint fd_out=-1, fd_err=-1;
    GPid  new_pid=0;
    GError *gerr=NULL;

    log_close();

    gboolean ok = g_spawn_async_with_pipes(
        NULL, (gchar**)argv, env,
        G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL, &new_pid, NULL, &fd_out, &fd_err, &gerr);

    g_free(ps);
    g_strfreev(env); // ✅ FIX: Free environment array

    if (!ok) {
        show_err("Failed to launch server", gerr ? gerr->message : "?");
        g_clear_error(&gerr);
        return;
    }

    atomic_fetch_add(&spawn_gen, 1);
    child_pid = new_pid; poll_count = 0;

    g_mutex_lock(&log_mutex);
    if (started_at) g_date_time_unref(started_at);
    started_at = g_date_time_new_now_local();
    g_mutex_unlock(&log_mutex);

    set_state(STATE_STARTING);
    g_print("  ▶ PID %d — http://%s:%d\n", child_pid, p->host, p->port);

    log_open();

    if (fd_out >= 0) {
        log_ch_out = g_io_channel_unix_new(fd_out);
        if (!log_ch_out) {
            close(fd_out);
            g_printerr("Failed to create log channel\n");
        } else {
            g_io_channel_set_close_on_unref(log_ch_out, TRUE); // ✅ FIX: FD ownership
            g_io_channel_set_flags(log_ch_out, G_IO_FLAG_NONBLOCK, NULL);
            g_io_channel_set_encoding(log_ch_out, NULL, NULL);
            log_wo = g_io_add_watch(log_ch_out, G_IO_IN|G_IO_HUP, on_log_io, NULL);
            if (!log_wo) {
                g_io_channel_unref(log_ch_out);
                log_ch_out = NULL;
                g_printerr("Failed to attach output watch\n");
            }
        }
    }
    if (fd_err >= 0) {
        log_ch_err = g_io_channel_unix_new(fd_err);
        if (!log_ch_err) {
            close(fd_err);
            g_printerr("Failed to create err channel\n");
        } else {
            g_io_channel_set_close_on_unref(log_ch_err, TRUE); // ✅ FIX: FD ownership
            g_io_channel_set_flags(log_ch_err, G_IO_FLAG_NONBLOCK, NULL);
            g_io_channel_set_encoding(log_ch_err, NULL, NULL);
            log_we = g_io_add_watch(log_ch_err, G_IO_IN|G_IO_HUP, on_log_io, NULL);
            if (!log_we) {
                g_io_channel_unref(log_ch_err);
                log_ch_err = NULL;
                g_printerr("Failed to attach error watch\n");
            }
        }
    }

    g_child_watch_add(child_pid, on_child_exit, NULL);
    poll_timer = g_timeout_add(POLL_INTERVAL_MS, on_poll_tick_final, NULL);
}

static void stop_child(void)
{
    if (poll_timer)    { g_source_remove(poll_timer);    poll_timer    = 0; }
    if (metrics_timer) { g_source_remove(metrics_timer); metrics_timer = 0; }
    if (child_pid == 0) return;
    set_state(STATE_STOPPING);
    g_print("  SIGTERM → PID %d\n", child_pid);
    if (kill(child_pid, SIGTERM) != 0) {
        g_printerr("  SIGTERM failed: %s\n", strerror(errno));
        kill(child_pid, SIGKILL);
        return;
    }
    if (sigkill_timer) g_source_remove(sigkill_timer);
    sigkill_timer = g_timeout_add(SIGKILL_DELAY_MS, escalate_sigkill, NULL);
}

static void on_start(GtkMenuItem *i, gpointer d) { (void)i;(void)d; do_start(); }

static gboolean do_start_wrapper(gpointer data)
{
    (void)data;
    do_start();
    return G_SOURCE_REMOVE;
}

static void on_stop(GtkMenuItem *i, gpointer d)
{
    (void)i;(void)d;
    if (child_pid==0) return;
    pending_restart = FALSE;
    stop_child();
}

static void on_restart(GtkMenuItem *i, gpointer d)
{
    (void)i;(void)d;
    if (child_pid==0) { do_start(); return; }
    pending_restart = TRUE;
    stop_child();
}

static void on_autorestart_toggled(GtkCheckMenuItem *item, gpointer d)
{
    (void)d;
    auto_restart = gtk_check_menu_item_get_active(item);
    if (auto_restart) { restart_count=0; restart_backoff=RESTART_BACKOFF_BASE; }
}

static void on_profile_activate(GtkMenuItem *item, gpointer data)
{
    (void)item;
    Profile *p = data;
    if (!p || p==active_profile) return;
    if (child_pid != 0) {
        show_err("Profile switch blocked",
                 "Stop the server before switching profiles."); return;
    }
    active_profile = p; profiles_save();
}

static void on_quit(GtkMenuItem *i, gpointer d)
{
    (void)i;(void)d;
    pending_restart = FALSE;
    g_atomic_int_set(&shutting_down, TRUE); // ✅ FIX: Signal shutdown early
    g_main_loop_quit(main_loop);
}

static void rebuild_menu(GtkWidget *menu, gpointer data)
{
    (void)data;
    mi_status = mi_start = mi_stop = mi_restart = NULL;

    GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = kids; l; l=l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    mi_status = gtk_menu_item_new_with_label("…");
    gtk_widget_set_sensitive(mi_status, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_status);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    mi_start = gtk_menu_item_new_with_label("Start");
    g_signal_connect(mi_start,   "activate", G_CALLBACK(on_start),   NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_start);

    mi_stop  = gtk_menu_item_new_with_label("Stop");
    g_signal_connect(mi_stop,    "activate", G_CALLBACK(on_stop),    NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_stop);

    mi_restart = gtk_menu_item_new_with_label("Restart");
    g_signal_connect(mi_restart, "activate", G_CALLBACK(on_restart), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_restart);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *ar = gtk_check_menu_item_new_with_label("Auto-restart on crash");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(ar), auto_restart);
    g_signal_connect(ar, "toggled", G_CALLBACK(on_autorestart_toggled), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), ar);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *ml = gtk_menu_item_new_with_label("Show Log…   Ctrl+L");
    g_signal_connect(ml, "activate", G_CALLBACK(on_show_log), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), ml);

    if (g_list_length(profiles) > 1) {
        GtkWidget *mp  = gtk_menu_item_new_with_label("Profile");
        GtkWidget *sub = gtk_menu_new();
        for (GList *l = profiles; l; l=l->next) {
            Profile   *p  = l->data;
            GtkWidget *mi = gtk_menu_item_new_with_label(p->name);
            g_signal_connect(mi, "activate", G_CALLBACK(on_profile_activate), p);
            gtk_widget_set_sensitive(mi, p != active_profile);
            gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
        }
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(mp), sub);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mp);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *ms = gtk_menu_item_new_with_label("Settings…");
    g_signal_connect(ms, "activate", G_CALLBACK(on_settings_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), ms);

    GtkWidget *ma = gtk_check_menu_item_new_with_label("Start on Login");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(ma), autostart_enabled());
    g_signal_connect(ma, "toggled", G_CALLBACK(on_autostart_toggled), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), ma);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *mq = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(mq, "activate", G_CALLBACK(on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mq);

    gtk_widget_show_all(menu);
    update_menu_sensitivity();
}

int main(int argc, char *argv[])
{
    if (!lock_acquire()) return EXIT_FAILURE;

    gtk_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    notify_init(APP_NAME);
    profiles_load();

    indicator = app_indicator_new(APP_ID, "dialog-information",
                                  APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    if (!indicator) {
        g_printerr("AppIndicator creation failed.\n");
        lock_release(); return EXIT_FAILURE;
    }
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title (indicator, APP_NAME);
    update_icon();

    GtkWidget     *menu = gtk_menu_new();
    GtkAccelGroup *ag   = gtk_accel_group_new();
    gtk_menu_set_accel_group(GTK_MENU(menu), ag);

    GClosure *cl = g_cclosure_new(G_CALLBACK(on_accel_log), NULL, NULL);
    gtk_accel_group_connect(ag, GDK_KEY_l, GDK_CONTROL_MASK,
                            GTK_ACCEL_VISIBLE, cl);

    /* ✅ FIX: Self-pipe trick setup with both ends nonblocking */
    if (pipe(signal_pipe) != 0) {
        g_printerr("Failed to create signal pipe: %s\n", strerror(errno));
        lock_release();
        return EXIT_FAILURE;
    }
    fcntl(signal_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(signal_pipe[1], F_SETFL, O_NONBLOCK); // ✅ FIX: both ends nonblocking

    GIOChannel *ch = g_io_channel_unix_new(signal_pipe[0]);
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_add_watch(ch, G_IO_IN, on_signal_pipe_io, NULL);
    g_io_channel_unref(ch);

    /* ✅ FIX: Use sigaction() for reliable signals */
    struct sigaction sa = {0};
    sa.sa_handler = on_unix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_signal_connect(menu, "deactivate", G_CALLBACK(rebuild_menu), NULL);
    rebuild_menu(menu, NULL);
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    main_loop = g_main_loop_new(NULL, FALSE);
    g_print("%s v4.7 — profile: %s  host: %s:%d\n",
            APP_NAME,
            active_profile ? active_profile->name : "?",
            active_profile ? active_profile->host : "?",
            active_profile ? active_profile->port : 0);
    g_main_loop_run(main_loop);

    pending_restart = FALSE;
    g_atomic_int_set(&shutting_down, TRUE); // ✅ FIX: Signal shutdown before teardown
    if (child_pid) stop_child();

    /* ✅ FIX: Wait for child exit before closing log */
    if (child_pid) {
        g_print("  Waiting for child exit...\n");
        while (g_main_context_iteration(NULL, FALSE)) {
            if (child_pid == 0) break;
            g_usleep(10000); // 10ms
        }
    }

    safe_log_channels_close();
    log_close();

    if (log_window)    gtk_widget_destroy(log_window);
    if (started_at) { g_mutex_lock(&log_mutex); g_date_time_unref(started_at); g_mutex_unlock(&log_mutex); }
    if (last_stable_at) { g_mutex_lock(&log_mutex); g_date_time_unref(last_stable_at); g_mutex_unlock(&log_mutex); }
    g_object_unref(indicator);
    notify_uninit();
    curl_global_cleanup();
    g_list_free_full(profiles, (GDestroyNotify)profile_free);
    lock_release();

    /* ✅ FIX: Close signal pipe */
    if (signal_pipe[0] >= 0) close(signal_pipe[0]);
    if (signal_pipe[1] >= 0) close(signal_pipe[1]);

    return EXIT_SUCCESS;
}
