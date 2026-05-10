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


#include <curl/curl.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <libnotify/notify.h>

/* ── Constants ─────────────────────────────────────────────────────────────── */
#define APP_NAME            "LLM-Daemon"
#define APP_ID              "com.example.llmtray"
#define LOCK_FILE           "/tmp/llmtray.lock"
#define CONFIG_DIR_NAME     "llm-daemon"
#define CONFIG_FILE_NAME    "config.ini"
#define CONFIG_VERSION      1

#define DEFAULT_HOST        "127.0.0.1"
#define DEFAULT_PORT        8080
#define DEFAULT_CTX_SIZE    "0"
#define DEFAULT_THREADS     1
#define DEFAULT_TEMP        0.8

#define POLL_INTERVAL_MS      2000
#define POLL_MAX_CHECKS       50
#define SIGKILL_DELAY_MS      8000
#define METRICS_INTERVAL_MS   30000
#define RESTART_BACKOFF_BASE  1000
#define RESTART_BACKOFF_MAX   64000
#define RESTART_STABLE_SECS   60

/* ── State machine ──────────────────────────────────────────────────────────── */
typedef enum {
    STATE_STOPPED  = 0,
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPING,
    STATE_COUNT
} ServiceState;

static const gboolean VALID_TX[STATE_COUNT][STATE_COUNT] = {
    { FALSE,  TRUE,  FALSE, FALSE },
    { TRUE,   FALSE, TRUE,  TRUE  },
    { FALSE,  FALSE, FALSE, TRUE  },
    { TRUE,   FALSE, FALSE, FALSE },
};

static const gchar *STATE_NAME[] = { "STOPPED","STARTING","RUNNING","STOPPING" };

/* ── Types ──────────────────────────────────────────────────────────────────── */
typedef struct {
    gchar   *server_bin;   /* full path to llama-server binary              */
    gchar   *model_path;   /* full path to .gguf model                      */
    gchar   *host;
    gchar   *ctx_size;
    gint     port;
    gint     threads;      /* -t N                                           */
    gdouble  temperature;  /* --temp F                                       */
    gboolean flash_attn;   /* --flash-attn on                               */
} ServerConfig;

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

/* ── Global config ──────────────────────────────────────────────────────────── */
static ServerConfig g_config = {NULL, NULL, NULL, NULL, 0,
                                 DEFAULT_THREADS, DEFAULT_TEMP, FALSE};

/* ── Globals ────────────────────────────────────────────────────────────────── */
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

static GtkWidget    *mi_status  = NULL;
static GtkWidget    *mi_start   = NULL;
static GtkWidget    *mi_stop    = NULL;
static GtkWidget    *mi_restart = NULL;

static int           signal_pipe[2]  = {-1, -1};
static _Atomic gboolean shutting_down = FALSE;

/* ── Forward declarations ───────────────────────────────────────────────────── */
static void          set_state(ServiceState s);
static void          rebuild_menu(GtkWidget *menu, gpointer data);
static void          do_start(void);
static gboolean      do_start_wrapper(gpointer data);
static void          stop_child(void);
static gboolean      start_metrics_idle_final(gpointer d);
static void          update_menu_sensitivity(void);
static void          update_icon(void);
static gboolean      port_free(const gchar *host, gint port);
static gboolean      on_signal_pipe_io(GIOChannel *ch, GIOCondition cond, gpointer data);
static void          on_unix_signal_handler(int sig);
static void          show_err(const gchar *title, const gchar *msg);
static const gchar  *safe_icon(const gchar *want, const gchar *fallback);
static void          render_badge(gdouble tps);
static gboolean      escalate_sigkill(gpointer data);
static void          on_child_exit(GPid pid, gint status, gpointer data);
static gboolean      on_poll_tick_final(gpointer d);
static gboolean      preflight(gchar **errmsg);
static void          on_settings_activate(GtkMenuItem *item, gpointer data);

/* ════════════════════════════════════════════════════════════════════════════
   Lock file
   ════════════════════════════════════════════════════════════════════════════ */
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
        if (alive) { g_printerr("%s already running (PID %d).\n", APP_NAME, pid); return FALSE; }
        unlink(LOCK_FILE);
        fd = open(LOCK_FILE, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0) { g_printerr("Cannot create lock file: %s\n", strerror(errno)); return FALSE; }
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (n < 0 || (size_t)n >= sizeof(buf)) { close(fd); unlink(LOCK_FILE); return FALSE; }
    if ((size_t)n != (size_t)write(fd, buf, (size_t)n)) { close(fd); unlink(LOCK_FILE); return FALSE; }
    close(fd);
    return TRUE;
}

static void lock_release(void) { unlink(LOCK_FILE); }

/* ════════════════════════════════════════════════════════════════════════════
   Config paths
   ════════════════════════════════════════════════════════════════════════════ */
static gchar *cfg_dir(void)
{
    return g_build_filename(g_get_home_dir(), ".config", CONFIG_DIR_NAME, NULL);
}
static gchar *cfg_path(void)
{
    return g_build_filename(g_get_home_dir(), ".config", CONFIG_DIR_NAME, CONFIG_FILE_NAME, NULL);
}

