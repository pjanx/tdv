/*
 * StarDict GTK+ UI
 *
 * Copyright (c) 2020, Přemysl Eric Janouch <p@janouch.name>
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

typedef struct dictionary Dictionary;

struct dictionary
{
	const gchar  *filename;          ///< Filename
	StardictDict *dict;              ///< Stardict dictionary data
	gchar        *name;              ///< Name to show
	guint         position;          ///< Current position
};

static struct
{
	GtkWidget    *window;            ///< Top-level window
	GtkWidget    *notebook;          ///< Notebook with tabs
	GtkWidget    *entry;             ///< Search entry widget
	GtkWidget    *grid;              ///< Entries container

	gint          dictionary;        ///< Index of the current dictionary
	Dictionary   *dictionaries;      ///< All open dictionaries
	gsize         dictionaries_len;  ///< Total number of dictionaries

	gboolean      watch_selection;   ///< Following X11 PRIMARY?
}
g;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gboolean
dictionary_load (Dictionary *self, gchar *filename, GError **e)
{
	self->filename = filename;
	if (!(self->dict = stardict_dict_new (self->filename, e)))
		return FALSE;

	if (!self->name)
	{
		self->name = g_strdup (stardict_info_get_book_name
			(stardict_dict_get_info (self->dict)));
	}
	return TRUE;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gboolean
init (gchar **filenames, GError **e)
{
	while (filenames[g.dictionaries_len])
		g.dictionaries_len++;

	g.dictionaries = g_malloc0_n (sizeof *g.dictionaries, g.dictionaries_len);
	for (gsize i = 0; i < g.dictionaries_len; i++)
	{
		Dictionary *dict = &g.dictionaries[i];
		if (!dictionary_load (dict, filenames[i], e))
			return FALSE;
	}
	return TRUE;
}

static void
add_row (StardictIterator *iterator, gint row, gint *height_acc)
{
	Dictionary *dict = &g.dictionaries[g.dictionary];

	StardictEntry *entry = stardict_iterator_get_entry (iterator);
	g_return_if_fail (entry != NULL);
	StardictEntryField *field = entry->fields->data;
	g_return_if_fail (g_ascii_islower (field->type));

	GtkEntryBuffer *buf = gtk_entry_get_buffer (GTK_ENTRY (g.entry));
	const gchar *input_utf8 = gtk_entry_buffer_get_text (buf);
	g_return_if_fail (input_utf8 != NULL);

	const gchar *word_str = stardict_iterator_get_word (iterator);
	gsize common_prefix = stardict_longest_common_collation_prefix
		(dict->dict, word_str, input_utf8);
	gchar *pre = g_markup_escape_text (word_str, common_prefix),
		*post = g_markup_escape_text (word_str + common_prefix, -1),
		*marked_up = g_strdup_printf ("<u>%s</u>%s", pre, post);

	GtkWidget *word = gtk_label_new (marked_up);
	gtk_label_set_use_markup (GTK_LABEL (word), TRUE);
	gtk_label_set_ellipsize (GTK_LABEL (word), PANGO_ELLIPSIZE_END);
	gtk_label_set_selectable (GTK_LABEL (word), TRUE);
	gtk_label_set_xalign (GTK_LABEL (word), 0);
	gtk_label_set_yalign (GTK_LABEL (word), 0);
	// FIXME: they can't be deselected by just clicking outside of them
	gtk_widget_set_can_focus (word, FALSE);

	g_free (pre);
	g_free (post);
	g_free (marked_up);

	GtkWidget *desc = gtk_label_new (field->data);
	gtk_label_set_ellipsize (GTK_LABEL (desc), PANGO_ELLIPSIZE_END);
	gtk_label_set_selectable (GTK_LABEL (desc), TRUE);
	gtk_label_set_xalign (GTK_LABEL (desc), 0);
	gtk_widget_set_can_focus (desc, FALSE);

	g_object_unref (entry);

	if (iterator->offset % 2 == 0)
	{
		GtkStyleContext *ctx;
		ctx = gtk_widget_get_style_context (word);
		gtk_style_context_add_class (ctx, "odd");
		ctx = gtk_widget_get_style_context (desc);
		gtk_style_context_add_class (ctx, "odd");
	}

	gtk_grid_attach (GTK_GRID (g.grid), word, 0, row, 1, 1);
	gtk_grid_attach (GTK_GRID (g.grid), desc, 1, row, 1, 1);

	gtk_widget_show (word);
	gtk_widget_show (desc);

	gint minimum_word = 0, minimum_desc = 0;
	gtk_widget_get_preferred_height (word, &minimum_word, NULL);
	gtk_widget_get_preferred_height (desc, &minimum_desc, NULL);
	*height_acc += MAX (minimum_word, minimum_desc);
}

static void
reload (GtkWidget *grid)
{
	Dictionary *dict = &g.dictionaries[g.dictionary];

	GList *children = gtk_container_get_children (GTK_CONTAINER (grid));
	for (GList *iter = children; iter != NULL; iter = g_list_next (iter))
		gtk_widget_destroy (GTK_WIDGET (iter->data));
	g_list_free (children);

	gint window_height = 0;
	gtk_window_get_size (GTK_WINDOW (g.window), NULL, &window_height);
	if (window_height <= 0)
		return;

	StardictIterator *iterator =
		stardict_iterator_new (dict->dict, dict->position);
	gint row = 0, height_acc = 0;
	while (stardict_iterator_is_valid (iterator))
	{
		add_row (iterator, row++, &height_acc);
		if (height_acc >= window_height)
			break;

		stardict_iterator_next (iterator);
	}
	gtk_widget_show_all (grid);
	g_object_unref (iterator);
}

static void
search (Dictionary *dict)
{
	GtkEntryBuffer *buf = gtk_entry_get_buffer (GTK_ENTRY (g.entry));
	const gchar *input_utf8 = gtk_entry_buffer_get_text (buf);

	StardictIterator *iterator =
		stardict_dict_search (dict->dict, input_utf8, NULL);
	dict->position = stardict_iterator_get_offset (iterator);
	g_object_unref (iterator);
}

static void
on_changed (G_GNUC_UNUSED GtkWidget *widget, G_GNUC_UNUSED gpointer data)
{
	search (&g.dictionaries[g.dictionary]);
	reload (g.grid);
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
	search (&g.dictionaries[g.dictionary]);
	reload (g.grid);
}

static gboolean
on_key_press (G_GNUC_UNUSED GtkWidget *widget, GdkEvent *event,
	G_GNUC_UNUSED gpointer data)
{
	if (event->key.state == GDK_CONTROL_MASK)
	{
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
	if (event->key.state == GDK_MOD1_MASK)
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
			NULL, N_("FILE...")},
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

	if (!filenames)
	{
		// TODO: eventually just load all dictionaries from configuration
		die_with_dialog ("No arguments have been passed.");
	}
	if (!init (filenames, &error))
		die_with_dialog (error->message);

	// Some Adwaita stupidity and our own additions
	const char *style = "notebook header tab { padding: 2px 8px; margin: 0; }"
		"grid { border-top: 1px solid rgba(0, 0, 0, 0.2); background: white; }"
		"grid label { padding: 0 5px; "
			"/*border-bottom: 1px solid rgba(0, 0, 0, 0.2);*/ }"
		"grid label.odd { background: rgba(0, 0, 0, 0.05); }";

	GdkScreen *screen = gdk_screen_get_default ();
	GtkCssProvider *provider = gtk_css_provider_new ();
	gtk_css_provider_load_from_data (provider, style, strlen (style), NULL);
	gtk_style_context_add_provider_for_screen (screen,
		GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	g.grid = gtk_grid_new ();
	gtk_grid_set_column_homogeneous (GTK_GRID (g.grid), TRUE);

	// FIXME: we'd rather like to trim the contents, not make it scrollable.
	// This just limits the allocation.
	// TODO: probably create a whole new custom widget, everything is text
	// anyway and mostly handled by Pango, including pango_layout_xy_to_index()
	//  - I don't know where to get selection colour but inversion works, too
	GtkWidget *scrolled_window = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled_window),
		GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
	gtk_widget_set_can_focus (scrolled_window, FALSE);
	gtk_container_add (GTK_CONTAINER (scrolled_window), g.grid);

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

	GtkWidget *hamburger = gtk_menu_button_new ();
	gtk_menu_button_set_direction (GTK_MENU_BUTTON (hamburger), GTK_ARROW_NONE);
	gtk_menu_button_set_popup (GTK_MENU_BUTTON (hamburger), menu);
	gtk_button_set_relief (GTK_BUTTON (hamburger), GTK_RELIEF_NONE);
	gtk_widget_show (hamburger);

	gtk_notebook_set_action_widget
		(GTK_NOTEBOOK (g.notebook), hamburger, GTK_PACK_END);

	// FIXME: when the clear icon shows, the widget changes in height
	g.entry = gtk_search_entry_new ();
	// TODO: attach to the "key-press-event" signal and implement ^W at least,
	// though ^U is working already!  Note that bindings can be done in CSS
	// as well, if we have any extra specially for the editor
	g_signal_connect (g.entry, "changed", G_CALLBACK (on_changed), g.grid);
	gtk_entry_set_has_frame (GTK_ENTRY (g.entry), FALSE);

	// TODO: supposedly attach to "key-press-event" here and react to
	// PageUp/PageDown and up/down arrow keys... either here or in the Entry
	g.window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	g_signal_connect (g.window, "destroy",
		G_CALLBACK (on_destroy), NULL);
	g_signal_connect (g.window, "key-press-event",
		G_CALLBACK (on_key_press), NULL);
	GtkWidget *superbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
	gtk_container_add (GTK_CONTAINER (g.window), superbox);
	gtk_container_add (GTK_CONTAINER (superbox), g.notebook);
	gtk_container_add (GTK_CONTAINER (superbox), g.entry);
	gtk_box_pack_end (GTK_BOX (superbox), scrolled_window, TRUE, TRUE, 0);

	for (gsize i = 0; i < g.dictionaries_len; i++)
	{
		Dictionary *dict = &g.dictionaries[i];
		GtkWidget *dummy = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
		GtkWidget *label = gtk_label_new (dict->name);
		gtk_notebook_append_page (GTK_NOTEBOOK (g.notebook), dummy, label);
	}

	GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_PRIMARY);
	g_signal_connect (clipboard, "owner-change",
		G_CALLBACK (on_selection), NULL);

	// Make sure to fill up the window with entries once we're resized
	// XXX: this is rather inefficient as we rebuild everything each time
	g_signal_connect (g.window, "configure-event",
		G_CALLBACK (on_changed), NULL);
	g_signal_connect (g.window, "map-event",
		G_CALLBACK (on_changed), NULL);

	gtk_widget_grab_focus (g.entry);
	gtk_widget_show_all (g.window);
	gtk_main ();

	g_strfreev (filenames);
	return 0;
}
