/*
 * A tool to add eSpeak-generated pronunciation to dictionaries
 *
 * Here I use the `espeak' process rather than libespeak because of the GPL.
 * It's far from ideal, rather good as a starting point.
 *
 * Copyright (c) 2013, Přemysl Eric Janouch <p@janouch.name>
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

#include <glib.h>
#include <gio/gio.h>

#include "stardict.h"
#include "stardict-private.h"
#include "generator.h"


// --- Pronunciation generator -------------------------------------------------

typedef struct worker_data WorkerData;

struct worker_data
{
	gchar **cmdline;                    ///< eSpeak command line
	guint ignore_acronyms : 1;          ///< Don't spell out acronyms
	GRegex *re_stop;                    ///< Regex for stop sequences
	GRegex *re_acronym;                 ///< Regex for ACRONYMS

	guint32 start_entry;                ///< The first entry to be processed
	guint32 end_entry;                  ///< Past the last entry to be processed

	// Reader, writer
	GMutex *dict_mutex;                 ///< Locks the dictionary object

	// Reader
	GThread *main_thread;               ///< A handle to the reader thread
	StardictDict *dict;                 ///< The dictionary object
	gpointer output;                    ///< Linked-list of pronunciation data

	GMutex *remaining_mutex;            ///< Locks the progress stats
	GCond *remaining_cond;              ///< Signals a change in progress
	guint32 remaining;                  ///< How many entries remain
	guint32 total;                      ///< Total number of entries

	// Writer
	StardictIterator *iterator;         ///< Iterates over the dictionary
	FILE *child_stdin;                  ///< Standard input of eSpeak
};

/// eSpeak splits the output on certain characters.
#define LINE_SPLITTING_CHARS            ".,:;?!"

/// We don't want to include brackets either.
#define OTHER_STOP_CHARS                "([{<"

/// A void word used to make a unique "no pronunciation available" mark.
#define VOID_ENTRY                      "not present in any dictionary"


/// Adds dots between characters.
static gboolean
writer_acronym_cb (const GMatchInfo *info, GString *res,
	G_GNUC_UNUSED gpointer data)
{
	gchar *preceding = g_match_info_fetch (info, 1);
	g_string_append (res, preceding);
	g_free (preceding);

	gchar *word = g_match_info_fetch (info, 2);

	g_string_append_c (res, *word);
	const gchar *p;
	for (p = word + 1; *p; p++)
	{
		g_string_append_c (res, '.');
		g_string_append_c (res, *p);
	}

	g_free (word);
	return FALSE;
}

/// Writes to espeak's stdin.
static gpointer
worker_writer (WorkerData *data)
{
	GError *error = NULL;
	GMatchInfo *match_info;
	while (stardict_iterator_get_offset (data->iterator) != data->end_entry)
	{
		g_mutex_lock (data->dict_mutex);
		const gchar *word = stardict_iterator_get_word (data->iterator);
		g_mutex_unlock (data->dict_mutex);

		word += strspn (word, LINE_SPLITTING_CHARS " \t");
		gchar *x = g_strdup (word);

		// Cut the word if needed be
		error = NULL;
		if (g_regex_match_full (data->re_stop,
			x, -1, 0, 0, &match_info, &error))
		{
			gint start_pos;
			g_match_info_fetch_pos (match_info, 0, &start_pos, NULL);
			x[start_pos] = 0;
		}
		g_match_info_free (match_info);

		// Change acronyms so that they're not pronounced as words
		if (!error && !data->ignore_acronyms)
		{
			char *tmp = g_regex_replace_eval (data->re_acronym,
				x, -1, 0, 0, writer_acronym_cb, NULL, &error);
			g_free (x);
			x = tmp;
		}

		if (error)
		{
			g_printerr ("Notice: error processing '%s': %s\n",
				word, error->message);
			g_clear_error (&error);
			*x = 0;
		}

		// We might have accidentally cut off everything
		if (!*x)
		{
			g_free (x);
			x = g_strdup (VOID_ENTRY);
		}

		stardict_iterator_next (data->iterator);
		if (fprintf (data->child_stdin, "%s\n", x) < 0)
			g_error ("write to eSpeak failed: %s", strerror (errno));

		g_free (x);
	}

	g_object_unref (data->iterator);
	return GINT_TO_POINTER (fclose (data->child_stdin));
}

/// Get the void entry (and test if espeak works).
static gchar *
get_void_entry (gchar *cmdline[])
{
	gchar *output;
	gint exit_status;

	GError *error = NULL;
	if (!g_spawn_sync (NULL, cmdline, NULL,
		G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
		&output, NULL, &exit_status, &error))
	{
		g_printerr ("Error: couldn't spawn espeak: %s", error->message);
		exit (EXIT_FAILURE);
	}

	if (exit_status)
	{
		g_printerr ("Error: espeak returned %d\n", exit_status);
		exit (EXIT_FAILURE);
	}

	return output;
}

/// Reads from espeak's stdout.
static gpointer
worker (WorkerData *data)
{
	// Spawn eSpeak
	GError *error = NULL;
	gint child_in, child_out;
	if (!g_spawn_async_with_pipes (NULL, data->cmdline, NULL,
		G_SPAWN_SEARCH_PATH, NULL, NULL,
		NULL, &child_in, &child_out, NULL, &error))
		g_error ("g_spawn() failed: %s", error->message);

	data->child_stdin = fdopen (child_in, "wb");
	if (!data->child_stdin)
		perror ("fdopen");

	FILE *child_stdout = fdopen (child_out, "rb");
	if (!child_stdout)
		perror ("fdopen");

	// Spawn a writer thread
	g_mutex_lock (data->dict_mutex);
	data->iterator = stardict_iterator_new (data->dict, data->start_entry);
	g_mutex_unlock (data->dict_mutex);

	GThread *writer = g_thread_new ("write worker",
		(GThreadFunc) worker_writer, data);

	// Read the output
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

		// We limit progress reporting so that
		// the mutex doesn't spin like crazy
		if ((--remaining & 255) != 0)
			continue;

		g_mutex_lock (data->remaining_mutex);
		data->remaining = remaining;
		g_cond_broadcast (data->remaining_cond);
		g_mutex_unlock (data->remaining_mutex);
	}

	if (fgetc (child_stdout) != EOF)
	{
		g_printerr ("Error: eSpeak has written more lines than it should. "
			"The output would be corrupt, aborting.\n");
		exit (EXIT_FAILURE);
	}

	fclose (child_stdout);
	return g_thread_join (writer);
}

// --- Main --------------------------------------------------------------------

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
	gint n_processes = 1;
	gchar *voice = NULL;
	gboolean ignore_acronyms = FALSE;

	GOptionEntry entries[] =
	{
		{ "processes", 'N', G_OPTION_FLAG_IN_MAIN,
		  G_OPTION_ARG_INT, &n_processes,
		  "The number of espeak processes run in parallel", "PROCESSES" },
		{ "voice", 'v', G_OPTION_FLAG_IN_MAIN,
		  G_OPTION_ARG_STRING, &voice,
		  "The voice to be used by eSpeak to pronounce the words", "VOICE" },
		{ "ignore-acronyms", 0, G_OPTION_FLAG_IN_MAIN,
		  G_OPTION_ARG_NONE, &ignore_acronyms,
		  "Don't spell out words composed of big letters only", NULL },
		{ NULL }
	};

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	if (glib_check_version (2, 36, 0))
		g_type_init ();
G_GNUC_END_IGNORE_DEPRECATIONS

	GError *error = NULL;
	GOptionContext *ctx = g_option_context_new
		("input.ifo output-basename - add pronunciation to dictionaries");
	g_option_context_add_main_entries (ctx, entries, NULL);
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
	{
		g_printerr ("Error: option parsing failed: %s\n", error->message);
		exit (EXIT_FAILURE);
	}

	if (argc != 3)
	{
		gchar *help = g_option_context_get_help (ctx, TRUE, FALSE);
		g_printerr ("%s", help);
		g_free (help);
		exit (EXIT_FAILURE);
	}

	g_option_context_free (ctx);

	// See if we can run espeak
	static gchar *cmdline[] = { "espeak", "--ipa", "-q", NULL, NULL, NULL };

	if (voice)
	{
		cmdline[3] = "-v";
		cmdline[4] = voice;
	}

	gchar *void_entry = g_strstrip (get_void_entry (cmdline));

	// Load the dictionary
	printf ("Loading the original dictionary...\n");
	StardictDict *dict = stardict_dict_new (argv[1], &error);
	if (!dict)
	{
		g_printerr ("Error: opening the dictionary failed: %s\n",
			error->message);
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

	// Spawn worker threads to generate pronunciation data
	static GMutex dict_mutex;

	static GMutex remaining_mutex;
	static GCond remaining_cond;

	WorkerData *data = g_alloca (sizeof *data * n_processes);

	GRegex *re_stop = g_regex_new ("[" LINE_SPLITTING_CHARS "][ ?]"
		"|\\.\\.\\.|[" OTHER_STOP_CHARS "]", G_REGEX_OPTIMIZE, 0, &error);
	g_assert (re_stop != NULL);

	GRegex *re_acronym = g_regex_new ("(^|\\pZ)(\\p{Lu}+)(?=\\pZ|$)",
		G_REGEX_OPTIMIZE, 0, &error);
	g_assert (re_acronym != NULL);

	gint i;
	for (i = 0; i < n_processes; i++)
	{
		data[i].start_entry = n_words *  i      / n_processes;
		data[i].end_entry   = n_words * (i + 1) / n_processes;

		data[i].total = data[i].remaining =
			data[i].end_entry - data[i].start_entry;
		data[i].remaining_mutex = &remaining_mutex;
		data[i].remaining_cond = &remaining_cond;

		data[i].dict = dict;
		data[i].dict_mutex = &dict_mutex;

		data[i].re_stop = re_stop;
		data[i].re_acronym = re_acronym;

		data[i].cmdline = cmdline;
		data[i].ignore_acronyms = ignore_acronyms;
		data[i].main_thread =
			g_thread_new ("worker", (GThreadFunc) worker, &data[i]);
	}

	// Loop while the threads still have some work to do and report status
	g_mutex_lock (&remaining_mutex);
	for (;;)
	{
		gboolean all_finished = TRUE;
		printf ("\rRetrieving pronunciation... ");
		for (i = 0; i < n_processes; i++)
		{
			printf ("%3u%% ", 100 - data[i].remaining * 100 / data[i].total);
			if (data[i].remaining)
				all_finished = FALSE;
		}

		if (all_finished)
			break;
		g_cond_wait (&remaining_cond, &remaining_mutex);
	}
	g_mutex_unlock (&remaining_mutex);

	putchar ('\n');
	for (i = 0; i < n_processes; i++)
		g_thread_join (data[i].main_thread);

	g_regex_unref (re_stop);
	g_regex_unref (re_acronym);

	// Put extended entries into a new dictionary
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

	if (info->same_type_sequence)
	{
		gchar *new_sts = g_strconcat ("t", info->same_type_sequence, NULL);
		g_free (info->same_type_sequence);
		info->same_type_sequence = new_sts;
	}

	// Write out all the entries together with the pronunciation
	for (i = 0; i < n_processes; i++)
	{
		StardictIterator *iterator =
			stardict_iterator_new (dict, data[i].start_entry);

		gpointer *output = data[i].output;
		while (stardict_iterator_get_offset (iterator) != data[i].end_entry)
		{
			printf ("\rCreating a new dictionary... %3lu%%",
				(gulong) stardict_iterator_get_offset (iterator) * 100
				/ stardict_dict_get_info (dict)->word_count);

			g_assert (output != NULL);

			gchar *pronunciation = g_strstrip ((gchar *) (output + 1));
			StardictEntry *entry = stardict_iterator_get_entry (iterator);

			generator_begin_entry (generator);

			if (!strcmp (pronunciation, void_entry))
				*pronunciation = 0;

//			g_printerr ("%s /%s/\n",
//				stardict_iterator_get_word (iterator), pronunciation);

			// For the sake of simplicity we fake a new start;
			// write_fields() only iterates the list in one direction.
			StardictEntryField field;
			field.type = 't';
			field.data = pronunciation;

			GList start_link;
			start_link.next = entry->fields;
			start_link.data = &field;

			if (!generator_write_fields (generator, &start_link, &error)
			 || !generator_finish_entry (generator,
					stardict_iterator_get_word (iterator), &error))
			{
				g_printerr ("Error: write failed: %s\n", error->message);
				exit (EXIT_FAILURE);
			}

			g_object_unref (entry);

			gpointer *tmp = output;
			output = *output;
			g_free (tmp);

			stardict_iterator_next (iterator);
		}

		g_assert (output == NULL);
		g_object_unref (iterator);
	}

	putchar ('\n');
	if (!generator_finish (generator, &error))
	{
		g_printerr ("Error: failed to write the dictionary: %s\n",
			error->message);
		exit (EXIT_FAILURE);
	}

	generator_free (generator);
	g_object_unref (dict);
	g_free (void_entry);
	return 0;
}