/* ════════════════════════════════════════════════════════════════════════════
   Schema migration
   ════════════════════════════════════════════════════════════════════════════ */
static void config_migrate(GKeyFile *kf, gint from)
{
    if (from < 1) {
        g_key_file_set_integer(kf, "llm-daemon", "version", 1);
        g_print("Config: migrated v0→v1\n");
    }
}

/* ════════════════════════════════════════════════════════════════════════════
   ServerConfig helpers
   ════════════════════════════════════════════════════════════════════════════ */
static void server_config_free(ServerConfig *c)
{
    if (!c) return;
    // g_free(c->server_bin);
    g_clear_pointer(&c->server_bin, g_free);
    g_free(c->model_path);
    g_free(c->host);
    g_free(c->ctx_size);
}

static void server_config_defaults(ServerConfig *c)
{
    c->server_bin = g_strdup("");
    c->model_path = g_strdup("");
    c->host       = g_strdup(DEFAULT_HOST);
    c->port       = DEFAULT_PORT;
    c->ctx_size   = g_strdup(DEFAULT_CTX_SIZE);
    c->threads    = DEFAULT_THREADS;
    c->temperature = DEFAULT_TEMP;
    c->flash_attn = FALSE;
}

/* ════════════════════════════════════════════════════════════════════════════
   Load config
   ════════════════════════════════════════════════════════════════════════════ */
static void config_load(void)
{
    server_config_free(&g_config);
    server_config_defaults(&g_config);

    GKeyFile *kf   = g_key_file_new();
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

#define KFS(k,d) (g_key_file_has_key(kf,"llm-daemon",(k),NULL) \
                  ? g_key_file_get_string(kf,"llm-daemon",(k),NULL) : g_strdup(d))
#define KFI(k,d) (g_key_file_has_key(kf,"llm-daemon",(k),NULL) \
                  ? g_key_file_get_integer(kf,"llm-daemon",(k),NULL) : (d))
#define KFD(k,d) (g_key_file_has_key(kf,"llm-daemon",(k),NULL) \
                  ? g_key_file_get_double(kf,"llm-daemon",(k),NULL) : (d))
#define KFB(k,d) (g_key_file_has_key(kf,"llm-daemon",(k),NULL) \
                  ? g_key_file_get_boolean(kf,"llm-daemon",(k),NULL) : (d))

    g_free(g_config.server_bin);
    g_free(g_config.model_path);
    g_free(g_config.host);
    g_free(g_config.ctx_size);

    g_config.server_bin  = KFS("server_bin",  "");
    g_config.model_path  = KFS("model_path",  "");
    g_config.host        = KFS("host",         DEFAULT_HOST);
    g_config.ctx_size    = KFS("ctx_size",     DEFAULT_CTX_SIZE);
    g_config.port        = KFI("port",         DEFAULT_PORT);
    g_config.threads     = KFI("threads",      DEFAULT_THREADS);
    g_config.temperature = KFD("temperature",  DEFAULT_TEMP);
    g_config.flash_attn  = KFB("flash_attn",   FALSE);

#undef KFS
#undef KFI
#undef KFD
#undef KFB

    g_key_file_free(kf);
}

/* ════════════════════════════════════════════════════════════════════════════
   Save config
   ════════════════════════════════════════════════════════════════════════════ */
static void config_save(void)
{
    GKeyFile *kf   = g_key_file_new();
    gchar    *dir  = cfg_dir();
    gchar    *path = cfg_path();

    g_key_file_set_integer(kf, "llm-daemon", "version",     CONFIG_VERSION);
    g_key_file_set_string (kf, "llm-daemon", "server_bin",  g_config.server_bin  ? g_config.server_bin  : "");
    g_key_file_set_string (kf, "llm-daemon", "model_path",  g_config.model_path  ? g_config.model_path  : "");
    g_key_file_set_string (kf, "llm-daemon", "host",        g_config.host        ? g_config.host        : DEFAULT_HOST);
    g_key_file_set_integer(kf, "llm-daemon", "port",        g_config.port);
    g_key_file_set_string (kf, "llm-daemon", "ctx_size",    g_config.ctx_size    ? g_config.ctx_size    : DEFAULT_CTX_SIZE);
    g_key_file_set_integer(kf, "llm-daemon", "threads",     g_config.threads);
    g_key_file_set_double (kf, "llm-daemon", "temperature", g_config.temperature);
    g_key_file_set_boolean(kf, "llm-daemon", "flash_attn",  g_config.flash_attn);

    g_mkdir_with_parents(dir, 0755);
    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, path, &err)) {
        g_printerr("Config save: %s\n", err ? err->message : "?");
        g_clear_error(&err);
    }
    g_key_file_free(kf);
    g_free(dir);
    g_free(path);
}

/* ════════════════════════════════════════════════════════════════════════════
   Settings dialog
   ════════════════════════════════════════════════════════════════════════════ */

/* Attach a text entry + optional browse button to a grid row.
   Returns the GtkEntry widget. */
