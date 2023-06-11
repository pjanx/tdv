/*
 * A tool to transform dictionaries dictionaries by an external filter
 *
 * The external filter needs to process NUL-separated textual entries.
 *
 * Example: tdv-transform input.ifo output -- perl -p0e s/bullshit/soykaf/g
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
#include "utils.h"

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
				g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
					"%s", g_strerror (errno));
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
				g_set_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
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
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
		fatal ("Error: option parsing failed: %s\n", error->message);

	if (argc < 3)
		fatal ("%s", g_option_context_get_help (ctx, TRUE, NULL));

	// GLib is bullshit, getopt_long() always correctly removes this
	gint program_argv_start = 3;
	if (!strcmp (argv[program_argv_start], "--"))
		program_argv_start++;

	g_option_context_free (ctx);

	printf ("Loading the original dictionary...\n");
	StardictDict *dict = stardict_dict_new (argv[1], &error);
	if (!dict)
		fatal ("Error: opening the dictionary failed: %s\n", error->message);

	printf ("Filtering entries...\n");
	gint child_in[2];
	if (!g_unix_open_pipe (child_in, 0, &error))
		fatal ("g_unix_open_pipe: %s\n", error->message);

	FILE *child_out = tmpfile ();
	if (!child_out)
		fatal ("tmpfile: %s\n", g_strerror (errno));

	GPid pid = -1;
	if (!g_spawn_async_with_fds (NULL /* working_directory */,
		argv + program_argv_start /* forward a part of ours */, NULL /* envp */,
		G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
		NULL /* child_setup */, NULL /* user_data */,
		&pid, child_in[PIPE_READ], fileno (child_out), STDERR_FILENO, &error))
		fatal ("g_spawn: %s\n", error->message);
	if (!write_to_filter (dict, child_in[PIPE_WRITE], &error))
		fatal ("write_to_filter: %s\n", error->message);
	if (!g_close (child_in[PIPE_READ], &error)
	 || !g_close (child_in[PIPE_WRITE], &error))
		fatal ("g_close: %s\n", error->message);

	printf ("Waiting for the filter to finish...\n");
	int wstatus = errno = 0;
	if (waitpid (pid, &wstatus, 0) < 1
	 || !WIFEXITED (wstatus) || WEXITSTATUS (wstatus) > 0)
		fatal ("Filter failed (%s, status %d)\n", g_strerror (errno), wstatus);

	GMappedFile *filtered = g_mapped_file_new_from_fd (fileno (child_out),
		FALSE /* writable */, &error);
	if (!filtered)
		fatal ("g_mapped_file_new_from_fd: %s\n", error->message);

	printf ("Writing the new dictionary...\n");
	Generator *generator = generator_new (argv[2], &error);
	if (!generator)
		fatal ("Error: failed to create the output dictionary: %s\n",
			error->message);

	StardictInfo *info = generator->info;
	stardict_info_copy (info, stardict_dict_get_info (dict));

	// This gets incremented each time an entry is finished
	info->word_count = 0;

	if (!update_from_filter (dict, generator, filtered, &error)
	 || !generator_finish (generator, &error))
		fatal ("Error: failed to write the dictionary: %s\n", error->message);

	g_mapped_file_unref (filtered);
	fclose (child_out);
	generator_free (generator);
	g_object_unref (dict);
	return 0;
}
