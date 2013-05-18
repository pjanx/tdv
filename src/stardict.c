/*
 * stardict.c: StarDict API
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
#include <locale.h>

#include <glib.h>
#include <gio/gio.h>

#include "stardict.h"
#include "stardict-private.h"


// --- Utilities ---------------------------------------------------------------

/** Read the whole stream into a byte array. */
static gboolean
stream_read_all (GByteArray *ba, GInputStream *is, GError **error)
{
	guint8 buffer[1024 * 64];
	gsize bytes_read;

	while (g_input_stream_read_all (is, buffer, sizeof buffer,
		&bytes_read, NULL, error))
	{
		g_byte_array_append (ba, buffer, bytes_read);
		if (bytes_read < sizeof buffer)
			return TRUE;
	}
	return FALSE;
}

/** Read a null-terminated string from a data input stream. */
static gchar *
stream_read_string (GDataInputStream *dis, GError **error)
{
	gsize length;
	gchar *s = g_data_input_stream_read_upto (dis, "", 1, &length, NULL, error);
	if (!s)
		return NULL;

	GError *err = NULL;
	g_data_input_stream_read_byte (dis, NULL, &err);
	if (err)
	{
		g_free (s);
		g_propagate_error (error, err);
		return NULL;
	}

	return s;
}

/** String compare function used for StarDict indexes. */
static inline gint
stardict_strcmp (const gchar *s1, const gchar *s2)
{
	gint a = g_ascii_strcasecmp (s1, s2);
	return a ? a : strcmp (s1, s2);
}

/** After this statement, the element has been found and its index is stored
 *  in the variable "imid". */
#define BINARY_SEARCH_BEGIN(max, compare)                                     \
	gint imin = 0, imax = max, imid;                                          \
	while (imin <= imax) {                                                    \
		imid = imin + (imax - imin) / 2;                                      \
		gint cmp = compare;                                                   \
		if      (cmp > 0) imin = imid + 1;                                    \
		else if (cmp < 0) imax = imid - 1;                                    \
		else {

/** After this statement, the binary search has failed and "imin" stores
 *  the position where the element can be inserted. */
#define BINARY_SEARCH_END                                                     \
		}                                                                     \
	}

// --- Errors ------------------------------------------------------------------

GQuark
stardict_error_quark (void)
{
	return g_quark_from_static_string ("stardict-error-quark");
}

// --- IFO reader --------------------------------------------------------------

/** Helper class for reading .ifo files. */
typedef struct ifo_reader               IfoReader;

struct ifo_reader
{
	gchar           * data;             //!< File data terminated with \0
	gchar           * data_end;         //!< Where the final \0 char. is

	gchar           * start;            //!< Start of the current token

	gchar           * key;              //!< The key (points into @a data)
	gchar           * value;            //!< The value (points into @a data)
};

static gboolean
ifo_reader_init (IfoReader *ir, const gchar *path, GError **error)
{
	gsize length;
	gchar *contents;
	if (!g_file_get_contents (path, &contents, &length, error))
		return FALSE;

	static const char first_line[] = "StarDict's dict ifo file\n";
	if (length < sizeof first_line - 1
	 || strncmp (contents, first_line, sizeof first_line - 1))
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: invalid header format", path);
		return FALSE;
	}

	ir->data = contents;
	ir->start = contents + sizeof first_line - 1;
	ir->data_end = contents + length;
	return TRUE;
}

static void
ifo_reader_free (IfoReader *ir)
{
	g_free (ir->data);
}

static gint
ifo_reader_read (IfoReader *ir)
{
	ir->key = NULL;
	ir->value = NULL;

	gchar *p;
	for (p = ir->start; p < ir->data_end; p++)
	{
		if (*p == '\n')
		{
			if (!ir->key)
				return -1;

			*p = 0;
			ir->value = ir->start;
			ir->start = p + 1;
			return 1;
		}

		if (*p == '=')
		{
			if (p == ir->start)
				return -1;

			*p = 0;
			ir->key = ir->start;
			ir->start = p + 1;
		}
	}

	if (!ir->key)
	{
		if (p != ir->start)
			return -1;
		return 0;
	}

	ir->value = ir->start;
	ir->start = p;
	return 1;
}

