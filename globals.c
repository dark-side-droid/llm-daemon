/* globals.c — definitions of all global variables */
#include "globals.h"

/* ── Globals ────────────────────────────────────────────────────────────────── */
GMainLoop    *main_loop       = NULL;
AppIndicator *indicator       = NULL;
GPid          child_pid       = 0;
guint         poll_timer      = 0;
guint         sigkill_timer   = 0;
guint         metrics_timer   = 0;
gint          poll_count      = 0;
ServiceState  svc_state       = STATE_STOPPED;
GDateTime    *started_at      = NULL;
GDateTime    *last_stable_at  = NULL;
gboolean      auto_restart    = FALSE;
gboolean      pending_restart = FALSE;
_Atomic guint spawn_gen       = 0;
gint          restart_count   = 0;
guint         restart_backoff = RESTART_BACKOFF_BASE;
gdouble       last_tps        = -1.0;
guint         uptime_timer    = 0;

GtkWidget    *mi_status  = NULL;
GtkWidget    *mi_start   = NULL;
GtkWidget    *mi_stop    = NULL;
GtkWidget    *mi_restart = NULL;

int           signal_pipe[2]  = {-1, -1};
_Atomic gboolean shutting_down = FALSE;

/* ── State machine tables ───────────────────────────────────────────────────── */
const gboolean VALID_TX[STATE_COUNT][STATE_COUNT] = {
    { FALSE,   TRUE,     FALSE,   FALSE },
    { TRUE,    FALSE,    TRUE,    TRUE  },
    { TRUE,    FALSE,    FALSE,   TRUE  },
    { TRUE,    FALSE,    FALSE,   FALSE },
};

const gchar *STATE_NAME[] = { "STOPPED", "STARTING", "RUNNING", "STOPPING" };

/* ── Config ─────────────────────────────────────────────────────────────────── */
ServerConfig g_config = {
    .server_bin = NULL,
    .model_path = NULL,
    .host       = NULL,
    .ctx_size   = NULL,
    .port       = 0,
    .threads    = 0,
    .temperature = 0.0,
    .rea        = NULL,
    .ngl        = NULL,
    .n_tokens   = NULL,
    .cache_type_k = NULL,
    .cache_type_v = NULL,
    .top_k      = 0,
    .top_p      = 0.0,
    .min_p      = 0.0
    // booleans will default to FALSE
};
