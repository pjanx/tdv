/*
 * test-stardict.c: StarDict API test
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

#include <glib.h>
#include <gio/gio.h>

#include "stardict.h"
#include "stardict-private.h"
#include "generator.h"


// --- Utilities ---------------------------------------------------------------

// Adapted http://gezeiten.org/post/2009/04/Writing-Your-Own-GIO-Jobs
static gboolean remove_recursive (GFile *file, GError **error);

static gboolean
remove_directory_contents (GFile *file, GError **error)
{
	GFileEnumerator *enumerator =
		g_file_enumerate_children (file, "standard::*",
			G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);

	if (!enumerator)
		return FALSE;

	gboolean success = TRUE;
	do
	{
		GError *err = NULL;
		GFileInfo *child_info =
			g_file_enumerator_next_file (enumerator, NULL, &err);

		if (!child_info)
		{
			if (err)
			{
				g_propagate_error (error, err);
				success = FALSE;
			}
			break;
		}

		GFile *child = g_file_resolve_relative_path
			(file, g_file_info_get_name (child_info));
		success = remove_recursive (child, error);
		g_object_unref (child);
		g_object_unref (child_info);
	}
	while (success);

	g_object_unref (enumerator);
	return success;
}

static gboolean
remove_recursive (GFile *file, GError **error)
{
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	GFileInfo *info = g_file_query_info (file, "standard::*",
		G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);

	if (!info)
		return FALSE;

	GFileType type = g_file_info_get_file_type (info);
	g_object_unref (info);

	if (type == G_FILE_TYPE_DIRECTORY &&
		!remove_directory_contents (file, error))
		return FALSE;

	return g_file_delete (file, NULL, error);
}

static gchar *
generate_random_string (gsize length, GRand *rand)
{
	GString *s = g_string_sized_new (length);
	while (length--)
		g_string_append_c (s, g_rand_int_range (rand, 'a', 'z' + 1));
	return g_string_free (s, FALSE);
}

static gpointer
generate_random_data (gsize length, GRand *rand)
{
	gchar *blob = g_malloc (length), *i = blob;
	while (length--)
		*i++ = g_rand_int_range (rand, 0, 256);
	return blob;
}

// --- Dictionary generation ---------------------------------------------------

typedef struct dictionary Dictionary;
typedef struct test_entry TestEntry;

struct dictionary
{
	GFile *tmp_dir;                     ///< A temporary dictionary
	GFile *ifo_file;                    ///< The dictionary's .ifo file
	GArray *data;                       ///< Array of TestEntry's
};

struct test_entry
{
	gchar *word;
	gchar *meaning;
	gpointer data;
	gsize data_size;
};

static void
test_entry_free (TestEntry *te)
{
	g_free (te->word);
	g_free (te->meaning);
	g_free (te->data);
}

static gint
test_entry_word_compare (gconstpointer a, gconstpointer b)
{
	return strcmp (((TestEntry *) a)->word, ((TestEntry *) b)->word);
}

static GArray *
generate_dictionary_data (gsize length)
{
	GRand *rand = g_rand_new_with_seed (0);

	GArray *a = g_array_sized_new (FALSE, FALSE, sizeof (TestEntry), length);
	g_array_set_clear_func (a, (GDestroyNotify) test_entry_free);

	while (length--)
	{
		TestEntry te;

		te.word    = generate_random_string
			(g_rand_int_range (rand, 1,   10), rand);
		te.meaning = generate_random_string
			(g_rand_int_range (rand, 1, 1024), rand);

		te.data_size = g_rand_int_range (rand, 0, 1048576);
		te.data = generate_random_data (te.data_size, rand);

		g_array_append_val (a, te);
	}

	g_rand_free (rand);
	g_array_sort (a, test_entry_word_compare);
	return a;
}

static Dictionary *
dictionary_create (void)
{
	GError *error = NULL;
	gchar *tmp_dir_path = g_dir_make_tmp ("stardict-test-XXXXXX", &error);
	if (!tmp_dir_path)
		g_error ("Failed to create a directory for the test dictionary: %s",
			error->message);

	Dictionary *dict = g_malloc (sizeof *dict);
	dict->tmp_dir = g_file_new_for_path (tmp_dir_path);
	dict->ifo_file = g_file_get_child (dict->tmp_dir, "test.ifo");

	gchar *base = g_build_filename (tmp_dir_path, "test", NULL);
	Generator *generator = generator_new (base, &error);
	g_free (base);

	if (!generator)
		g_error ("Failed to create a dictionary: %s", error->message);

	static const guint dictionary_size = 8;
	dict->data = generate_dictionary_data (dictionary_size);

	generator->info->version             = SD_VERSION_3_0_0;
	generator->info->book_name           = g_strdup ("Test Book");
	generator->info->author              = g_strdup ("Lyra Heartstrings");
	generator->info->email               = g_strdup ("lyra@equestria.net");
	generator->info->description         = g_strdup ("Test dictionary");
	generator->info->date                = g_strdup ("21.12.2012");
	generator->info->same_type_sequence  = g_strdup ("mX");

	guint i;
	for (i = 0; i < dictionary_size; i++)
	{
		TestEntry *te = &g_array_index (dict->data, TestEntry, i);

		generator_begin_entry (generator);
		if (!generator_write_string (generator, te->meaning, TRUE, &error)
		 || !generator_write_raw (generator,
			te->data, te->data_size, FALSE, &error))
			g_error ("Write to dictionary data failed: %s", error->message);

		if (!generator_finish_entry (generator, te->word, &error))
			g_error ("Write to index failed: %s", error->message);
	}

	if (!generator_finish (generator, &error))
		g_error ("Failed to finish the dictionary: %s", error->message);

	g_message ("Successfully created a test dictionary in %s", tmp_dir_path);

	generator_free (generator);
	g_free (tmp_dir_path);
	return dict;
}

static void
dictionary_destroy (Dictionary *dict)
{
	GError *error = NULL;
	if (!remove_recursive (dict->tmp_dir, &error))
		g_error ("Failed to delete the temporary directory: %s",
			error->message);

	g_message ("The test dictionary has been deleted");

	g_object_unref (dict->tmp_dir);
	g_object_unref (dict->ifo_file);
	g_array_free (dict->data, TRUE);
	g_free (dict);
}

// --- Testing -----------------------------------------------------------------

typedef struct dict_fixture DictFixture;

struct dict_fixture
{
	StardictDict *dict;
};

static void
dict_setup (DictFixture *fixture, gconstpointer test_data)
{
	Dictionary *dict = (Dictionary *) test_data;

	gchar *ifo_filename = g_file_get_path (dict->ifo_file);
	fixture->dict = stardict_dict_new (ifo_filename, NULL);
	g_free (ifo_filename);
}

static void
dict_teardown (DictFixture *fixture, G_GNUC_UNUSED gconstpointer test_data)
{
	g_object_unref (fixture->dict);
}

static void
dict_test_list (gconstpointer user_data)
{
	Dictionary *dict = (Dictionary *) user_data;

	gchar *tmp_path = g_file_get_path (dict->tmp_dir);
	GList *dictionaries = stardict_list_dictionaries (tmp_path);
	g_free (tmp_path);

	g_assert (dictionaries != NULL);
	g_assert (dictionaries->next == NULL);

	StardictInfo *info = dictionaries->data;
	GFile *ifo_file = g_file_new_for_path (stardict_info_get_path (info));
	g_assert (g_file_equal (ifo_file, dict->ifo_file) == TRUE);
	g_object_unref (ifo_file);

	g_list_free_full (dictionaries, (GDestroyNotify) stardict_info_free);
}

static void
dict_test_new (gconstpointer user_data)
{
	Dictionary *dict = (Dictionary *) user_data;

	gchar *ifo_filename = g_file_get_path (dict->ifo_file);
	StardictDict *sd = stardict_dict_new (ifo_filename, NULL);
	g_free (ifo_filename);

	g_assert (sd != NULL);
	g_object_unref (sd);
}

static void
dict_test_data_entry (StardictDict *sd, TestEntry *entry)
{
	gboolean success;
	StardictIterator *sdi =
		stardict_dict_search (sd, entry->word, &success);

	g_assert (success == TRUE);
	g_assert (sdi != NULL);
	g_assert (stardict_iterator_is_valid (sdi));

	const gchar *word = stardict_iterator_get_word (sdi);
	g_assert_cmpstr (word, ==, entry->word);

	StardictEntry *sde = stardict_iterator_get_entry (sdi);
	g_assert (sde != NULL);

	const GList *fields = stardict_entry_get_fields (sde);
	const StardictEntryField *sdef;
	g_assert (fields != NULL);
	g_assert (fields->data != NULL);

	sdef = fields->data;
	g_assert (sdef->type == 'm');
	g_assert_cmpstr (sdef->data, ==, entry->meaning);

	fields = fields->next;
	g_assert (fields != NULL);
	g_assert (fields->data != NULL);

	sdef = fields->data;
	g_assert (sdef->type == 'X');
	g_assert_cmpuint (sdef->data_size, ==, entry->data_size);
	g_assert (memcmp (sdef->data, entry->data, entry->data_size) == 0);

	fields = fields->next;
	g_assert (fields == NULL);

	g_object_unref (sde);
	g_object_unref (sdi);
}

static void
dict_test_data (DictFixture *fixture, gconstpointer user_data)
{
	Dictionary *dict = (Dictionary *) user_data;
	GArray *data = dict->data;
	StardictDict *sd = fixture->dict;

	guint i;
	for (i = 0; i < data->len; i++)
	{
		TestEntry *entry = &g_array_index (data, TestEntry, i);
		dict_test_data_entry (sd, entry);
	}
}

int
main (int argc, char *argv[])
{
	g_test_init (&argc, &argv, NULL);

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	if (glib_check_version (2, 36, 0))
		g_type_init ();
G_GNUC_END_IGNORE_DEPRECATIONS

	Dictionary *dict = dictionary_create ();

	g_test_add_data_func ("/dict/list", dict, dict_test_list);
	g_test_add_data_func ("/dict/new", dict, dict_test_new);

	g_test_add ("/dict/data", DictFixture, dict,
		dict_setup, dict_test_data, dict_teardown);

	int result = g_test_run ();
	dictionary_destroy (dict);
	return result;
}
