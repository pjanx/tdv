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
#include <ncurses.h>

#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>

#include "stardict.h"


#define KEY_ESCAPE  27                 /**< Curses doesn't define this. */
#define KEY_VT      11                 /**< Ctrl-K */
#define KEY_NAK     21                 /**< Ctrl-U */
#define KEY_ETB     23                 /**< Ctrl-W */

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

static gsize
utf8_offset (const gchar *s, gsize offset)
{
	return g_utf8_offset_to_pointer (s, offset) - s;
}

/** Wrapper for curses event data. */
typedef struct curses_event            CursesEvent;

struct curses_event
{
	wint_t  code;
	guint   is_char : 1;
	MEVENT  mouse;
};

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

// --- Application -------------------------------------------------------------

/** Data relating to one entry within the dictionary. */
typedef struct view_entry               ViewEntry;

struct view_entry
{
	gchar   * word;                     //!< Word
	gchar  ** definitions;              //!< Word definition entries
	gsize     definitions_length;       //!< Length of the @a definitions array
};

GIConv g_utf8_to_wchar;                 //!< utf-8 -> wchar_t conversion
GIConv g_wchar_to_utf8;                 //!< wchar_t -> utf-8 conversion

StardictDict *g_dict;                   //!< The current dictionary

guint32 g_top_position;                 //!< Index of the topmost view entry
guint g_top_offset;                     //!< Offset into the top entry
guint g_selected;                       //!< Offset to the selected definition
GPtrArray *g_entries;                   //!< ViewEntry's within the view