static GtkWidget *add_entry_row(GtkWidget *grid, int row,
                                const gchar *label_text,
                                const gchar *value,
                                const gchar *key,
                                GtkWidget   *dlg,
                                gboolean     add_browse)
{
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), value ? value : "");
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 50);
    gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);

    if (key && dlg)
        g_object_set_data(G_OBJECT(dlg), key, entry);

    if (add_browse) {
        GtkWidget *btn = gtk_button_new_with_label("Browse…");
        /* Store the entry in the button so the callback can update it */
        g_object_set_data(G_OBJECT(btn), "entry", entry);
        /* Tag whether we want files or any file */
        g_object_set_data(G_OBJECT(btn), "action",
                          GINT_TO_POINTER(GTK_FILE_CHOOSER_ACTION_OPEN));
        g_signal_connect(btn, "clicked", G_CALLBACK(
            /* inline lambda via nested function — use a named static instead */
            ({
                /* can't do true lambdas in C; connect callback below */
                (void (*)(GtkButton *, gpointer))NULL;
            })
        ), NULL);
        /* We'll use a named callback — attach below */
        gtk_grid_attach(GTK_GRID(grid), btn, 2, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg),
                          g_strdup_printf("%s_browse", key ? key : ""), btn);
    }

    return entry;
}

/* Browse button callback — shared by both server-binary and model-path rows */
static void on_browse_clicked(GtkButton *btn, gpointer user_data)
{
    (void)user_data;
    GtkWidget *entry  = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "entry"));
    GtkWidget *parent = gtk_widget_get_toplevel(GTK_WIDGET(btn));

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Select file",
        GTK_WINDOW(parent),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Select", GTK_RESPONSE_ACCEPT,
        NULL);

    /* Pre-seed the chooser with the current entry value if it looks valid */
    const gchar *cur = gtk_entry_get_text(GTK_ENTRY(entry));
    if (cur && *cur) {
        if (g_file_test(cur, G_FILE_TEST_EXISTS))
            gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), cur);
        else {
            gchar *dir = g_path_get_dirname(cur);
            if (g_file_test(dir, G_FILE_TEST_IS_DIR))
                gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), dir);
            g_free(dir);
        }
    }

    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (filename) {
            gtk_entry_set_text(GTK_ENTRY(entry), filename);
            g_free(filename);
        }
    }
    gtk_widget_destroy(chooser);
}

static void on_settings_response(GtkDialog *dlg, gint resp, gpointer data)
{
    (void)data;
    if (resp == GTK_RESPONSE_ACCEPT) {
        /* Text fields */
        gchar *server_bin = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "server_bin"))));
        gchar *model_path = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "model_path"))));
        gchar *host = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "host"))));
        gchar *ctx_size = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "ctx_size"))));

        /* Numeric fields */
        gint port = (gint)gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "port")));
        gint threads = (gint)gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "threads")));
        gdouble temperature = gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "temperature")));

        /* Boolean */
        gboolean flash_attn = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(dlg), "flash_attn")));

        if (port < 1 || port > 65535) {
            GtkWidget *err_dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "Invalid port: must be 1–65535");
            gtk_dialog_run(GTK_DIALOG(err_dlg));
            gtk_widget_destroy(err_dlg);
            g_free(server_bin); g_free(model_path);
            g_free(host);       g_free(ctx_size);
            gtk_widget_destroy(GTK_WIDGET(dlg));
            return;
        }

        server_config_free(&g_config);
        g_config.server_bin  = server_bin;
        g_config.model_path  = model_path;
        g_config.host        = host;
        g_config.ctx_size    = ctx_size;
        g_config.port        = port;
        g_config.threads     = threads;
        g_config.temperature = temperature;
        g_config.flash_attn  = flash_attn;

        config_save();
    }
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

/* Helper: attach a spin-button row */
static GtkWidget *add_spin_row(GtkWidget *grid, int row,
                               const gchar *label_text,
                               gdouble min, gdouble max, gdouble step,
                               gdouble value, gint digits,
                               const gchar *key, GtkWidget *dlg)
{
    GtkWidget *lbl = gtk_label_new(label_text);
    gtk_widget_set_halign(lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    gtk_widget_set_halign(spin, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), spin, 1, row, 1, 1);

    if (key && dlg) g_object_set_data(G_OBJECT(dlg), key, spin);
    return spin;
}

