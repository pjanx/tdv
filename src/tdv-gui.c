/*
 * StarDict GTK+ UI
 *
 * Copyright (c) 2020 - 2024, Přemysl Eric Janouch <p@janouch.name>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include <stdlib.h>

#include "config.h"
#include "stardict.h"
#include "utils.h"
#include "stardict-view.h"

static struct
{
	GtkWidget    *window;            ///< Top-level window
	GtkWidget    *notebook;          ///< Notebook with tabs
	GtkWidget    *hamburger;         ///< Hamburger menu
	GtkWidget    *entry;             ///< Search entry widget
	GtkWidget    *view;              ///< Entries view

	gint          dictionary;        ///< Index of the current dictionary
	gint          last;              ///< The last dictionary index
	GPtrArray    *dictionaries;      ///< All open dictionaries

	gboolean      loading;           ///< Dictionaries are being loaded

	gboolean      watch_selection;   ///< Following X11 PRIMARY?
}
g;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
load_from_filenames (GPtrArray *out, gchar **filenames)
{
	for (gsize i = 0; filenames[i]; i++)
	{
		Dictionary *dict = g_malloc0 (sizeof *dict);
		dict->filename = g_strdup (filenames[i]);
		g_ptr_array_add (out, dict);
	}
}

// TODO: try to deduplicate, similar to app_load_config_values()
static gboolean
load_from_key_file (GPtrArray *out, GKeyFile *kf, GError **error)
{
	const gchar *dictionaries = "Dictionaries";
	gchar **names = g_key_file_get_keys (kf, dictionaries, NULL, NULL);
	if (!names)
		return TRUE;

	for (gsize i = 0; names[i]; i++)
	{
		Dictionary *dict = g_malloc0 (sizeof *dict);
		dict->name = names[i];
		g_ptr_array_add (out, dict);
	}
	g_free (names);

	for (gsize i = 0; i < out->len; i++)
	{
		Dictionary *dict = g_ptr_array_index (out, i);
		gchar *path =
			g_key_file_get_string (kf, dictionaries, dict->name, error);
		if (!path)
			return FALSE;

		// Try to resolve relative paths and expand tildes
		if (!(dict->filename =
			resolve_filename (path, resolve_relative_config_filename)))
			dict->filename = path;
		else
			g_free (path);
	}
	return TRUE;
}

static gboolean
load_from_config (GPtrArray *out, GError **error)
{
	GKeyFile *key_file = load_project_config_file (error);
	if (!key_file)
		return FALSE;

	gboolean result = load_from_key_file (out, key_file, error);
	g_key_file_free (key_file);
	return result;
}

static void
search (Dictionary *dict)
{
	GtkEntryBuffer *buf = gtk_entry_get_buffer (GTK_ENTRY (g.entry));
	const gchar *input_utf8 = gtk_entry_buffer_get_text (buf);
	if (!dict->dict)
		return;

	StardictIterator *iterator =
		stardict_dict_search (dict->dict, input_utf8, NULL);
	stardict_view_set_position (STARDICT_VIEW (g.view),
		dict->dict, stardict_iterator_get_offset (iterator));
	g_object_unref (iterator);

	stardict_view_set_matched (STARDICT_VIEW (g.view), input_utf8);
}

static void
on_changed (G_GNUC_UNUSED GtkWidget *widget, G_GNUC_UNUSED gpointer data)
{
	search (g_ptr_array_index (g.dictionaries, g.dictionary));
}

static void
on_send (G_GNUC_UNUSED StardictView *view,
	const char *word, G_GNUC_UNUSED gpointer data)
{
	GtkEntryBuffer *buf = gtk_entry_get_buffer (GTK_ENTRY (g.entry));
	gtk_entry_buffer_set_text (buf, word, -1);
	gtk_editable_select_region (GTK_EDITABLE (g.entry), 0, -1);
}

static void
on_selection_received (G_GNUC_UNUSED GtkClipboard *clipboard, const gchar *text,
	G_GNUC_UNUSED gpointer data)
{
	if (!text)
		return;

	gchar *trimmed = g_strstrip (g_strdup (text));
	gtk_entry_set_text (GTK_ENTRY (g.entry), trimmed);
	g_free (trimmed);

	gtk_editable_set_position (GTK_EDITABLE (g.entry), -1);
}

static void
on_selection (GtkClipboard *clipboard, GdkEvent *event,
	G_GNUC_UNUSED gpointer data)
{
	if (g.watch_selection
	 && !gtk_window_has_toplevel_focus (GTK_WINDOW (g.window))
	 && event->owner_change.owner != NULL)
		gtk_clipboard_request_text (clipboard, on_selection_received, NULL);
}

static void
on_selection_watch_toggle (GtkCheckMenuItem *item, G_GNUC_UNUSED gpointer data)
{
	g.watch_selection = gtk_check_menu_item_get_active (item);
}

static void
on_switch_page (G_GNUC_UNUSED GtkWidget *widget, G_GNUC_UNUSED GtkWidget *page,
	guint page_num, G_GNUC_UNUSED gpointer data)
{
	g.last = g.dictionary;
	g.dictionary = page_num;
	search (g_ptr_array_index (g.dictionaries, g.dictionary));

	// Hack: Make right-clicking notebook arrows also re-focus the entry.
	GdkEvent *event = gtk_get_current_event ();
	if (event && event->type == GDK_BUTTON_PRESS)
		gtk_widget_grab_focus (g.entry);
}

static gboolean
accelerate_hamburger (GdkEvent *event)
{
	gchar *accelerator = NULL;
	g_object_get (gtk_widget_get_settings (g.window), "gtk-menu-bar-accel",
		&accelerator, NULL);
	if (!accelerator)
		return FALSE;

	guint key = 0;
	GdkModifierType mods = 0;
	gtk_accelerator_parse (accelerator, &key, &mods);
	g_free (accelerator);

	guint mask = gtk_accelerator_get_default_mod_mask ();
	if (!key || event->key.keyval != key || (event->key.state & mask) != mods)
		return FALSE;

	gtk_button_clicked (GTK_BUTTON (g.hamburger));
	return TRUE;
}

static gboolean
on_key_press (G_GNUC_UNUSED GtkWidget *widget, GdkEvent *event,
	G_GNUC_UNUSED gpointer data)
{
	// The "activate" signal of the GtkMenuButton cannot be used
	// from a real accelerator, due to "no trigger event for menu popup".
	if (accelerate_hamburger (event))
		return TRUE;

	GtkNotebook *notebook = GTK_NOTEBOOK (g.notebook);
	guint mods = event->key.state & gtk_accelerator_get_default_mod_mask ();
	if (mods == GDK_CONTROL_MASK)
	{
		// Can't use gtk_widget_add_accelerator() to change-current-page(-1/+1)
		// because that signal has arguments, which cannot be passed.
		gint current = gtk_notebook_get_current_page (notebook);
		if (event->key.keyval == GDK_KEY_Page_Up)
			return gtk_notebook_set_current_page (notebook, --current), TRUE;
		if (event->key.keyval == GDK_KEY_Page_Down)
			return gtk_notebook_set_current_page (notebook,
				++current % gtk_notebook_get_n_pages (notebook)), TRUE;
	}
	if (mods == GDK_MOD1_MASK)
	{
		if (event->key.keyval >= GDK_KEY_0
		 && event->key.keyval <= GDK_KEY_9)
		{
			gint n = event->key.keyval - GDK_KEY_0;
			gtk_notebook_set_current_page (notebook, (n ? n : 10) - 1);
			return TRUE;
		}
		if (event->key.keyval == GDK_KEY_Tab)
		{
			gtk_notebook_set_current_page (notebook, g.last);
			return TRUE;
		}
	}
	if (mods == 0)
	{
		StardictView *view = STARDICT_VIEW (g.view);
		if (event->key.keyval == GDK_KEY_Page_Up)
			return stardict_view_scroll (view, GTK_SCROLL_PAGES, -0.5), TRUE;
		if (event->key.keyval == GDK_KEY_Page_Down)
			return stardict_view_scroll (view, GTK_SCROLL_PAGES, +0.5), TRUE;
		if (event->key.keyval == GDK_KEY_Up)
			return stardict_view_scroll (view, GTK_SCROLL_STEPS, -1), TRUE;
		if (event->key.keyval == GDK_KEY_Down)
			return stardict_view_scroll (view, GTK_SCROLL_STEPS, +1), TRUE;
	}
	return FALSE;
}

static gboolean
on_tab_focus (G_GNUC_UNUSED GtkWidget *widget,
	G_GNUC_UNUSED GtkDirectionType direction, G_GNUC_UNUSED gpointer user_data)
{
	// Hack: Make it so that tab headers don't retain newly gained focus
	// when clicked, re-focus the entry instead.
	GdkEvent *event = gtk_get_current_event ();
	if (!event || event->type != GDK_BUTTON_PRESS
		|| event->button.button != GDK_BUTTON_PRIMARY)
		return FALSE;

	gtk_widget_grab_focus (g.entry);
	return TRUE;
}

static void
init_tabs (void)
{
	for (gsize i = g.dictionaries->len; i--; )
	{
		Dictionary *dict = g_ptr_array_index (g.dictionaries, i);
		GtkWidget *dummy = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		g_signal_connect (dummy, "focus", G_CALLBACK (on_tab_focus), NULL);
		GtkWidget *label = gtk_label_new (dict->name);
		gtk_notebook_insert_page (GTK_NOTEBOOK (g.notebook), dummy, label, 0);
	}

	gtk_widget_show_all (g.notebook);
	gtk_widget_grab_focus (g.entry);
}

static void
show_error_dialog (GError *error)
{
	GtkWidget *dialog = gtk_message_dialog_new (GTK_WINDOW (g.window), 0,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", error->message);
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
	g_error_free (error);
}

// --- Loading -----------------------------------------------------------------

static void
on_new_dictionaries_loaded (G_GNUC_UNUSED GObject* source_object,
	GAsyncResult* res, G_GNUC_UNUSED gpointer user_data)
{
	g.loading = FALSE;

	GError *error = NULL;
	GPtrArray *new_dictionaries =
		g_task_propagate_pointer (G_TASK (res), &error);
	if (!new_dictionaries)
	{
		show_error_dialog (error);
		return;
	}

	while (gtk_notebook_get_n_pages (GTK_NOTEBOOK (g.notebook)))
		gtk_notebook_remove_page (GTK_NOTEBOOK (g.notebook), -1);

	g.dictionary = -1;
	if (g.dictionaries)
		g_ptr_array_free (g.dictionaries, TRUE);

	stardict_view_set_position (STARDICT_VIEW (g.view), NULL, 0);
	g.dictionaries = new_dictionaries;
	init_tabs ();
}

static void
on_reload_dictionaries_task (GTask *task, G_GNUC_UNUSED gpointer source_object,
	gpointer task_data, G_GNUC_UNUSED GCancellable *cancellable)
{
	GError *error = NULL;
	if (load_dictionaries (task_data, &error))
	{
		g_task_return_pointer (task,
			g_ptr_array_ref (task_data), (GDestroyNotify) g_ptr_array_unref);
	}
	else
		g_task_return_error (task, error);
}

static gboolean
reload_dictionaries (GPtrArray *new_dictionaries, GError **error)
{
	// TODO: We could cancel that task.
	if (g.loading)
	{
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"already loading dictionaries");
		return FALSE;
	}

	// TODO: Some other kind of indication.
	//   Note that "action widgets" aren't visible without GtkNotebook tabs.
	g.loading = TRUE;

	GTask *task = g_task_new (NULL, NULL, on_new_dictionaries_loaded, NULL);
	g_task_set_name (task, __func__);
	g_task_set_task_data (task,
		new_dictionaries, (GDestroyNotify) g_ptr_array_unref);
	g_task_run_in_thread (task, on_reload_dictionaries_task);
	g_object_unref (task);
	return TRUE;
}

static GtkWidget *
new_open_dialog (void)
{
	// The default is local-only.  Paths are returned absolute.
	GtkWidget *dialog = gtk_file_chooser_dialog_new (_("Open dictionary"),
		GTK_WINDOW (g.window), GTK_FILE_CHOOSER_ACTION_OPEN,
		_("_Cancel"), GTK_RESPONSE_CANCEL,
		_("_Open"), GTK_RESPONSE_ACCEPT, NULL);

	GtkFileFilter *filter = gtk_file_filter_new ();
	gtk_file_filter_add_pattern (filter, "*.ifo");
	gtk_file_filter_set_name (filter, "*.ifo");
	GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
	gtk_file_chooser_add_filter (chooser, filter);
	gtk_file_chooser_set_select_multiple (chooser, TRUE);
	return dialog;
}

static void
on_open (G_GNUC_UNUSED GtkMenuItem *item, G_GNUC_UNUSED gpointer data)
{
	GtkWidget *dialog = new_open_dialog ();
	GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
	GPtrArray *new_dictionaries =
		g_ptr_array_new_with_free_func ((GDestroyNotify) dictionary_destroy);
	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
	{
		GSList *paths = gtk_file_chooser_get_filenames (chooser);
		for (GSList *iter = paths; iter; iter = iter->next)
		{
			Dictionary *dict = g_malloc0 (sizeof *dict);
			dict->filename = iter->data;
			g_ptr_array_add (new_dictionaries, dict);
		}
		g_slist_free (paths);
	}

	gtk_widget_destroy (dialog);

	GError *error = NULL;
	if (!new_dictionaries->len
	 || !reload_dictionaries (new_dictionaries, &error))
		g_ptr_array_free (new_dictionaries, TRUE);

	if (error)
		show_error_dialog (error);
}

static void
on_drag_data_received (G_GNUC_UNUSED GtkWidget *widget, GdkDragContext *context,
	G_GNUC_UNUSED gint x, G_GNUC_UNUSED gint y, GtkSelectionData *data,
	G_GNUC_UNUSED guint info, guint time, G_GNUC_UNUSED gpointer user_data)
{
	GError *error = NULL;
	gchar **dropped_uris = gtk_selection_data_get_uris (data);
	if (!dropped_uris)
		return;

	GPtrArray *new_dictionaries =
		g_ptr_array_new_with_free_func ((GDestroyNotify) dictionary_destroy);
	for (gsize i = 0; !error && dropped_uris[i]; i++)
	{
		Dictionary *dict = g_malloc0 (sizeof *dict);
		dict->filename = g_filename_from_uri (dropped_uris[i], NULL, &error);
		g_ptr_array_add (new_dictionaries, dict);
	}

	g_strfreev (dropped_uris);
	if (!new_dictionaries->len
	 || !reload_dictionaries (new_dictionaries, &error))
		g_ptr_array_free (new_dictionaries, TRUE);

	gtk_drag_finish (context, error == NULL, FALSE, time);

	if (error)
		show_error_dialog (error);
}

// --- Settings ----------------------------------------------------------------

typedef struct settings_data            SettingsData;

enum
{
	SETTINGS_COLUMN_NAME,
	SETTINGS_COLUMN_PATH,
	SETTINGS_COLUMN_COUNT
};

struct settings_data
{
	GKeyFile *key_file;                 ///< Configuration file
	GtkTreeModel *model;                ///< GtkListStore
};

static void
settings_load (SettingsData *data)
{
	// We want to keep original comments, as well as any other data.
	GError *error = NULL;
	data->key_file = load_project_config_file (&error);
	if (!data->key_file)
	{
		if (error)
			show_error_dialog (error);
		data->key_file = g_key_file_new ();
	}

	GtkListStore *list_store = gtk_list_store_new (SETTINGS_COLUMN_COUNT,
		G_TYPE_STRING, G_TYPE_STRING);
	data->model = GTK_TREE_MODEL (list_store);

	const gchar *dictionaries = "Dictionaries";
	gchar **names =
		g_key_file_get_keys (data->key_file, dictionaries, NULL, NULL);
	if (!names)
		return;

	for (gsize i = 0; names[i]; i++)
	{
		gchar *path = g_key_file_get_string (data->key_file,
			dictionaries, names[i], NULL);
		if (!path)
			continue;

		GtkTreeIter iter = { 0 };
		gtk_list_store_append (list_store, &iter);
		gtk_list_store_set (list_store, &iter,
			SETTINGS_COLUMN_NAME, names[i], SETTINGS_COLUMN_PATH, path, -1);
		g_free (path);
	}
	g_strfreev (names);
}

static void
settings_save (SettingsData *data)
{
	const gchar *dictionaries = "Dictionaries";
	g_key_file_remove_group (data->key_file, dictionaries, NULL);

	GtkTreeIter iter = { 0 };
	gboolean valid = gtk_tree_model_get_iter_first (data->model, &iter);
	while (valid)
	{
		gchar *name = NULL, *path = NULL;
		gtk_tree_model_get (data->model, &iter,
			SETTINGS_COLUMN_NAME, &name, SETTINGS_COLUMN_PATH, &path, -1);
		if (name && path)
			g_key_file_set_string (data->key_file, dictionaries, name, path);
		g_free (name);
		g_free (path);

		valid = gtk_tree_model_iter_next (data->model, &iter);
	}

	GError *e = NULL;
	if (!save_project_config_file (data->key_file, &e))
		show_error_dialog (e);
}

static void
on_settings_name_edited (G_GNUC_UNUSED GtkCellRendererText *cell,
	const gchar *path_string, const gchar *new_text, gpointer data)
{
	GtkTreeModel *model = GTK_TREE_MODEL (data);
	GtkTreePath *path = gtk_tree_path_new_from_string (path_string);
	GtkTreeIter iter = { 0 };
	gtk_tree_model_get_iter (model, &iter, path);
	gtk_list_store_set (GTK_LIST_STORE (model), &iter,
		SETTINGS_COLUMN_NAME, new_text, -1);
	gtk_tree_path_free (path);
}

static void
on_settings_path_edited (G_GNUC_UNUSED GtkCellRendererText *cell,
	const gchar *path_string, const gchar *new_text, gpointer data)
{
	GtkTreeModel *model = GTK_TREE_MODEL (data);
	GtkTreePath *path = gtk_tree_path_new_from_string (path_string);
	GtkTreeIter iter = { 0 };
	gtk_tree_model_get_iter (model, &iter, path);
	gtk_list_store_set (GTK_LIST_STORE (model), &iter,
		SETTINGS_COLUMN_PATH, new_text, -1);
	gtk_tree_path_free (path);
}

static void
on_settings_add (G_GNUC_UNUSED GtkButton *button, gpointer user_data)
{
	GtkWidget *dialog = new_open_dialog ();
	GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);

	GSList *paths = NULL;
	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
		paths = gtk_file_chooser_get_filenames (chooser);
	gtk_widget_destroy (dialog);
	// When the dialog is aborted, we simply add an empty list.

	GtkTreeView *tree_view = GTK_TREE_VIEW (user_data);
	gtk_tree_selection_unselect_all (gtk_tree_view_get_selection (tree_view));
	GtkTreeModel *model = gtk_tree_view_get_model (tree_view);
	GtkListStore *list_store = GTK_LIST_STORE (model);

	const gchar *home = g_get_home_dir ();
	for (GSList *iter = paths; iter; iter = iter->next)
	{
		GError *error = NULL;
		StardictInfo *ifo = stardict_info_new (iter->data, &error);
		g_free (iter->data);
		if (!ifo)
		{
			show_error_dialog (error);
			continue;
		}

		// We also expand tildes, even on Windows, so no problem there.
		const gchar *path = stardict_info_get_path (ifo);
		gchar *tildified = g_str_has_prefix (stardict_info_get_path (ifo), home)
			? g_strdup_printf ("~%s", path + strlen (home))
			: g_strdup (path);

		GtkTreeIter iter = { 0 };
		gtk_list_store_append (list_store, &iter);
		gtk_list_store_set (list_store, &iter,
			SETTINGS_COLUMN_NAME, stardict_info_get_book_name (ifo),
			SETTINGS_COLUMN_PATH, tildified, -1);
		g_free (tildified);
		stardict_info_free (ifo);
	}
	g_slist_free (paths);
}

static void
on_settings_remove (G_GNUC_UNUSED GtkButton *button, gpointer user_data)
{
	GtkTreeView *tree_view = GTK_TREE_VIEW (user_data);
	GtkTreeSelection *selection = gtk_tree_view_get_selection (tree_view);
	GtkTreeModel *model = gtk_tree_view_get_model (tree_view);
	GtkListStore *list_store = GTK_LIST_STORE (model);

	GList *selected = gtk_tree_selection_get_selected_rows (selection, &model);
	for (GList *iter = selected; iter; iter = iter->next)
	{
		GtkTreePath *path = iter->data;
		iter->data = gtk_tree_row_reference_new (model, path);
		gtk_tree_path_free (path);
	}
	for (GList *iter = selected; iter; iter = iter->next)
	{
		GtkTreePath *path = gtk_tree_row_reference_get_path (iter->data);
		if (path)
		{
			GtkTreeIter tree_iter = { 0 };
			if (gtk_tree_model_get_iter (model, &tree_iter, path))
				gtk_list_store_remove (list_store, &tree_iter);
			gtk_tree_path_free (path);
		}
	}
	g_list_free_full (selected, (GDestroyNotify) gtk_tree_row_reference_free);
}

static void
on_settings_selection_changed
	(GtkTreeSelection* selection, gpointer user_data)
{
	GtkWidget *remove = GTK_WIDGET (user_data);
	gtk_widget_set_sensitive (remove,
		gtk_tree_selection_count_selected_rows (selection) > 0);
}

static void
on_settings (G_GNUC_UNUSED GtkMenuItem *item, G_GNUC_UNUSED gpointer data)
{
	SettingsData sd = {};
	settings_load (&sd);

	GtkWidget *treeview = gtk_tree_view_new_with_model (sd.model);
	gtk_tree_view_set_reorderable (GTK_TREE_VIEW (treeview), TRUE);
	g_object_unref (sd.model);

	GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
	g_object_set (renderer, "editable", TRUE, NULL);
	g_signal_connect (renderer, "edited",
		G_CALLBACK (on_settings_name_edited), sd.model);
	GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes
		(_("Name"), renderer, "text", SETTINGS_COLUMN_NAME, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (treeview), column);

	renderer = gtk_cell_renderer_text_new ();
	g_object_set (renderer, "editable", TRUE, NULL);
	g_signal_connect (renderer, "edited",
		G_CALLBACK (on_settings_path_edited), sd.model);
	column = gtk_tree_view_column_new_with_attributes
		(_("Path"), renderer, "text", SETTINGS_COLUMN_PATH, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (treeview), column);

	GtkWidget *scrolled = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_shadow_type
		(GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_ETCHED_IN);
	gtk_container_add (GTK_CONTAINER (scrolled), treeview);
	GtkWidget *dialog = gtk_dialog_new_with_buttons (_("Settings"),
		GTK_WINDOW (g.window),
		GTK_DIALOG_MODAL,
		_("_Cancel"), GTK_RESPONSE_CANCEL,
		_("_Save"), GTK_RESPONSE_ACCEPT,
		NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);
	gtk_window_set_default_size (GTK_WINDOW (dialog), 600, 400);

	GtkWidget *remove = gtk_button_new_with_mnemonic (_("_Remove"));
	gtk_widget_set_sensitive (remove, FALSE);
	g_signal_connect (remove, "clicked",
		G_CALLBACK (on_settings_remove), treeview);

	GtkWidget *add = gtk_button_new_with_mnemonic (_("_Add..."));
	g_signal_connect (add, "clicked",
		G_CALLBACK (on_settings_add), treeview);

	GtkTreeSelection *selection =
		gtk_tree_view_get_selection (GTK_TREE_VIEW (treeview));
	gtk_tree_selection_set_mode (selection, GTK_SELECTION_MULTIPLE);
	g_signal_connect (selection, "changed",
		G_CALLBACK (on_settings_selection_changed), remove);

	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_box_pack_start (GTK_BOX (box),
		gtk_label_new (_("Here you can configure the default dictionaries.")),
		FALSE, FALSE, 0);
	gtk_box_pack_end (GTK_BOX (box), remove, FALSE, FALSE, 0);
	gtk_box_pack_end (GTK_BOX (box), add, FALSE, FALSE, 0);

	GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
	g_object_set (content_area, "margin", 12, NULL);
	gtk_box_pack_start (GTK_BOX (content_area), box, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (content_area), scrolled, TRUE, TRUE, 12);

	gtk_widget_show_all (dialog);
	switch (gtk_dialog_run (GTK_DIALOG (dialog)))
	{
	case GTK_RESPONSE_NONE:
		break;
	case GTK_RESPONSE_ACCEPT:
		settings_save (&sd);
		// Fall through
	default:
		gtk_widget_destroy (dialog);
	}
	g_key_file_free (sd.key_file);
}

// --- Main --------------------------------------------------------------------

static void
on_destroy (G_GNUC_UNUSED GtkWidget *widget, G_GNUC_UNUSED gpointer data)
{
	gtk_main_quit ();
}

static void
die_with_dialog (const gchar *message)
{
	GtkWidget *dialog = gtk_message_dialog_new (NULL, 0,
		GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message);
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
	exit (EXIT_FAILURE);
}

int
gui_main (char *argv[])
{
	// Just like with GtkApplication, argv has been parsed by the option group.
	gtk_init (NULL, NULL);

	gtk_window_set_default_icon_name (PROJECT_NAME);

	GError *error = NULL;
	GPtrArray *new_dictionaries =
		g_ptr_array_new_with_free_func ((GDestroyNotify) dictionary_destroy);
	if (argv[0])
		load_from_filenames (new_dictionaries, argv);
	else if (!load_from_config (new_dictionaries, &error) && error)
		die_with_dialog (error->message);

	if (!new_dictionaries->len)
	{
		GtkWidget *dialog = gtk_message_dialog_new (NULL, 0,
			GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s",
			_("No dictionaries found either in "
			"the configuration or on the command line"));
		gtk_dialog_run (GTK_DIALOG (dialog));
		gtk_widget_destroy (dialog);

		// This is better than nothing.
		// Our GtkNotebook action widget would be invisible without any tabs.
		on_settings (NULL, NULL);
		exit (EXIT_SUCCESS);
	}

	// Some Adwaita stupidity, plus defaults for our own widget.
	// All the named colours have been there since GNOME 3.4
	// (see gnome-extra-themes git history, Adwaita used to live there).
	const char *style = "notebook header tab { padding: 2px 8px; margin: 0; }"
		// `gsettings set org.gnome.desktop.interface gtk-key-theme "Emacs"`
		// isn't quite what I want, and note that ^U works by default
		"@binding-set Readline {"
			"bind '<Control>H' { 'delete-from-cursor' (chars, -1) };"
			"bind '<Control>W' { 'delete-from-cursor' (word-ends, -1) }; }"
		"entry { -gtk-key-bindings: Readline; border-radius: 0; }"
		"stardict-view { padding: 0 .25em; }"
		"stardict-view.odd {"
			"background: @theme_base_color; "
			"color: @theme_text_color; }"
		"stardict-view.odd:backdrop {"
			"background: @theme_unfocused_base_color; "
			"color: @theme_fg_color; /* should be more faded than 'text' */ }"
		"stardict-view.even {"
			"background: mix(@theme_base_color, @theme_text_color, 0.03); "
			"color: @theme_text_color; }"
		"stardict-view.even:backdrop {"
			"background: mix(@theme_unfocused_base_color, "
				"@theme_fg_color, 0.03); "
			"color: @theme_fg_color; /* should be more faded than 'text' */ }"
		"stardict-view:selected {"
			"background-color: @theme_selected_bg_color; "
			"color: @theme_selected_fg_color; }";

	GdkScreen *screen = gdk_screen_get_default ();
	GtkCssProvider *provider = gtk_css_provider_new ();
	gtk_css_provider_load_from_data (provider, style, strlen (style), NULL);
	gtk_style_context_add_provider_for_screen (screen,
		GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	GtkWidget *item_open = gtk_menu_item_new_with_mnemonic (_("_Open..."));
	g_signal_connect (item_open, "activate", G_CALLBACK (on_open), NULL);

	GtkWidget *item_settings = gtk_menu_item_new_with_mnemonic (_("_Settings"));
	g_signal_connect (item_settings, "activate",
		G_CALLBACK (on_settings), NULL);

	g.watch_selection = TRUE;
	GtkWidget *item_selection =
		gtk_check_menu_item_new_with_mnemonic (_("_Follow selection"));
	gtk_check_menu_item_set_active
		(GTK_CHECK_MENU_ITEM (item_selection), g.watch_selection);
	g_signal_connect (item_selection, "toggled",
		G_CALLBACK (on_selection_watch_toggle), NULL);

	GtkWidget *menu = gtk_menu_new ();
	gtk_widget_set_halign (menu, GTK_ALIGN_END);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item_open);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item_settings);
