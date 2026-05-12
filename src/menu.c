/* menu.c — Settings dialog, callbacks, menu rebuild */
#include "globals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ── Helper: add_entry_row ──────────────────────────────────────────────────── */
GtkWidget *add_entry_row(GtkWidget *grid, int row,
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
        g_object_set_data(G_OBJECT(btn), "entry", entry);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_browse_clicked), NULL);
        gtk_grid_attach(GTK_GRID(grid), btn, 2, row, 1, 1);
        if (key) {
            gchar *browse_key = g_strdup_printf("%s_browse", key);
            g_object_set_data_full(G_OBJECT(dlg), browse_key, btn, NULL);
            g_free(browse_key);
        }
    }

    return entry;
}

/* ── Browse button callback ─────────────────────────────────────────────────── */
void on_browse_clicked(GtkButton *btn, gpointer user_data)
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

/* ── Settings response ──────────────────────────────────────────────────────── */
void on_settings_response(GtkDialog *dlg, gint resp, gpointer data)
{
    (void)data;
    if (resp == GTK_RESPONSE_ACCEPT) {
        gchar *server_bin = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "server_bin"))));
        gchar *model_path = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "model_path"))));
        gchar *host = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "host"))));
        gchar *ctx_size = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "ctx_size"))));
        gchar *ngl_copy = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "ngl"))));
        gchar *n_tokens_copy = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "n_tokens"))));
        gchar *cache_type_k = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "cache_type_k"))));
        gchar *cache_type_v = g_strdup(gtk_entry_get_text(
            GTK_ENTRY(g_object_get_data(G_OBJECT(dlg), "cache_type_v"))));
        gchar *rea_copy = gtk_combo_box_text_get_active_text(
            GTK_COMBO_BOX_TEXT(g_object_get_data(G_OBJECT(dlg), "rea")));
        if (!rea_copy) rea_copy = g_strdup("auto");

        gint port = (gint)gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "port")));
        gint threads = (gint)gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "threads")));
        gdouble temperature = gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "temperature")));
        gint top_k = (gint)gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "top_k")));
        gdouble top_p = gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "top_p")));
        gdouble min_p = gtk_spin_button_get_value(
            GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dlg), "min_p")));

        gboolean flash_attn = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(dlg), "flash_attn")));
        gboolean no_mmap = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(dlg), "no_mmap")));
        gboolean tools = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(dlg), "tools")));
        gboolean no_warmup = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(dlg), "no_warmup")));
        gboolean no_webui = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(dlg), "no_webui")));
        gboolean no_ctx_shift = gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(g_object_get_data(G_OBJECT(dlg), "no_ctx_shift")));

        if (port < 1 || port > 65535) {
            GtkWidget *err_dlg = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                "Invalid port: must be 1–65535");
            gtk_dialog_run(GTK_DIALOG(err_dlg));
            gtk_widget_destroy(err_dlg);
            g_free(server_bin); g_free(model_path);
            g_free(host);       g_free(ctx_size);
            g_free(ngl_copy);   g_free(n_tokens_copy);
            g_free(rea_copy);
            gtk_widget_destroy(GTK_WIDGET(dlg));
            return;
        }

        server_config_free(&g_config);

        g_config.server_bin   = server_bin;
        g_config.model_path   = model_path;
        g_config.host         = host;
        g_config.ctx_size     = ctx_size;
        g_config.port         = port;
        g_config.threads      = threads;
        g_config.temperature  = temperature;
        g_config.rea          = rea_copy;
        g_config.flash_attn   = flash_attn;
        g_config.no_mmap      = no_mmap;
        g_config.tools        = tools;
        g_config.no_warmup    = no_warmup;
        g_config.no_webui     = no_webui;
        g_config.no_ctx_shift = no_ctx_shift;
        g_config.ngl          = ngl_copy;
        g_config.n_tokens     = n_tokens_copy;
        g_config.top_k        = top_k;
        g_config.top_p        = top_p;
        g_config.min_p        = min_p;
        g_config.cache_type_k = cache_type_k;
        g_config.cache_type_v = cache_type_v;
        config_save();
    }
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

