/*
 * generator.h: dictionary generator
 *
 * Nothing fancy.  Just something moved out off the `stardict' test to be
 * conveniently reused by the included tools.
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

#ifndef GENERATOR_H
#define GENERATOR_H

/// Simplifies the task of creating a StarDict dictionary.
typedef struct generator               Generator;

struct generator
{
	StardictInfo       * info;         ///< Dictionary information, fill it in

	goffset              entry_mark;   ///< Marks the entry's start offset

	GFileOutputStream  * dict_stream;  ///< Dictionary stream
	GDataOutputStream  * dict_data;    ///< Dictionary data stream wrapper

	GFileOutputStream  * idx_stream;   ///< Index file stream
	GDataOutputStream  * idx_data;     ///< Index file data stream wrapper
};

Generator *generator_new (const gchar *base, GError **error);
gboolean generator_finish (Generator *self, GError **error);
void generator_free (Generator *self);

void generator_begin_entry (Generator *self);
gboolean generator_write_type (Generator *self, gchar type, GError **error);
gboolean generator_write_raw (Generator *self,
	gpointer data, gsize data_size, gboolean mark_end, GError **error);
gboolean generator_write_string (Generator *self,
	const gchar *s, gboolean mark_end, GError **error);
gboolean generator_finish_entry (Generator *self,
	const gchar *word, GError **error);

#endif  // ! GENERATOR_H
