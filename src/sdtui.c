/*
 * StarDict terminal UI
 *
 * Copyright (c) 2013, Přemysl Janouch <p.janouch@gmail.com>
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

#define _XOPEN_SOURCE_EXTENDED         /**< Yes, we want ncursesw. */

#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <stdarg.h>
#include <limits.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>
#include <pango/pango.h>
#include <ncurses.h>

#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>

#include "stardict.h"


#define KEY_SOH      1                 /**< Ctrl-A */
#define KEY_ENQ      5                 /**< Ctrl-E */
#define KEY_VT      11                 /**< Ctrl-K */
#define KEY_NAK     21                 /**< Ctrl-U */
#define KEY_ETB     23                 /**< Ctrl-W */

#define KEY_RETURN  13                 /**< Enter  */
#define KEY_ESCAPE  27                 /**< Esc    */

// These codes may or may not work, depending on the terminal
// They lie above KEY_MAX, originally discovered on gnome-terminal
#define KEY_CTRL_UP     565
#define KEY_CTRL_DOWN   524
#define KEY_CTRL_LEFT   544
#define KEY_CTRL_RIGHT  559

#define KEY_ALT_UP      563
#define KEY_ALT_DOWN    522
#define KEY_ALT_LEFT    542
#define KEY_ALT_RIGHT   557

#define _(x)  x                        /**< Fake gettext, for now. */

// --- Utilities ---------------------------------------------------------------

static int
poll_restart (struct pollfd *fds, nfds_t nfds, int timeout)
{
	int ret;
	do
		ret = poll (fds, nfds, timeout);
	while (ret == -1 && errno == EINTR);
	return ret;
}

/** Wrapper for curses event data. */
typedef struct curses_event            CursesEvent;

struct curses_event
{
	wint_t  code;
	guint   is_char : 1;
	MEVENT  mouse;
};

// --- Application -------------------------------------------------------------

/** Data relating to one entry within the dictionary. */
typedef struct view_entry               ViewEntry;
/** Encloses application data. */
typedef struct application              Application;

struct view_entry
{
	gchar   * word;                     //!< Word
	gchar  ** definitions;              //!< Word definition entries
	gsize     definitions_length;       //!< Length of the @a definitions array
};

struct application
{
	GIConv utf8_to_wchar;               //!< utf-8 -> wchar_t conversion
	GIConv wchar_to_utf8;               //!< wchar_t -> utf-8 conversion

	StardictDict *dict;                 //!< The current dictionary

	guint32 top_position;               //!< Index of the topmost dict. entry
	guint top_offset;                   //!< Offset into the top entry
	guint selected;                     //!< Offset to the selected definition
	GPtrArray *entries;                 //!< ViewEntry's within the view

	GArray *input;                      //!< The current search input
	guint input_pos;                    //!< Cursor position within input
	gboolean input_confirmed;           //!< Input has been confirmed

	gfloat division;                    //!< Position of the division column
};


/** Splits the entry and adds it to a pointer array. */
static void
view_entry_split_add (GPtrArray *out, const gchar *text)
{
	gchar **it, **tmp = g_strsplit (text, "\n", -1);
	for (it = tmp; *it; it++)
		if (**it)
			g_ptr_array_add (out, g_strdup (*it));
	g_strfreev (tmp);
}

/** Decomposes a dictionary entry into the format we want. */
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

/** Release resources associated with the view entry. */
static void
view_entry_free (ViewEntry *ve)
{
	g_free (ve->word);
	g_strfreev (ve->definitions);
	g_slice_free1 (sizeof *ve, ve);
}

