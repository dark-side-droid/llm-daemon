/* globals.h — shared declarations for LLM-Daemon split */
#ifndef GLOBALS_H
#define GLOBALS_H

#include <glib.h>
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

extern const gboolean VALID_TX[STATE_COUNT][STATE_COUNT];
extern const gchar *STATE_NAME[];

/* ── Types ──────────────────────────────────────────────────────────────────── */
typedef struct {
    gchar   *server_bin;
    gchar   *model_path;
    gchar   *host;
    gchar   *ctx_size;
    gint     port;
    gint     threads;
    gdouble  temperature;
    gchar   *rea;
    gboolean flash_attn;
    gboolean no_mmap;
    gboolean tools;
    gboolean no_warmup;
    gboolean no_webui;
    gboolean no_ctx_shift;
    gchar   *ngl;
    gchar   *n_tokens;
    gint     top_k;
    gdouble  top_p;
    gdouble  min_p;
    gchar   *cache_type_k;
    gchar   *cache_type_v;
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
extern ServerConfig g_config;

/* ── Globals ────────────────────────────────────────────────────────────────── */
extern GMainLoop    *main_loop;
extern AppIndicator *indicator;
extern GPid          child_pid;
extern guint         poll_timer;
extern guint         sigkill_timer;
extern guint         metrics_timer;
extern gint          poll_count;
extern ServiceState  svc_state;
extern GDateTime    *started_at;
extern GDateTime    *last_stable_at;
extern gboolean      auto_restart;
extern gboolean      pending_restart;
extern _Atomic guint spawn_gen;
extern gint          restart_count;
extern guint         restart_backoff;
extern gdouble       last_tps;
extern guint         uptime_timer;

extern GtkWidget    *mi_status;
extern GtkWidget    *mi_start;
extern GtkWidget    *mi_stop;
extern GtkWidget    *mi_restart;

extern int           signal_pipe[2];
extern _Atomic gboolean shutting_down;

/* ── Function declarations (forward) ─────────────────────────────────────────── */
void          set_state(ServiceState s);
void          rebuild_menu(GtkWidget *menu, gpointer data);
void          do_start(void);
gboolean      do_start_wrapper(gpointer data);

/* Menu callbacks */
void          on_start(GtkMenuItem *i, gpointer d);
void          on_stop(GtkMenuItem *i, gpointer d);
void          on_restart(GtkMenuItem *i, gpointer d);
void          on_autorestart_toggled(GtkCheckMenuItem *item, gpointer d);
void          on_quit(GtkMenuItem *i, gpointer d);


void          stop_child(void);
gboolean      start_metrics_idle_final(gpointer d);
void          update_menu_sensitivity(void);
void          update_icon(void);
gboolean      port_free(const gchar *host, gint port);
gboolean      on_signal_pipe_io(GIOChannel *ch, GIOCondition cond, gpointer data);
void          on_unix_signal_handler(int sig);
void          show_err(const gchar *title, const gchar *msg);
const gchar  *safe_icon(const gchar *want, const gchar *fallback);
gboolean      escalate_sigkill(gpointer data);
void          on_child_exit(GPid pid, gint status, gpointer data);
gboolean      on_poll_tick_final(gpointer d);
gboolean      on_metrics_tick_final(gpointer d);
gboolean      start_metrics_idle_final(gpointer d);
gboolean      preflight(gchar **errmsg);
void          on_settings_activate(GtkMenuItem *item, gpointer data);
void          on_browse_clicked(GtkButton *btn, gpointer user_data);

/* Config helpers */
gchar *cfg_dir(void);
gchar *cfg_path(void);
void config_migrate(GKeyFile *kf, gint from);
void server_config_free(ServerConfig *c);
void server_config_defaults(ServerConfig *c);
void config_load(void);
void config_save(void);

/* Menu callbacks */
void on_settings_response(GtkDialog *dlg, gint resp, gpointer data);

/* CURL */
size_t curl_write_cb(char *ptr, size_t sz, size_t nm, void *ud);
gboolean curl_dispatch_fixed(gpointer data);
gpointer curl_thread_fn(gpointer data);
void launch_curl(const gchar *path, gboolean is_metrics);

/* Notification */
void notify_send(const gchar *sum, const gchar *body, NotifyUrgency u);

/* Uptime */
gboolean update_uptime(gpointer data);

/* Lock file */
gboolean lock_acquire(void);
void lock_release(void);

/* ── End globals.h ───────────────────────────────────────────────────────────── */

#endif /* GLOBALS_H */