static void on_settings_activate(GtkMenuItem *item, gpointer data)
{
    (void)item; (void)data;

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Settings", NULL,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        "Save",   GTK_RESPONSE_ACCEPT,
        NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);

    int row = 0;

    /* ── Server binary (with Browse) ── */
    {
        GtkWidget *lbl = gtk_label_new("Server binary:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), g_config.server_bin ? g_config.server_bin : "");
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 50);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "server_bin", entry);

        GtkWidget *btn = gtk_button_new_with_label("Browse…");
        g_object_set_data(G_OBJECT(btn), "entry", entry);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_browse_clicked), NULL);
        gtk_grid_attach(GTK_GRID(grid), btn, 2, row, 1, 1);
        row++;
    }

    /* ── Model path (with Browse) ── */
    {
        GtkWidget *lbl = gtk_label_new("Model path:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), g_config.model_path ? g_config.model_path : "");
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 50);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "model_path", entry);

        GtkWidget *btn = gtk_button_new_with_label("Browse…");
        g_object_set_data(G_OBJECT(btn), "entry", entry);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_browse_clicked), NULL);
        gtk_grid_attach(GTK_GRID(grid), btn, 2, row, 1, 1);
        row++;
    }

    /* ── Host ── */
    {
        GtkWidget *lbl = gtk_label_new("Host:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), g_config.host ? g_config.host : DEFAULT_HOST);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 20);
        gtk_widget_set_halign(entry, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "host", entry);
        row++;
    }

    /* ── Port ── */
    add_spin_row(grid, row++, "Port:", 1, 65535, 1,
                 g_config.port, 0, "port", dlg);

    /* ── Context size ── */
    {
        GtkWidget *lbl = gtk_label_new("Context size:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), g_config.ctx_size ? g_config.ctx_size : DEFAULT_CTX_SIZE);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 10);
        gtk_widget_set_halign(entry, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "ctx_size", entry);
        row++;
    }

    /* ── Threads (-t) ── */
    add_spin_row(grid, row++, "Threads:", 1, 256, 1,
                 g_config.threads, 0, "threads", dlg);


    /* ── Temperature (--temp) ── */
    add_spin_row(grid, row++, "Temperature:", 0.0, 1.0, 0.01,
                 g_config.temperature, 2, "temperature", dlg);

    /* ── Flash Attention checkbox ── */
    {
        GtkWidget *lbl = gtk_label_new("Flash Attention:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        GtkWidget *chk = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), g_config.flash_attn);
        gtk_widget_set_halign(chk, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), chk, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "flash_attn", chk);
        row++;
    }

    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                       grid, TRUE, TRUE, 0);
    g_signal_connect(dlg, "response", G_CALLBACK(on_settings_response), NULL);
    gtk_widget_show_all(dlg);
}

/* ════════════════════════════════════════════════════════════════════════════
   Notifications
   ════════════════════════════════════════════════════════════════════════════ */
static void notify_send(const gchar *sum, const gchar *body, NotifyUrgency u)
{
    NotifyNotification *n = notify_notification_new(sum, body, NULL);
    notify_notification_set_urgency(n, u);
    notify_notification_set_timeout(n, 4000);
    GError *err = NULL;
    if (!notify_notification_show(n, &err)) {
        g_printerr("Notify: %s\n", err->message);
        g_clear_error(&err);
    }
    g_object_unref(n);
}

/* ════════════════════════════════════════════════════════════════════════════
   Uptime ticker
   ════════════════════════════════════════════════════════════════════════════ */
static gboolean update_uptime(gpointer data)
{
    (void)data;
    if (svc_state == STATE_RUNNING) update_menu_sensitivity();
    return G_SOURCE_CONTINUE;
}

/* ════════════════════════════════════════════════════════════════════════════
   Self-pipe signal handling
   ════════════════════════════════════════════════════════════════════════════ */
static void on_unix_signal_handler(int sig)
{
    (void)sig;
    static const char byte = 1;
    (void)write(signal_pipe[1], &byte, 1);
}

static gboolean on_signal_pipe_io(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    (void)ch; (void)data; (void)cond;
    char buf[16];
    while (read(signal_pipe[0], buf, sizeof(buf)) > 0);
    if (main_loop) g_main_loop_quit(main_loop);
    return G_SOURCE_CONTINUE;
}

/* ════════════════════════════════════════════════════════════════════════════
   SIGKILL escalation
   ════════════════════════════════════════════════════════════════════════════ */
static gboolean escalate_sigkill(gpointer data)
{
    (void)data;
    sigkill_timer = 0;
    if (child_pid == 0) return G_SOURCE_REMOVE;
    g_printerr("  ⚠ SIGKILL → PID %d\n", child_pid);
    kill(child_pid, SIGKILL);
    return G_SOURCE_REMOVE;
}

/* ════════════════════════════════════════════════════════════════════════════
   CURL helpers
   ════════════════════════════════════════════════════════════════════════════ */
static size_t curl_write_cb(char *ptr, size_t sz, size_t nm, void *ud)
{
    g_string_append_len((GString *)ud, ptr, (gssize)(sz * nm));
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
            update_icon();
        }
    } else {
        if (res->success) {
            if (poll_timer) { g_source_remove(poll_timer); poll_timer = 0; }
            poll_count = 0;
            set_state(STATE_RUNNING);

            if (last_stable_at) g_date_time_unref(last_stable_at);
            last_stable_at = g_date_time_new_now_local();

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

static gpointer curl_thread_fn(gpointer data)
{
    CurlTask *t   = data;
    GString  *buf = g_string_new(NULL);
    CURL     *c   = curl_easy_init();
    if (!c) {
        g_string_free(buf, TRUE);
        g_free(t->url); g_free(t->host_copy); g_free(t);
        return NULL;
    }

    curl_easy_setopt(c, CURLOPT_URL,            t->url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        3L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 2L);
    if (!t->is_metrics) curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  curl_write_cb);
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
                    if (clean && *clean) res->tps = g_ascii_strtod(clean, NULL);
                }
                g_strfreev(p);
                break;
            }
        }
        g_strfreev(lines);
    }

    g_string_free(buf, TRUE);
    g_free(t->url); g_free(t->host_copy); g_free(t);
    g_idle_add(curl_dispatch_fixed, res);
    return NULL;
}