/** Reload view items. */
static void
app_reload_view (Application *self)
{
	if (self->entries->len != 0)
		g_ptr_array_remove_range (self->entries, 0, self->entries->len);

	self->selected = 0;
	gint remains = LINES - 1;
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

/** Initialize the application core. */
static void
app_init (Application *self, const gchar *filename)
{
	GError *error;
	self->dict = stardict_dict_new (filename, &error);
	if (!self->dict)
	{
		g_printerr ("Error loading dictionary: %s\n", error->message);
		exit (EXIT_FAILURE);
	}

	self->top_position = 0;
	self->top_offset = 0;
	self->selected = 0;
	self->entries = g_ptr_array_new_with_free_func
		((GDestroyNotify) view_entry_free);

	self->input = g_array_new (TRUE, FALSE, sizeof (gunichar));
	self->input_pos = 0;
	self->input_confirmed = FALSE;

	self->division = 0.5;

	self->wchar_to_utf8 = g_iconv_open ("utf-8//translit", "wchar_t");
	self->utf8_to_wchar = g_iconv_open ("wchar_t//translit", "utf-8");

	app_reload_view (self);
}

/** Free any resources used by the application. */
static void
app_destroy (Application *self)
{
	g_object_unref (self->dict);
	g_ptr_array_free (self->entries, TRUE);
	g_array_free (self->input, TRUE);

	g_iconv_close (self->wchar_to_utf8);
	g_iconv_close (self->utf8_to_wchar);
}

/** Write the given utf-8 string padded with spaces, max. @a n characters. */
static void
add_padded_string (Application *self, const gchar *str, int n)
{
	wchar_t *wide_str = (wchar_t *) g_convert_with_iconv
		(str, -1, self->utf8_to_wchar, NULL, NULL, NULL);
	g_return_if_fail (wide_str != NULL);

	ssize_t wide_len = wcslen (wide_str);
	wchar_t padding = L' ';

	gint i;
	cchar_t cch;
	for (i = 0; i < n; i++)
	{
		setcchar (&cch, (i < wide_len ? &wide_str[i] : &padding),
			A_NORMAL, 0, NULL);
		add_wch (&cch);
	}

	g_free (wide_str);
}

/** Render the top bar. */
static void
app_redraw_top (Application *self)
{
	mvwhline (stdscr, 0, 0, A_UNDERLINE, COLS);
	attrset (A_UNDERLINE);
	printw ("%s: ", _("Search"));

	int y, x;
	getyx (stdscr, y, x);

	gchar *input_utf8 = g_ucs4_to_utf8
		((gunichar *) self->input->data, -1, NULL, NULL, NULL);
	g_return_if_fail (input_utf8 != NULL);

	if (self->input_confirmed)
		attron (A_BOLD);
	add_padded_string (self, input_utf8, COLS - x);
	g_free (input_utf8);

	move (y, x + self->input_pos);
	refresh ();
}

/** Computes width for the left column. */
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

/** Redraw the dictionary view. */
static void
app_redraw_view (Application *self)
{
	move (1, 0);

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
			add_padded_string (self, ve->word, left_width);
			addwstr (L" ");
			add_padded_string (self, ve->definitions[k], COLS - left_width - 1);

			if ((gint) ++shown == LINES - 1)
				goto done;
		}

		k = 0;
	}

done:
	attrset (0);
	clrtobot ();
	refresh ();
}

/** Just prepends a new view entry into the entries array. */
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

/** Just appends a new view entry to the entries array. */
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

/** Counts the number of definitions available for seeing. */
static guint
count_view_items (Application *self)
{
	guint i, n_definitions = 0;
	for (i = 0; i < self->entries->len; i++)
	{
		ViewEntry *entry = g_ptr_array_index (self->entries, i);
		n_definitions += entry->definitions_length;
	}
	return n_definitions;
}

/** Scroll up @a n entries. */
static gboolean
app_scroll_up (Application *self, guint n)
{
	gboolean success = TRUE;
	guint n_definitions = count_view_items (self);
	while (n--)
	{
		if (self->top_offset > 0)
		{
			self->top_offset--;
			continue;
		}

		/* We've reached the top */
		if (self->top_position == 0)
		{
			success = FALSE;
			break;
		}

		ViewEntry *ve = prepend_entry (self, --self->top_position);
		self->top_offset = ve->definitions_length - 1;
		n_definitions += ve->definitions_length;

		/* Remove the last entry if not shown */
		ViewEntry *last_entry =
			g_ptr_array_index (self->entries, self->entries->len - 1);
		if ((gint) (n_definitions - self->top_offset
			- last_entry->definitions_length) >= LINES - 1)
		{
			n_definitions -= last_entry->definitions_length;
			g_ptr_array_remove_index_fast
				(self->entries, self->entries->len - 1);
		}
	}

	app_redraw_view (self);
	return success;
}

/** Scroll down @a n entries. */
static gboolean
app_scroll_down (Application *self, guint n)
{
	gboolean success = TRUE;
	guint n_definitions = count_view_items (self);
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

		if ((gint) (n_definitions - self->top_offset) < LINES - 1)
		{
			ViewEntry *ve = append_entry (self,
				self->top_position + self->entries->len);
			if (ve != NULL)
				n_definitions += ve->definitions_length;
		}
	}

	/* Fix cursor to not point below the view items */
	if (self->selected >= n_definitions - self->top_offset)
		self->selected  = n_definitions - self->top_offset - 1;

	app_redraw_view (self);
	return success;
}

/** Moves the selection one entry up. */
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

