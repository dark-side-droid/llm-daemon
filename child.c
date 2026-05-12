/* child.c — on_child_exit(), escalate_sigkill() */
#include "globals.h"
#include <sys/wait.h>
#include <stdatomic.h>

/* ── SIGKILL escalation ─────────────────────────────────────────────────────── */
gboolean escalate_sigkill(gpointer data)
{
    (void)data;
    sigkill_timer = 0;
    if (child_pid == 0) return G_SOURCE_REMOVE;
    g_printerr("  ⚠ SIGKILL → PID %d\n", child_pid);
    kill(child_pid, SIGKILL);
    return G_SOURCE_REMOVE;
}

/* ── Child-exit handler ─────────────────────────────────────────────────────── */
void on_child_exit(GPid pid, gint status, gpointer data)
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