#ifndef G_OS_WIN32
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item_selection);
#endif  // ! G_OS_WIN32
	gtk_widget_show_all (menu);

	g.hamburger = gtk_menu_button_new ();
	gtk_button_set_relief (GTK_BUTTON (g.hamburger), GTK_RELIEF_NONE);
	gtk_button_set_image (GTK_BUTTON (g.hamburger), gtk_image_new_from_icon_name
		("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
	gtk_menu_button_set_popup (GTK_MENU_BUTTON (g.hamburger), menu);
	gtk_widget_show (g.hamburger);

	g.notebook = gtk_notebook_new ();
	g_signal_connect (g.notebook, "switch-page",
		G_CALLBACK (on_switch_page), NULL);
	gtk_notebook_set_scrollable (GTK_NOTEBOOK (g.notebook), TRUE);
	gtk_notebook_set_action_widget
		(GTK_NOTEBOOK (g.notebook), g.hamburger, GTK_PACK_END);

	g.entry = gtk_search_entry_new ();
	g_signal_connect (g.entry, "changed", G_CALLBACK (on_changed), g.view);

	g.window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title (GTK_WINDOW (g.window), PROJECT_NAME);
	gtk_window_set_default_size (GTK_WINDOW (g.window), 300, 600);
	g_signal_connect (g.window, "destroy",
		G_CALLBACK (on_destroy), NULL);
	g_signal_connect (g.window, "key-press-event",
		G_CALLBACK (on_key_press), NULL);

	GtkWidget *superbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
	gtk_container_add (GTK_CONTAINER (g.window), superbox);
	gtk_container_add (GTK_CONTAINER (superbox), g.notebook);
	gtk_container_add (GTK_CONTAINER (superbox), g.entry);
	gtk_container_add (GTK_CONTAINER (superbox),
		gtk_separator_new (GTK_ORIENTATION_HORIZONTAL));

	g.view = stardict_view_new ();
	gtk_box_pack_end (GTK_BOX (superbox), g.view, TRUE, TRUE, 0);

	GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_PRIMARY);
	g_signal_connect (clipboard, "owner-change",
		G_CALLBACK (on_selection), NULL);

	gtk_drag_dest_set (g.view,
		GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
	gtk_drag_dest_add_uri_targets (g.view);
	g_signal_connect (g.view, "drag-data-received",
		G_CALLBACK (on_drag_data_received), NULL);
	g_signal_connect (g.view, "send",
		G_CALLBACK (on_send), NULL);

	if (!reload_dictionaries (new_dictionaries, &error))
		die_with_dialog (error->message);

	gtk_widget_show_all (g.window);
	gtk_main ();
	return 0;
}
