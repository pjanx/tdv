/*
 * A tool to query multiple dictionaries for the specified word
 *
 * Intended for use in IRC bots and similar silly things---words go in, one
 * on a line, and entries come out, one dictionary at a time, finalised with
 * an empty line.  Newlines are escaped with `\n', backslashes with `\\'.
 *
 * So far only the `m' field is supported.  Feel free to extend the program
 * according to your needs, it's not very complicated.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <gio/gio.h>

#include "stardict.h"
#include "stardict-private.h"
#include "generator.h"

static guint
count_equal_chars (const gchar *a, const gchar *b)
{
	guint count = 0;
	while (*a && *b)
		if (*a++ == *b++)
			count++;
	return count;
}

static void
do_dictionary (StardictDict *dict, const gchar *word)
{
	gboolean found;
	StardictIterator *iter = stardict_dict_search (dict, word, &found);
	if (!found)
		goto out;

	// Default Stardict ordering is ASCII case-insensitive.
	// Try to find a better matching entry based on letter case:

	gint64 best_offset = stardict_iterator_get_offset (iter);
	guint best_score = count_equal_chars
		(stardict_iterator_get_word (iter), word);

	while (TRUE)
	{
		stardict_iterator_next (iter);
		if (!stardict_iterator_is_valid (iter))
			break;

		const gchar *iter_word = stardict_iterator_get_word (iter);
		if (g_ascii_strcasecmp (iter_word, word))
			break;

		guint score = count_equal_chars (iter_word, word);
		if (score > best_score)
		{
			best_offset = stardict_iterator_get_offset (iter);
			best_score = score;
		}
	}

	stardict_iterator_set_offset (iter, best_offset, FALSE);

	StardictEntry *entry = stardict_iterator_get_entry (iter);
	StardictInfo *info = stardict_dict_get_info (dict);
	const GList *list = stardict_entry_get_fields (entry);
	for (; list; list = list->next)
	{
		StardictEntryField *field = list->data;
		if (field->type == STARDICT_FIELD_MEANING)
		{
			const gchar *desc = field->data;
			printf ("%s:", info->book_name);
			for (; *desc; desc++)
			{
				if (*desc == '\\')
					printf ("\\\\");
				else if (*desc == '\n')
					printf ("\\n");
				else
					putchar (*desc);
			}
			putchar ('\n');
		}
	}
	g_object_unref (entry);
out:
	g_object_unref (iter);
}

int
main (int argc, char *argv[])
{
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	if (glib_check_version (2, 36, 0))
		g_type_init ();
G_GNUC_END_IGNORE_DEPRECATIONS

	GError *error = NULL;
	GOptionContext *ctx = g_option_context_new
		("DICTIONARY.ifo... - query multiple dictionaries");
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
	{
		g_printerr ("Error: option parsing failed: %s\n", error->message);
		exit (EXIT_FAILURE);
	}
	g_option_context_free (ctx);

	if (argc < 2)
	{
		g_printerr ("Error: no dictionaries given\n");
		exit (EXIT_FAILURE);
	}

	guint n_dicts = argc - 1;
	StardictDict **dicts = g_alloca (sizeof *dicts * n_dicts);

	guint i;
	for (i = 1; i <= n_dicts; i++)
	{
		dicts[i - 1] = stardict_dict_new (argv[i], &error);
		if (error)
		{
			g_printerr ("Error: opening dictionary `%s' failed: %s\n",
				argv[i], error->message);
			exit (EXIT_FAILURE);
		}
	}

	while (TRUE)
	{
		GString *s = g_string_new (NULL);

		gint c;
		while ((c = getchar ()) != EOF && c != '\n')
			if (c != '\r')
				g_string_append_c (s, c);

		if (s->len)
			for (i = 0; i < n_dicts; i++)
				do_dictionary (dicts[i], s->str);

		printf ("\n");
		fflush (NULL);
		g_string_free (s, TRUE);

		if (c == EOF)
			break;
	}

	for (i = 0; i < n_dicts; i++)
		g_object_unref (dicts[i]);

	return 0;
}
