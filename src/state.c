/* state.c — state machine, icon, menu sensitivity */
#include "globals.h"

/* ── Core: state machine ────────────────────────────────────────────────────── */
void set_state(ServiceState ns)
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

/* ── Icon / badge ───────────────────────────────────────────────────────────── */
const gchar *safe_icon(const gchar *want, const gchar *fallback)
{
    return gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), want)
           ? want : fallback;
}

void update_icon(void)
{
    if (!indicator) return;

    switch (svc_state) {
    case STATE_RUNNING:
        app_indicator_set_icon(indicator,
            safe_icon("emblem-ok-symbolic", "object-select-symbolic"));
        break;
    case STATE_STARTING:
    case STATE_STOPPING:
        app_indicator_set_icon(indicator,
            safe_icon("system-run-symbolic", "view-refresh-symbolic"));
        break;
    case STATE_STOPPED:
    default:
        app_indicator_set_icon(indicator,
            safe_icon("system-software-update-symbolic", "window-close-symbolic"));
        break;
    }
}

/* ── Menu sensitivity / label ───────────────────────────────────────────────── */
void update_menu_sensitivity(void)
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

/* ── Menu build ─────────────────────────────────────────────────────────────── */
void rebuild_menu(GtkWidget *menu, gpointer data)
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
