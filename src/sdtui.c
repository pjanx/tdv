/*
 * StarDict terminal UI
 *
 * Copyright (c) 2013 - 2015, Přemysl Janouch <p.janouch@gmail.com>
 * All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
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

#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <stdarg.h>
#include <limits.h>
#include <string.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <pango/pango.h>
#include <glib/gi18n.h>

#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>

#include <termo.h> // input
#include <ncurses.h> // output

#include "config.h"
#include "stardict.h"
#include "utils.h"

#ifdef WITH_GTK
#include <gtk/gtk.h>
#endif  // WITH_GTK

#define CTRL_KEY(x)  ((x) - 'A' + 1)

#define TOP_BAR_CUTOFF  2               ///< How many lines are reserved on top

// --- Utilities ---------------------------------------------------------------

static size_t
unichar_width (gunichar ch)
{
	if (g_unichar_iszerowidth (ch))
		return 0;
	return 1 + g_unichar_iswide (ch);
}

// --- Application -------------------------------------------------------------

/// Data relating to one entry within the dictionary.
typedef struct view_entry               ViewEntry;
/// Encloses application data.
typedef struct application              Application;
/// Application options.
typedef struct app_options              AppOptions;

struct view_entry
{
	gchar   * word;                     ///< Word
	gchar  ** definitions;              ///< Word definition entries
	gsize     definitions_length;       ///< Length of the @a definitions array
};

struct application
{
	GMainLoop     * loop;               ///< Main loop
	termo_t       * tk;                 ///< termo handle
	guint           tk_timer;           ///< termo timeout timer
	GIConv          ucs4_to_locale;     ///< UTF-32 -> locale conversion
	gboolean        locale_is_utf8;     ///< The locale is Unicode

	StardictDict  * dict;               ///< The current dictionary
	guint           show_help : 1;      ///< Whether help can be shown

	guint32         top_position;       ///< Index of the topmost dict. entry
	guint           top_offset;         ///< Offset into the top entry
	guint           selected;           ///< Offset to the selected definition
	GPtrArray     * entries;            ///< ViewEntry's within the view

	gchar         * search_label;       ///< Text of the "Search" label
	GArray        * input;              ///< The current search input
	guint           input_pos;          ///< Cursor position within input
	gboolean        input_confirmed;    ///< Input has been confirmed

	gfloat          division;           ///< Position of the division column

	guint           selection_timer;    ///< Selection watcher timeout timer
	gint            selection_interval; ///< Selection watcher timer interval
	gchar         * selection_contents; ///< Selection contents
};

struct app_options
{
	gboolean show_version;              ///< Output version information and quit
	gint     selection_watcher;         ///< Interval in milliseconds, or -1
};

/// Splits the entry and adds it to a pointer array.
static void
view_entry_split_add (GPtrArray *out, const gchar *text)
{
	gchar **it, **tmp = g_strsplit (text, "\n", -1);
	for (it = tmp; *it; it++)
		if (**it)
			g_ptr_array_add (out, g_strdup (*it));
	g_strfreev (tmp);
}

/// Decomposes a dictionary entry into the format we want.
static ViewEntry *
view_entry_new (StardictIterator *iterator)
{
	g_return_val_if_fail (stardict_iterator_is_valid (iterator), NULL);

	ViewEntry *ve = g_slice_alloc (sizeof *ve);
	GString *word = g_string_new (stardict_iterator_get_word (iterator));

	StardictEntry *entry = stardict_iterator_get_entry (iterator);
	g_return_val_if_fail (entry != NULL, NULL);

	GPtrArray *definitions = g_ptr_array_new ();
	const GList *fields = stardict_entry_get_fields (entry);
	gboolean found_anything_displayable = FALSE;
	while (fields)
	{
		const StardictEntryField *field = fields->data;
		switch (field->type)
		{
		case STARDICT_FIELD_MEANING:
			view_entry_split_add (definitions, field->data);
			found_anything_displayable = TRUE;
			break;
		case STARDICT_FIELD_PANGO:
		{
			char *text;
			if (!pango_parse_markup (field->data, -1,
				0, NULL, &text, NULL, NULL))
				text = g_strdup_printf ("<%s>", _("error in entry"));
			view_entry_split_add (definitions, text);
			found_anything_displayable = TRUE;
			g_free (text);
			break;
		}
		case STARDICT_FIELD_PHONETIC:
			g_string_append_printf (word, " /%s/", (const gchar *) field->data);
			break;
		default:
			// TODO support more of them
			break;
		}
		fields = fields->next;
	}
	g_object_unref (entry);

	if (!found_anything_displayable)
		g_ptr_array_add (definitions,
			g_strdup_printf ("<%s>", _("no usable field found")));

	ve->word = g_string_free (word, FALSE);
	ve->definitions_length = definitions->len;
	g_ptr_array_add (definitions, NULL);
	ve->definitions = (gchar **) g_ptr_array_free (definitions, FALSE);
	return ve;
}

/// Release resources associated with the view entry.
static void
view_entry_free (ViewEntry *ve)
{
	g_free (ve->word);
	g_strfreev (ve->definitions);
	g_slice_free1 (sizeof *ve, ve);
}

/// Reload view items.
static void
app_reload_view (Application *self)
{
	if (self->entries->len != 0)
		g_ptr_array_remove_range (self->entries, 0, self->entries->len);

	gint remains = LINES - TOP_BAR_CUTOFF + self->top_offset;
	StardictIterator *iterator =
		stardict_iterator_new (self->dict, self->top_position);
	while (remains > 0 && stardict_iterator_is_valid (iterator))
	{
		ViewEntry *entry = view_entry_new (iterator);
		remains -= entry->definitions_length;
		g_ptr_array_add (self->entries, entry);
		stardict_iterator_next (iterator);
	}
	g_object_unref (iterator);
}

#ifdef WITH_GTK
static gboolean on_selection_timer (gpointer data);

static void
rearm_selection_watcher (Application *self)
{
	if (self->selection_interval > 0)
		self->selection_timer = g_timeout_add
			(self->selection_interval, on_selection_timer, self);
}
#endif  // WITH_GTK

/// Initialize the application core.
static void
app_init (Application *self, AppOptions *options, const gchar *filename)
{
	self->loop = NULL;
	self->selection_interval = options->selection_watcher;
	self->selection_timer = 0;
	self->selection_contents = NULL;

#ifdef WITH_GTK
	if (gtk_init_check (0, NULL))
	{
		// So that we set the input only when it actually changes
		GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_PRIMARY);
		self->selection_contents = gtk_clipboard_wait_for_text (clipboard);
		rearm_selection_watcher (self);
	}
	else
#endif  // WITH_GTK
		self->loop = g_main_loop_new (NULL, FALSE);

	self->tk = NULL;
	self->tk_timer = 0;

	GError *error = NULL;
	self->dict = stardict_dict_new (filename, &error);
	if (!self->dict)
	{
		g_printerr ("%s: %s\n", _("Error loading dictionary"), error->message);
		exit (EXIT_FAILURE);
	}

	self->show_help = TRUE;

	self->top_position = 0;
	self->top_offset = 0;
	self->selected = 0;
	self->entries = g_ptr_array_new_with_free_func
		((GDestroyNotify) view_entry_free);

	self->search_label = g_strdup_printf ("%s: ", _("Search"));

	self->input = g_array_new (TRUE, FALSE, sizeof (gunichar));
	self->input_pos = 0;
	self->input_confirmed = FALSE;

	self->division = 0.5;

	const char *charset;
	self->locale_is_utf8 = g_get_charset (&charset);
	self->ucs4_to_locale = g_iconv_open (charset, "UTF-32");

	app_reload_view (self);
}

/// Free any resources used by the application.
static void
app_destroy (Application *self)
{
	if (self->loop)
		g_main_loop_unref (self->loop);
	if (self->tk)
		termo_destroy (self->tk);
	if (self->tk_timer)
		g_source_remove (self->tk_timer);

	if (self->selection_timer)
		g_source_remove (self->selection_timer);
	g_free (self->selection_contents);

	g_object_unref (self->dict);
	g_ptr_array_free (self->entries, TRUE);
	g_free (self->search_label);
	g_array_free (self->input, TRUE);

	g_iconv_close (self->ucs4_to_locale);
}

/// Run the main event dispatch loop.
static void
app_run (Application *self)
{
	if (self->loop)
		g_main_loop_run (self->loop);
#ifdef WITH_GTK
	else
		gtk_main ();
#endif  // WITH_GTK
}

/// Quit the main event dispatch loop.
static void
app_quit (Application *self)
{
	if (self->loop)
		g_main_loop_quit (self->loop);
#ifdef WITH_GTK
	else
		gtk_main_quit ();
#endif  // WITH_GTK
}

/// Returns if the Unicode character is representable in the current locale.
static gboolean
app_is_character_in_locale (Application *self, gunichar ch)
{
	// Avoid the overhead joined with calling iconv() for all characters
	if (self->locale_is_utf8)
		return TRUE;

	gchar *tmp = g_convert_with_iconv ((const gchar *) &ch, sizeof ch,
		self->ucs4_to_locale, NULL, NULL, NULL);
	if (!tmp)
		return FALSE;
	g_free (tmp);
	return TRUE;
}

/// Write the given UTF-8 string padded with spaces.
/// @param[in] n  The number of characters to write, or -1 for the whole string.
/// @param[in] attrs  Text attributes for the text, without padding.
///                   To change the attributes of all output, use attrset().
/// @return The number of characters output.
static gsize
app_add_utf8_string (Application *self, const gchar *str, int attrs, int n)
{
	if (!n)
		return 0;

	glong ucs4_len;
	gunichar *ucs4 = g_utf8_to_ucs4_fast (str, -1, &ucs4_len);

	// Replace invalid chars and compute how many characters fit in the limit
	gint cols, i;
	for (cols = i = 0; i < ucs4_len; i++)
	{
		if (!app_is_character_in_locale (self, ucs4[i]))
			ucs4[i] = '?';

		gint width = unichar_width (ucs4[i]);
		if (n >= 0 && cols + width > n)
			break;
		cols += width;
	}

	if (n < 0)
		n = cols;

	// Append ellipsis if the whole string didn't fit
	gunichar ellipsis = L'…';
	gint ellipsis_width = unichar_width (ellipsis);

	gint len = i;
	if (len != ucs4_len)
	{
		if (app_is_character_in_locale (self, ellipsis))
		{
			if (cols + ellipsis_width > n)
				cols -= unichar_width (ucs4[len - 1]);
			else
				len++;

			ucs4[len - 1] = ellipsis;
			cols += ellipsis_width;
		}
		else if (n >= 3 && len >= 3)
		{
			// With zero-width characters this overflows
			// It's just a fallback anyway
			cols -= unichar_width (ucs4[len - 1]); ucs4[len - 1] = '.';
			cols -= unichar_width (ucs4[len - 2]); ucs4[len - 2] = '.';
			cols -= unichar_width (ucs4[len - 3]); ucs4[len - 3] = '.';
			cols += 3;
		}
	}

	guchar *locale_str;
	gsize locale_str_len;
	locale_str = (guchar *) g_convert_with_iconv ((const gchar *) ucs4,
		len * sizeof *ucs4, self->ucs4_to_locale, NULL, &locale_str_len, NULL);
	g_return_val_if_fail (locale_str != NULL, 0);

	for (gsize i = 0; i < locale_str_len; i++)
		addch (locale_str[i] | attrs);
	while (cols++ < n)
		addch (' ');

	g_free (locale_str);
	g_free (ucs4);
	return n;
}

/// Render the top bar.
static void
app_redraw_top (Application *self)
{
	attrset (A_REVERSE);
	mvwhline (stdscr, 0, 0, A_REVERSE, COLS);
	gsize indent = app_add_utf8_string (self, PROJECT_NAME "  ", A_BOLD, -1);
	app_add_utf8_string (self, stardict_info_get_book_name
		(stardict_dict_get_info (self->dict)), 0, COLS - indent);

	attrset (A_UNDERLINE);
	mvwhline (stdscr, 1, 0, A_UNDERLINE, COLS);
	indent = app_add_utf8_string (self, self->search_label, 0, -1);

	gchar *input_utf8 = g_ucs4_to_utf8
		((gunichar *) self->input->data, -1, NULL, NULL, NULL);
	g_return_if_fail (input_utf8 != NULL);

	int word_attrs = 0;
	if (self->input_confirmed)
		word_attrs |= A_BOLD;
	app_add_utf8_string (self, input_utf8, word_attrs, COLS - indent - 1);
	g_free (input_utf8);

	guint offset, i;
	for (offset = i = 0; i < self->input_pos; i++)
		// This may be inconsistent with the output of app_add_utf8_string()
		offset += unichar_width (g_array_index (self->input, gunichar, i));

	move (1, MIN ((gint) (indent + offset), COLS - 1));
	refresh ();
}

/// Computes width for the left column.
static guint
app_get_left_column_width (Application *self)
{
	gint width = COLS * self->division + 0.5;
	if (width < 1)
		width = 1;
	else if (width > COLS - 2)
		width = COLS - 2;
	return width;
}

/// Display a message in the view area.
static void
app_show_message (Application *self, const gchar *lines[], gsize len)
{
	gint top = (LINES - TOP_BAR_CUTOFF - len) / 2;

	gint i;
	for (i = 0; i < top; i++)
	{
		move (TOP_BAR_CUTOFF + i, 0);
		clrtoeol ();
	}

	attrset (0);
	while (len-- && i < LINES - TOP_BAR_CUTOFF)
	{
		move (TOP_BAR_CUTOFF + i, 0);
		clrtoeol ();

		gint x = (COLS - g_utf8_strlen (*lines, -1)) / 2;
		if (x < 0)
			x = 0;

		move (TOP_BAR_CUTOFF + i, x);
		app_add_utf8_string (self, *lines, 0, COLS - x);

		lines++;
		i++;
	}

	clrtobot ();
	refresh ();
}

/// Show some information about the program.
static void
app_show_help (Application *self)
{
	const gchar *lines[] =
	{
		PROJECT_NAME " " PROJECT_VERSION,
		_("Terminal UI for StarDict dictionaries"),
		"Copyright (c) 2013 - 2015, Přemysl Janouch",
		"",
		_("Type to search")
	};

	app_show_message (self, lines, G_N_ELEMENTS (lines));
}

/// Redraw the dictionary view.
static void
app_redraw_view (Application *self)
{
	if (self->show_help)
	{
		app_show_help (self);
		return;
	}

	move (TOP_BAR_CUTOFF, 0);

	guint i, k = self->top_offset, shown = 0;
	for (i = 0; i < self->entries->len; i++)
	{
		ViewEntry *ve = g_ptr_array_index (self->entries, i);
		for (; k < ve->definitions_length; k++)
		{
			int attrs = 0;
			if (shown == self->selected)          attrs |= A_REVERSE;
			if (k + 1 == ve->definitions_length)  attrs |= A_UNDERLINE;
			attrset (attrs);

			guint left_width = app_get_left_column_width (self);
			app_add_utf8_string (self, ve->word, 0, left_width);
			addstr (" ");
			app_add_utf8_string (self,
				ve->definitions[k], 0, COLS - left_width - 1);

			if ((gint) ++shown == LINES - TOP_BAR_CUTOFF)
				goto done;
		}

		k = 0;
	}

done:
	attrset (0);
	clrtobot ();
	refresh ();
}

/// Just prepends a new view entry into the entries array.
static ViewEntry *
prepend_entry (Application *self, guint32 position)
{
	StardictIterator *iterator = stardict_iterator_new (self->dict, position);
	ViewEntry *ve = view_entry_new (iterator);
	g_object_unref (iterator);

	g_ptr_array_add (self->entries, NULL);
	memmove (self->entries->pdata + 1, self->entries->pdata,
		sizeof ve * (self->entries->len - 1));
	return g_ptr_array_index (self->entries, 0) = ve;
}

/// Just appends a new view entry to the entries array.
static ViewEntry *
append_entry (Application *self, guint32 position)
{
	ViewEntry *ve = NULL;
	StardictIterator *iterator = stardict_iterator_new
		(self->dict, position);
	if (stardict_iterator_is_valid (iterator))
	{
		ve = view_entry_new (iterator);
		g_ptr_array_add (self->entries, ve);
	}
	g_object_unref (iterator);
	return ve;
}

/// Counts the number of definitions available for seeing.
static guint
app_count_view_items (Application *self)
{
	guint i, n_definitions = 0;
	for (i = 0; i < self->entries->len; i++)
	{
		ViewEntry *entry = g_ptr_array_index (self->entries, i);
		n_definitions += entry->definitions_length;
	}
	return n_definitions;
}

/// Scroll up @a n entries.
static gboolean
app_scroll_up (Application *self, guint n)
{
	gboolean success = TRUE;
	guint n_definitions = app_count_view_items (self);
	while (n--)
	{
		if (self->top_offset > 0)
		{
			self->top_offset--;
			continue;
		}

		// We've reached the top
		if (self->top_position == 0)
		{
			success = FALSE;
			break;
		}

		ViewEntry *ve = prepend_entry (self, --self->top_position);
		self->top_offset = ve->definitions_length - 1;
		n_definitions += ve->definitions_length;

		// Remove the last entry if not shown
		ViewEntry *last_entry =
			g_ptr_array_index (self->entries, self->entries->len - 1);
		if ((gint) (n_definitions - self->top_offset
			- last_entry->definitions_length) >= LINES - TOP_BAR_CUTOFF)
		{
			n_definitions -= last_entry->definitions_length;
			g_ptr_array_remove_index_fast
				(self->entries, self->entries->len - 1);
		}
	}

	app_redraw_view (self);
	return success;
}

/// Scroll down @a n entries.
static gboolean
app_scroll_down (Application *self, guint n)
{
	gboolean success = TRUE;
	guint n_definitions = app_count_view_items (self);
	while (n--)
	{
		if (self->entries->len == 0)
		{
			success = FALSE;
			break;
		}

		ViewEntry *first_entry = g_ptr_array_index (self->entries, 0);
		if (self->top_offset < first_entry->definitions_length - 1)
			self->top_offset++;
		else
		{
			n_definitions -= first_entry->definitions_length;
			g_ptr_array_remove_index (self->entries, 0);
			self->top_position++;
			self->top_offset = 0;
		}

		if ((gint) (n_definitions - self->top_offset) < LINES - TOP_BAR_CUTOFF)
		{
			ViewEntry *ve = append_entry (self,
				self->top_position + self->entries->len);
			if (ve != NULL)
				n_definitions += ve->definitions_length;
		}
	}

	// Fix cursor to not point below the view items
	if (self->selected >= n_definitions - self->top_offset)
		self->selected  = n_definitions - self->top_offset - 1;

	app_redraw_view (self);
	return success;
}

/// Moves the selection one entry up.
static gboolean
app_one_entry_up (Application *self)
{
	if (self->selected == 0 && self->top_offset == 0)
	{
		if (self->top_position == 0)
			return FALSE;
		prepend_entry (self, --self->top_position);
	}

	// Find the last entry that starts above the selection
	gint first = -self->top_offset;
	guint i;
	for (i = 0; i < self->entries->len; i++)
	{
		ViewEntry *ve = g_ptr_array_index (self->entries, i);
		gint new_first = first + ve->definitions_length;
		if (new_first >= (gint) self->selected)
			break;
		first = new_first;
	}

	if (first < 0)
	{
		self->selected = 0;
		app_scroll_up (self, -first);
	}
	else
	{
		self->selected = first;
		app_redraw_view (self);
	}
	return TRUE;
}

/// Moves the selection one entry down.
static void
app_one_entry_down (Application *self)
{
	// Find the first entry that starts below the selection
	gint first = -self->top_offset;
	guint i;
	for (i = 0; i < self->entries->len; i++)
	{
		ViewEntry *ve = g_ptr_array_index (self->entries, i);
		first += ve->definitions_length;
		if (first > (gint) self->selected)
			break;
	}

	if (first > LINES - TOP_BAR_CUTOFF - 1)
	{
		self->selected = LINES - TOP_BAR_CUTOFF - 1;
		app_scroll_down (self, first - (LINES - TOP_BAR_CUTOFF - 1));
	}
	else
	{
		self->selected = first;
		app_redraw_view (self);
	}
}

/// Redraw everything.
static void
app_redraw (Application *self)
{
	app_redraw_view (self);
	app_redraw_top (self);
}

/// Search for the current entry.
static void
app_search_for_entry (Application *self)
{
	gchar *input_utf8 = g_ucs4_to_utf8
		((gunichar *) self->input->data, -1, NULL, NULL, NULL);
	g_return_if_fail (input_utf8 != NULL);

	StardictIterator *iterator =
		stardict_dict_search (self->dict, input_utf8, NULL);
	g_free (input_utf8);

	self->top_position = stardict_iterator_get_offset (iterator);
	self->top_offset = 0;
	self->selected = 0;
	g_object_unref (iterator);

	self->show_help = FALSE;
	app_reload_view (self);
	app_redraw_view (self);
}

#define SAVE_CURSOR                 \
	int last_x, last_y;             \
	getyx (stdscr, last_y, last_x);

#define RESTORE_CURSOR              \
	move (last_y, last_x);          \
	refresh ();

/// The terminal has been resized, make appropriate changes.
static gboolean
app_process_resize (Application *self)
{
	app_reload_view (self);

	guint n_visible = app_count_view_items (self) - self->top_offset;
	if ((gint) n_visible > LINES - TOP_BAR_CUTOFF)
		n_visible = LINES - TOP_BAR_CUTOFF;

	if (self->selected >= n_visible)
	{
		app_scroll_down (self, self->selected - n_visible + 1);
		self->selected = n_visible - 1;
	}

	app_redraw (self);
	return TRUE;
}

// --- User input handling -----------------------------------------------------

/// All the actions that can be performed by the user.
typedef enum user_action                UserAction;

enum user_action
{
	USER_ACTION_NONE,

	USER_ACTION_QUIT,
	USER_ACTION_REDRAW,

	USER_ACTION_MOVE_SPLITTER_LEFT,
	USER_ACTION_MOVE_SPLITTER_RIGHT,

	USER_ACTION_GOTO_ENTRY_PREVIOUS,
	USER_ACTION_GOTO_ENTRY_NEXT,
	USER_ACTION_GOTO_DEFINITION_PREVIOUS,
	USER_ACTION_GOTO_DEFINITION_NEXT,
	USER_ACTION_GOTO_PAGE_PREVIOUS,
	USER_ACTION_GOTO_PAGE_NEXT,

	USER_ACTION_INPUT_CONFIRM,
	USER_ACTION_INPUT_HOME,
	USER_ACTION_INPUT_END,
	USER_ACTION_INPUT_LEFT,
	USER_ACTION_INPUT_RIGHT,
	USER_ACTION_INPUT_DELETE_PREVIOUS,
	USER_ACTION_INPUT_DELETE_NEXT,
	USER_ACTION_INPUT_DELETE_TO_HOME,
	USER_ACTION_INPUT_DELETE_TO_END,
	USER_ACTION_INPUT_DELETE_PREVIOUS_WORD,
	USER_ACTION_INPUT_TRANSPOSE,

	USER_ACTION_COUNT
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gboolean
app_process_user_action (Application *self, UserAction action)
{
	SAVE_CURSOR
	switch (action)
	{
	case USER_ACTION_QUIT:
		return FALSE;
	case USER_ACTION_REDRAW:
		clear ();
		app_redraw (self);
		return TRUE;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_MOVE_SPLITTER_LEFT:
		self->division = (app_get_left_column_width (self) - 1.) / COLS;
		app_redraw_view (self);
		RESTORE_CURSOR
		return TRUE;
	case USER_ACTION_MOVE_SPLITTER_RIGHT:
		self->division = (app_get_left_column_width (self) + 1.) / COLS;
		app_redraw_view (self);
		RESTORE_CURSOR
		return TRUE;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_GOTO_DEFINITION_PREVIOUS:
		if (self->selected > 0)
		{
			self->selected--;
			app_redraw_view (self);
		}
		else
			app_scroll_up (self, 1);
		RESTORE_CURSOR
		return TRUE;
	case USER_ACTION_GOTO_DEFINITION_NEXT:
		if ((gint) self->selected < LINES - TOP_BAR_CUTOFF - 1 &&
			self->selected < app_count_view_items (self) - self->top_offset - 1)
		{
			self->selected++;
			app_redraw_view (self);
		}
		else
			app_scroll_down (self, 1);
		RESTORE_CURSOR
		return TRUE;

	case USER_ACTION_GOTO_ENTRY_PREVIOUS:
		app_one_entry_up (self);
		RESTORE_CURSOR
		return TRUE;
	case USER_ACTION_GOTO_ENTRY_NEXT:
		app_one_entry_down (self);
		RESTORE_CURSOR
		return TRUE;

	case USER_ACTION_GOTO_PAGE_PREVIOUS:
		app_scroll_up (self, LINES - TOP_BAR_CUTOFF);
		// FIXME: selection
		RESTORE_CURSOR
		return TRUE;
	case USER_ACTION_GOTO_PAGE_NEXT:
		app_scroll_down (self, LINES - TOP_BAR_CUTOFF);
		// FIXME: selection
		RESTORE_CURSOR
		return TRUE;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_INPUT_HOME:
		self->input_pos = 0;
		app_redraw_top (self);
		return TRUE;
	case USER_ACTION_INPUT_END:
		self->input_pos = self->input->len;
		app_redraw_top (self);
		return TRUE;
	case USER_ACTION_INPUT_LEFT:
		if (self->input_pos > 0)
		{
			self->input_pos--;
			app_redraw_top (self);
		}
		return TRUE;
	case USER_ACTION_INPUT_RIGHT:
		if (self->input_pos < self->input->len)
		{
			self->input_pos++;
			app_redraw_top (self);
		}
		return TRUE;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_INPUT_CONFIRM:
		self->input_confirmed = TRUE;
		app_redraw_top (self);
		return TRUE;

	case USER_ACTION_INPUT_TRANSPOSE:
	{
		if (!self->input_pos || self->input->len < 2)
			break;

		guint start = self->input_pos - 1;
		if (self->input_pos >= self->input->len)
			start--;

		gunichar tmp = g_array_index (self->input, gunichar, start);
		g_array_index (self->input, gunichar, start)
			= g_array_index (self->input, gunichar, start + 1);
		g_array_index (self->input, gunichar, start + 1) = tmp;

		if (self->input_pos < self->input->len)
			self->input_pos++;

		app_search_for_entry (self);
		app_redraw_top (self);
		return TRUE;
	}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_INPUT_DELETE_PREVIOUS:
		if (self->input_pos > 0)
		{
			g_array_remove_index (self->input, --self->input_pos);
			app_search_for_entry (self);
			app_redraw_top (self);
		}
		return TRUE;
	case USER_ACTION_INPUT_DELETE_NEXT:
		if (self->input_pos < self->input->len)
		{
			g_array_remove_index (self->input, self->input_pos);
			app_search_for_entry (self);
			app_redraw_top (self);
		}
		return TRUE;
	case USER_ACTION_INPUT_DELETE_TO_HOME:
		if (self->input->len != 0)
		{
			g_array_remove_range (self->input, 0, self->input_pos);
			self->input_pos = 0;

			app_search_for_entry (self);
			app_redraw_top (self);
		}
		return TRUE;
	case USER_ACTION_INPUT_DELETE_TO_END:
		if (self->input_pos < self->input->len)
		{
			g_array_remove_range (self->input,
				self->input_pos, self->input->len - self->input_pos);
			app_search_for_entry (self);
			app_redraw_top (self);
		}
		return TRUE;
	case USER_ACTION_INPUT_DELETE_PREVIOUS_WORD:
	{
		if (self->input_pos == 0)
			return TRUE;

		gint i = self->input_pos;
		while (i)
			if (g_array_index (self->input, gunichar, --i) != L' ')
				break;
		while (i--)
			if (g_array_index (self->input, gunichar,   i) == L' ')
				break;

		i++;
		g_array_remove_range (self->input, i, self->input_pos - i);
		self->input_pos = i;

		app_search_for_entry (self);
		app_redraw_top (self);
		return TRUE;
	}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_NONE:
		return TRUE;
	default:
		g_assert_not_reached ();
	}
	return TRUE;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gboolean
app_process_keysym (Application *self, termo_key_t *event)
{
	UserAction action = USER_ACTION_NONE;
	typedef const UserAction ActionMap[TERMO_N_SYMS];

	static ActionMap actions =
	{
		[TERMO_SYM_ESCAPE]    = USER_ACTION_QUIT,

		[TERMO_SYM_UP]        = USER_ACTION_GOTO_DEFINITION_PREVIOUS,
		[TERMO_SYM_DOWN]      = USER_ACTION_GOTO_DEFINITION_NEXT,
		[TERMO_SYM_PAGEUP]    = USER_ACTION_GOTO_PAGE_PREVIOUS,
		[TERMO_SYM_PAGEDOWN]  = USER_ACTION_GOTO_PAGE_NEXT,

		[TERMO_SYM_ENTER]     = USER_ACTION_INPUT_CONFIRM,

		[TERMO_SYM_HOME]      = USER_ACTION_INPUT_HOME,
		[TERMO_SYM_END]       = USER_ACTION_INPUT_END,
		[TERMO_SYM_LEFT]      = USER_ACTION_INPUT_LEFT,
		[TERMO_SYM_RIGHT]     = USER_ACTION_INPUT_RIGHT,

		[TERMO_SYM_BACKSPACE] = USER_ACTION_INPUT_DELETE_PREVIOUS,
		[TERMO_SYM_DEL]       = USER_ACTION_INPUT_DELETE_PREVIOUS,
		[TERMO_SYM_DELETE]    = USER_ACTION_INPUT_DELETE_NEXT,
	};
	static ActionMap actions_alt =
	{
		[TERMO_SYM_LEFT]      = USER_ACTION_MOVE_SPLITTER_LEFT,
		[TERMO_SYM_RIGHT]     = USER_ACTION_MOVE_SPLITTER_RIGHT,
	};
	static ActionMap actions_ctrl =
	{
		[TERMO_SYM_UP]        = USER_ACTION_GOTO_ENTRY_PREVIOUS,
		[TERMO_SYM_DOWN]      = USER_ACTION_GOTO_ENTRY_NEXT,
	};

	if (!event->modifiers)
		action = actions[event->code.sym];
	else if (event->modifiers == TERMO_KEYMOD_ALT)
		action = actions_alt[event->code.sym];
	else if (event->modifiers == TERMO_KEYMOD_CTRL)
		action = actions_ctrl[event->code.sym];

	return app_process_user_action (self, action);
}

static gboolean
app_process_ctrl_key (Application *self, termo_key_t *event)
{
	static const UserAction actions[32] =
	{
		[CTRL_KEY ('L')]      = USER_ACTION_REDRAW,

		[CTRL_KEY ('P')]      = USER_ACTION_GOTO_DEFINITION_PREVIOUS,
		[CTRL_KEY ('N')]      = USER_ACTION_GOTO_DEFINITION_NEXT,
		[CTRL_KEY ('B')]      = USER_ACTION_GOTO_PAGE_PREVIOUS,
		[CTRL_KEY ('F')]      = USER_ACTION_GOTO_PAGE_NEXT,

		[CTRL_KEY ('A')]      = USER_ACTION_INPUT_HOME,
		[CTRL_KEY ('E')]      = USER_ACTION_INPUT_END,

		[CTRL_KEY ('H')]      = USER_ACTION_INPUT_DELETE_PREVIOUS,
		[CTRL_KEY ('K')]      = USER_ACTION_INPUT_DELETE_TO_END,
		[CTRL_KEY ('W')]      = USER_ACTION_INPUT_DELETE_PREVIOUS_WORD,
		[CTRL_KEY ('U')]      = USER_ACTION_INPUT_DELETE_TO_HOME,
		[CTRL_KEY ('T')]      = USER_ACTION_INPUT_TRANSPOSE,
	};

	gint64 i = (gint64) event->code.codepoint - 'a' + 1;
	if (i > 0 && i < (gint64) G_N_ELEMENTS (actions))
		return app_process_user_action (self, actions[i]);

	return TRUE;
}

static gboolean
app_process_key (Application *self, termo_key_t *event)
{
	if (event->modifiers == TERMO_KEYMOD_CTRL)
		return app_process_ctrl_key (self, event);
	if (event->modifiers)
		return TRUE;

	gunichar c = event->code.codepoint;
	if (!g_unichar_isprint (c))
	{
		beep ();
		return TRUE;
	}

	if (self->input_confirmed)
	{
		if (self->input->len != 0)
			g_array_remove_range (self->input, 0, self->input->len);
		self->input_pos = 0;
		self->input_confirmed = FALSE;
	}

	g_array_insert_val (self->input, self->input_pos++, c);
	app_search_for_entry (self);
	app_redraw_top (self);
	return TRUE;
}

static void
app_process_left_mouse_click (Application *self, int line, int column)
{
	SAVE_CURSOR
	if (line == 0)
		;  // At the moment there's nothing useful for us to do
	else if (line == 1)
	{
		gsize label_len = g_utf8_strlen (self->search_label, -1);
		gint pos = column - label_len;
		if (pos >= 0)
		{
			self->input_pos = MIN ((guint) pos, self->input->len);
			move (0, label_len + self->input_pos);
			refresh ();
		}
	}
	else if (line <= (int) (app_count_view_items (self) - self->top_offset))
	{
		self->selected = line - TOP_BAR_CUTOFF;
		app_redraw_view (self);
		RESTORE_CURSOR
	}
}

/// Process mouse input.
static gboolean
app_process_mouse (Application *self, termo_key_t *event)
{
	int line, column, button;
	termo_mouse_event_t type;
	termo_interpret_mouse (self->tk, event, &type, &button, &line, &column);

	if (type != TERMO_MOUSE_PRESS)
		return TRUE;

	if (button == 1)
		app_process_left_mouse_click (self, line, column);
	else if (button == 4)
		app_process_user_action (self, USER_ACTION_GOTO_DEFINITION_PREVIOUS);
	else if (button == 5)
		app_process_user_action (self, USER_ACTION_GOTO_DEFINITION_NEXT);

	return TRUE;
}

/// Process input events from the terminal.
static gboolean
app_process_termo_event (Application *self, termo_key_t *event)
{
	switch (event->type)
	{
	case TERMO_TYPE_MOUSE:
		return app_process_mouse (self, event);
	case TERMO_TYPE_KEY:
		return app_process_key (self, event);
	case TERMO_TYPE_KEYSYM:
		return app_process_keysym (self, event);
	default:
		return TRUE;
	}
}

// --- SIGWINCH ----------------------------------------------------------------

static int g_winch_pipe[2];            ///< SIGWINCH signalling pipe.

static void
winch_handler (int signum)
{
	(void) signum;
	write (g_winch_pipe[1], "x", 1);
}

static void
install_winch_handler (void)
{
	struct sigaction act, oldact;

	if (pipe (g_winch_pipe) == -1)
		abort ();

	act.sa_handler = winch_handler;
	act.sa_flags = SA_RESTART;
	sigemptyset (&act.sa_mask);
	sigaction (SIGWINCH, &act, &oldact);
}

// --- Initialisation, event handling ------------------------------------------

static gboolean on_stdin_input_timeout (gpointer data);

static gboolean
process_stdin_input (G_GNUC_UNUSED GIOChannel *source,
	G_GNUC_UNUSED GIOCondition condition, gpointer data)
{
	Application *app = data;
	if (app->tk_timer)
	{
		g_source_remove (app->tk_timer);
		app->tk_timer = 0;
	}

	termo_advisereadable (app->tk);

	termo_key_t event;
	termo_result_t res;
	while ((res = termo_getkey (app->tk, &event)) == TERMO_RES_KEY)
		if (!app_process_termo_event (app, &event))
			goto quit;

	if (res == TERMO_RES_AGAIN)
		app->tk_timer = g_timeout_add (termo_get_waittime (app->tk),
			on_stdin_input_timeout, app);
	else if (res == TERMO_RES_ERROR || res == TERMO_RES_EOF)
		goto quit;

	return TRUE;

quit:
	app_quit (app);
	return FALSE;
}

static gboolean
on_stdin_input_timeout (gpointer data)
{
	Application *app = data;
	termo_key_t event;
	if (termo_getkey_force (app->tk, &event) == TERMO_RES_KEY)
		if (!app_process_termo_event (app, &event))
			app_quit (app);

	app->tk_timer = 0;
	return FALSE;
}

static gboolean
process_winch_input (GIOChannel *source,
	G_GNUC_UNUSED GIOCondition condition, gpointer data)
 {
	char c;
	(void) read (g_io_channel_unix_get_fd (source), &c, 1);

	update_curses_terminal_size ();
	app_process_resize (data);
	return TRUE;
}

static gboolean
on_terminated (gpointer user_data)
{
	app_quit (user_data);
	return TRUE;
}

#ifdef WITH_GTK
static void
app_set_input (Application *self, const gchar *input)
{
	glong size;
	gunichar *output = g_utf8_to_ucs4 (input, -1, NULL, &size, NULL);

	// XXX: signal invalid data?
	if (!output)
		return;

	g_array_free (self->input, TRUE);
	self->input = g_array_new (TRUE, FALSE, sizeof (gunichar));
	self->input_pos = 0;

	gunichar *p = output;
	while (size--)
	{
		// XXX: skip?
		if (!g_unichar_isprint (*p))
			break;

		g_array_insert_val (self->input, self->input_pos++, *p++);
	}
	g_free (output);

	self->input_confirmed = FALSE;
	app_search_for_entry (self);
	app_redraw_top (self);
}

static void
on_selection_text_received (G_GNUC_UNUSED GtkClipboard *clipboard,
	const gchar *text, gpointer data)
{
	Application *app = data;
	rearm_selection_watcher (app);

	if (text)
	{
		if (app->selection_contents && !strcmp (app->selection_contents, text))
			return;

		g_free (app->selection_contents);
		app->selection_contents = g_strdup (text);
		app_set_input (app, text);
	}
	else if (app->selection_contents)
	{
		g_free (app->selection_contents);
		app->selection_contents = NULL;
	}
}

static gboolean
on_selection_timer (gpointer data)
{
	Application *app = data;
	GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_PRIMARY);
	gtk_clipboard_request_text (clipboard, on_selection_text_received, app);

	app->selection_timer = 0;
	return FALSE;
}

static gboolean
on_watch_primary_selection (G_GNUC_UNUSED const gchar *option_name,
	const gchar *value, gpointer data, GError **error)
{
	AppOptions *options = data;

	if (!value)
	{
		options->selection_watcher = 500;
		return TRUE;
	}

	unsigned long timer;
	if (!xstrtoul (&timer, value, 10) || !timer || timer > G_MAXINT)
	{
		g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_FAILED,
			_("Invalid timer value"));
		return FALSE;
	}
	options->selection_watcher = timer;
	return TRUE;
}
#endif  // WITH_GTK

static void
log_handler (const gchar *domain, GLogLevelFlags level,
	const gchar *message, gpointer data)
{
	// There's probably no point in trying to display a fatal message nicely
	if (level & G_LOG_FLAG_FATAL)
		g_log_default_handler (domain, level, message, NULL);

	const gchar *prefix;
	switch (level & G_LOG_LEVEL_MASK)
	{
	case G_LOG_LEVEL_ERROR:    prefix = "E"; break;
	case G_LOG_LEVEL_CRITICAL: prefix = "C"; break;
	case G_LOG_LEVEL_WARNING:  prefix = "W"; break;
	case G_LOG_LEVEL_MESSAGE:  prefix = "M"; break;
	case G_LOG_LEVEL_INFO:     prefix = "I"; break;
	case G_LOG_LEVEL_DEBUG:    prefix = "D"; break;
	default:                   prefix = "?";
	}

	gchar *out;
	if (domain)
		out = g_strdup_printf ("%s: %s: %s", prefix, domain, message);
	else
		out = g_strdup_printf ("%s: %s", prefix, message);

	// If the standard error output isn't redirected, try our best at showing
	// the message to the user; it will probably get overdrawn soon
	if (isatty (STDERR_FILENO))
	{
		// Beep, beep, I'm a jeep; let the user know
		beep ();

		// We certainly don't want to end up in a possibly infinite recursion
		static gboolean in_processing;
		if (in_processing)
			goto out;

		in_processing = TRUE;
		SAVE_CURSOR

		Application *self = data;
		attrset (A_REVERSE);
		mvwhline (stdscr, 0, 0, A_REVERSE, COLS);
		app_add_utf8_string (self, out, 0, COLS);

		RESTORE_CURSOR
		in_processing = FALSE;
	}
	else
		fprintf (stderr, "%s\n", out);

out:
	g_free (out);
}

int
main (int argc, char *argv[])
{
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	if (glib_check_version (2, 36, 0))
		g_type_init ();
G_GNUC_END_IGNORE_DEPRECATIONS

	AppOptions options =
	{
		.show_version = FALSE,
		.selection_watcher = -1,
	};

	GOptionEntry entries[] =
	{
		{ "version", 0, G_OPTION_FLAG_IN_MAIN,
		  G_OPTION_ARG_NONE, &options.show_version,
		  N_("Output version information and exit"), NULL },
#ifdef WITH_GTK
		{ "watch-primary-selection", 'w',
		  G_OPTION_FLAG_IN_MAIN | G_OPTION_FLAG_OPTIONAL_ARG,
		  G_OPTION_ARG_CALLBACK, (gpointer) on_watch_primary_selection,
		  N_("Watch the value of the primary selection for input"),
		  N_("TIMER") },
#endif  // WITH_GTK
		{ NULL }
	};

	if (!setlocale (LC_ALL, ""))
		g_printerr ("%s: %s\n", _("Warning"), _("failed to set the locale"));

	bindtextdomain (GETTEXT_PACKAGE, GETTEXT_DIRNAME);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	GError *error = NULL;
	GOptionContext *ctx = g_option_context_new
		(N_("dictionary.ifo - StarDict terminal UI"));
	GOptionGroup *group = g_option_group_new ("", "", "", &options, NULL);
	g_option_group_add_entries (group, entries);
	g_option_group_set_translation_domain (group, GETTEXT_PACKAGE);
	g_option_context_add_group (ctx, group);
	g_option_context_set_translation_domain (ctx, GETTEXT_PACKAGE);
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
	{
		g_printerr ("%s: %s: %s\n", _("Error"), _("option parsing failed"),
			error->message);
		exit (EXIT_FAILURE);
	}

	if (options.show_version)
	{
		g_print (PROJECT_NAME " " PROJECT_VERSION "\n");
		exit (EXIT_SUCCESS);
	}

	if (argc != 2)
	{
		gchar *help = g_option_context_get_help (ctx, TRUE, FALSE);
		g_printerr ("%s", help);
		g_free (help);
		exit (EXIT_FAILURE);
	}

	g_option_context_free (ctx);

	Application app;
	app_init (&app, &options, argv[1]);

	TERMO_CHECK_VERSION;
	if (!(app.tk = termo_new (STDIN_FILENO, NULL, 0)))
		abort ();

	if (!initscr () || nonl () == ERR)
		abort ();
	app_redraw (&app);

	// g_unix_signal_add() cannot handle SIGWINCH
	install_winch_handler ();

	// GtkClipboard can internally issue some rather disruptive warnings
	g_log_set_default_handler (log_handler, &app);

	// Message loop
	guint watch_term  = g_unix_signal_add (SIGTERM, on_terminated, &app);
	guint watch_int   = g_unix_signal_add (SIGINT,  on_terminated, &app);
	guint watch_stdin = g_io_add_watch (g_io_channel_unix_new (STDIN_FILENO),
		G_IO_IN, process_stdin_input, &app);
	guint watch_winch = g_io_add_watch (g_io_channel_unix_new (g_winch_pipe[0]),
		G_IO_IN, process_winch_input, &app);

	app_run (&app);

	g_source_remove (watch_term);
	g_source_remove (watch_int);
	g_source_remove (watch_stdin);
	g_source_remove (watch_winch);

	endwin ();
	g_log_set_default_handler (g_log_default_handler, NULL);
	app_destroy (&app);

	if (close (g_winch_pipe[0]) == -1
	 || close (g_winch_pipe[1]) == -1)
		abort ();

	return 0;
}

