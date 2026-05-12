/* config.c — Config file handling */
#include "globals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

/* ── Config paths ───────────────────────────────────────────────────────────── */
gchar *cfg_dir(void)
{
    return g_build_filename(g_get_home_dir(), ".config", CONFIG_DIR_NAME, NULL);
}
gchar *cfg_path(void)
{
    return g_build_filename(g_get_home_dir(), ".config", CONFIG_DIR_NAME, CONFIG_FILE_NAME, NULL);
}

/* ── Schema migration ───────────────────────────────────────────────────────── */
void config_migrate(GKeyFile *kf, gint from)
{
    if (from < 1) {
        g_key_file_set_integer(kf, "llm-daemon", "version", 1);
        g_print("Config: migrated v0→v1\n");
    }
}

/* ── ServerConfig helpers ───────────────────────────────────────────────────── */
void server_config_free(ServerConfig *c)
{
    if (!c) return;
    g_clear_pointer(&c->server_bin, g_free);
    g_free(c->model_path);
    g_free(c->host);
    g_free(c->ctx_size);
    g_free(g_config.ngl);
    g_free(g_config.n_tokens);
    g_free(c->cache_type_k);
    g_free(c->cache_type_v);
    g_free(c->rea);
}

void server_config_defaults(ServerConfig *c)
{
    c->server_bin = g_strdup("");
    c->model_path = g_strdup("");
    c->host       = g_strdup(DEFAULT_HOST);
    c->port       = DEFAULT_PORT;
    c->ctx_size   = g_strdup(DEFAULT_CTX_SIZE);
    c->threads    = DEFAULT_THREADS;
    c->temperature = DEFAULT_TEMP;
    c->rea         = g_strdup("auto");
    c->flash_attn = FALSE;
    c->no_mmap     = FALSE;
    c->tools       = FALSE;
    c->no_warmup   = FALSE;
    c->no_webui    = FALSE;
    c->no_ctx_shift= FALSE;
    c->ngl         = NULL;
    c->n_tokens    = NULL;
    c->top_k       = 40;
    c->top_p       = 0.95;
    c->min_p       = 0.05;
    c->cache_type_k= NULL;
    c->cache_type_v= NULL;
}

/* ── Load config ────────────────────────────────────────────────────────────── */
void config_load(void)
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
    g_free(g_config.rea);
    g_config.rea         = KFS("rea",          "auto");
    g_config.flash_attn  = KFB("flash_attn",   FALSE);
    g_config.no_mmap     = KFB("no_mmap",      FALSE);
    g_config.tools       = KFB("tools",        FALSE);
    g_config.no_warmup   = KFB("no_warmup",    FALSE);
    g_config.no_webui    = KFB("no_webui",     FALSE);
    g_config.no_ctx_shift= KFB("no_ctx_shift", FALSE);
    g_free(g_config.ngl);
    g_config.ngl = KFS("ngl", "");
    g_free(g_config.n_tokens);
    g_config.n_tokens = KFS("n_tokens", "");
    g_config.top_k = KFI("top_k", 40);
    g_config.top_p = KFD("top_p", 0.95);
    g_config.min_p = KFD("min_p", 0.05);
    g_config.cache_type_k = KFS("cache_type_k", NULL);
    g_config.cache_type_v = KFS("cache_type_v", NULL);

#undef KFS
#undef KFI
#undef KFD
#undef KFB

    g_key_file_free(kf);
}

/* ── Save config ────────────────────────────────────────────────────────────── */
void config_save(void)
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
    g_key_file_set_string (kf, "llm-daemon", "rea",         g_config.rea         ? g_config.rea         : "auto");
    g_key_file_set_boolean(kf, "llm-daemon", "no_mmap",     g_config.no_mmap);
    g_key_file_set_boolean(kf, "llm-daemon", "tools",       g_config.tools);
    g_key_file_set_boolean(kf, "llm-daemon", "no_warmup",   g_config.no_warmup);
    g_key_file_set_boolean(kf, "llm-daemon", "no_webui",    g_config.no_webui);
    g_key_file_set_boolean(kf, "llm-daemon", "no_ctx_shift",g_config.no_ctx_shift);
    g_key_file_set_string(kf, "llm-daemon", "ngl", g_config.ngl ? g_config.ngl : "");
    g_key_file_set_string(kf, "llm-daemon", "n_tokens", g_config.n_tokens ? g_config.n_tokens : "");
    g_key_file_set_integer(kf, "llm-daemon", "top_k", g_config.top_k);
    g_key_file_set_double(kf, "llm-daemon", "top_p", g_config.top_p);
    g_key_file_set_double(kf, "llm-daemon", "min_p", g_config.min_p);
    g_key_file_set_string(kf, "llm-daemon", "cache_type_k", g_config.cache_type_k ? g_config.cache_type_k : "");
    g_key_file_set_string(kf, "llm-daemon", "cache_type_v", g_config.cache_type_v ? g_config.cache_type_v : "");

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
