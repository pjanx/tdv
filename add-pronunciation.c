/*
 * A tool to add eSpeak-generated pronunciation to dictionaries
 *
 * Here I use the `espeak' process rather than libespeak because of the GPL.
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


// --- Pronunciation generator -------------------------------------------------

typedef struct worker_data WorkerData;

struct worker_data
{
	guint32 start_entry;                //! The first entry to be processed
	guint32 end_entry;                  //! Past the last entry to be processed

	/* Reader, writer */
	GMutex *dict_mutex;                 //! Locks the dictionary object

	/* Reader */
	GThread *main_thread;               //! A handle to the reader thread
	StardictDict *dict;                 //! The dictionary object
	gpointer output;                    //! Linked-list of pronunciation data

	GMutex *remaining_mutex;            //! Locks the progress stats
	GCond *remaining_cond;              //! Signals a change in progress
	guint32 remaining;                  //! How many entries remain

	/* Writer */
	StardictIterator *iterator;         //! Iterates over the dictionary
	FILE *child_stdin;                  //! Standard input of eSpeak
};

/** Writes to espeak's stdin. */
static gpointer
worker_writer (WorkerData *data)
{
	while (stardict_iterator_get_offset (data->iterator) != data->end_entry)
	{
		g_mutex_lock (data->dict_mutex);
		const gchar *word = stardict_iterator_get_word (data->iterator);
		g_mutex_unlock (data->dict_mutex);

		stardict_iterator_next (data->iterator);
		if (fprintf (data->child_stdin, "%s\n", word) < 0)
			g_error ("write to eSpeak failed: %s", strerror (errno));
	}

	g_object_unref (data->iterator);
	return GINT_TO_POINTER (fclose (data->child_stdin));
}

/** Reads from espeak's stdout. */
static gpointer
worker (WorkerData *data)
{
	/* Spawn eSpeak */
	static gchar *cmdline[] = { "espeak", "--ipa", "-q", NULL };
	gint child_in, child_out;

	GError *error;
	if (!g_spawn_async_with_pipes (NULL, cmdline, NULL,
		G_SPAWN_SEARCH_PATH, NULL, NULL,
		NULL, &child_in, &child_out, NULL, &error))
		g_error ("g_spawn() failed: %s", error->message);

	data->child_stdin = fdopen (child_in, "wb");
	if (!data->child_stdin)
		perror ("fdopen");

	FILE *child_stdout = fdopen (child_out, "rb");
	if (!child_stdout)
		perror ("fdopen");

	/* Spawn a writer thread */
	g_mutex_lock (data->dict_mutex);
	data->iterator = stardict_iterator_new (data->dict, data->start_entry);
	g_mutex_unlock (data->dict_mutex);

	GThread *writer = g_thread_new ("write worker",
		(GThreadFunc) worker_writer, data);

	/* Read the output */
	g_mutex_lock (data->remaining_mutex);
	guint32 remaining = data->remaining;
	g_mutex_unlock (data->remaining_mutex);

	data->output = NULL;
	gpointer *output_end = &data->output;
	while (remaining)
	{
		static gchar next[sizeof (gpointer)];
		GString *s = g_string_new (NULL);
		g_string_append_len (s, next, sizeof next);

		gint c;
		while ((c = fgetc (child_stdout)) != EOF && c != '\n')
			g_string_append_c (s, c);
		if (c == EOF)
			g_error ("eSpeak process died too soon");

		gchar *translation = g_string_free (s, FALSE);
		*output_end = translation;
		output_end = (gpointer *) translation;

		/* We limit progress reporting so that
		 * the mutex doesn't spin like crazy */
		if ((--remaining & 1023) != 0)
			continue;

		g_mutex_lock (data->remaining_mutex);
		data->remaining = remaining;
		g_cond_broadcast (data->remaining_cond);
		g_mutex_unlock (data->remaining_mutex);
	}

	fclose (child_stdout);
	return g_thread_join (writer);
}

// --- Main --------------------------------------------------------------------

int
main (int argc, char *argv[])
{
	gint n_processes = 1;

	GOptionEntry entries[] =
	{
		{ "processes", 'N', G_OPTION_FLAG_IN_MAIN,
		  G_OPTION_ARG_INT, &n_processes,
		  "the number of espeak processes run in parallel", "PROCESSES" },
		{ NULL }
	};

	GError *error = NULL;
	GOptionContext *ctx = g_option_context_new
		("input.ifo output.ifo - add pronunciation to dictionaries");
	g_option_context_add_main_entries (ctx, entries, NULL);
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
	{
		g_print ("option parsing failed: %s\n", error->message);
		exit (EXIT_FAILURE);
	}

	if (argc != 3)
	{
		gchar *help = g_option_context_get_help (ctx, TRUE, FALSE);
		g_print ("%s", help);
		g_free (help);
		exit (EXIT_FAILURE);
	}

	StardictDict *dict = stardict_dict_new (argv[1], &error);
	if (!dict)
	{
		g_printerr ("opening the dictionary failed: %s\n", error->message);
		exit (EXIT_FAILURE);
	}

	gsize n_words = stardict_info_get_word_count
		(stardict_dict_get_info (dict));

	if (n_processes <= 0)
	{
		g_printerr ("Error: there must be at least one process\n");
		exit (EXIT_FAILURE);
	}

	if ((gsize) n_processes > n_words * 1024)
	{
		n_processes = n_words / 1024;
		if (!n_processes)
			n_processes = 1;
		g_printerr ("Warning: too many processes, reducing to %d\n",
			n_processes);
	}

	/* Spawn worker threads to generate pronunciations */
	static GMutex dict_mutex;

	static GMutex remaining_mutex;
	static GCond remaining_cond;

	WorkerData *data = g_alloca (sizeof *data * n_processes);

	gint i;
	for (i = 0; i < n_processes; i++)
	{
		data[i].start_entry = (n_words - 1) *  i      / n_processes;
		data[i].end_entry   = (n_words - 1) * (i + 1) / n_processes;

		data[i].remaining = data[i].end_entry - data[i].start_entry;
		data[i].remaining_mutex = &remaining_mutex;
		data[i].remaining_cond = &remaining_cond;

		data[i].dict = dict;
		data[i].dict_mutex = &dict_mutex;

		data->main_thread = g_thread_new ("worker", (GThreadFunc) worker, data);
	}

	/* Loop while the threads still have some work to do and report status */
	g_mutex_lock (&remaining_mutex);
	for (;;)
	{
		gboolean all_finished = TRUE;
		printf ("\rRetrieving pronunciation... ");
		for (i = 0; i < n_processes; i++)
		{
			printf ("%3u%% ", data[i].remaining * 100
				/ (data[i].end_entry - data[i].start_entry));
			if (data[i].remaining)
				all_finished = FALSE;
		}

		if (all_finished)
			break;
		g_cond_wait (&remaining_cond, &remaining_mutex);
	}
	g_mutex_unlock (&remaining_mutex);

	for (i = 0; i < n_processes; i++)
		g_thread_join (data[i].main_thread);

	// TODO after all processing is done, the program will go through the whole
	//      dictionary and put extended data entries into a new one.
	StardictIterator *iterator = stardict_iterator_new (dict, 0);
	while (stardict_iterator_is_valid (iterator))
	{
		// ...
		stardict_iterator_next (iterator);
	}

	return 0;
}
