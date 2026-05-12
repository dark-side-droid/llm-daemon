/* curl.c — CURL helpers, metrics polling, readiness checks */
#include "globals.h"
#include <curl/curl.h>
#include <glib.h>
#include <stdatomic.h>

/* ── CURL helpers ───────────────────────────────────────────────────────────── */
size_t curl_write_cb(char *ptr, size_t sz, size_t nm, void *ud)
{
    g_string_append_len((GString *)ud, ptr, (gssize)(sz * nm));
    return sz * nm;
}

gboolean curl_dispatch_fixed(gpointer data)
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

gpointer curl_thread_fn(gpointer data)
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

void launch_curl(const gchar *path, gboolean is_metrics)
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

gboolean on_poll_tick_final(gpointer d)
{
    (void)d;
    launch_curl("/v1/models", FALSE);
    return G_SOURCE_CONTINUE;
}

gboolean on_metrics_tick_final(gpointer d)
{
    (void)d;
    if (svc_state == STATE_RUNNING) launch_curl("/metrics", TRUE);
    return G_SOURCE_CONTINUE;
}

gboolean start_metrics_idle_final(gpointer d)
{
    (void)d;
    if (metrics_timer) g_source_remove(metrics_timer);
    metrics_timer = g_timeout_add(METRICS_INTERVAL_MS, on_metrics_tick_final, NULL);
    return G_SOURCE_REMOVE;
}