// --- StardictInfo ------------------------------------------------------------

/** Return the filesystem path for the dictionary. */
const gchar *
stardict_info_get_path (StardictInfo *sdi)
{
	return sdi->path;
}

/** Return the name of the dictionary. */
const gchar *
stardict_info_get_book_name (StardictInfo *sdi)
{
	return sdi->book_name;
}

/** Return the word count of the dictionary.  Note that this information comes
 *  from the .ifo file, while the dictionary could successfully load with
 *  a different count of word entries.
 */
gsize
stardict_info_get_word_count (StardictInfo *sdi)
{
	return sdi->word_count;
}

/** Destroy the dictionary info object. */
void
stardict_info_free (StardictInfo *sdi)
{
	g_free (sdi->path);
	g_free (sdi->book_name);
	g_free (sdi->author);
	g_free (sdi->email);
	g_free (sdi->website);
	g_free (sdi->description);
	g_free (sdi->date);
	g_free (sdi->same_type_sequence);
	g_free (sdi);
}

#define DEFINE_IFO_KEY(n, t, e) { (n), IFO_##t, offsetof (StardictInfo, e) }

const struct stardict_ifo_key _stardict_ifo_keys[] =
{
	DEFINE_IFO_KEY ("bookname",         STRING, book_name),
	DEFINE_IFO_KEY ("wordcount",        NUMBER, word_count),
	DEFINE_IFO_KEY ("synwordcount",     NUMBER, syn_word_count),
	DEFINE_IFO_KEY ("idxfilesize",      NUMBER, idx_filesize),
	DEFINE_IFO_KEY ("idxoffsetbits",    NUMBER, idx_offset_bits),
	DEFINE_IFO_KEY ("author",           STRING, author),
	DEFINE_IFO_KEY ("email",            STRING, email),
	DEFINE_IFO_KEY ("website",          STRING, website),
	DEFINE_IFO_KEY ("description",      STRING, description),
	DEFINE_IFO_KEY ("date",             STRING, date),
	DEFINE_IFO_KEY ("sametypesequence", STRING, same_type_sequence)
};

gsize _stardict_ifo_keys_length = G_N_ELEMENTS (_stardict_ifo_keys);

static gboolean
load_ifo (StardictInfo *sti, const gchar *path, GError **error)
{
	IfoReader ir;
	if (!ifo_reader_init (&ir, path, error))
		return FALSE;

	gboolean ret_val = FALSE;
	memset (sti, 0, sizeof *sti);

	if (ifo_reader_read (&ir) != 1 || strcmp (ir.key, "version"))
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: version not specified", path);
		goto error;
	}

	if (!strcmp (ir.value, "2.4.2"))
		sti->version = SD_VERSION_2_4_2;
	else if (!strcmp (ir.value, "3.0.0"))
		sti->version = SD_VERSION_3_0_0;
	else
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: invalid version: %s", path, ir.value);
		goto error;
	}

	gint ret;
	while ((ret = ifo_reader_read (&ir)) == 1)
	{
		guint i;
		for (i = 0; i < _stardict_ifo_keys_length; i++)
			if (!strcmp (ir.key, _stardict_ifo_keys[i].name))
				break;

		if (i == _stardict_ifo_keys_length)
		{
			g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
				"%s: unknown key, ignoring: %s", path, ir.key);
			continue;
		}

		if (_stardict_ifo_keys[i].type == IFO_STRING)
		{
			G_STRUCT_MEMBER (gchar *, sti, _stardict_ifo_keys[i].offset)
				= g_strdup (ir.value);
			continue;
		}

		// Otherwise it has to be IFO_NUMBER
		gchar *end;
		gulong wc = strtol (ir.value, &end, 10);
		if (*end)
		{
			g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
				"%s: invalid integer", path);
			goto error;
		}

		G_STRUCT_MEMBER (gulong, sti, _stardict_ifo_keys[i].offset) = wc;
	}

	if (ret == -1)
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: option format error", path);
		goto error;
	}

	ret_val = TRUE;

	// FIXME check for zeros, don't assume that 0 means for "not set"
	if (!sti->book_name || !*sti->book_name)
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: no book name specified\n", path);
		ret_val = FALSE;
	}
	if (!sti->word_count)
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: word count not specified\n", path);
		ret_val = FALSE;
	}
	if (!sti->idx_filesize)
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: .idx file size not specified\n", path);
		ret_val = FALSE;
	}

	if (!sti->idx_offset_bits)
		sti->idx_offset_bits = 32;
	else if (sti->idx_offset_bits != 32 && sti->idx_offset_bits != 64)
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: wrong index offset bits: %lu\n", path, sti->idx_offset_bits);
		ret_val = FALSE;
	}