/** Moves the selection one entry down. */
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

	if (first > LINES - 2)
	{
		self->selected = LINES - 2;
		app_scroll_down (self, first - (LINES - 2));
	}
	else
	{
		self->selected = first;
		app_redraw_view (self);
	}
}

/** Redraw everything. */
static void
app_redraw (Application *self)
{
	app_redraw_view (self);
	app_redraw_top (self);
}

/** Search for the current entry. */
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
	g_object_unref (iterator);

	app_reload_view (self);
	app_redraw_view (self);
}

/** Process input that's not a character. */
static gboolean
app_process_nonchar_code (Application *self, CursesEvent *event)
{
	switch (event->code)
	{
	case KEY_RESIZE:
		// TODO adapt to the new window size, COLS, LINES
		//      mind the position of the selection cursor
		app_reload_view (self);
		app_redraw (self);
		break;
	case KEY_MOUSE:
		// TODO move the input entry cursor
		if ((event->mouse.bstate & BUTTON1_PRESSED) && event->mouse.y > 0 &&
			event->mouse.y <= (int)
			(count_view_items (self) - self->top_offset))
		{
			self->selected = event->mouse.y - 1;
			app_redraw_view (self);
			app_redraw_top (self); // FIXME just focus
		}
		break;

	case KEY_CTRL_UP:
		app_one_entry_up (self);
		app_redraw_top (self); // FIXME just focus
		break;
	case KEY_CTRL_DOWN:
		app_one_entry_down (self);
		app_redraw_top (self); // FIXME just focus
		break;

	case KEY_ALT_LEFT:
		self->division = (app_get_left_column_width (self) - 1.) / COLS;
		app_redraw_view (self);
		app_redraw_top (self); // FIXME just focus
		break;
	case KEY_ALT_RIGHT:
		self->division = (app_get_left_column_width (self) + 1.) / COLS;
		app_redraw_view (self);
		app_redraw_top (self); // FIXME just focus
		break;

	case KEY_UP:
		if (self->selected > 0)
		{
			self->selected--;
			app_redraw_view (self);
		}
		else
			app_scroll_up (self, 1);
		app_redraw_top (self); // FIXME just focus
		break;
	case KEY_DOWN:
		if ((gint) self->selected < LINES - 2 &&
			self->selected < count_view_items (self) - self->top_offset - 1)
		{
			self->selected++;
			app_redraw_view (self);
		}
		else
			app_scroll_down (self, 1);
		app_redraw_top (self); // FIXME just focus
		break;
	case KEY_PPAGE:
		app_scroll_up (self, LINES - 1);
		app_redraw_top (self); // FIXME just focus
		break;
	case KEY_NPAGE:
		app_scroll_down (self, LINES - 1);
		app_redraw_top (self); // FIXME just focus
		break;

	case KEY_HOME:
		self->input_pos = 0;
		app_redraw_top (self);
		break;
	case KEY_END:
		self->input_pos = self->input->len;
		app_redraw_top (self);
		break;
	case KEY_LEFT:
		if (self->input_pos > 0)
		{
			self->input_pos--;
			app_redraw_top (self);
		}
		break;
	case KEY_RIGHT:
		if (self->input_pos < self->input->len)
		{
			self->input_pos++;
			app_redraw_top (self);
		}
		break;
	case KEY_BACKSPACE:
		if (self->input_pos > 0)
		{
			g_array_remove_index (self->input, --self->input_pos);
			app_search_for_entry (self);
			app_redraw_top (self);
		}
		break;
	case KEY_DC:
		if (self->input_pos < self->input->len)
		{
			g_array_remove_index (self->input, self->input_pos);
			app_search_for_entry (self);
			app_redraw_top (self);
		}
		break;
	}
	return TRUE;
}

/** Process input events from ncurses. */
static gboolean
app_process_curses_event (Application *self, CursesEvent *event)
{
	if (!event->is_char)
		return app_process_nonchar_code (self, event);

	switch (event->code)
	{
	case KEY_ESCAPE:
		return FALSE;
	case KEY_RETURN:
		self->input_confirmed = TRUE;
		app_redraw_top (self);
		break;

	case KEY_SOH: // Ctrl-A -- move to the start of line
		self->input_pos = 0;
		app_redraw_top (self);
		break;
	case KEY_ENQ: // Ctrl-E -- move to the end of line
		self->input_pos = self->input->len;
		app_redraw_top (self);
		break;
	case KEY_VT:  // Ctrl-K -- delete until the end of line
		if (self->input_pos < self->input->len)
		{
			g_array_remove_range (self->input,
				self->input_pos, self->input->len - self->input_pos);
			app_search_for_entry (self);
			app_redraw_top (self);
		}
		return TRUE;
	case KEY_ETB: // Ctrl-W -- delete word before cursor
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
	case KEY_NAK: // Ctrl-U -- delete everything before the cursor
		if (self->input->len != 0)
		{
			g_array_remove_range (self->input, 0, self->input_pos);
			self->input_pos = 0;

			app_search_for_entry (self);
			app_redraw_top (self);
		}
		return TRUE;
	}

	wchar_t code = event->code;
	gchar *letter = g_convert_with_iconv ((gchar *) &code, sizeof code,
		self->wchar_to_utf8, NULL, NULL, NULL);
	g_return_val_if_fail (letter != NULL, FALSE);

	gunichar c = g_utf8_get_char (letter);
	if (g_unichar_isprint (c))
	{
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
	}
	g_free (letter);

	return TRUE;
}

