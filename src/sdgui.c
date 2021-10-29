/*
 * StarDict GTK+ UI
 *
 * Copyright (c) 2020 - 2021, Přemysl Eric Janouch <p@janouch.name>
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

#include <locale.h>
#include <stdlib.h>

#include "config.h"
#include "stardict.h"
#include "utils.h"
#include "stardict-view.h"

#undef PROJECT_NAME
#define PROJECT_NAME "sdgui"

static struct
{
	GtkWidget    *window;            ///< Top-level window
	GtkWidget    *notebook;          ///< Notebook with tabs
	GtkWidget    *hamburger;         ///< Hamburger menu
	GtkWidget    *entry;             ///< Search entry widget
	GtkWidget    *view;              ///< Entries view

	gint          dictionary;        ///< Index of the current dictionary
	GPtrArray    *dictionaries;      ///< All open dictionaries

	gboolean      watch_selection;   ///< Following X11 PRIMARY?
}
g;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
init (gchar **filenames)
{
	for (gsize i = 0; filenames[i]; i++)
	{
		Dictionary *dict = g_malloc0 (sizeof *dict);
		dict->filename = g_strdup (filenames[i]);
		g_ptr_array_add (g.dictionaries, dict);
	}
}

// TODO: try to deduplicate, similar to app_load_config_values()
static gboolean
init_from_key_file (GKeyFile *kf, GError **error)
{
	const gchar *dictionaries = "Dictionaries";
	gchar **names = g_key_file_get_keys (kf, dictionaries, NULL, NULL);
	if (!names)
		return TRUE;

	for (gsize i = 0; names[i]; i++)
	{
		Dictionary *dict = g_malloc0 (sizeof *dict);
		dict->name = names[i];
		g_ptr_array_add (g.dictionaries, dict);
	}
	g_free (names);

	for (gsize i = 0; i < g.dictionaries->len; i++)
	{
		Dictionary *dict = g_ptr_array_index (g.dictionaries, i);
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
init_from_config (GError **error)
{
	GKeyFile *key_file = load_project_config_file (error);
	if (!key_file)
		return FALSE;

	gboolean result = init_from_key_file (key_file, error);
	g_key_file_free (key_file);
	return result;
}

static void
search (Dictionary *dict)
{
	GtkEntryBuffer *buf = gtk_entry_get_buffer (GTK_ENTRY (g.entry));
	const gchar *input_utf8 = gtk_entry_buffer_get_text (buf);

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
on_selection_received (G_GNUC_UNUSED GtkClipboard *clipboard, const gchar *text,
	G_GNUC_UNUSED gpointer data)
{
	if (!text)
		return;

	gchar *trimmed = g_strstrip (g_strdup (text));
	gtk_entry_set_text (GTK_ENTRY (g.entry), trimmed);
	g_free (trimmed);
	g_signal_emit_by_name (g.entry,
		"move-cursor", GTK_MOVEMENT_BUFFER_ENDS, 1, FALSE);
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
	g.dictionary = page_num;
	search (g_ptr_array_index (g.dictionaries, g.dictionary));
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

static void
init_tabs (void)
{
	for (gsize i = 0; i < g.dictionaries->len; i++)
	{
		Dictionary *dict = g_ptr_array_index (g.dictionaries, i);
		GtkWidget *dummy = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		GtkWidget *label = gtk_label_new (dict->name);
		gtk_notebook_append_page (GTK_NOTEBOOK (g.notebook), dummy, label);
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

static gboolean
reload_dictionaries (GPtrArray *new_dictionaries)
{
	GError *error = NULL;
	if (!load_dictionaries (new_dictionaries, &error))
	{
		show_error_dialog (error);
		return FALSE;
	}

	while (gtk_notebook_get_n_pages (GTK_NOTEBOOK (g.notebook)))
		gtk_notebook_remove_page (GTK_NOTEBOOK (g.notebook), -1);

	g.dictionary = -1;
	stardict_view_set_position (STARDICT_VIEW (g.view), NULL, 0);
	g_ptr_array_free (g.dictionaries, TRUE);
	g.dictionaries = new_dictionaries;
	init_tabs ();
	return TRUE;
}

static void
on_open (G_GNUC_UNUSED GtkMenuItem *item, G_GNUC_UNUSED gpointer data)
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
	if (!new_dictionaries->len || !reload_dictionaries (new_dictionaries))
		g_ptr_array_free (new_dictionaries, TRUE);
}

static void
on_drag_data_received (G_GNUC_UNUSED GtkWidget *widget,
	G_GNUC_UNUSED GdkDragContext *context, G_GNUC_UNUSED gint x,
	G_GNUC_UNUSED gint y, GtkSelectionData *data, G_GNUC_UNUSED guint info,
	G_GNUC_UNUSED guint time, G_GNUC_UNUSED gpointer user_data)
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
	if (error)
		show_error_dialog (error);
	else if (new_dictionaries->len && reload_dictionaries (new_dictionaries))
		return;

	g_ptr_array_free (new_dictionaries, TRUE);
}

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
main (int argc, char *argv[])
{
	if (!setlocale (LC_ALL, ""))
		g_printerr ("%s: %s\n", _("Warning"), _("failed to set the locale"));

	bindtextdomain (GETTEXT_PACKAGE, GETTEXT_DIRNAME);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gchar **filenames = NULL;
	GOptionEntry option_entries[] =
	{
		{G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &filenames,
			NULL, N_("[FILE]...")},
		{},
	};

	GError *error = NULL;
	gtk_init_with_args (&argc, &argv, N_("- StarDict GTK+ UI"),
		option_entries, GETTEXT_PACKAGE, &error);
	if (error)
	{
		g_warning ("%s", error->message);
		g_error_free (error);
		return 1;
	}

	gtk_window_set_default_icon_name (PROJECT_NAME);

	g.dictionaries =
		g_ptr_array_new_with_free_func ((GDestroyNotify) dictionary_destroy);
	if (filenames)
		init (filenames);
	else if (!init_from_config (&error) && error)
		die_with_dialog (error->message);
	g_strfreev (filenames);

	if (!g.dictionaries->len)
		die_with_dialog (_("No dictionaries found either in "
			"the configuration or on the command line"));
	if (!load_dictionaries (g.dictionaries, &error))
		die_with_dialog (error->message);

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
			"color: @theme_fg_color; /* should be more faded than 'text' */ }";

	GdkScreen *screen = gdk_screen_get_default ();
	GtkCssProvider *provider = gtk_css_provider_new ();
	gtk_css_provider_load_from_data (provider, style, strlen (style), NULL);
	gtk_style_context_add_provider_for_screen (screen,
		GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	GtkWidget *item_open = gtk_menu_item_new_with_mnemonic (_("_Open..."));
	g_signal_connect (item_open, "activate", G_CALLBACK (on_open), NULL);

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
#ifndef WIN32
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item_selection);
#endif  // ! WIN32
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
	init_tabs ();

	GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_PRIMARY);
	g_signal_connect (clipboard, "owner-change",
		G_CALLBACK (on_selection), NULL);

	gtk_drag_dest_set (g.view,
		GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY);
	gtk_drag_dest_add_uri_targets (g.view);
	g_signal_connect (g.view, "drag-data-received",
		G_CALLBACK (on_drag_data_received), NULL);

	gtk_widget_show_all (g.window);
	gtk_main ();
	return 0;
}