error:
	if (!ret_val)
	{
		guint i;
		for (i = 0; i < _stardict_ifo_keys_length; i++)
			if (_stardict_ifo_keys[i].type == IFO_STRING)
				g_free (G_STRUCT_MEMBER (gchar *, sti,
					_stardict_ifo_keys[i].offset));
	}
	else
		sti->path = g_strdup (path);

	ifo_reader_free (&ir);
	return ret_val;
}

/** List all dictionary files located in a path.
 *  @return GList<StardictInfo *>. Deallocate the list with:
 *  @code
 *    g_list_free_full ((GDestroyNotify) stardict_info_free);
 *  @endcode
 */
GList *
stardict_list_dictionaries (const gchar *path)
{
	GPatternSpec *ps = g_pattern_spec_new ("*.ifo");
	GDir *dir = g_dir_open (path, 0, NULL);
	g_return_val_if_fail (dir != NULL, NULL);

	GList *dicts = NULL;
	const gchar *name;
	while ((name = g_dir_read_name (dir)))
	{
		if (!g_pattern_match_string (ps, name))
			continue;

		gchar *filename = g_build_filename (path, name, NULL);
		StardictInfo *ifo = g_new (StardictInfo, 1);
		if (load_ifo (ifo, filename, NULL))
			dicts = g_list_append (dicts, ifo);
		else
			g_free (ifo);
		g_free (filename);
	}
	g_dir_close (dir);
	g_pattern_spec_free (ps);
	return dicts;
}

// --- StardictDict ------------------------------------------------------------

G_DEFINE_TYPE (StardictDict, stardict_dict, G_TYPE_OBJECT)

static void
stardict_dict_finalize (GObject *self)
{
	StardictDict *sd = STARDICT_DICT (self);

	stardict_info_free (sd->info);
	g_array_free (sd->index, TRUE);
	g_array_free (sd->synonyms, TRUE);

	if (sd->mapped_dict)
		g_mapped_file_unref (sd->mapped_dict);
	else
		g_free (sd->dict);

	G_OBJECT_CLASS (stardict_dict_parent_class)->finalize (self);
}

static void
stardict_dict_class_init (StardictDictClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = stardict_dict_finalize;
}

static void
stardict_dict_init (G_GNUC_UNUSED StardictDict *sd)
{
}

/** Load a StarDict dictionary.
 *  @param[in] filename  Path to the .ifo file
 */
StardictDict *
stardict_dict_new (const gchar *filename, GError **error)
{
	StardictInfo *ifo = g_new (StardictInfo, 1);
	if (!load_ifo (ifo, filename, error))
	{
		g_free (ifo);
		return NULL;
	}

	StardictDict *sd = stardict_dict_new_from_info (ifo, error);
	if (!sd)  stardict_info_free (ifo);
	return sd;
}

/** Return information about a loaded dictionary. */
StardictInfo *
stardict_dict_get_info (StardictDict *sd)
{
	g_return_val_if_fail (STARDICT_IS_DICT (sd), NULL);
	return sd->info;
}