static void launch_curl(const gchar *path, gboolean is_metrics)
{
    if (!g_config.host) return;

    CurlTask *t   = g_new0(CurlTask, 1);
    t->url        = g_strdup_printf("http://%s:%d%s", g_config.host, g_config.port, path);
    t->is_metrics = is_metrics;
    t->generation = atomic_load(&spawn_gen);
    t->host_copy  = g_strdup(g_config.host);
    t->port_copy  = g_config.port;

    if (g_atomic_int_get(&shutting_down)) {
        g_free(t->url); g_free(t->host_copy); g_free(t);
        return;
    }

    GThread *thr = g_thread_new("curl", curl_thread_fn, t);
    if (!thr) {
        g_printerr("Failed to spawn curl thread\n");
        g_free(t->url); g_free(t->host_copy); g_free(t);
    } else {
        g_thread_unref(thr);
    }
}

static gboolean on_poll_tick_final(gpointer d)
{
    (void)d;
    launch_curl("/v1/models", FALSE);
    return G_SOURCE_CONTINUE;
}

static gboolean on_metrics_tick_final(gpointer d)
{
    (void)d;
    if (svc_state == STATE_RUNNING) launch_curl("/metrics", TRUE);
    return G_SOURCE_CONTINUE;
}

static gboolean start_metrics_idle_final(gpointer d)
{
    (void)d;
    if (metrics_timer) g_source_remove(metrics_timer);
    metrics_timer = g_timeout_add(METRICS_INTERVAL_MS, on_metrics_tick_final, NULL);
    return G_SOURCE_REMOVE;
}

/* ════════════════════════════════════════════════════════════════════════════
   Child-exit handler
   ════════════════════════════════════════════════════════════════════════════ */
static void on_child_exit(GPid pid, gint status, gpointer data)
{
    (void)data;
    if (poll_timer)    { g_source_remove(poll_timer);    poll_timer    = 0; }
    if (sigkill_timer) { g_source_remove(sigkill_timer); sigkill_timer = 0; }
    if (metrics_timer) { g_source_remove(metrics_timer); metrics_timer = 0; }

    gboolean was_running  = (svc_state == STATE_RUNNING);
    gboolean was_stopping = (svc_state == STATE_STOPPING);

    g_spawn_close_pid(pid);
    child_pid  = 0;
    poll_count = 0;
    last_tps   = -1.0;

    if (started_at) { g_date_time_unref(started_at); started_at = NULL; }
    atomic_fetch_add(&spawn_gen, 1);

    if (uptime_timer) { g_source_remove(uptime_timer); uptime_timer = 0; }

    set_state(STATE_STOPPED);
    app_indicator_set_title(indicator, APP_NAME);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        g_print("  Child exit: %d\n", code);
        if (code != 0 && was_running)
            notify_send("LLM Server Crashed", "Exited unexpectedly.", NOTIFY_URGENCY_CRITICAL);
    } else if (WIFSIGNALED(status)) {
        g_print("  Child signal: %d\n", WTERMSIG(status));
        if (was_running && !was_stopping)
            notify_send("LLM Server Stopped", "Killed by signal.", NOTIFY_URGENCY_NORMAL);
    }

    if (pending_restart) {
        pending_restart = FALSE;
        g_print("  Restarting…\n");
        do_start();
        return;
    }

    if (auto_restart && was_running && !was_stopping) {
        if (last_stable_at) {
            GDateTime *now = g_date_time_new_now_local();
            gint64 up = g_date_time_difference(now, last_stable_at) / G_TIME_SPAN_SECOND;
            g_date_time_unref(now);
            if (up >= RESTART_STABLE_SECS) {
                restart_count   = 0;
                restart_backoff = RESTART_BACKOFF_BASE;
            }
        }
        guint delay = restart_backoff;
        restart_count++;
        restart_backoff = MIN(restart_backoff * 2, RESTART_BACKOFF_MAX);
        g_print("[llm-daemon] Auto-restart in %u ms (attempt %d)\n", delay, restart_count);
        g_timeout_add(delay, do_start_wrapper, NULL);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
   Port check / preflight
   ════════════════════════════════════════════════════════════════════════════ */
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
    gboolean is_free  = (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    close(fd);
    return is_free;
}

static gboolean preflight(gchar **errmsg)
{
    if (!g_config.server_bin || !*g_config.server_bin) {
        *errmsg = g_strdup("Server binary path is not set. Open Settings to configure it.");
        return FALSE;
    }
    if (!g_file_test(g_config.server_bin, G_FILE_TEST_IS_EXECUTABLE)) {
        *errmsg = g_strdup_printf("Binary not found or not executable:\n%s", g_config.server_bin);
        return FALSE;
    }
    if (!g_config.model_path || !*g_config.model_path) {
        *errmsg = g_strdup("Model path is not set. Open Settings to configure it.");
        return FALSE;
    }
    if (!g_file_test(g_config.model_path, G_FILE_TEST_EXISTS)) {
        *errmsg = g_strdup_printf("Model file not found:\n%s", g_config.model_path);
        return FALSE;
    }
    if (!port_free(g_config.host, g_config.port)) {
        *errmsg = g_strdup_printf("Port %d on %s is already in use.", g_config.port, g_config.host);
        return FALSE;
    }
    return TRUE;
}

/* ════════════════════════════════════════════════════════════════════════════
   Error dialog
   ════════════════════════════════════════════════════════════════════════════ */
static void show_err(const gchar *title, const gchar *msg)
{
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", msg);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* ════════════════════════════════════════════════════════════════════════════
   Core: state machine
   ════════════════════════════════════════════════════════════════════════════ */
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
        if (uptime_timer) { g_source_remove(uptime_timer); uptime_timer = 0; }
    } else if (ns == STATE_RUNNING) {
        if (uptime_timer) g_source_remove(uptime_timer);
        uptime_timer = g_timeout_add(1000, update_uptime, NULL);
    }

    svc_state = ns;
    update_icon();
    update_menu_sensitivity();
}

