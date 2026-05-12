#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>              // ← for SA_RESTART
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <curl/curl.h>           // ← for curl_global_init/cleanup

#include "globals.h"

/* ── Lock file ──────────────────────────────────────────────────────────────── */
gboolean lock_acquire(void)
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
        unlink(LOCK_FILE);
        fd = open(LOCK_FILE, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0) {
            g_printerr("Cannot create lock file: %s\n", strerror(errno));
            return FALSE;
        }
    }
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", getpid());
    if (n < 0 || (size_t)n >= sizeof(buf)) { close(fd); unlink(LOCK_FILE); return FALSE; }
    if ((size_t)n != (size_t)write(fd, buf, (size_t)n)) { close(fd); unlink(LOCK_FILE); return FALSE; }
    close(fd);
    return TRUE;
}

void lock_release(void) { unlink(LOCK_FILE); }

/* ── Signal handler ─────────────────────────────────────────────────────────── */
void on_unix_signal_handler(int sig)
{
    (void)sig;
    const char byte = 1;
    //(void)write(signal_pipe[1], &byte, 1);
    ssize_t __attribute__((unused)) w = write(signal_pipe[1], &byte, 1);
}

gboolean on_signal_pipe_io(GIOChannel *ch, GIOCondition cond, gpointer data)
{
    (void)ch; (void)data; (void)cond;
    char buf[16];
    while (read(signal_pipe[0], buf, sizeof(buf)) > 0);
    if (main_loop) g_main_loop_quit(main_loop);
    return G_SOURCE_CONTINUE;
}

/* ── main ───────────────────────────────────────────────────────────────────── */
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

    /* Shutdown */
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