/* ── Spin row helper ────────────────────────────────────────────────────────── */
GtkWidget *add_spin_row(GtkWidget *grid, int row,
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

/* ── on_settings_activate ───────────────────────────────────────────────────── */
void on_settings_activate(GtkMenuItem *item, gpointer data)
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

    /* Server binary (with Browse) */
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


    /* Model path (with Browse) */
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


    /* Host */
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

    add_spin_row(grid, row++, "Port:", 1, 65535, 1,
                 g_config.port, 0, "port", dlg);
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
    {
        GtkWidget *lbl = gtk_label_new("Max tokens (-n):");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), g_config.n_tokens ? g_config.n_tokens : "");
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 10);
        gtk_widget_set_halign(entry, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "n_tokens", entry);
        row++;
    }
    {
        GtkWidget *lbl = gtk_label_new("GPU layers (-ngl):");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), g_config.ngl ? g_config.ngl : "");
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 20);
        gtk_widget_set_halign(entry, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "ngl", entry);
        row++;
    }
    add_spin_row(grid, row++, "Threads:", 1, 256, 1,
                 g_config.threads, 0, "threads", dlg);
    add_spin_row(grid, row++, "Temperature:", 0.0, 1.0, 0.01,
                 g_config.temperature, 2, "temperature", dlg);

    /* Sampling row */
    {
        GtkWidget *lbl = gtk_label_new("Sampling:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget *sampling_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_halign(sampling_box, GTK_ALIGN_START);

        GtkWidget *top_k_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *top_k_label = gtk_label_new("top-k");
        GtkWidget *top_k_spin = gtk_spin_button_new_with_range(0, 1000, 1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(top_k_spin), 0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(top_k_spin), g_config.top_k);
        gtk_box_pack_start(GTK_BOX(top_k_box), top_k_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(top_k_box), top_k_spin, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(dlg), "top_k", top_k_spin);

        GtkWidget *top_p_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *top_p_label = gtk_label_new("top-p");
        GtkWidget *top_p_spin = gtk_spin_button_new_with_range(0.0, 1.0, 0.01);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(top_p_spin), 2);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(top_p_spin), g_config.top_p);
        gtk_box_pack_start(GTK_BOX(top_p_box), top_p_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(top_p_box), top_p_spin, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(dlg), "top_p", top_p_spin);

        GtkWidget *min_p_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *min_p_label = gtk_label_new("min-p");
        GtkWidget *min_p_spin = gtk_spin_button_new_with_range(0.0, 1.0, 0.01);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(min_p_spin), 2);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(min_p_spin), g_config.min_p);
        gtk_box_pack_start(GTK_BOX(min_p_box), min_p_label, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(min_p_box), min_p_spin, FALSE, FALSE, 0);
        g_object_set_data(G_OBJECT(dlg), "min_p", min_p_spin);

        gtk_box_pack_start(GTK_BOX(sampling_box), top_k_box, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sampling_box), top_p_box, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(sampling_box), min_p_box, FALSE, FALSE, 0);

        gtk_grid_attach(GTK_GRID(grid), sampling_box, 1, row, 1, 1);
        row += 1;
    }

    {
        GtkWidget *lbl = gtk_label_new("CTK (-ctk):");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), g_config.cache_type_k ? g_config.cache_type_k : "");
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 12);
        gtk_widget_set_halign(entry, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "cache_type_k", entry);
        row++;
    }
    {
        GtkWidget *lbl = gtk_label_new("CTV (-ctv):");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), g_config.cache_type_v ? g_config.cache_type_v : "");
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_entry_set_width_chars(GTK_ENTRY(entry), 12);
        gtk_widget_set_halign(entry, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "cache_type_v", entry);
        row++;
    }
    {
        GtkWidget *lbl = gtk_label_new("Reasoning (-rea):");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
        GtkWidget *combo = gtk_combo_box_text_new();
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "on");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "off");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), "auto");
        const gchar *cur_rea = g_config.rea ? g_config.rea : "auto";
        gint idx = 2;
        if (g_strcmp0(cur_rea, "on")  == 0) idx = 0;
        else if (g_strcmp0(cur_rea, "off") == 0) idx = 1;
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), idx);
        gtk_widget_set_halign(combo, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), combo, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "rea", combo);
        row++;
    }

    /* Toggles */
        /* Toggles */
    {
        GtkWidget *lbl = gtk_label_new("Flash attention:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget *chk = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), g_config.flash_attn);
        gtk_widget_set_halign(chk, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), chk, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "flash_attn", chk);
        row++;
    }
    {
        GtkWidget *lbl = gtk_label_new("Disable mmap:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget *chk = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), g_config.no_mmap);
        gtk_widget_set_halign(chk, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), chk, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "no_mmap", chk);
        row++;
    }
    {
        GtkWidget *lbl = gtk_label_new("Enable tools:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget *chk = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), g_config.tools);
        gtk_widget_set_halign(chk, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), chk, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "tools", chk);
        row++;
    }
    {
        GtkWidget *lbl = gtk_label_new("Disable warmup:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget *chk = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), g_config.no_warmup);
        gtk_widget_set_halign(chk, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), chk, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "no_warmup", chk);
        row++;
    }
    {
        GtkWidget *lbl = gtk_label_new("Disable web UI:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget *chk = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), g_config.no_webui);
        gtk_widget_set_halign(chk, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), chk, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "no_webui", chk);
        row++;
    }
    {
        GtkWidget *lbl = gtk_label_new("Disable context shift:");
        gtk_widget_set_halign(lbl, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

        GtkWidget *chk = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), g_config.no_ctx_shift);
        gtk_widget_set_halign(chk, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), chk, 1, row, 1, 1);
        g_object_set_data(G_OBJECT(dlg), "no_ctx_shift", chk);
        row++;
    }


    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
                       grid, TRUE, TRUE, 0);
    g_signal_connect(dlg, "response", G_CALLBACK(on_settings_response), NULL);
    gtk_widget_show_all(dlg);
}
