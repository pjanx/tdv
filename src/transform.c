/*
 * A tool to transform dictionaries dictionaries by an external filter
 *
 * The external filter needs to process NUL-separated textual entries.
 *
 * Example: transform input.info output -- perl -p0e s/bullshit/soykaf/g
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <locale.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gio.h>

#include "stardict.h"
#include "stardict-private.h"
#include "generator.h"

enum { PIPE_READ, PIPE_WRITE };


// --- Main --------------------------------------------------------------------

static inline void
print_progress (gulong *last_percent, StardictIterator *iterator, gsize total)
{
	gulong percent =
		(gulong) stardict_iterator_get_offset (iterator) * 100 / total;
	if (percent != *last_percent)
	{
		printf ("\r  Writing entries... %3lu%%", percent);
		*last_percent = percent;
	}
}

static gboolean
write_to_filter (StardictDict *dict, gint fd, GError **error)
{
	StardictInfo *info = stardict_dict_get_info (dict);
	gsize n_words = stardict_info_get_word_count (info);

	StardictIterator *iterator = stardict_iterator_new (dict, 0);
	gulong last_percent = -1;
	while (stardict_iterator_is_valid (iterator))
	{
		print_progress (&last_percent, iterator, n_words);

		StardictEntry *entry = stardict_iterator_get_entry (iterator);
		for (const GList *fields = stardict_entry_get_fields (entry);
			fields; fields = fields->next)
		{
			StardictEntryField *field = fields->data;
			if (!g_ascii_islower (field->type))
				continue;

			if (write (fd, field->data, field->data_size)
				!= (ssize_t) field->data_size)
			{
				g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
					"%s", strerror (errno));
				return FALSE;
			}
		}

		g_object_unref (entry);
		stardict_iterator_next (iterator);
	}
	printf ("\n");
	return TRUE;
}

static gboolean
update_from_filter (StardictDict *dict, Generator *generator,
	GMappedFile *filtered_file, GError **error)
{
	gchar *filtered = g_mapped_file_get_contents (filtered_file);
	gchar *filtered_end = filtered + g_mapped_file_get_length (filtered_file);

	StardictInfo *info = stardict_dict_get_info (dict);
	gsize n_words = stardict_info_get_word_count (info);

	StardictIterator *iterator = stardict_iterator_new (dict, 0);
	gulong last_percent = -1;
	while (stardict_iterator_is_valid (iterator))
	{
		print_progress (&last_percent, iterator, n_words);

		StardictEntry *entry = stardict_iterator_get_entry (iterator);
		generator_begin_entry (generator);

		for (GList *fields = entry->fields; fields; fields = fields->next)
		{
			StardictEntryField *field = fields->data;
			if (!g_ascii_islower (field->type))
				continue;

			gchar *end = memchr (filtered, 0, filtered_end - filtered);
			if (!end)
			{
				g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
					"filter seems to have ended too early");
				return FALSE;
			}

			g_free (field->data);
			field->data = g_strdup (filtered);
			field->data_size = end - filtered + 1;
			filtered = end + 1;
		}

		if (!generator_write_fields (generator, entry->fields, error)
		 || !generator_finish_entry (generator,
				stardict_iterator_get_word (iterator), error))
			return FALSE;

		g_object_unref (entry);
		stardict_iterator_next (iterator);
	}
	printf ("\n");
	return TRUE;
}

// FIXME: copied from add-pronunciation.c, should merge it somewhere (utils?)
/// Copy the contents of one StardictInfo object into another.  Ignores path.
static void
stardict_info_copy (StardictInfo *dest, const StardictInfo *src)
{
	dest->version = src->version;

	guint i;
	for (i = 0; i < _stardict_ifo_keys_length; i++)
	{
		const struct stardict_ifo_key *key = &_stardict_ifo_keys[i];
		if (key->type == IFO_STRING)
		{
			gchar **p = &G_STRUCT_MEMBER (gchar *, dest, key->offset);
			gchar  *q =  G_STRUCT_MEMBER (gchar *, src,  key->offset);

			g_free (*p);
			*p = q ? g_strdup (q) : NULL;
		}
		else
			G_STRUCT_MEMBER (gulong, dest, key->offset) =
				G_STRUCT_MEMBER (gulong, src, key->offset);
	}
}

int
main (int argc, char *argv[])
{
	// The GLib help includes an ellipsis character, for some reason
	(void) setlocale (LC_ALL, "");

	GError *error = NULL;
	GOptionContext *ctx = g_option_context_new
		("input.ifo output-basename -- FILTER [ARG...]");
	g_option_context_set_summary
		(ctx, "Transform dictionaries using a filter program.");
	g_option_context_set_description (ctx, "Test?");
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
	{
		g_printerr ("Error: option parsing failed: %s\n", error->message);
		exit (EXIT_FAILURE);
	}

	if (argc < 3)
	{
		gchar *help = g_option_context_get_help (ctx, TRUE, FALSE);
		g_printerr ("%s", help);
		g_free (help);
		exit (EXIT_FAILURE);
	}

	// GLib is bullshit, getopt_long() always correctly removes this
	gint program_argv_start = 3;
	if (!strcmp (argv[program_argv_start], "--"))
		program_argv_start++;

	g_option_context_free (ctx);

	printf ("Loading the original dictionary...\n");
	StardictDict *dict = stardict_dict_new (argv[1], &error);
	if (!dict)
	{
		g_printerr ("Error: opening the dictionary failed: %s\n",
			error->message);
		exit (EXIT_FAILURE);
	}

	printf ("Filtering entries...\n");
	gint child_in[2];
	if (!g_unix_open_pipe (child_in, 0, &error))
		g_error ("g_unix_open_pipe: %s", error->message);

	FILE *child_out = tmpfile ();
	if (!child_out)
		g_error ("tmpfile: %s", strerror (errno));

	GPid pid = -1;
	if (!g_spawn_async_with_fds (NULL /* working_directory */,
		argv + program_argv_start /* forward a part of ours */, NULL /* envp */,
		G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
		NULL /* child_setup */, NULL /* user_data */,
		&pid, child_in[PIPE_READ], fileno (child_out), STDERR_FILENO, &error))
		g_error ("g_spawn: %s", error->message);
	if (!write_to_filter (dict, child_in[PIPE_WRITE], &error))
		g_error ("write_to_filter: %s", error->message);
	if (!g_close (child_in[PIPE_READ], &error)
	 || !g_close (child_in[PIPE_WRITE], &error))
		g_error ("g_close: %s", error->message);

	printf ("Waiting for the filter to finish...\n");
	int wstatus = errno = 0;
	if (waitpid (pid, &wstatus, 0) < 1
	 || !WIFEXITED (wstatus) || WEXITSTATUS (wstatus) > 0)
		g_error ("Filter failed (%s, status %d)", strerror (errno), wstatus);

	GMappedFile *filtered = g_mapped_file_new_from_fd (fileno (child_out),
		FALSE /* writable */, &error);
	if (!filtered)
		g_error ("g_mapped_file_new_from_fd: %s", error->message);

	printf ("Writing the new dictionary...\n");
	Generator *generator = generator_new (argv[2], &error);
	if (!generator)
	{
		g_printerr ("Error: failed to create the output dictionary: %s\n",
			error->message);
		exit (EXIT_FAILURE);
	}

	StardictInfo *info = generator->info;
	stardict_info_copy (info, stardict_dict_get_info (dict));

	// This gets incremented each time an entry is finished
	info->word_count = 0;

	if (!update_from_filter (dict, generator, filtered, &error)
	 || !generator_finish (generator, &error))
	{
		g_printerr ("Error: failed to write the dictionary: %s\n",
			error->message);
		exit (EXIT_FAILURE);
	}

	g_mapped_file_unref (filtered);
	fclose (child_out);
	generator_free (generator);
	g_object_unref (dict);
	return 0;
}
