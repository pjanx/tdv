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

	gtk_entry_set_text (GTK_ENTRY (g.entry), text);
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

	guint mods = event->key.state & gtk_accelerator_get_default_mod_mask ();
	if (mods == GDK_CONTROL_MASK)
	{
		// Can't use gtk_widget_add_accelerator() to change-current-page(-1/+1)
		// because that signal has arguments, which cannot be passed.
		if (event->key.keyval == GDK_KEY_Page_Up)
		{
			gtk_notebook_prev_page (GTK_NOTEBOOK (g.notebook));
			return TRUE;
		}
		if (event->key.keyval == GDK_KEY_Page_Down)
		{
			gtk_notebook_next_page (GTK_NOTEBOOK (g.notebook));
			return TRUE;
		}
	}
	if (mods == GDK_MOD1_MASK)
	{
		if (event->key.keyval >= GDK_KEY_0
		 && event->key.keyval <= GDK_KEY_9)
		{
			gint n = event->key.keyval - GDK_KEY_0;
			gtk_notebook_set_current_page
				(GTK_NOTEBOOK (g.notebook), n ? (n - 1) : 10);
			return TRUE;
		}
	}
	return FALSE;
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
		"entry { -gtk-key-bindings: Readline }"
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

	g.notebook = gtk_notebook_new ();
	g_signal_connect (g.notebook, "switch-page",
		G_CALLBACK (on_switch_page), NULL);
	gtk_notebook_set_scrollable (GTK_NOTEBOOK (g.notebook), TRUE);

	g.watch_selection = TRUE;
	GtkWidget *item =
		gtk_check_menu_item_new_with_label (_("Follow selection"));
	gtk_check_menu_item_set_active
		(GTK_CHECK_MENU_ITEM (item), g.watch_selection);
	g_signal_connect (item, "toggled",
		G_CALLBACK (on_selection_watch_toggle), NULL);

	GtkWidget *menu = gtk_menu_new ();
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	gtk_widget_show_all (menu);

	g.hamburger = gtk_menu_button_new ();
	gtk_menu_button_set_direction
		(GTK_MENU_BUTTON (g.hamburger), GTK_ARROW_NONE);
	gtk_menu_button_set_popup (GTK_MENU_BUTTON (g.hamburger), menu);
	gtk_button_set_relief (GTK_BUTTON (g.hamburger), GTK_RELIEF_NONE);
	gtk_widget_show (g.hamburger);

	gtk_notebook_set_action_widget
		(GTK_NOTEBOOK (g.notebook), g.hamburger, GTK_PACK_END);

	// FIXME: when the clear icon shows, the widget changes in height
	g.entry = gtk_search_entry_new ();
	g_signal_connect (g.entry, "changed", G_CALLBACK (on_changed), g.view);
	// TODO: make the entry have a background colour, rather than transparency
	gtk_entry_set_has_frame (GTK_ENTRY (g.entry), FALSE);

	// TODO: supposedly attach to "key-press-event" here and react to
	// PageUp/PageDown and up/down arrow keys... either here or in the Entry
	g.window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
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

	for (gsize i = 0; i < g.dictionaries->len; i++)
	{
		Dictionary *dict = g_ptr_array_index (g.dictionaries, i);
		GtkWidget *dummy = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		GtkWidget *label = gtk_label_new (dict->name);
		gtk_notebook_append_page (GTK_NOTEBOOK (g.notebook), dummy, label);
	}

	GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_PRIMARY);
	g_signal_connect (clipboard, "owner-change",
		G_CALLBACK (on_selection), NULL);

	gtk_widget_grab_focus (g.entry);
	gtk_widget_show_all (g.window);
	gtk_main ();
	return 0;
}