/* ════════════════════════════════════════════════════════════════════════════
   Icon / badge
   ════════════════════════════════════════════════════════════════════════════ */
static const gchar *safe_icon(const gchar *want, const gchar *fallback)
{
    return gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), want)
           ? want : fallback;
}

static void render_badge(gdouble tps)
{
    if (!indicator) return;

    const gint SIZE = 48;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SIZE, SIZE);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        g_printerr("Cairo surface failed\n");
        return;
    }
    cairo_t *cr = cairo_create(surf);

    cairo_arc(cr, SIZE / 2.0, SIZE / 2.0, SIZE / 2.0 - 2, 0, 2 * G_PI);
    if (tps >= 0) cairo_set_source_rgb(cr, 0.2, 0.8, 0.2);
    else          cairo_set_source_rgb(cr, 0.9, 0.2, 0.2);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);

    if (tps >= 0) {
        gchar *lbl = tps >= 10 ? g_strdup_printf("%.0f", tps)
                               : g_strdup_printf("%.1f", tps);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, tps >= 100 ? 14.0 : 16.0);
        cairo_text_extents_t te;
        cairo_text_extents(cr, lbl, &te);
        cairo_move_to(cr, SIZE / 2.0 - te.width / 2.0 - te.x_bearing,
                          SIZE / 2.0 - te.height / 2.0 - te.y_bearing);
        cairo_show_text(cr, lbl);
        g_free(lbl);
    } else {
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_set_line_width(cr, 4.0);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        gdouble cx = SIZE / 2.0, cy = SIZE / 2.0, r = SIZE / 4.0;
        cairo_move_to(cr, cx - r, cy - r); cairo_line_to(cr, cx + r, cy + r);
        cairo_move_to(cr, cx + r, cy - r); cairo_line_to(cr, cx - r, cy + r);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);

    /* Flush so pixel data is accessible */
    cairo_surface_flush(surf);

    guchar *pixels = cairo_image_surface_get_data(surf);
    gint    stride = cairo_image_surface_get_stride(surf);

    /* Cairo uses ARGB pre-multiplied; gdk-pixbuf wants RGBA */
    guchar *rgba = g_malloc(SIZE * SIZE * 4);
    for (gint y = 0; y < SIZE; y++) {
        for (gint x = 0; x < SIZE; x++) {
            guint32 px;
            memcpy(&px, pixels + y * stride + x * 4, 4);
            guint8 a = (px >> 24) & 0xff;
            guint8 r = (px >> 16) & 0xff;
            guint8 g_ch = (px >>  8) & 0xff;
            guint8 b = (px      ) & 0xff;
            gint off = (y * SIZE + x) * 4;
            rgba[off+0] = r;
            rgba[off+1] = g_ch;
            rgba[off+2] = b;
            rgba[off+3] = a;
        }
    }

    GdkPixbuf *pb = gdk_pixbuf_new_from_data(rgba, GDK_COLORSPACE_RGB, TRUE, 8,
                                              SIZE, SIZE, SIZE * 4,
                                              (GdkPixbufDestroyNotify)g_free, NULL);
    if (pb) {
        gchar  *tmp = g_build_filename(g_get_tmp_dir(), "llmtray_badge.png", NULL);
        GError *err = NULL;
        if (gdk_pixbuf_save(pb, tmp, "png", &err, NULL))
            app_indicator_set_icon_full(indicator, tmp, "LLM");
        else {
            g_printerr("Badge save: %s\n", err ? err->message : "?");
            g_clear_error(&err);
        }
        g_free(tmp);
        g_object_unref(pb);
    } else {
        g_free(rgba);
    }
    cairo_surface_destroy(surf);
}


