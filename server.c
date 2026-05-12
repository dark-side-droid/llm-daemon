/* server.c — do_start(), stop_child(), preflight(), port_free() */
#include "globals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdatomic.h>


/* ── Port check / preflight ─────────────────────────────────────────────────── */
gboolean port_free(const gchar *host, gint port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return TRUE;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = inet_addr(host);
    gboolean is_free = (bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    close(fd);
    return is_free;
}

gboolean preflight(gchar **errmsg)
{
    if (!g_config.server_bin || !*g_config.server_bin)
    {
        *errmsg = g_strdup("Server binary path is not set. Open Settings to configure it.");
        return FALSE;
    }
    if (!g_file_test(g_config.server_bin, G_FILE_TEST_IS_EXECUTABLE))
    {
        *errmsg = g_strdup_printf("Binary not found or not executable:\n%s", g_config.server_bin);
        return FALSE;
    }
    if (!g_config.model_path || !*g_config.model_path)
    {
        *errmsg = g_strdup("Model path is not set. Open Settings to configure it.");
        return FALSE;
    }
    if (!g_file_test(g_config.model_path, G_FILE_TEST_EXISTS))
    {
        *errmsg = g_strdup_printf("Model file not found:\n%s", g_config.model_path);
        return FALSE;
    }
    if (!port_free(g_config.host, g_config.port))
    {
        *errmsg = g_strdup_printf("Port %d on %s is already in use.", g_config.port, g_config.host);
        return FALSE;
    }
    return TRUE;
}

void show_err(const gchar *title, const gchar *msg)
{
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", msg);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

/* ── Start / stop / restart ─────────────────────────────────────────────────── */
void do_start(void)
{
    if (child_pid != 0)
        return;
    if (svc_state != STATE_STOPPED)
        return;

    gchar *errmsg = NULL;
    if (!preflight(&errmsg))
    {
        g_printerr("  ❌ Preflight: %s\n", errmsg);
        show_err("Cannot start server", errmsg);
        g_free(errmsg);
        auto_restart = FALSE;
        return;
    }

    gchar *bin_dir = g_path_get_dirname(g_config.server_bin);
    gchar *ps = g_strdup_printf("%d", g_config.port);
    gchar *ts = g_strdup_printf("%d", g_config.threads);
    gchar *temps = g_strdup_printf("%.2f", g_config.temperature);

    gchar **env = g_get_environ();

    const gchar *old_ld = g_environ_getenv(env, "LD_LIBRARY_PATH");
    gchar *new_ld = old_ld && *old_ld
                        ? g_strdup_printf("%s:%s", bin_dir, old_ld)
                        : g_strdup(bin_dir);
    env = g_environ_setenv(env, "LD_LIBRARY_PATH", new_ld, TRUE);
    g_free(new_ld);
    g_free(bin_dir);

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

    if (g_config.n_tokens && *g_config.n_tokens)
    {
        gchar *endptr;
        long val = strtol(g_config.n_tokens, &endptr, 10);
        if (*endptr == '\0' && val > 0)
        {
            g_ptr_array_add(argv, (gchar *)"-n");
            g_ptr_array_add(argv, g_strdup(g_config.n_tokens));
        }
    }

    g_ptr_array_add(argv, (gchar *)"-rea");
    g_ptr_array_add(argv, g_config.rea ? g_config.rea : "auto");
    g_ptr_array_add(argv, (gchar *)"-t");
    g_ptr_array_add(argv, ts);
    g_ptr_array_add(argv, (gchar *)"--temp");
    g_ptr_array_add(argv, temps);

    if (g_config.top_k != 40)
    {
        g_ptr_array_add(argv, (gchar *)"--top-k");
        g_ptr_array_add(argv, g_strdup_printf("%d", g_config.top_k));
    }
    if (g_config.top_p != 0.95)
    {
        g_ptr_array_add(argv, (gchar *)"--top-p");
        g_ptr_array_add(argv, g_strdup_printf("%.2f", g_config.top_p));
    }
    if (g_config.min_p != 0.05)
    {
        g_ptr_array_add(argv, (gchar *)"--min-p");
        g_ptr_array_add(argv, g_strdup_printf("%.2f", g_config.min_p));
    }
    if (g_config.flash_attn)
    {
        g_ptr_array_add(argv, (gchar *)"--flash-attn");
        g_ptr_array_add(argv, (gchar *)"on");
    }
    if (g_config.no_mmap)
    {
        g_ptr_array_add(argv, (gchar *)"--no-mmap");
    }
    if (g_config.tools)
    {
        g_ptr_array_add(argv, (gchar *)"--tools");
        g_ptr_array_add(argv, (gchar *)"all");
    }
    if (g_config.no_warmup)
    {
        g_ptr_array_add(argv, (gchar *)"--no-warmup");
    }
    if (g_config.no_webui)
    {
        g_ptr_array_add(argv, (gchar *)"--no-webui");
    }
    if (g_config.no_ctx_shift)
    {
        g_ptr_array_add(argv, (gchar *)"--no-context-shift");
    }
    if (g_config.ngl && *g_config.ngl)
    {
        g_ptr_array_add(argv, (gchar *)"-ngl");
        g_ptr_array_add(argv, g_config.ngl);
    }
    if (g_config.cache_type_k && *g_config.cache_type_k)
    {
        g_ptr_array_add(argv, (gchar *)"-ctk");
        g_ptr_array_add(argv, g_config.cache_type_k);
    }
    if (g_config.cache_type_v && *g_config.cache_type_v)
    {
        g_ptr_array_add(argv, (gchar *)"-ctv");
        g_ptr_array_add(argv, g_config.cache_type_v);
    }

    if (g_config.no_webui)
    {
        g_printerr("[llm-daemon] Web UI is disabled (--no-webui).\n");
    }

    g_ptr_array_add(argv, NULL);

    {
        GString *cmd_line = g_string_new("[llm-daemon] Launching with:");
        for (guint i = 0; i < argv->len - 1; i++)
        {
            g_string_append(cmd_line, " ");
            g_string_append(cmd_line, (gchar *)argv->pdata[i]);
        }
        g_printerr("%s\n", cmd_line->str);
        g_string_free(cmd_line, TRUE);
    }

    GPid new_pid = 0;
    GError *gerr = NULL;

    gboolean ok = g_spawn_async(
        NULL, (gchar **)argv->pdata, env,
        G_SPAWN_DO_NOT_REAP_CHILD |
            G_SPAWN_STDOUT_TO_DEV_NULL |
            G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, &new_pid, &gerr);

    g_ptr_array_free(argv, FALSE);
    g_free(ps);
    g_free(ts);
    g_free(temps);
    g_strfreev(env);

    if (!ok)
    {
        show_err("Failed to launch server", gerr ? gerr->message : "?");
        g_clear_error(&gerr);
        return;
    }

    atomic_fetch_add(&spawn_gen, 1);
    child_pid = new_pid;
    poll_count = 0;

    if (started_at)
        g_date_time_unref(started_at);
    started_at = g_date_time_new_now_local();

    set_state(STATE_STARTING);
    g_print("  ▶ PID %d — http://%s:%d\n", child_pid, g_config.host, g_config.port);

    g_child_watch_add(child_pid, on_child_exit, NULL);
    poll_timer = g_timeout_add(POLL_INTERVAL_MS, on_poll_tick_final, NULL);
}

void stop_child(void)
{
    if (poll_timer)
    {
        g_source_remove(poll_timer);
        poll_timer = 0;
    }
    if (metrics_timer)
    {
        g_source_remove(metrics_timer);
        metrics_timer = 0;
    }
    if (child_pid == 0)
        return;
    set_state(STATE_STOPPING);
    g_print("  SIGTERM → PID %d\n", child_pid);
    if (kill(child_pid, SIGTERM) != 0)
    {
        g_printerr("  SIGTERM failed: %s\n", strerror(errno));
        kill(child_pid, SIGKILL);
        return;
    }
    if (sigkill_timer)
        g_source_remove(sigkill_timer);
    sigkill_timer = g_timeout_add(SIGKILL_DELAY_MS, escalate_sigkill, NULL);
}

void on_start(GtkMenuItem *i, gpointer d)
{
    (void)i;
    (void)d;
    do_start();
}

gboolean do_start_wrapper(gpointer data)
{
    (void)data;
    do_start();
    return G_SOURCE_REMOVE;
}

void on_stop(GtkMenuItem *i, gpointer d)
{
    (void)i;
    (void)d;
    if (child_pid == 0)
        return;
    pending_restart = FALSE;
    stop_child();
}

void on_restart(GtkMenuItem *i, gpointer d)
{
    (void)i;
    (void)d;
    if (child_pid == 0)
    {
        do_start();
        return;
    }
    pending_restart = TRUE;
    stop_child();
}

void on_autorestart_toggled(GtkCheckMenuItem *item, gpointer d)
{
    (void)d;
    auto_restart = gtk_check_menu_item_get_active(item);
    if (auto_restart)
    {
        restart_count = 0;
        restart_backoff = RESTART_BACKOFF_BASE;
    }
}

void on_quit(GtkMenuItem *i, gpointer d)
{
    (void)i;
    (void)d;
    pending_restart = FALSE;
    g_atomic_int_set(&shutting_down, TRUE);
    g_main_loop_quit(main_loop);
}
