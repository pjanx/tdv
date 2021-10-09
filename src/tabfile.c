/*
 * A clean reimplementation of StarDict's tabfile
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <locale.h>

#include <glib.h>
#include <gio/gio.h>
#include <pango/pango.h>

#include <unicode/ucol.h>

#include "config.h"
#include "stardict.h"
#include "stardict-private.h"
#include "generator.h"
#include "utils.h"


static gboolean
set_data_error (GError **error, const gchar *message)
{
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
	return FALSE;
}

static const gchar escapes[256] = { ['n'] = '\n', ['t'] = '\t', ['\\'] = '\\' };

static gboolean
inplace_unescape (gchar *line, GError **error)
{
	gboolean escape = FALSE;
	gchar *dest = line;
	for (gchar *src = line; *src; src++)
	{
		if (escape)
		{
			escape = FALSE;
			if (!(*dest++ = escapes[(guchar) *src]))
				return set_data_error (error, "unsupported escape");
		}
		else if (*src == '\\')
			escape = TRUE;
		else
			*dest++ = *src;
	}
	if (escape)
		return set_data_error (error, "trailing escape character");

	*dest = 0;
	return TRUE;
}

static gboolean
import_line (Generator *generator, gchar *line, gsize len, GError **error)
{
	if (!len)
		return TRUE;
	if (!g_utf8_validate_len (line, len, NULL))
		return set_data_error (error, "not valid UTF-8");

	gchar *separator = strchr (line, '\t');
	if (!separator)
		return set_data_error (error, "keyword separator not found");

	*separator++ = 0;
	if (strchr (line, '\\'))
		// The index wouldn't be sorted correctly with our method
		return set_data_error (error, "escapes not allowed in keywords");

	gchar *newline = strpbrk (separator, "\r\n");
	if (newline)
		*newline = 0;

	if (!inplace_unescape (line, error)
	 || !inplace_unescape (separator, error))
		return FALSE;

	if (generator->info->same_type_sequence
	 && *generator->info->same_type_sequence == STARDICT_FIELD_PANGO
	 && !pango_parse_markup (separator, -1, 0, NULL, NULL, NULL, error))
		return FALSE;

	generator_begin_entry (generator);
	return generator_write_string (generator, separator, TRUE, error)
		&& generator_finish_entry (generator, line, error);
}

static gboolean
transform (FILE *fsorted, Generator *generator, GError **error)
{
	gchar *line = NULL;
	gsize size = 0, ln = 1;
	for (ssize_t read; (read = getline (&line, &size, fsorted)) >= 0; ln++)
		if (!import_line (generator, line, read, error))
			break;

	free (line);
	if (ferror (fsorted))
	{
		g_set_error_literal (error, G_IO_ERROR,
			g_io_error_from_errno (errno), g_strerror (errno));
		return FALSE;
	}
	if (!feof (fsorted))
	{
		// You'll only get good line number output with presorted input!
		g_prefix_error (error, "line %zu: ", ln);
		return FALSE;
	}
	return TRUE;
}

static void
validate_collation_locale (const gchar *locale)
{
	UErrorCode error = U_ZERO_ERROR;
	UCollator *collator = ucol_open (locale, &error);
	if (!collator)
		fatal ("failed to create a collator for %s: %s\n",
			locale, u_errorName (error));
	ucol_close (collator);
}

int
main (int argc, char *argv[])
{
	// The GLib help includes an ellipsis character, for some reason
	(void) setlocale (LC_ALL, "");

	GError *error = NULL;
	GOptionContext *ctx = g_option_context_new ("output-basename < input");
	g_option_context_set_summary (ctx,
		"Create a StarDict dictionary from plaintext.");

	gboolean pango_markup = FALSE;
	StardictInfo template = {};
	GOptionEntry entries[] =
	{
		{ "pango",       'p', 0, G_OPTION_ARG_NONE,   &pango_markup,
		  "Entries use Pango markup", NULL },

		{ "book-name",   'b', 0, G_OPTION_ARG_STRING, &template.book_name,
		  "Set the book name field", "TEXT" },
		{ "author",      'a', 0, G_OPTION_ARG_STRING, &template.author,
		  "Set the author field ", "NAME" },
		{ "e-mail",      'e', 0, G_OPTION_ARG_STRING, &template.email,
		  "Set the e-mail field", "ADDRESS" },
		{ "website",     'w', 0, G_OPTION_ARG_STRING, &template.website,
		  "Set the website field", "LINK" },
		{ "description", 'd', 0, G_OPTION_ARG_STRING, &template.description,
		  "Set the description field (newlines supported)", "TEXT" },
		{ "date",        'D', 0, G_OPTION_ARG_STRING, &template.date,
		  "Set the date field", "DATE" },
		{ "collation",   'c', 0, G_OPTION_ARG_STRING, &template.collation,
		  "Set the collation field (for ICU)", "LOCALE" },
		{ }
	};

	g_option_context_add_main_entries (ctx, entries, GETTEXT_PACKAGE);
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
		fatal ("Error: option parsing failed: %s\n", error->message);
	if (argc != 2)
		fatal ("%s", g_option_context_get_help (ctx, TRUE, FALSE));
	g_option_context_free (ctx);

	template.version = SD_VERSION_3_0_0;
	template.same_type_sequence = pango_markup
		? (char[]) { STARDICT_FIELD_PANGO, 0 }
		: (char[]) { STARDICT_FIELD_MEANING, 0 };

	if (!template.book_name)
		template.book_name = argv[1];
	if (template.description)
	{
		gchar **lines = g_strsplit (template.description, "\n", -1);
		g_free (template.description);
		gchar *in_one_line = g_strjoinv ("<br>", lines);
		g_strfreev (lines);
		template.description = in_one_line;
	}
	if (template.collation)
		validate_collation_locale (template.collation);

	// This actually implements stardict_strcmp(), POSIX-compatibly.
	// Your sort(1) is not expected to be stable by default, like bsdsort is.
	FILE *fsorted = popen ("LC_ALL=C sort -t'\t' -k1f,1", "r");
	if (!fsorted)
		fatal ("%s: %s\n", "popen", g_strerror (errno));

	Generator *generator = generator_new (argv[1], &error);
	if (!generator)
		fatal ("Error: failed to create the output dictionary: %s\n",
			error->message);

	StardictInfo *info = generator->info;
	stardict_info_copy (info, &template);
	if (!transform (fsorted, generator, &error)
	 || !generator_finish (generator, &error))
		fatal ("Error: failed to write the dictionary: %s\n", error->message);

	generator_free (generator);
	fclose (fsorted);
	return 0;
}