static void update_icon(void)
{
    if (!indicator) return;

    switch (svc_state) {

    case STATE_RUNNING:
        app_indicator_set_icon(
            indicator,
            safe_icon("emblem-ok-symbolic",
                      "object-select-symbolic"));
        break;

    case STATE_STARTING:
    case STATE_STOPPING:
        app_indicator_set_icon(
            indicator,
            safe_icon("system-run-symbolic",
                      "view-refresh-symbolic"));
        break;

    case STATE_STOPPED:
    default:
        app_indicator_set_icon(
            indicator,
            safe_icon("process-stop-symbolic",
                      "window-close-symbolic"));
        break;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
   Menu sensitivity / label
   ════════════════════════════════════════════════════════════════════════════ */
static void update_menu_sensitivity(void)
{
    if (mi_start)   gtk_widget_set_sensitive(mi_start,   svc_state == STATE_STOPPED);
    if (mi_stop)    gtk_widget_set_sensitive(mi_stop,
        svc_state == STATE_RUNNING || svc_state == STATE_STARTING);
    if (mi_restart) gtk_widget_set_sensitive(mi_restart, svc_state == STATE_RUNNING);

    if (mi_status) {
        gchar *up = NULL;
        if (started_at) {
            GDateTime *now = g_date_time_new_now_local();
            gint64 s = g_date_time_difference(now, started_at) / G_TIME_SPAN_SECOND;
            g_date_time_unref(now);
            gint h = (gint)(s / 3600), m = (gint)((s % 3600) / 60), sec = (gint)(s % 60);
            up = h > 0 ? g_strdup_printf("%dh %02dm %02ds", h, m, sec)
               : m > 0 ? g_strdup_printf("%dm %02ds", m, sec)
                       : g_strdup_printf("%ds", sec);
        }
        const gchar *dot = (svc_state == STATE_RUNNING) ? "●"
                         : (svc_state == STATE_STOPPED)  ? "✕" : "◌";
        gchar *lbl = (svc_state == STATE_RUNNING && up && child_pid)
            ? g_strdup_printf("%s Running  PID %d  up %s", dot, child_pid, up)
            : g_strdup_printf("%s %s", dot, STATE_NAME[svc_state]);
        gtk_menu_item_set_label(GTK_MENU_ITEM(mi_status), lbl);
        g_free(lbl);
        g_free(up);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
   Start / stop / restart
   ════════════════════════════════════════════════════════════════════════════ */
static void do_start(void)
{
    if (child_pid != 0) return;
    if (svc_state != STATE_STOPPED) return;

    gchar *errmsg = NULL;
    if (!preflight(&errmsg)) {
        g_printerr("  ❌ Preflight: %s\n", errmsg);
        show_err("Cannot start server", errmsg);
        g_free(errmsg);
        auto_restart = FALSE;
        return;
    }

    /* Derive LD_LIBRARY_PATH from the binary's own directory */
    gchar *bin_dir = g_path_get_dirname(g_config.server_bin);

    gchar  *ps  = g_strdup_printf("%d", g_config.port);
    gchar  *ts  = g_strdup_printf("%d", g_config.threads);
    gchar  *temps = g_strdup_printf("%.2f", g_config.temperature);

    gchar **env = g_get_environ();

    // env = g_environ_setenv(env, "LD_LIBRARY_PATH", bin_dir, TRUE);
    const gchar *old_ld = g_environ_getenv(env, "LD_LIBRARY_PATH");
    gchar *new_ld = old_ld && *old_ld
        ? g_strdup_printf("%s:%s", bin_dir, old_ld)
        : g_strdup(bin_dir);
    env = g_environ_setenv(env, "LD_LIBRARY_PATH", new_ld, TRUE);
    g_free(new_ld);

    g_free(bin_dir);

    /* Build argv dynamically to accommodate optional flags */
    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, g_config.server_bin);
    g_ptr_array_add(argv, (gchar *)"-m");
    g_ptr_array_add(argv, g_config.model_path);
    g_ptr_array_add(argv, (gchar *)"--host");
    g_ptr_array_add(argv, g_config.host);
    g_ptr_array_add(argv, (gchar *)"--port");
    g_ptr_array_add(argv, ps);
    g_ptr_array_add(argv, (gchar *)"-c");
    g_ptr_array_add(argv, g_config.ctx_size);
    g_ptr_array_add(argv, (gchar *)"-t");
    g_ptr_array_add(argv, ts);
    g_ptr_array_add(argv, (gchar *)"--temp");
    g_ptr_array_add(argv, temps);
    if (g_config.flash_attn) {
        g_ptr_array_add(argv, (gchar *)"--flash-attn");
        g_ptr_array_add(argv, (gchar *)"on");
    }
    g_ptr_array_add(argv, NULL);

    GPid    new_pid = 0;
    GError *gerr    = NULL;

    // i discard all logs. Remove some of these flags if you want to debug startup issues. I should add a "verbose logging" mode or something.
    gboolean ok = g_spawn_async(
        NULL, (gchar **)argv->pdata, env,
        G_SPAWN_DO_NOT_REAP_CHILD |
        G_SPAWN_STDOUT_TO_DEV_NULL |
        G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, &new_pid, &gerr);

    g_ptr_array_free(argv, FALSE);
    g_free(ps); g_free(ts); g_free(temps);
    g_strfreev(env);

    if (!ok) {
        show_err("Failed to launch server", gerr ? gerr->message : "?");
        g_clear_error(&gerr);
        return;
    }

    atomic_fetch_add(&spawn_gen, 1);
    child_pid  = new_pid;
    poll_count = 0;

    if (started_at) g_date_time_unref(started_at);
    started_at = g_date_time_new_now_local();

    set_state(STATE_STARTING);
    g_print("  ▶ PID %d — http://%s:%d\n", child_pid, g_config.host, g_config.port);

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

static void on_start(GtkMenuItem *i, gpointer d) { (void)i; (void)d; do_start(); }

static gboolean do_start_wrapper(gpointer data)
{
    (void)data;
    do_start();
    return G_SOURCE_REMOVE;
}

static void on_stop(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    if (child_pid == 0) return;
    pending_restart = FALSE;
    stop_child();
}

static void on_restart(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    if (child_pid == 0) { do_start(); return; }
    pending_restart = TRUE;
    stop_child();
}

static void on_autorestart_toggled(GtkCheckMenuItem *item, gpointer d)
{
    (void)d;
    auto_restart = gtk_check_menu_item_get_active(item);
    if (auto_restart) { restart_count = 0; restart_backoff = RESTART_BACKOFF_BASE; }
}

static void on_quit(GtkMenuItem *i, gpointer d)
{
    (void)i; (void)d;
    pending_restart = FALSE;
    g_atomic_int_set(&shutting_down, TRUE);
    g_main_loop_quit(main_loop);
}

/* ════════════════════════════════════════════════════════════════════════════
   Menu build
   ════════════════════════════════════════════════════════════════════════════ */
static void rebuild_menu(GtkWidget *menu, gpointer data)
{
    (void)data;
    mi_status = mi_start = mi_stop = mi_restart = NULL;

    GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = kids; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    mi_status = gtk_menu_item_new_with_label("…");
    gtk_widget_set_sensitive(mi_status, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_status);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    mi_start = gtk_menu_item_new_with_label("Start");
    g_signal_connect(mi_start, "activate", G_CALLBACK(on_start), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_start);

    mi_stop = gtk_menu_item_new_with_label("Stop");
    g_signal_connect(mi_stop, "activate", G_CALLBACK(on_stop), NULL);
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

    GtkWidget *ms = gtk_menu_item_new_with_label("Settings…");
    g_signal_connect(ms, "activate", G_CALLBACK(on_settings_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), ms);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *mq = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(mq, "activate", G_CALLBACK(on_quit), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mq);

    gtk_widget_show_all(menu);
    update_menu_sensitivity();
}

/* ════════════════════════════════════════════════════════════════════════════
   main
   ════════════════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (!lock_acquire()) return EXIT_FAILURE;

    gtk_init(&argc, &argv);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    notify_init(APP_NAME);
    config_load();

    indicator = app_indicator_new(APP_ID, "dialog-information",
                                  APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    if (!indicator) {
        g_printerr("AppIndicator creation failed.\n");
        lock_release();
        return EXIT_FAILURE;
    }
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title (indicator, APP_NAME);
    update_icon();

    GtkWidget *menu = gtk_menu_new();

    if (pipe(signal_pipe) != 0) {
        g_printerr("Failed to create signal pipe: %s\n", strerror(errno));
        lock_release();
        return EXIT_FAILURE;
    }
    fcntl(signal_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(signal_pipe[1], F_SETFL, O_NONBLOCK);

    GIOChannel *ch = g_io_channel_unix_new(signal_pipe[0]);
    g_io_channel_set_close_on_unref(ch, TRUE);
    g_io_add_watch(ch, G_IO_IN, on_signal_pipe_io, NULL);
    g_io_channel_unref(ch);

    struct sigaction sa = {0};
    sa.sa_handler = on_unix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    g_signal_connect(menu, "deactivate", G_CALLBACK(rebuild_menu), NULL);
    rebuild_menu(menu, NULL);
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    main_loop = g_main_loop_new(NULL, FALSE);
    g_print("%s v5 — %s:%d\n", APP_NAME, g_config.host, g_config.port);
    g_main_loop_run(main_loop);

    /* ── Shutdown ── */
    pending_restart = FALSE;
    g_atomic_int_set(&shutting_down, TRUE);
    if (child_pid) stop_child();

    if (child_pid) {
        g_print("  Waiting for child exit…\n");
        for (int i = 0; i < 500 && child_pid; i++) {
            g_main_context_iteration(NULL, FALSE);
            g_usleep(10000);
        }
    }

    if (started_at)     g_date_time_unref(started_at);
    if (last_stable_at) g_date_time_unref(last_stable_at);
    g_object_unref(indicator);
    notify_uninit();
    curl_global_cleanup();
    server_config_free(&g_config);
    lock_release();

    if (signal_pipe[0] >= 0) close(signal_pipe[0]);
    if (signal_pipe[1] >= 0) close(signal_pipe[1]);

    return EXIT_SUCCESS;
}