/** Load a StarDict index from a GIO input stream. */
static gboolean
load_idx_internal (StardictDict *sd, GInputStream *is, GError **error)
{
	GDataInputStream *dis = g_data_input_stream_new (G_INPUT_STREAM (is));
	g_data_input_stream_set_byte_order (dis,
		G_DATA_STREAM_BYTE_ORDER_BIG_ENDIAN);

	StardictIndexEntry entry;
	GError *err = NULL;
	// Ignoring "wordcount", just reading as long as we can
	while ((entry.name = stream_read_string (dis, &err)))
	{
		if (sd->info->idx_offset_bits == 32)
			entry.data_offset
				= g_data_input_stream_read_uint32 (dis, NULL, &err);
		else
			entry.data_offset
				= g_data_input_stream_read_uint64 (dis, NULL, &err);
		if (err)
			goto error;

		entry.data_size = g_data_input_stream_read_uint32 (dis, NULL, &err);
		if (err)
			goto error;

		g_array_append_val (sd->index, entry);
	}

	if (err != NULL)
		goto error;

	g_object_unref (dis);
	return TRUE;

error:
	g_propagate_error (error, err);
	g_free (entry.name);
	g_object_unref (dis);
	return FALSE;
}

/** Load a StarDict index. */
static gboolean
load_idx (StardictDict *sd, const gchar *filename,
	gboolean gzipped, GError **error)
{
	gboolean ret_val = FALSE;
	GFile *file = g_file_new_for_path (filename);
	GFileInputStream *fis = g_file_read (file, NULL, error);

	if (!fis)
		goto cannot_open;

	if (gzipped)
	{
		GZlibDecompressor *zd
			= g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
		GInputStream *cis = g_converter_input_stream_new
			(G_INPUT_STREAM (fis), G_CONVERTER (zd));

		ret_val = load_idx_internal (sd, cis, error);

		g_object_unref (cis);
		g_object_unref (zd);
	}
	else
		ret_val = load_idx_internal (sd, G_INPUT_STREAM (fis), error);

	g_object_unref (fis);
cannot_open:
	g_object_unref (file);
	return ret_val;
}

static gboolean
load_syn (StardictDict *sd, const gchar *filename, GError **error)
{
	gboolean ret_val = FALSE;
	GFile *file = g_file_new_for_path (filename);
	GFileInputStream *fis = g_file_read (file, NULL, error);

	if (!fis)
		goto cannot_open;

	GDataInputStream *dis = g_data_input_stream_new (G_INPUT_STREAM (fis));
	g_data_input_stream_set_byte_order (dis,
		G_DATA_STREAM_BYTE_ORDER_BIG_ENDIAN);

	StardictSynonymEntry entry;
	GError *err = NULL;
	// Ignoring "synwordcount", just reading as long as we can
	while ((entry.word = stream_read_string (dis, &err)))
	{
		entry.original_word = g_data_input_stream_read_uint32 (dis, NULL, &err);
		if (err)
			break;

		g_array_append_val (sd->synonyms, entry);
	}

	if (err != NULL)
	{
		g_free (entry.word);
		g_propagate_error (error, err);
	}
	else
		ret_val = TRUE;

	g_object_unref (dis);
	g_object_unref (fis);
cannot_open:
	g_object_unref (file);
	return ret_val;
}

/** Destroy an index entry. */
static void
index_destroy_cb (gpointer sde)
{
	StardictIndexEntry *e = sde;
	g_free (e->name);
}

/** Destroy a synonym entry. */
static void
syn_destroy_cb (gpointer sde)
{
	StardictSynonymEntry *e = sde;
	g_free (e->word);
}

/** Load StarDict dictionary data. */
static gboolean
load_dict (StardictDict *sd, const gchar *filename, gboolean gzipped,
	GError **error)
{
	if (gzipped)
	{
		gboolean ret_val = FALSE;
		GFile *file = g_file_new_for_path (filename);
		GFileInputStream *fis = g_file_read (file, NULL, error);

		if (!fis)
			goto cannot_open;

		// Just read it all, as it is, into memory
		GByteArray *ba = g_byte_array_new ();
		GZlibDecompressor *zd
			= g_zlib_decompressor_new (G_ZLIB_COMPRESSOR_FORMAT_GZIP);
		GInputStream *cis = g_converter_input_stream_new
			(G_INPUT_STREAM (fis), G_CONVERTER (zd));

		ret_val = stream_read_all (ba, cis, error);

		g_object_unref (cis);
		g_object_unref (zd);

		if (ret_val)
		{
			sd->dict_length = ba->len;
			sd->dict = g_byte_array_free (ba, FALSE);
		}
		else
			g_byte_array_free (ba, TRUE);

		g_object_unref (fis);
cannot_open:
		g_object_unref (file);
		return ret_val;
	}

	sd->mapped_dict = g_mapped_file_new (filename, FALSE, error);
	if (!sd->mapped_dict)
		return FALSE;

	sd->dict_length = g_mapped_file_get_length (sd->mapped_dict);
	sd->dict = g_mapped_file_get_contents (sd->mapped_dict);
	return TRUE;
}

