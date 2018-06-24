/*
 * stardict.h: StarDict API
 *
 * This module doesn't cover all the functionality available to StarDict
 * dictionaries, it should however be good enough for most of them that are
 * freely available on the Internet.
 *
 * Copyright (c) 2013 - 2016, Přemysl Janouch <p@janouch.name>
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

#ifndef STARDICT_H
#define STARDICT_H

/// An object intended for interacting with a dictionary.
typedef struct stardict_dict            StardictDict;
typedef struct stardict_dict_class      StardictDictClass;
typedef struct stardict_dict_private    StardictDictPrivate;

/// Overall information about a particular dictionary.
typedef struct stardict_info            StardictInfo;

/// Handles the task of moving around the dictionary.
typedef struct stardict_iterator        StardictIterator;
typedef struct stardict_iterator_class  StardictIteratorClass;

/// Contains the decoded data for a single word definition.
typedef struct stardict_entry           StardictEntry;
typedef struct stardict_entry_class     StardictEntryClass;

/// A single field of a word definition.
typedef struct stardict_entry_field     StardictEntryField;

// GObject boilerplate.
#define STARDICT_TYPE_DICT  (stardict_dict_get_type ())
#define STARDICT_DICT(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST ((obj), \
	STARDICT_TYPE_DICT, StardictDict))
#define STARDICT_IS_DICT(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
	STARDICT_TYPE_DICT))
#define STARDICT_DICT_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST ((klass), \
	STARDICT_TYPE_DICT, StardictDictClass))
#define STARDICT_IS_DICT_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_TYPE ((klass), \
	STARDICT_TYPE_DICT))
#define STARDICT_DICT_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS ((obj), \
	STARDICT_TYPE_DICT, StardictDictClass))

#define STARDICT_TYPE_ITERATOR  (stardict_iterator_get_type ())
#define STARDICT_ITERATOR(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST ((obj), \
	STARDICT_TYPE_ITERATOR, StardictIterator))
#define STARDICT_IS_ITERATOR(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
	STARDICT_TYPE_ITERATOR))
#define STARDICT_ITERATOR_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST ((klass), \
	STARDICT_TYPE_ITERATOR, StardictIteratorClass))
#define STARDICT_IS_ITERATOR_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_TYPE ((klass), \
	STARDICT_TYPE_ITERATOR))
#define STARDICT_ITERATOR_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS ((obj), \
	STARDICT_TYPE_ITERATOR, StardictIteratorClass))

#define STARDICT_TYPE_ENTRY  (stardict_entry_get_type ())
#define STARDICT_ENTRY(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST ((obj), \
	STARDICT_TYPE_ENTRY, StardictEntry))
#define STARDICT_IS_ENTRY(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
	STARDICT_TYPE_ENTRY))
#define STARDICT_ENTRY_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST ((klass), \
	STARDICT_TYPE_ENTRY, StardictEntryClass))
#define STARDICT_IS_ENTRY_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_TYPE ((klass), \
	STARDICT_TYPE_ENTRY))
#define STARDICT_ENTRY_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS ((obj), \
	STARDICT_TYPE_ENTRY, StardictEntryClass))

// --- Errors ------------------------------------------------------------------

/// General error type.
typedef enum {
	STARDICT_ERROR_FILE_NOT_FOUND,      //!< Some file was not found
	STARDICT_ERROR_INVALID_DATA         //!< Dictionary contains invalid data
} StardictError;

#define STARDICT_ERROR  (stardict_error_quark ())

GQuark stardict_error_quark (void);

// --- Dictionary information --------------------------------------------------

const gchar *stardict_info_get_path (StardictInfo *sdi) G_GNUC_PURE;
const gchar *stardict_info_get_book_name (StardictInfo *sdi) G_GNUC_PURE;
gsize stardict_info_get_word_count (StardictInfo *sd) G_GNUC_PURE;
void stardict_info_free (StardictInfo *sdi);

GList *stardict_list_dictionaries (const gchar *path);

// --- Dictionaries ------------------------------------------------------------

struct stardict_dict
{
	GObject                parent_instance;
	StardictDictPrivate  * priv;
};

struct stardict_dict_class
{
	GObjectClass           parent_class;
};

GType stardict_dict_get_type (void);
StardictDict *stardict_dict_new (const gchar *filename, GError **error);
StardictDict *stardict_dict_new_from_info (StardictInfo *sdi, GError **error);
StardictInfo *stardict_dict_get_info (StardictDict *sd);
gchar **stardict_dict_get_synonyms (StardictDict *sd, const gchar *word);
StardictIterator *stardict_dict_search
	(StardictDict *sd, const gchar *word, gboolean *success);

size_t stardict_longest_common_collation_prefix
	(StardictDict *sd, const gchar *w1, const gchar *w2);

// --- Dictionary iterators ----------------------------------------------------

struct stardict_iterator
{
	GObject         parent_instance;
	StardictDict  * owner;              //!< The related dictionary
	gint64          offset;             //!< Index within the dictionary
};

struct stardict_iterator_class
{
	GObjectClass    parent_class;
};

GType stardict_iterator_get_type (void);
StardictIterator *stardict_iterator_new (StardictDict *sd, guint32 index);
const gchar *stardict_iterator_get_word (StardictIterator *sdi) G_GNUC_PURE;
StardictEntry *stardict_iterator_get_entry (StardictIterator *sdi);
gboolean stardict_iterator_is_valid (StardictIterator *sdi) G_GNUC_PURE;
gint64 stardict_iterator_get_offset (StardictIterator *sdi) G_GNUC_PURE;
void stardict_iterator_set_offset
	(StardictIterator *sdi, gint64 offset, gboolean relative);

/// Go to the next entry.
#define stardict_iterator_next(sdi) \
	(stardict_iterator_set_offset (sdi,  1, TRUE))

/// Go to the previous entry.
#define stardict_iterator_prev(sdi) \
	(stardict_iterator_set_offset (sdi, -1, TRUE))

// --- Dictionary entries ------------------------------------------------------

typedef enum {
	STARDICT_FIELD_MEANING    = 'm',    ///< Word's purely textual meaning
	STARDICT_FIELD_LOCALE     = 'l',    ///< Locale-dependent meaning
	STARDICT_FIELD_PANGO      = 'g',    ///< Pango text markup language
	STARDICT_FIELD_PHONETIC   = 't',    ///< English phonetic string
	STARDICT_FIELD_XDXF       = 'x',    ///< xdxf language
	STARDICT_FIELD_YB_KANA    = 'y',    ///< Chinese YinBiao or Japanese KANA
	STARDICT_FIELD_POWERWORD  = 'k',    ///< KingSoft PowerWord's data
	STARDICT_FIELD_MEDIAWIKI  = 'w',    ///< MediaWiki markup language
	STARDICT_FIELD_HTML       = 'h',    ///< HTML codes
	STARDICT_FIELD_RESOURCE   = 'r',    ///< Resource file list
	STARDICT_FIELD_WAV        = 'W',    ///< WAV file
	STARDICT_FIELD_PICTURE    = 'P',    ///< Picture file
	STARDICT_FIELD_X          = 'X'     ///< Reserved, experimental extensions
} StardictEntryFieldType;

struct stardict_entry_field
{
	gchar           type;               ///< Type of entry (EntryFieldType)
	gpointer        data;               ///< Raw data or null-terminated string
	gsize           data_size;          ///< Size of data, includding any \0
};

struct stardict_entry
{
	GObject         parent_instance;
	GList         * fields;             ///< List of StardictEntryField's
};

struct stardict_entry_class
{
	GObjectClass    parent_class;
};

GType stardict_entry_get_type (void);
const GList *stardict_entry_get_fields (StardictEntry *sde) G_GNUC_PURE;

 #endif  // ! STARDICT_H
