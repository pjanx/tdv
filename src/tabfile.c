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

#include "stardict.h"
#include "stardict-private.h"
#include "generator.h"
#include "utils.h"

static gboolean
set_data_error (GError **error, const char *message)
{
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
	return FALSE;
}

static const gchar escapes[256] = { ['n'] = '\n', ['t'] = '\t', ['\\'] = '\\' };

static gboolean
inplace_unescape (char *line, GError **error)
{
	gboolean escape = FALSE;
	char *dest = line;
	for (char *src = line; *src; src++)
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
import_line (Generator *generator, char *line, size_t len, GError **error)
{
	if (!len)
		return TRUE;

	char *separator = strchr (line, '\t');
	if (!separator)
		return set_data_error (error, "keyword separator not found");

	*separator++ = 0;
	if (strchr (line, '\\'))
		// The index wouldn't be sorted correctly with our method
		return set_data_error (error, "escapes not allowed in keywords");

	char *newline = strpbrk (separator, "\r\n");
	if (newline)
		*newline = 0;

	if (!inplace_unescape (line, error)
	 || !inplace_unescape (separator, error))
		return FALSE;

	generator_begin_entry (generator);
	return generator_write_string (generator, separator, TRUE, error)
		&& generator_finish_entry (generator, line, error);
}

static gboolean
transform (FILE *fsorted, Generator *generator, GError **error)
{
	char *line = NULL;
	size_t size = 0, ln = 1;
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

int
main (int argc, char *argv[])
{
	// The GLib help includes an ellipsis character, for some reason
	(void) setlocale (LC_ALL, "");

	GError *error = NULL;
	GOptionContext *ctx = g_option_context_new ("output-basename < input");
	g_option_context_set_summary (ctx,
		"Create a StarDict dictionary from plaintext.");
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
		fatal ("Error: option parsing failed: %s\n", error->message);

	if (argc != 2)
		fatal ("%s", g_option_context_get_help (ctx, TRUE, FALSE));
	g_option_context_free (ctx);

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
	info->version = SD_VERSION_3_0_0;
	info->book_name = g_strdup (argv[1]);
	info->same_type_sequence = g_strdup ("m");

	// This gets incremented each time an entry is finished
	info->word_count = 0;

	if (!transform (fsorted, generator, &error)
	 || !generator_finish (generator, &error))
		fatal ("Error: failed to write the dictionary: %s\n", error->message);

	generator_free (generator);
	fclose (fsorted);
	return 0;
}
