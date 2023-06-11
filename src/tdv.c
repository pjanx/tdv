/*
 * Translation dictionary viewer
 *
 * Copyright (c) 2023, Přemysl Eric Janouch <p@janouch.name>
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

#include "config.h"

#include <glib.h>
#include <glib/gi18n.h>
#ifdef WITH_GUI
#include <gtk/gtk.h>
#endif

#include <locale.h>
#ifndef G_OS_WIN32
#include <unistd.h>
#endif

int tui_main (char *[]);
int gui_main (char *[]);

int
main (int argc, char *argv[])
{
	if (!setlocale (LC_ALL, ""))
		g_printerr ("%s: %s\n", _("Warning"), _("failed to set the locale"));

	bindtextdomain (GETTEXT_PACKAGE, GETTEXT_DIRNAME);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	gboolean show_version = FALSE;
#ifdef WITH_GUI
# ifndef G_OS_WIN32
	gboolean gui = FALSE;
# endif
#endif
	GOptionEntry entries[] =
	{
		{ "version", 0, G_OPTION_FLAG_IN_MAIN,
		  G_OPTION_ARG_NONE, &show_version,
		  N_("Output version information and exit"), NULL },
#ifdef WITH_GUI
# ifndef G_OS_WIN32
		{ "gui", 0, G_OPTION_FLAG_IN_MAIN,
		  G_OPTION_ARG_NONE, &gui,
		  N_("Launch the GUI even when run from a terminal"), NULL },
# endif
#endif
		{ },
	};

	GOptionContext *ctx = g_option_context_new
		(N_("[dictionary.ifo...] - Translation dictionary viewer"));
	g_option_context_add_main_entries (ctx, entries, GETTEXT_PACKAGE);
#ifdef WITH_GUI
	g_option_context_add_group (ctx, gtk_get_option_group (FALSE));
#endif
	g_option_context_set_translation_domain (ctx, GETTEXT_PACKAGE);

	GError *error = NULL;
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
	{
		g_printerr ("%s: %s: %s\n", _("Error"), _("option parsing failed"),
			error->message);
		exit (EXIT_FAILURE);
	}
	g_option_context_free (ctx);

	if (show_version)
	{
		g_print (PROJECT_NAME " " PROJECT_VERSION "\n");
		exit (EXIT_SUCCESS);
	}

#ifdef WITH_GUI
# ifndef G_OS_WIN32
	if (gui || !isatty (STDIN_FILENO))
# endif
		return gui_main (argv + 1);
#endif
#ifndef G_OS_WIN32
	return tui_main (argv + 1);
#endif
}
