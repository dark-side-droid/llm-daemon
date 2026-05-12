/* notify.c — notifications & uptime ticker */
#include "globals.h"

void notify_send(const gchar *sum, const gchar *body, NotifyUrgency u)
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

/* ── Uptime ticker ──────────────────────────────────────────────────────────── */
gboolean update_uptime(gpointer data)
{
    (void)data;
    if (svc_state == STATE_RUNNING) update_menu_sensitivity();
    return G_SOURCE_CONTINUE;
}
