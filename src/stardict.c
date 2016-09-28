/*
 * stardict.c: StarDict API
 *
 * Copyright (c) 2013 - 2015, Přemysl Janouch <p.janouch@gmail.com>
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
#include <glib/gi18n.h>

#include <unicode/ucol.h>
#include <unicode/ustring.h>
#include <unicode/ubrk.h>

#include "stardict.h"
#include "stardict-private.h"
#include "dictzip-input-stream.h"
#include "utils.h"

#if ! GLIB_CHECK_VERSION (2, 40, 0)
#define g_info g_debug
#endif


// --- Utilities ---------------------------------------------------------------

/// String compare function used for StarDict indexes.
static inline gint
stardict_strcmp (const gchar *s1, const gchar *s2)
{
	gint a = g_ascii_strcasecmp (s1, s2);
	return a ? a : strcmp (s1, s2);
}

// --- Errors ------------------------------------------------------------------

GQuark
stardict_error_quark (void)
{
	return g_quark_from_static_string ("stardict-error-quark");
}

// --- IFO reader --------------------------------------------------------------

/// Helper class for reading .ifo files.
typedef struct ifo_reader               IfoReader;

struct ifo_reader
{
	gchar           * data;             ///< File data terminated with \0
	gchar           * data_end;         ///< Where the final \0 char. is

	gchar           * start;            ///< Start of the current token

	gchar           * key;              ///< The key (points into @a data)
	gchar           * value;            ///< The value (points into @a data)
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
			"%s: %s", path, _("invalid header format"));
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

/// Return the filesystem path for the dictionary.
const gchar *
stardict_info_get_path (StardictInfo *sdi)
{
	return sdi->path;
}

/// Return the name of the dictionary.
const gchar *
stardict_info_get_book_name (StardictInfo *sdi)
{
	return sdi->book_name;
}

/// Return the word count of the dictionary.  Note that this information comes
/// from the .ifo file, while the dictionary could successfully load with
/// a different count of word entries.
gsize
stardict_info_get_word_count (StardictInfo *sdi)
{
	return sdi->word_count;
}

/// Destroy the dictionary info object.
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

	g_free (sdi->collation);
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
	DEFINE_IFO_KEY ("sametypesequence", STRING, same_type_sequence),

	// These are our own custom
	DEFINE_IFO_KEY ("collation",        STRING, collation)
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
			"%s: %s", path, _("version not specified"));
		goto error;
	}

	if (!strcmp (ir.value, "2.4.2"))
		sti->version = SD_VERSION_2_4_2;
	else if (!strcmp (ir.value, "3.0.0"))
		sti->version = SD_VERSION_3_0_0;
	else
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: %s: %s", path, _("invalid version"), ir.value);
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
			g_info ("%s: %s: %s", path, _("unknown key, ignoring"), ir.key);
			continue;
		}

		if (!g_utf8_validate (ir.value, -1, NULL))
		{
			g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
				"%s: %s", path, _("invalid encoding, must be valid UTF-8"));
			goto error;
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
				"%s: %s", path, _("invalid integer"));
			goto error;
		}

		G_STRUCT_MEMBER (gulong, sti, _stardict_ifo_keys[i].offset) = wc;
	}

	if (ret == -1)
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: %s", path, _("option format error"));
		goto error;
	}

	ret_val = TRUE;

	// FIXME check for zeros, don't assume that 0 means for "not set"
	if (!sti->book_name || !*sti->book_name)
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: %s", path, _("no book name specified"));
		ret_val = FALSE;
	}
	if (!sti->word_count)
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: %s", path, _("word count not specified"));
		ret_val = FALSE;
	}
	if (!sti->idx_filesize)
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: %s", path, _("index file size not specified"));
		ret_val = FALSE;
	}

	if (!sti->idx_offset_bits)
		sti->idx_offset_bits = 32;
	else if (sti->idx_offset_bits != 32 && sti->idx_offset_bits != 64)
	{
		g_set_error (error, STARDICT_ERROR, STARDICT_ERROR_INVALID_DATA,
			"%s: %s: %lu", path, _("invalid index offset bits"),
			sti->idx_offset_bits);
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

/// List all dictionary files located in a path.
/// @return GList<StardictInfo *>. Deallocate the list with:
/// @code
///   g_list_free_full ((GDestroyNotify) stardict_info_free);
/// @endcode
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

struct stardict_dict_private
{
	StardictInfo  * info;               //!< General information about the dict
	GArray        * index;              //!< Word index
	GArray        * synonyms;           //!< Synonyms

	// The collated indexes are only permutations of their normal selves.

	UCollator     * collator;           //!< ICU index collator
	GArray        * collated_index;     //!< Sorted indexes into @a index
	GArray        * collated_synonyms;  //!< Sorted indexes into @a synonyms

	// There are currently three ways the dictionary data can be read:
	// through mmap(), from a seekable GInputStream, or from a preallocated
	// chunk of memory that the whole dictionary has been decompressed into.
	//
	// It wouldn't be unreasonable to drop the support for regular gzip files.

	GInputStream  * dict_stream;        //!< Dictionary input stream handle
	GMappedFile   * mapped_dict;        //!< Dictionary memory map handle
	gpointer        dict;               //!< Dictionary data
	gsize           dict_length;        //!< Length of the dict data in bytes
};

G_DEFINE_TYPE (StardictDict, stardict_dict, G_TYPE_OBJECT)

static void
stardict_dict_finalize (GObject *self)
{
	StardictDictPrivate *priv = STARDICT_DICT (self)->priv;

	if (priv->info)
		stardict_info_free (priv->info);

	g_array_free (priv->index, TRUE);
	g_array_free (priv->synonyms, TRUE);

	if (priv->collator)
		ucol_close (priv->collator);
	if (priv->collated_index)
		g_array_free (priv->collated_index, TRUE);
	if (priv->collated_synonyms)
		g_array_free (priv->collated_synonyms, TRUE);

	if (priv->mapped_dict)
		g_mapped_file_unref (priv->mapped_dict);
	else if (priv->dict_stream)
		g_object_unref (priv->dict_stream);
	else
		g_free (priv->dict);

	G_OBJECT_CLASS (stardict_dict_parent_class)->finalize (self);
}

static void
stardict_dict_class_init (StardictDictClass *klass)
{
	g_type_class_add_private (klass, sizeof (StardictDictPrivate));
	G_OBJECT_CLASS (klass)->finalize = stardict_dict_finalize;
}

static void
stardict_dict_init (StardictDict *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
		STARDICT_TYPE_DICT, StardictDictPrivate);
}

/// Load a StarDict dictionary.
/// @param[in] filename  Path to the .ifo file
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

/// Return information about a loaded dictionary.  The returned reference is
/// only valid for the lifetime of the dictionary object.
StardictInfo *
stardict_dict_get_info (StardictDict *sd)
{
	g_return_val_if_fail (STARDICT_IS_DICT (sd), NULL);
	return sd->priv->info;
}

/// Load a StarDict index from a GIO input stream.
static gboolean
load_idx_internal (StardictDict *sd, GInputStream *is, GError **error)
{
	StardictDictPrivate *priv = sd->priv;
	GDataInputStream *dis = g_data_input_stream_new (G_INPUT_STREAM (is));
	g_data_input_stream_set_byte_order (dis,
		G_DATA_STREAM_BYTE_ORDER_BIG_ENDIAN);

	StardictIndexEntry entry;
	GError *err = NULL;
	// Ignoring "wordcount", just reading as long as we can
	while ((entry.name = stream_read_string (dis, &err)))
	{
		if (priv->info->idx_offset_bits == 32)
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

		g_array_append_val (priv->index, entry);
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

/// Load a StarDict index.
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

		g_array_append_val (sd->priv->synonyms, entry);
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

/// Destroy an index entry.
static void
index_destroy_cb (gpointer sde)
{
	StardictIndexEntry *e = sde;
	g_free (e->name);
}

/// Destroy a synonym entry.
static void
syn_destroy_cb (gpointer sde)
{
	StardictSynonymEntry *e = sde;
	g_free (e->word);
}

/// Load StarDict dictionary data.
static gboolean
load_dict (StardictDict *sd, const gchar *filename, gboolean gzipped,
	GError **error)
{
	StardictDictPrivate *priv = sd->priv;

	if (gzipped)
	{
		gboolean ret_val = FALSE;
		GFile *file = g_file_new_for_path (filename);
		GFileInputStream *fis = g_file_read (file, NULL, error);

		if (!fis)
			goto cannot_open;

	// As a simple workaround for GLib < 2.33.1 and the lack of support for
	// the GSeekable interface in GDataInputStream, disable dictzip.
	//
	// http://lists.gnu.org/archive/html/qemu-devel/2013-06/msg04690.html
	if (!glib_check_version (2, 33, 1))
	{
		// Try opening it as a dictzip file first
		DictzipInputStream *dzis =
			dictzip_input_stream_new (G_INPUT_STREAM (fis), NULL);
		if (dzis)
		{
			priv->dict_stream = G_INPUT_STREAM (dzis);
			ret_val = TRUE;
			goto done;
		}

		// If unsuccessful, just read it all, as it is, into memory
		if (!g_seekable_seek (G_SEEKABLE (fis), 0, G_SEEK_SET, NULL, error))
			goto done;
	}

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
			priv->dict_length = ba->len;
			priv->dict = g_byte_array_free (ba, FALSE);
		}
		else
			g_byte_array_free (ba, TRUE);

done:
		g_object_unref (fis);
cannot_open:
		g_object_unref (file);
		return ret_val;
	}

	priv->mapped_dict = g_mapped_file_new (filename, FALSE, error);
	if (!priv->mapped_dict)
		return FALSE;

	priv->dict_length = g_mapped_file_get_length (priv->mapped_dict);
	priv->dict = g_mapped_file_get_contents (priv->mapped_dict);
	return TRUE;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Compare the two strings by collation rules.
static inline gint
stardict_dict_strcoll (gconstpointer s1, gconstpointer s2, gpointer data)
{
	StardictDict *sd = data;
	UErrorCode error = U_ZERO_ERROR;

#if U_ICU_VERSION_MAJOR_NUM >= 50
	return ucol_strcollUTF8 (sd->priv->collator, s1, -1, s2, -1, &error);
#else  // U_ICU_VERSION_MAJOR_NUM >= 50
	// This remarkably retarded API absolutely reeks of corporate;
	// I don't have to tell you that this code runs slow, do I?

	int32_t uc1_len = 0;
	int32_t uc2_len = 0;

	error = U_ZERO_ERROR;
	u_strFromUTF8WithSub (NULL, 0, &uc1_len, s1, -1, 0xFFFD, NULL, &error);
	error = U_ZERO_ERROR;
	u_strFromUTF8WithSub (NULL, 0, &uc2_len, s2, -1, 0xFFFD, NULL, &error);

	UChar uc1[uc1_len];
	UChar uc2[uc2_len];
	error = U_ZERO_ERROR;
	u_strFromUTF8WithSub (uc1, uc1_len, NULL, s1, -1, 0xFFFD, NULL, &error);
	error = U_ZERO_ERROR;
	u_strFromUTF8WithSub (uc2, uc2_len, NULL, s2, -1, 0xFFFD, NULL, &error);

	return ucol_strcoll (sd->priv->collator, uc1, uc1_len, uc2, uc2_len);
#endif  // U_ICU_VERSION_MAJOR_NUM >= 50
}

/// Stricter stardict_dict_strcoll() used to sort the collated index.
static inline gint
stardict_dict_strcoll_for_sorting
	(gconstpointer s1, gconstpointer s2, gpointer data)
{
	UCollationResult a = stardict_dict_strcoll (s1, s2, data);
	return a ? a : strcmp (s1, s2);
}

static inline gint
stardict_dict_index_coll_for_sorting
	(gconstpointer x1, gconstpointer x2, gpointer data)
{
	StardictDict *sd = data;
	const gchar *s1 = g_array_index
		(sd->priv->index, StardictIndexEntry, *(guint32 *) x1).name;
	const gchar *s2 = g_array_index
		(sd->priv->index, StardictIndexEntry, *(guint32 *) x2).name;
	return stardict_dict_strcoll_for_sorting (s1, s2, data);
}

static inline gint
stardict_dict_synonyms_coll_for_sorting
	(gconstpointer x1, gconstpointer x2, gpointer data)
{
	StardictDict *sd = data;
	const gchar *s1 = g_array_index
		(sd->priv->index, StardictSynonymEntry, *(guint32 *) x1).word;
	const gchar *s2 = g_array_index
		(sd->priv->index, StardictSynonymEntry, *(guint32 *) x2).word;
	return stardict_dict_strcoll_for_sorting (s1, s2, data);
}

static gboolean
stardict_dict_set_collation (StardictDict *sd, const gchar *collation)
{
	StardictDictPrivate *priv = sd->priv;
	UErrorCode error = U_ZERO_ERROR;
	if (!(priv->collator = ucol_open (collation, &error)))
	{
		// TODO: set a meaningful error
		g_info ("failed to create a collator for `%s'", collation);
		return FALSE;
	}

	// TODO: if error != U_ZERO_ERROR, report a meaningful message

	ucol_setAttribute (priv->collator, UCOL_CASE_FIRST, UCOL_OFF, &error);

	priv->collated_index = g_array_sized_new (FALSE, FALSE,
		sizeof (guint32), priv->index->len);
	for (guint32 i = 0; i < priv->index->len; i++)
		g_array_append_val (priv->collated_index, i);
	g_array_sort_with_data (sd->priv->collated_index,
		stardict_dict_index_coll_for_sorting, sd);

	priv->collated_synonyms = g_array_sized_new (FALSE, FALSE,
		sizeof (guint32), priv->synonyms->len);
	for (guint32 i = 0; i < priv->synonyms->len; i++)
		g_array_append_val (priv->collated_synonyms, i);
	g_array_sort_with_data (sd->priv->collated_synonyms,
		stardict_dict_synonyms_coll_for_sorting, sd);

	// Make the collator something like case-insensitive, see:
	// http://userguide.icu-project.org/collation/concepts
	// We shouldn't need to sort the data anymore, and if we did, we could just
	// reset the strength to its default value for the given locale.
	ucol_setStrength (priv->collator, UCOL_SECONDARY);
	return TRUE;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Load a StarDict dictionary.
/// @param[in] sdi  Parsed .ifo data.  The dictionary assumes ownership.
StardictDict *
stardict_dict_new_from_info (StardictInfo *sdi, GError **error)
{
	g_return_val_if_fail (sdi != NULL, NULL);

	StardictDict *sd = g_object_new (STARDICT_TYPE_DICT, NULL);
	StardictDictPrivate *priv = sd->priv;
	priv->info = sdi;
	priv->index = g_array_new (FALSE, FALSE, sizeof (StardictIndexEntry));
	g_array_set_clear_func (priv->index, index_destroy_cb);
	priv->synonyms = g_array_new (FALSE, FALSE, sizeof (StardictSynonymEntry));
	g_array_set_clear_func (priv->synonyms, syn_destroy_cb);

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
				"%s: %s", sdi->path, _("cannot find .idx file"));
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
				"%s: %s", sdi->path, _("cannot find .dict file"));
		}
	}
	g_free (base_dict);

	if (!ret)
		goto error;

	gchar *base_syn = g_strconcat (base, ".syn", NULL);
	if (g_file_test (base_syn, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_REGULAR))
		(void) load_syn (sd, base_syn, NULL);
	g_free (base_syn);

	if (sdi->collation)
		(void) stardict_dict_set_collation (sd, sdi->collation);

	g_free (base);
	return sd;

error:
	g_free (base);
	priv->info = NULL;
	g_object_unref (sd);
	return NULL;
}

static gint
stardict_dict_cmp_synonym (StardictDict *sd, const gchar *word, gint i)
{
	GArray *collated = sd->priv->collated_synonyms;
	GArray *synonyms = sd->priv->synonyms;

	if (sd->priv->collator)
		return stardict_dict_strcoll (word,
			g_array_index (synonyms, StardictSynonymEntry,
				g_array_index (collated, guint32, i)).word, sd);
	return g_ascii_strcasecmp (word,
		g_array_index (synonyms, StardictSynonymEntry, i).word);
}

/// Return words for which the argument is a synonym of or NULL
/// if there are no such words.
gchar **
stardict_dict_get_synonyms (StardictDict *sd, const gchar *word)
{
	GArray *synonyms = sd->priv->synonyms;
	GArray *index = sd->priv->index;

	BINARY_SEARCH_BEGIN (synonyms->len - 1,
		stardict_dict_cmp_synonym (sd, word, imid))

	// Back off to the first matching entry
	while (imid > 0 && !stardict_dict_cmp_synonym (sd, word, --imid))
		;

	GPtrArray *array = g_ptr_array_new ();

	// And add all matching entries from that position on to the array
	do
		g_ptr_array_add (array, g_strdup (g_array_index
			(index, StardictIndexEntry, g_array_index
			(synonyms, StardictSynonymEntry, ++imid).original_word).name));
	while ((guint) imid < synonyms->len - 1 && !stardict_strcmp (word,
		g_array_index (synonyms, StardictSynonymEntry, imid + 1).word));

	return (gchar **) g_ptr_array_free (array, FALSE);

	BINARY_SEARCH_END
	return NULL;
}

static gint
stardict_dict_cmp_index (StardictDict *sd, const gchar *word, gint i)
{
	GArray *collated = sd->priv->collated_index;
	GArray *index = sd->priv->index;

	if (sd->priv->collator)
		return stardict_dict_strcoll (word,
			g_array_index (index, StardictIndexEntry,
				g_array_index (collated, guint32, i)).name, sd);
	return g_ascii_strcasecmp (word,
		g_array_index (index, StardictIndexEntry, i).name);
}

/// Search for a word.  The search is ASCII-case-insensitive.
/// @param[in] word  The word in utf-8 encoding
/// @param[out] success  TRUE if found
/// @return An iterator object pointing to the word, or where it would be
StardictIterator *
stardict_dict_search (StardictDict *sd, const gchar *word, gboolean *success)
{
	GArray *index = sd->priv->index;

	BINARY_SEARCH_BEGIN (index->len - 1,
		stardict_dict_cmp_index (sd, word, imid))

	// Back off to the first matching entry
	while (imid > 0 && !stardict_dict_cmp_index (sd, word, imid - 1))
		imid--;

	if (success) *success = TRUE;
	return stardict_iterator_new (sd, imid);

	BINARY_SEARCH_END

	// Try to find a longer common prefix with a preceding entry
#define PREFIX(i) stardict_longest_common_collation_prefix \
	(sd, word, g_array_index (index, StardictIndexEntry, i).name)

	if (sd->priv->collator)
	{
		GArray *collated = sd->priv->collated_index;
		size_t probe, best = PREFIX (g_array_index (collated, guint32, imin));
		while (imin > 0 && (probe =
			PREFIX (g_array_index (collated, guint32, imin - 1))) >= best)
		{
			best = probe;
			imin--;
		}
	}
	else
	{
		// XXX: only looking for _better_ backward matches here, since the
		//   fallback common prefix searching algorithm doesn't ignore case
		size_t probe, best = PREFIX (imin);
		while (imin > 0 && (probe = PREFIX (imin - 1)) > best)
		{
			best = probe;
			imin--;
		}
	}

#undef PREFIX

	if (success) *success = FALSE;
	return stardict_iterator_new (sd, imin);
}

/// Return the longest sequence of bytes from @a s1 that form a common prefix
/// with @a s2 wrt. collation rules for this dictionary.
size_t
stardict_longest_common_collation_prefix (StardictDict *sd,
	const gchar *s1, const gchar *s2)
{
	UErrorCode error;
	int32_t uc1_len = 0;
	int32_t uc2_len = 0;

	// It sets the error to overflow each time, even during pre-flight
	error = U_ZERO_ERROR;
	u_strFromUTF8 (NULL, 0, &uc1_len, s1, -1, &error);
	error = U_ZERO_ERROR;
	u_strFromUTF8 (NULL, 0, &uc2_len, s2, -1, &error);
	error = U_ZERO_ERROR;

	UChar uc1[uc1_len];
	UChar uc2[uc2_len];
	u_strFromUTF8 (uc1, uc1_len, NULL, s1, -1, &error);
	u_strFromUTF8 (uc2, uc2_len, NULL, s2, -1, &error);

	// Both inputs need to be valid UTF-8 because of all the iteration mess
	if (U_FAILURE (error))
		return 0;

	// ucol_getSortKey() can't be used for these purposes, so the only
	// reasonable thing remaining is iterating by full graphemes.  It doesn't
	// work entirely correctly (e.g. Czech "ch" should be regarded as a single
	// unit, and punctuation could be ignored).  It's just good enough.
	//
	// In theory we could set the strength to UCOL_PRIMARY and ignore accents
	// but that's likely not what the user wants most of the time.
	//
	// Locale shouldn't matter much with graphemes, let's use the default.
	UBreakIterator *it1 =
		ubrk_open (UBRK_CHARACTER, NULL, uc1, uc1_len, &error);
	UBreakIterator *it2 =
		ubrk_open (UBRK_CHARACTER, NULL, uc2, uc2_len, &error);

	int32_t longest = 0;
	int32_t pos1, pos2;
	while ((pos1 = ubrk_next (it1)) != UBRK_DONE
		&& (pos2 = ubrk_next (it2)) != UBRK_DONE)
	{
		if (sd->priv->collator)
		{
			if (!ucol_strcoll (sd->priv->collator, uc1, pos1, uc2, pos2))
				longest = pos1;
		}
		// XXX: I'd need a new collator, so just do the minimal working thing
		else if (pos1 == pos2 && !memcmp (uc1, uc2, pos1 * sizeof *uc1))
			longest = pos1;
	}
	ubrk_close (it1);
	ubrk_close (it2);

	if (!longest)
		return 0;

	int32_t common_len = 0;
	u_strToUTF8 (NULL, 0, &common_len, uc1, longest, &error);

	// Since this heavily depends on UTF-16 <-> UTF-8 not modifying the chars
	// (surrogate pairs interference?), let's add some paranoia here
	char common[common_len];
	error = U_ZERO_ERROR;
	u_strToUTF8 (common, common_len, NULL, uc1, longest, &error);
	g_return_val_if_fail (!memcmp (s1, common, common_len), 0);

	return (size_t) common_len;
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
		_("invalid data entry"));
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
		_("invalid data entry"));
	g_list_free_full (result, (GDestroyNotify) stardict_entry_field_free);
	return NULL;
}

/// Read entry data from GInputStream.
static gchar *
read_entry_data_from_stream
	(GInputStream *stream, guint32 offset, StardictIndexEntry *sie)
{
	GError *error = NULL;
	if (!g_seekable_seek (G_SEEKABLE (stream), sie->data_offset,
		G_SEEK_SET, NULL, &error))
	{
		g_debug ("problem seeking to entry #%"
			G_GUINT32_FORMAT ": %s", offset, error->message);
		g_error_free (error);
		return NULL;
	}

	gchar *data = g_malloc (sie->data_size);
	gssize read = g_input_stream_read (stream,
		data, sie->data_size, NULL, &error);
	if (read < sie->data_size)
	{
		if (error)
		{
			g_debug ("problem reading entry #%"
				G_GUINT32_FORMAT ": %s", offset, error->message);
			g_error_free (error);
		}
		else
			g_debug ("probably overflowing entry #%"
				G_GUINT32_FORMAT, offset);

		g_free (data);
		return NULL;
	}
	return data;
}

/// Return the data for the specified offset in the index.  Unsafe.
static StardictEntry *
stardict_dict_get_entry (StardictDict *sd, guint32 offset)
{
	// TODO maybe cache the entries, maybe don't hide the errors (also above)
	StardictDictPrivate *priv = sd->priv;
	StardictIndexEntry *sie = &g_array_index (priv->index,
		StardictIndexEntry, offset);
	GError *error = NULL;

	gchar *data;
	if (priv->dict_stream)
	{
		data = read_entry_data_from_stream (priv->dict_stream, offset, sie);
		if (!data)
			return NULL;
	}
	else
	{
		if (sie->data_offset + sie->data_size > priv->dict_length)
		{
			g_debug ("overflowing entry #%" G_GUINT32_FORMAT, offset);
			return NULL;
		}
		data = priv->dict + sie->data_offset;
	}

	GList *entries;
	if (priv->info->same_type_sequence)
		entries = read_entries_sts (data, sie->data_size,
			priv->info->same_type_sequence, &error);
	else
		entries = read_entries (data, sie->data_size, &error);

	if (error)
	{
		g_debug ("problem processing entry #%"
			G_GUINT32_FORMAT ": %s", offset, error->message);
		g_error_free (error);
	}
	if (priv->dict_stream)
		g_free (data);
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

/// Return the entries present within the entry.
/// @return GList<StardictEntryField *>
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

/// Create a new iterator for the dictionary with offset @a offset.
StardictIterator *
stardict_iterator_new (StardictDict *sd, guint32 offset)
{
	g_return_val_if_fail (STARDICT_IS_DICT (sd), NULL);

	StardictIterator *si = g_object_new (STARDICT_TYPE_ITERATOR, NULL);
	si->owner = g_object_ref (sd);
	si->offset = offset;
	return si;
}

static gint64
stardict_iterator_get_real_offset (StardictIterator *sdi)
{
	return sdi->owner->priv->collator ? g_array_index
		(sdi->owner->priv->collated_index, guint32, sdi->offset) : sdi->offset;
}

/// Return the word in the index that the iterator points at, or NULL.
const gchar *
stardict_iterator_get_word (StardictIterator *sdi)
{
	g_return_val_if_fail (STARDICT_IS_ITERATOR (sdi), NULL);
	if (!stardict_iterator_is_valid (sdi))
		return NULL;
	return g_array_index (sdi->owner->priv->index,
		StardictIndexEntry, stardict_iterator_get_real_offset (sdi)).name;
}

/// Return the dictionary entry that the iterator points at, or NULL.
StardictEntry *
stardict_iterator_get_entry (StardictIterator *sdi)
{
	g_return_val_if_fail (STARDICT_IS_ITERATOR (sdi), NULL);
	if (!stardict_iterator_is_valid (sdi))
		return FALSE;
	return stardict_dict_get_entry (sdi->owner,
		stardict_iterator_get_real_offset (sdi));
}

/// Return whether the iterator points to a valid index entry.
gboolean
stardict_iterator_is_valid (StardictIterator *sdi)
{
	g_return_val_if_fail (STARDICT_IS_ITERATOR (sdi), FALSE);
	return sdi->offset >= 0 && sdi->offset < sdi->owner->priv->index->len;
}

/// Return the offset of the iterator within the dictionary index.
gint64
stardict_iterator_get_offset (StardictIterator *sdi)
{
	g_return_val_if_fail (STARDICT_IS_ITERATOR (sdi), -1);
	return sdi->offset;
}

/// Set the offset of the iterator.
void
stardict_iterator_set_offset
	(StardictIterator *sdi, gint64 offset, gboolean relative)
{
	g_return_if_fail (STARDICT_IS_ITERATOR (sdi));
	sdi->offset = relative ? sdi->offset + offset : offset;
}