// --- SIGWINCH ----------------------------------------------------------------

static int g_winch_pipe[2];            /**< SIGWINCH signalling pipe. */
static void (*g_old_winch_handler) (int);

static void
winch_handler (int signum)
{
	/* Call the ncurses handler. */
	if (g_old_winch_handler)
		g_old_winch_handler (signum);

	/* And wake up the poll() call. */
	write (g_winch_pipe[1], "x", 1);
}

static void
install_winch_handler (void)
{
	struct sigaction act, oldact;

	act.sa_handler = winch_handler;
	act.sa_flags = SA_RESTART;
	sigemptyset (&act.sa_mask);
	sigaction (SIGWINCH, &act, &oldact);

	/* Save the ncurses handler. */
	if (oldact.sa_handler != SIG_DFL
	 && oldact.sa_handler != SIG_IGN)
		g_old_winch_handler = oldact.sa_handler;
}

// --- Initialisation, event handling ------------------------------------------

Application g_application;

static gboolean
process_stdin_input (void)
{
	CursesEvent event;
	int sta;

	while ((sta = get_wch (&event.code)) != ERR)
	{
		event.is_char = (sta == OK);
		if (sta == KEY_CODE_YES && event.code == KEY_MOUSE
			&& getmouse (&event.mouse) == ERR)
			abort ();
		if (!app_process_curses_event (&g_application, &event))
			return FALSE;
	}

	return TRUE;
}

static gboolean
process_winch_input (int fd)
{
	char c;

	read (fd, &c, 1);
	return process_stdin_input ();
}

int
main (int argc, char *argv[])
{
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	if (glib_check_version (2, 36, 0))
		g_type_init ();
G_GNUC_END_IGNORE_DEPRECATIONS

	static GOptionEntry entries[] =
	{
		{ NULL }
	};

	if (!setlocale (LC_ALL, ""))
		abort ();

	GError *error = NULL;
	GOptionContext *ctx = g_option_context_new
		("dictionary.ifo - StarDict terminal UI");
	g_option_context_add_main_entries (ctx, entries, NULL);
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
	{
		g_printerr ("%s: %s: %s\n", _("Error"), _("option parsing failed"),
			error->message);
		exit (EXIT_FAILURE);
	}

	if (argc != 2)
	{
		gchar *help = g_option_context_get_help (ctx, TRUE, FALSE);
		g_printerr ("%s", help);
		g_free (help);
		exit (EXIT_FAILURE);
	}

	g_option_context_free (ctx);

	app_init (&g_application, argv[1]);

	if (!initscr ()
	 || cbreak () == ERR
	 || noecho () == ERR
	 || nonl () == ERR)
		abort ();

	keypad (stdscr, TRUE);                /* Enable character processing. */
	nodelay (stdscr, TRUE);               /* Don't block on get_wch(). */

	mousemask (ALL_MOUSE_EVENTS, NULL);   /* Register mouse events. */
	mouseinterval (0);

	if (pipe (g_winch_pipe) == -1)
		abort ();
	install_winch_handler ();

	app_redraw (&g_application);

	/* Message loop. */
	struct pollfd pollfd[2];

	pollfd[0].fd = fileno (stdin);
	pollfd[0].events = POLLIN;
	pollfd[1].fd = g_winch_pipe[0];
	pollfd[1].events = POLLIN;

	while (TRUE)
	{
		if (poll_restart (pollfd, 3, -1) == -1)
			abort ();

		if ((pollfd[0].revents & POLLIN)
		 && !process_stdin_input ())
			break;
		if ((pollfd[1].revents & POLLIN)
		 && !process_winch_input (pollfd[1].fd))
			break;
	}

	endwin ();
	app_destroy (&g_application);

	if (close (g_winch_pipe[0]) == -1
	 || close (g_winch_pipe[1]) == -1)
		abort ();

	return 0;
}