GString *g_input;                       //!< The current search input
guint g_input_pos;                      //!< Cursor position within input


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
	while (fields)
	{
		const StardictEntryField *field = fields->data;
		switch (field->type)
		{
		case STARDICT_FIELD_MEANING:
		{
			gchar **it, **tmp = g_strsplit (field->data, "\n", -1);
			for (it = tmp; *it; it++)
				if (**it)
					g_ptr_array_add (definitions, g_strdup (*it));
			g_strfreev (tmp);
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
app_reload_view (void)
{
	if (g_entries->len != 0)
		g_ptr_array_remove_range (g_entries, 0, g_entries->len);

	g_selected = 0;
	gint remains = LINES - 1;
	StardictIterator *iterator = stardict_iterator_new (g_dict, g_top_position);
	while (remains > 0 && stardict_iterator_is_valid (iterator))
	{
		ViewEntry *entry = view_entry_new (iterator);
		remains -= entry->definitions_length;
		g_ptr_array_add (g_entries, entry);
		stardict_iterator_next (iterator);
	}
	g_object_unref (iterator);
}

/** Initialize the application core. */
static void
app_init (const gchar *filename)
{
	GError *error;
	g_dict = stardict_dict_new (filename, &error);
	if (!g_dict)
	{
		g_printerr ("Error loading dictionary: %s\n", error->message);
		exit (EXIT_FAILURE);
	}

	g_top_position = 0;
	g_top_offset = 0;
	g_selected = 0;
	g_entries = g_ptr_array_new_with_free_func
		((GDestroyNotify) view_entry_free);

	g_input = g_string_new (NULL);
	g_input_pos = 0;

	g_wchar_to_utf8 = g_iconv_open ("utf-8//translit", "wchar_t");
	g_utf8_to_wchar = g_iconv_open ("wchar_t//translit", "utf-8");

	app_reload_view ();
}

/** Free any resources used by the application. */
static void
app_destroy (void)
{
	g_object_unref (g_dict);
	g_ptr_array_free (g_entries, TRUE);
	g_string_free (g_input, TRUE);

	g_iconv_close (g_wchar_to_utf8);
	g_iconv_close (g_utf8_to_wchar);
}

/** Render the top bar. */
static void
app_redraw_top (void)
{
	mvwhline (stdscr, 0, 0, A_UNDERLINE, COLS);
	attrset (A_UNDERLINE);
	printw ("%s: ", _("Search"));

	int y, x;
	getyx (stdscr, y, x);

	gchar *input = g_locale_from_utf8 (g_input->str, -1, NULL, NULL, NULL);
	g_return_if_fail (input != NULL);

	addstr (input);
	g_free (input);

	move (y, x + g_input_pos);
	refresh ();
}

/** Write the given utf-8 string padded with spaces, max. @a n characters. */
static void
add_padded_string (const gchar *str, int n)
{
	wchar_t *wide_str = (wchar_t *) g_convert_with_iconv
		(str, -1, g_utf8_to_wchar, NULL, NULL, NULL);
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

/** Redraw the dictionary view. */
static void
app_redraw_view (void)
{
	move (1, 0);

	guint i, k = g_top_offset, shown = 0;
	for (i = 0; i < g_entries->len; i++)
	{
		ViewEntry *ve = g_ptr_array_index (g_entries, i);
		for (; k < ve->definitions_length; k++)
		{
			int attrs = 0;
			if (shown == g_selected)              attrs |= A_REVERSE;
			if (k + 1 == ve->definitions_length)  attrs |= A_UNDERLINE;
			attrset (attrs);

			add_padded_string (ve->word, COLS / 2);
			addwstr (L" ");
			add_padded_string (ve->definitions[k], COLS - COLS / 2 - 1);

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
prepend_entry (guint32 position)
{
	StardictIterator *iterator = stardict_iterator_new (g_dict, position);
	ViewEntry *ve = view_entry_new (iterator);
	g_object_unref (iterator);

	g_ptr_array_add (g_entries, NULL);
	memmove (g_entries->pdata + 1, g_entries->pdata,
		sizeof ve * (g_entries->len - 1));
	return g_ptr_array_index (g_entries, 0) = ve;
}

/** Just appends a new view entry to the entries array. */
static ViewEntry *
append_entry (guint32 position)
{
	ViewEntry *ve = NULL;
	StardictIterator *iterator = stardict_iterator_new
		(g_dict, position);
	if (stardict_iterator_is_valid (iterator))
	{
		ve = view_entry_new (iterator);
		g_ptr_array_add (g_entries, ve);
	}
	g_object_unref (iterator);
	return ve;
}

/** Counts the number of definitions available for seeing. */
static guint
count_view_items (void)
{
	guint i, n_definitions = 0;
	for (i = 0; i < g_entries->len; i++)
	{
		ViewEntry *entry = g_ptr_array_index (g_entries, i);
		n_definitions += entry->definitions_length;
	}
	return n_definitions;
}

/** Scroll up @a n entries. */
static gboolean
app_scroll_up (guint n)
{
	gboolean success = TRUE;
	guint n_definitions = count_view_items ();
	while (n--)
	{
		if (g_top_offset > 0)
		{
			g_top_offset--;
			continue;
		}

		/* We've reached the top */
		if (g_top_position == 0)
		{
			success = FALSE;
			break;
		}

		ViewEntry *ve = prepend_entry (--g_top_position);
		g_top_offset = ve->definitions_length - 1;
		n_definitions += ve->definitions_length;

		/* Remove the last entry if not shown */
		ViewEntry *last_entry =
			g_ptr_array_index (g_entries, g_entries->len - 1);
		if ((gint) (n_definitions - g_top_offset
			- last_entry->definitions_length) >= LINES - 1)
		{
			g_ptr_array_remove_index_fast
				(g_entries, g_entries->len - 1);
		}
	}

	app_redraw_view ();
	return success;
}

/** Scroll down @a n entries. */
static gboolean
app_scroll_down (guint n)
{
	gboolean success = TRUE;
	guint n_definitions = count_view_items ();
	while (n--)
	{
		if (g_entries->len == 0)
		{
			success = FALSE;
			break;
		}

		ViewEntry *first_entry = g_ptr_array_index (g_entries, 0);
		if (g_top_offset < first_entry->definitions_length - 1)
			g_top_offset++;
		else
		{
			n_definitions -= first_entry->definitions_length;
			g_ptr_array_remove_index (g_entries, 0);
			g_top_position++;
			g_top_offset = 0;
		}

		if ((gint) (n_definitions - g_top_offset) < LINES - 1)
		{
			ViewEntry *ve = append_entry (g_top_position + g_entries->len);
			if (ve != NULL)
				n_definitions += ve->definitions_length;
		}
	}

	/* Fix cursor to not point below the view items */
	if (g_selected >= n_definitions - g_top_offset)
		g_selected  = n_definitions - g_top_offset - 1;

	app_redraw_view ();
	return success;
}

/** Redraw everything. */
static void
app_redraw (void)
{
	app_redraw_view ();
	app_redraw_top ();
}

/** Search for the current entry. */
static void
app_search_for_entry (void)
{
	StardictIterator *iterator = stardict_dict_search
		(g_dict, g_input->str, NULL);
	g_top_position = stardict_iterator_get_offset (iterator);
	g_top_offset = 0;
	g_object_unref (iterator);

	app_reload_view ();
	app_redraw_view ();
}

static gboolean
app_process_curses_event (CursesEvent *event)
{
	/* g_utf8_offset_to_pointer() is too dumb to detect this */
	g_assert (g_utf8_strlen (g_input->str, -1) >= g_input_pos);

	if (!event->is_char)
	{
		switch (event->code)
		{
		case KEY_RESIZE:
			// TODO adapt to the new window size, COLS, LINES
			//      mind the position of the selection cursor
			app_redraw ();
			break;
		case KEY_MOUSE:
			// TODO move the item cursor, event->mouse.{x,y,bstate}
			break;

		case KEY_UP:
			if (g_selected > 0)
			{
				g_selected--;
				app_redraw_view ();
			}
			else
				app_scroll_up (1);
			app_redraw_top (); // FIXME just focus
			break;
		case KEY_DOWN:
			if ((gint) g_selected < LINES - 2 &&
				g_selected < count_view_items () - g_top_offset - 1)
			{
				g_selected++;
				app_redraw_view ();
			}
			else
				app_scroll_down (1);
			app_redraw_top (); // FIXME just focus
			break;
		case KEY_PPAGE:
			app_scroll_up (LINES - 1);
			app_redraw_top (); // FIXME just focus, selection
			break;
		case KEY_NPAGE:
			app_scroll_down (LINES - 1);
			app_redraw_top (); // FIXME just focus, selection
			break;

		case KEY_HOME:
			g_input_pos = 0;
			app_redraw_top ();
			break;
		case KEY_END:
			g_input_pos = g_utf8_strlen (g_input->str, -1);
			app_redraw_top ();
			break;
		case KEY_LEFT:
			if (g_input_pos > 0)
			{
				g_input_pos--;
				app_redraw_top ();
			}
			break;
		case KEY_RIGHT:
			if (g_input_pos < g_utf8_strlen (g_input->str, -1))
			{
				g_input_pos++;
				app_redraw_top ();
			}
			break;
		case KEY_BACKSPACE:
			if (g_input_pos > 0)
			{
				gchar *current = g_utf8_offset_to_pointer
					(g_input->str, g_input_pos);
				gchar *prev = g_utf8_prev_char (current);
				g_string_erase (g_input, prev - g_input->str, current - prev);
				g_input_pos--;
				app_search_for_entry ();
				app_redraw_top ();
			}
			break;
		case KEY_DC:
			if (g_input_pos < g_utf8_strlen (g_input->str, -1))
			{
				gchar *current = g_utf8_offset_to_pointer
					(g_input->str, g_input_pos);
				g_string_erase (g_input, current - g_input->str,
					g_utf8_next_char (current) - current);
				app_search_for_entry ();
				app_redraw_top ();
			}
			break;
		}
		return TRUE;
	}

	switch (event->code)
	{
	case KEY_ESCAPE:
		return FALSE;
	case KEY_VT:  // Ctrl-K -- delete until the end of line
		g_string_erase (g_input, utf8_offset (g_input->str, g_input_pos), -1);

		app_search_for_entry ();
		app_redraw_top ();
		return TRUE;
	case KEY_ETB: // Ctrl-W -- delete word before cursor
	{
		if (!g_input_pos)
			return TRUE;

		gchar *current = g_utf8_offset_to_pointer (g_input->str, g_input_pos);
		gchar *space = g_utf8_strrchr (g_input->str,
			g_utf8_prev_char (current) - g_input->str, ' ');

		if (space)
		{
			space = g_utf8_next_char (space);
			g_string_erase (g_input, space - g_input->str, current - space);
			g_input_pos = g_utf8_pointer_to_offset (g_input->str, space);
		}
		else
		{
			g_string_erase (g_input, 0, current - g_input->str);
			g_input_pos = 0;
		}

		app_search_for_entry ();
		app_redraw_top ();
		return TRUE;
	}
	case KEY_NAK: // Ctrl-U -- delete everything before the cursor
		g_string_erase (g_input, 0, utf8_offset (g_input->str, g_input_pos));
		g_input_pos = 0;

		app_search_for_entry ();
		app_redraw_top ();
		return TRUE;
	}

	wchar_t code = event->code;
	gchar *letter = g_convert_with_iconv ((gchar *) &code, sizeof code,
		g_wchar_to_utf8, NULL, NULL, NULL);
	g_return_val_if_fail (letter != NULL, FALSE);

	if (g_unichar_isprint (g_utf8_get_char (letter)))
	{
		g_string_insert (g_input,
			utf8_offset (g_input->str, g_input_pos), letter);
		g_input_pos += g_utf8_strlen (letter, -1);

		app_search_for_entry ();
		app_redraw_top ();
	}
	g_free (letter);

	return TRUE;
}

// --- Event handlers ----------------------------------------------------------

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
		if (!app_process_curses_event (&event))
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

// --- Main --------------------------------------------------------------------

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

	app_init (argv[1]);

	if (!initscr ()
	 || cbreak () == ERR
	 || noecho () == ERR
	 || nonl () == ERR)
		abort ();

	keypad (stdscr, TRUE);                /* Enable character processing. */
	nodelay (stdscr, TRUE);               /* Don't block on get_wch(). */

	mousemask (ALL_MOUSE_EVENTS, NULL);   /* Register mouse events. */

	if (pipe (g_winch_pipe) == -1)
		abort ();
	install_winch_handler ();

	app_redraw ();

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
	app_destroy ();

	if (close (g_winch_pipe[0]) == -1
	 || close (g_winch_pipe[1]) == -1)
		abort ();

	return 0;
}