/** Load a StarDict dictionary.
 *  @param[in] sdi  Parsed .ifo data.
 */
StardictDict *
stardict_dict_new_from_info (StardictInfo *sdi, GError **error)
{
	g_return_val_if_fail (sdi != NULL, NULL);

	StardictDict *sd = g_object_new (STARDICT_TYPE_DICT, NULL);
	sd->info = sdi;
	sd->index = g_array_new (FALSE, FALSE, sizeof (StardictIndexEntry));
	g_array_set_clear_func (sd->index, index_destroy_cb);
	sd->synonyms = g_array_new (FALSE, FALSE, sizeof (StardictSynonymEntry));
	g_array_set_clear_func (sd->synonyms, syn_destroy_cb);

	const gchar *dot = strrchr (sdi->path, '.');
	gchar *base = dot ? g_strndup (sdi->path, dot - sdi->path)
		: g_strdup (sdi->path);

	gchar *base_idx = g_strconcat (base, ".idx", NULL);
	gboolean ret = FALSE;
	if (g_file_test (base_idx, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
		ret = load_idx (sd, base_idx, FALSE, error);
	else
	{
		gchar *base_idx_gz = g_strconcat (base_idx, ".gz", NULL);
		g_free (base_idx);
		base_idx = base_idx_gz;

		if (g_file_test (base_idx, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
			ret = load_idx (sd, base_idx, TRUE, error);
		else
		{
			g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_FILE_NOT_FOUND,
				"%s: cannot find index file", sdi->path);
		}
	}
	g_free (base_idx);

	if (!ret)
		goto error;

	gchar *base_dict = g_strconcat (base, ".dict", NULL);
	ret = FALSE;
	if (g_file_test (base_dict, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
		ret = load_dict (sd, base_dict, FALSE, error);
	else
	{
		gchar *base_dict_dz = g_strconcat (base_dict, ".dz", NULL);
		g_free (base_dict);
		base_dict = base_dict_dz;

		if (g_file_test (base_dict, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
			ret = load_dict (sd, base_dict, TRUE, error);
		else
		{
			g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_FILE_NOT_FOUND,
				"%s: cannot find dict file", sdi->path);
		}
	}
	g_free (base_dict);

	if (!ret)
		goto error;

	gchar *base_syn = g_strconcat (base, ".syn", NULL);
	if (g_file_test (base_syn, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
		load_syn (sd, base_syn, NULL);
	g_free (base_syn);

	g_free (base);
	return sd;

error:
	g_array_free (sd->index, TRUE);
	g_free (base);
	g_object_unref (sd);
	return NULL;
}

/** Return words for which the argument is a synonym of or NULL
 *  if there are no such words.
 */
gchar **
stardict_dict_get_synonyms (StardictDict *sd, const gchar *word)
{
	BINARY_SEARCH_BEGIN (sd->synonyms->len - 1, g_ascii_strcasecmp (word,
			g_array_index (sd->synonyms, StardictSynonymEntry, imid).word))

	// Back off to the first matching entry
	while (imid > 0 && !g_ascii_strcasecmp (word,
		g_array_index (sd->synonyms, StardictSynonymEntry, --imid).word));

	GPtrArray *array = g_ptr_array_new ();

	// And add all matching entries from that position on to the array
	do
		g_ptr_array_add (array, g_strdup (g_array_index
			(sd->index, StardictIndexEntry, g_array_index
			(sd->synonyms, StardictSynonymEntry, ++imid).original_word).name));
	while ((guint) imid < sd->synonyms->len - 1 && !stardict_strcmp (word,
		g_array_index (sd->synonyms, StardictSynonymEntry, imid + 1).word));

	return (gchar **) g_ptr_array_free (array, FALSE);

	BINARY_SEARCH_END

	return NULL;
}

/** Search for a word.
 *  @param[in] word  The word in utf-8 encoding
 *  @param[out] success  TRUE if found
 *  @return An iterator object pointing to the word, or where it would be
 */
StardictIterator *
stardict_dict_search (StardictDict *sd, const gchar *word, gboolean *success)
{
	BINARY_SEARCH_BEGIN (sd->index->len - 1, g_ascii_strcasecmp (word,
		g_array_index (sd->index, StardictIndexEntry, imid).name))

	// Back off to the first matching entry
	while (imid > 0 && !g_ascii_strcasecmp (word,
		g_array_index (sd->index, StardictIndexEntry, imid - 1).name))
		imid--;

	if (success) *success = TRUE;
	return stardict_iterator_new (sd, imid);

	BINARY_SEARCH_END

	if (success) *success = FALSE;
	return stardict_iterator_new (sd, imin);
}

static void
stardict_entry_field_free (StardictEntryField *sef)
{
	g_free (sef->data);
	g_slice_free1 (sizeof *sef, sef);
}

static StardictEntryField *
read_entry (gchar type, const gchar **entry_iterator,
	const gchar *end, gboolean is_final)
{
	const gchar *entry = *entry_iterator;
	if (g_ascii_islower (type))
	{
		GString *data = g_string_new (NULL);

		if (is_final)
		{
			g_string_append_len (data, entry, end - entry);
			entry += end - entry;
		}
		else
		{
			gint c = EOF;
			while (entry < end && (c = *entry++))
				g_string_append_c (data, c);

			if (c != '\0')
				return (gpointer) g_string_free (data, TRUE);
		}

		StardictEntryField *sef = g_slice_alloc (sizeof *sef);
		sef->type = type;
		sef->data_size = data->len + 1;
		sef->data = g_string_free (data, FALSE);
		*entry_iterator = entry;
		return sef;
	}

	gsize length;
	if (is_final)
		length = end - entry;
	else
	{
		if (entry + sizeof (guint32) > end)
			return NULL;

		length = GUINT32_FROM_BE (*(guint32 *) entry);
		entry += sizeof (guint32);

		if (entry + length > end)
			return NULL;
	}

	StardictEntryField *sef = g_slice_alloc (sizeof *sef);
	sef->type = type;
	sef->data_size = length;
	sef->data = memcpy (g_malloc (length), entry, length);
	*entry_iterator = entry + length;
	return sef;
}

static GList *
read_entries (const gchar *entry, gsize entry_size, GError **error)
{
	const gchar *end = entry + entry_size;
	GList *result = NULL;

	while (entry < end)
	{
		gchar type = *entry++;
		StardictEntryField *sef = read_entry (type, &entry, end, FALSE);
		if (!sef)
			goto error;
		result = g_list_append (result, sef);
	}

	return result;

error:
	g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
		"invalid data entry");
	g_list_free_full (result, (GDestroyNotify) stardict_entry_field_free);
	return NULL;
}

static GList *
read_entries_sts (const gchar *entry, gsize entry_size,
	const gchar *sts, GError **error)
{
	const gchar *end = entry + entry_size;
	GList *result = NULL;

	while (*sts)
	{
		gchar type = *sts++;
		StardictEntryField *sef = read_entry (type, &entry, end, !*sts);
		if (!sef)
			goto error;
		result = g_list_append (result, sef);
	}

	return result;

error:
	g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
		"invalid data entry");
	g_list_free_full (result, (GDestroyNotify) stardict_entry_field_free);
	return NULL;
}

/** Return the data for the specified offset in the index.  Unsafe. */
static StardictEntry *
stardict_dict_get_entry (StardictDict *sd, guint32 offset)
{
	// TODO cache the entries
	StardictIndexEntry *sie = &g_array_index (sd->index,
		StardictIndexEntry, offset);

	g_return_val_if_fail (sie->data_offset + sie->data_size
		<= sd->dict_length, NULL);

	GList *entries;
	if (sd->info->same_type_sequence)
		entries = read_entries_sts (sd->dict + sie->data_offset,
			sie->data_size, sd->info->same_type_sequence, NULL);
	else
		entries = read_entries (sd->dict + sie->data_offset,
			sie->data_size, NULL);

	if (!entries)
		return NULL;

	StardictEntry *se = g_object_new (STARDICT_TYPE_ENTRY, NULL);
	se->fields = entries;
	return se;
}

// --- StardictEntry -----------------------------------------------------------

G_DEFINE_TYPE (StardictEntry, stardict_entry, G_TYPE_OBJECT)

static void
stardict_entry_finalize (GObject *self)
{
	StardictEntry *sde = STARDICT_ENTRY (self);

	g_list_free_full (sde->fields, (GDestroyNotify) stardict_entry_field_free);

	G_OBJECT_CLASS (stardict_entry_parent_class)->finalize (self);
}

static void
stardict_entry_class_init (StardictEntryClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = stardict_entry_finalize;
}

static void
stardict_entry_init (G_GNUC_UNUSED StardictEntry *sde)
{
}

/** Return the entries present within the entry.
 *  @return GList<StardictEntryField *>
 */
const GList *
stardict_entry_get_fields (StardictEntry *sde)
{
	g_return_val_if_fail (STARDICT_IS_ENTRY (sde), NULL);
	return sde->fields;
}

// --- StardictIterator---------------------------------------------------------

G_DEFINE_TYPE (StardictIterator, stardict_iterator, G_TYPE_OBJECT)

static void
stardict_iterator_finalize (GObject *self)
{
	StardictIterator *si = STARDICT_ITERATOR (self);

	g_object_unref (si->owner);

	G_OBJECT_CLASS (stardict_iterator_parent_class)->finalize (self);
}

static void
stardict_iterator_class_init (StardictIteratorClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = stardict_iterator_finalize;
}

static void
stardict_iterator_init (G_GNUC_UNUSED StardictIterator *sd)
{
}

/** Create a new iterator for the dictionary with offset @a offset. */
StardictIterator *
stardict_iterator_new (StardictDict *sd, guint32 offset)
{
	g_return_val_if_fail (STARDICT_IS_DICT (sd), NULL);

	StardictIterator *si = g_object_new (STARDICT_TYPE_ITERATOR, NULL);
	si->owner = g_object_ref (sd);
	si->offset = offset;
	return si;
}

/** Return the word in the index that the iterator points at, or NULL. */
const gchar *
stardict_iterator_get_word (StardictIterator *sdi)
{
	g_return_val_if_fail (STARDICT_IS_ITERATOR (sdi), NULL);
	if (!stardict_iterator_is_valid (sdi))
		return NULL;
	return g_array_index (sdi->owner->index,
		StardictIndexEntry, sdi->offset).name;
}

/** Return the dictionary entry that the iterator points at, or NULL. */
StardictEntry *
stardict_iterator_get_entry (StardictIterator *sdi)
{
	g_return_val_if_fail (STARDICT_IS_ITERATOR (sdi), NULL);
	if (!stardict_iterator_is_valid (sdi))
		return FALSE;
	return stardict_dict_get_entry (sdi->owner, sdi->offset);
}

/** Return whether the iterator points to a valid index entry. */
gboolean
stardict_iterator_is_valid (StardictIterator *sdi)
{
	g_return_val_if_fail (STARDICT_IS_ITERATOR (sdi), FALSE);
	return sdi->offset >= 0 && sdi->offset < sdi->owner->index->len;
}

/** Return the offset of the iterator within the dictionary index. */
gint64
stardict_iterator_get_offset (StardictIterator *sdi)
{
	g_return_val_if_fail (STARDICT_IS_ITERATOR (sdi), -1);
	return sdi->offset;
}

/** Set the offset of the iterator. */
void
stardict_iterator_set_offset
	(StardictIterator *sdi, gint64 offset, gboolean relative)
{
	g_return_if_fail (STARDICT_IS_ITERATOR (sdi));
	sdi->offset = relative ? sdi->offset + offset : offset;
}
