/*
 * stardict-private.h: internal StarDict API
 *
 * Copyright (c) 2013 - 2015, Přemysl Eric Janouch <p@janouch.name>
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

#ifndef STARDICTPRIVATE_H
#define STARDICTPRIVATE_H

/// Describes a single entry in the dictionary index.
typedef struct stardict_index_entry     StardictIndexEntry;

/// Describes a single entry in the synonyms index.
typedef struct stardict_synonym_entry   StardictSynonymEntry;


typedef enum stardict_version StardictVersion;
enum stardict_version { SD_VERSION_2_4_2, SD_VERSION_3_0_0 };

struct stardict_info
{
	gchar           * path;
	StardictVersion   version;

	gchar           * book_name;
	gulong            word_count;
	gulong            syn_word_count;
	gulong            idx_filesize;
	gulong            idx_offset_bits;
	gchar           * author;
	gchar           * email;
	gchar           * website;
	gchar           * description;
	gchar           * date;
	gchar           * same_type_sequence;

	gchar           * collation;
};

struct stardict_index_entry
{
	gchar           * name;             ///< The word in utf-8
	guint64           data_offset;      ///< Offset of the definition
	guint32           data_size;        ///< Size of the definition
	guint32           reverse_index;    ///< Word at this index before sorting
};

struct stardict_synonym_entry
{
	gchar           * word;             ///< A synonymous word
	guint32           original_word;    ///< The original word's index
};

struct stardict_ifo_key
{
	const gchar *name;                  ///< Name of the key
	enum {
		IFO_STRING,                     ///< A @code gchar * @endcode value
		IFO_NUMBER                      ///< A @code gulong @endcode value
	} type;                             ///< Type of the value
	size_t offset;                      ///< Offset within StardictInfo
};

/// Lists all the entries in StardictInfo.
extern const struct stardict_ifo_key _stardict_ifo_keys[];

/// Denotes the length of _stardict_ifo_keys.
extern gsize _stardict_ifo_keys_length;

void stardict_info_copy (StardictInfo *dest, const StardictInfo *src);

#endif  // ! STARDICTPRIVATE_H
