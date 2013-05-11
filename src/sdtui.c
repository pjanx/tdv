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

/* TODO use iconv() wchar_t -> utf-8 */
static gchar *
wchar_to_mb (wchar_t ch)
{
	/* Convert the character back to a multi-byte sequence. */
	static gchar buffer[MB_LEN_MAX + 1];
	size_t len = wcrtomb (buffer, ch, NULL);

	/* This shouldn't happen.  It would mean that the user has
	 * somehow managed to enter something inexpressable in the
	 * current locale.  */
	if (len == (size_t) -1)
		abort ();

	/* Here I hope the buffer doesn't overflow. Who uses
	 * shift states nowadays, anyway? */
	if (wcrtomb (buffer + len, L'\0', NULL) == (size_t) -1)
		abort ();

	return buffer;
}

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

StardictDict *g_dict;                   //!< The current dictionary

guint32 g_top_position;                 //!< Index of the topmost entry
guint g_selected;                       //!< Offset to the selected entry

GString *g_input;                       //!< The current search input
guint g_input_pos;                      //!< Cursor position within input

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
	g_selected = 0;

	g_input = g_string_new (NULL);
	g_input_pos = 0;
}

/** Render the top bar. */
static void
app_redraw_top (void)
{
	mvprintw (0, 0, "%s: ", _("Search"));

	int y, x;
	getyx (stdscr, y, x);

	gchar *input = g_locale_from_utf8 (g_input->str, -1, NULL, NULL, NULL);
	g_return_if_fail (input != NULL);

	addstr (input);
	clrtoeol ();
	g_free (input);

	move (y, x + g_input_pos);
	refresh ();
}

/** Redraw the dictionary view. */
static void
app_redraw_view (void)
{
	// TODO
	refresh ();
}

/** Redraw everything. */
static void
app_redraw (void)
{
	app_redraw_view ();
	app_redraw_top ();
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
			app_redraw ();
			break;
		case KEY_MOUSE:
			// TODO move the item cursor, event->mouse.{x,y,bstate}
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

		app_redraw_top ();
		return TRUE;
	}
	case KEY_NAK: // Ctrl-U -- delete everything before the cursor
		g_string_erase (g_input, 0, utf8_offset (g_input->str, g_input_pos));
		g_input_pos = 0;
		app_redraw_top ();
		return TRUE;
	}

	/* What can you do... wchar_t, utf-8, locale encoding... */
	gchar *letter = g_locale_to_utf8 (wchar_to_mb (event->code),
		-1, NULL, NULL, NULL);
	g_return_val_if_fail (letter != NULL, FALSE);

	if (g_unichar_isprint (g_utf8_get_char (letter)))
	{
		g_string_insert (g_input,
			utf8_offset (g_input->str, g_input_pos), letter);
		g_input_pos += g_utf8_strlen (letter, -1);

		app_redraw_top ();
	}
	g_free (letter);

	return TRUE;
}

/** Free any resources used by the application. */
static void
app_destroy (void)
{
	g_string_free (g_input, TRUE);
	g_object_unref (g_dict);
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
	scrollok (stdscr, TRUE);              /* Also scrolling, pretty please. */

	setscrreg (1, LINES - 1);             /* Create a scroll region. */
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

