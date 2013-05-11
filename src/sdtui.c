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

// --- Utilities ---------------------------------------------------------------

static void
display (const gchar *format, ...)
{
	va_list ap;

	va_start (ap, format);
	vw_printw (stdscr, format, ap);
	va_end (ap);
	refresh ();
}

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

static const gchar *
wchar_to_mb_escaped (wchar_t ch)
{
	switch (ch)
	{
	case L'\r':  return "\\r";
	case L'\n':  return "\\n";
	case L'\t':  return "\\t";
	default:     return wchar_to_mb (ch);
	}
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

// --- Event handlers ----------------------------------------------------------

typedef struct
{
	wint_t  code;
	guint   is_char : 1;
	MEVENT  mouse;
}
CursesEvent;

static gboolean
process_curses_event (CursesEvent *event)
{
	if (!event->is_char)
	{
		switch (event->code)
		{
		case KEY_RESIZE:
			display ("Screen has been resized to %u x %u\n",
				COLS, LINES);
			break;
		case KEY_MOUSE:
			display ("Mouse event at (%d, %d), state %#lx\n",
				event->mouse.x, event->mouse.y, event->mouse.bstate);
			break;
		default:
			display ("Keyboard event: non-character: %u\n",
				event->code);
		}
		return TRUE;
	}

	display ("Keyboard event: character: '%s'\n",
		wchar_to_mb_escaped (event->code));

	if (event->code == L'q' || event->code == KEY_ESCAPE)
	{
		display ("Quitting...\n");
		return FALSE;
	}

	return TRUE;
}

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
		if (!process_curses_event (&event))
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
	GOptionContext *ctx = g_option_context_new ("- StarDict terminal UI");
	g_option_context_add_main_entries (ctx, entries, NULL);
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
	{
		g_printerr ("Error: option parsing failed: %s\n", error->message);
		exit (EXIT_FAILURE);
	}

	if (!initscr ()
	 || cbreak () == ERR
	 || noecho () == ERR)
		abort ();

	keypad (stdscr, TRUE);                /* Enable character processing. */
	nodelay (stdscr, TRUE);               /* Don't block on get_wch(). */

	mousemask (ALL_MOUSE_EVENTS, NULL);

	display ("Press Q, Escape or ^C to quit\n");

	if (pipe (g_winch_pipe) == -1)
		abort ();

	install_winch_handler ();

// --- Message loop ------------------------------------------------------------

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
		 && !process_winch_input (pollfd[2].fd))
			break;
	}

// --- Cleanup -----------------------------------------------------------------

	endwin ();

	if (close (g_winch_pipe[0]) == -1
	 || close (g_winch_pipe[1]) == -1)
		abort ();

	return 0;
}

