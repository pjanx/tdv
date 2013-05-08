/*
 * generator.c: dictionary generator
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


/** Creates an output stream for a path plus suffix. */
static GFileOutputStream *
replace_file_by_suffix (const gchar *base, const gchar *suffix, GError **error)
{
	gchar *full_path = g_strconcat (base, suffix, NULL);
	GFile *file = g_file_new_for_path (full_path);
	g_free (full_path);

	GFileOutputStream *stream = g_file_replace (file,
		NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
	g_object_unref (file);
	return stream;
}

/** Creates a Stardict dictionary generator for the specified base. */
Generator *
generator_new (const gchar *base, GError **error)
{
	Generator *self = g_malloc0 (sizeof *self);
	self->info = g_malloc0 (sizeof *self->info);
	self->info->path = g_strconcat (base, ".ifo", NULL);

	self->dict_stream = replace_file_by_suffix (base, ".dict", error);
	if (!self->dict_stream)
		goto error_dict;

	self->idx_stream = replace_file_by_suffix (base, ".idx", error);
	if (!self->idx_stream)
		goto error_idx;

	self->dict_data = g_data_output_stream_new
		(G_OUTPUT_STREAM (self->dict_stream));
	g_data_output_stream_set_byte_order
		(self->dict_data, G_DATA_STREAM_BYTE_ORDER_BIG_ENDIAN);

	self->idx_data = g_data_output_stream_new
		(G_OUTPUT_STREAM (self->idx_stream));
	g_data_output_stream_set_byte_order
		(self->idx_data, G_DATA_STREAM_BYTE_ORDER_BIG_ENDIAN);

	return self;

error_idx:
	g_object_unref (self->dict_stream);
error_dict:
	stardict_info_free (self->info);
	g_free (self);
	return NULL;
}

/** Finishes the dictionary and writes the .ifo file. */
gboolean
generator_finish (Generator *self, GError **error)
{
	GString *ifo_contents = g_string_new ("StarDict's dict ifo file\n");

	if (self->info->version == SD_VERSION_3_0_0)
		g_string_append (ifo_contents, "version=3.0.0\n");
	else
		g_string_append (ifo_contents, "version=2.4.2\n");

	self->info->idx_filesize = g_seekable_tell (G_SEEKABLE (self->idx_stream));
	self->info->idx_offset_bits = 32;

	if (!g_output_stream_close
		(G_OUTPUT_STREAM (self->dict_stream), NULL, error)
	 || !g_output_stream_close
		(G_OUTPUT_STREAM (self->idx_stream), NULL, error))
		return FALSE;

	guint i;
	for (i = 0; i < _stardict_ifo_keys_length; i++)
	{
		const struct stardict_ifo_key *key = &_stardict_ifo_keys[i];
		if (key->type == IFO_STRING)
		{
			const gchar *value = G_STRUCT_MEMBER (const gchar *,
				self->info, key->offset);
			if (value)
				g_string_append_printf (ifo_contents, "%s=%s\n",
					key->name, value);
		}
		else
		{
			gulong value = G_STRUCT_MEMBER (gulong,
				self->info, key->offset);
			if (value)
				g_string_append_printf (ifo_contents, "%s=%lu\n",
					key->name, value);
		}
	}

	gboolean success = g_file_set_contents (self->info->path,
		ifo_contents->str, -1, error);
	g_string_free (ifo_contents, TRUE);
	return success;
}

/** Start writing a dictionary entry. */
void
generator_begin_entry (Generator *self)
{
	self->entry_mark = g_seekable_tell (G_SEEKABLE (self->dict_stream));
}

/** Write the data type of an entry field, when there's no sametypesequence. */
gboolean
generator_write_type (Generator *self, gchar type, GError **error)
{
	return g_data_output_stream_put_byte (self->dict_data, type, NULL, error);
}

/** Write a raw binary field. */
gboolean
generator_write_raw (Generator *self,
	gpointer data, gsize data_size, gboolean mark_end, GError **error)
{
	gsize written;
	if ((mark_end && !g_data_output_stream_put_uint32
			(self->dict_data, data_size, NULL, error))
	 || !g_output_stream_write_all (G_OUTPUT_STREAM (self->dict_stream),
			data, data_size, &written, NULL, error))
		return FALSE;
	return TRUE;
}

/** Write a text string. */
gboolean
generator_write_string (Generator *self,
	const gchar *s, gboolean mark_end, GError **error)
{
	if (!g_data_output_stream_put_string (self->dict_data, s, NULL, error)
	 || (mark_end && !g_data_output_stream_put_byte
			(self->dict_data, '\0', NULL, error)))
		return FALSE;
	return TRUE;
}

/** Finishes the current entry and writes it into the index. */
gboolean
generator_finish_entry (Generator *self, const gchar *word, GError **error)
{
	if (!g_data_output_stream_put_string (self->idx_data, word, NULL, error)
	 || !g_data_output_stream_put_byte (self->idx_data, '\0', NULL, error)
	 || !g_data_output_stream_put_uint32 (self->idx_data,
			self->entry_mark, NULL, error)
	 || !g_data_output_stream_put_uint32 (self->idx_data,
			g_seekable_tell (G_SEEKABLE (self->dict_stream)) -
			self->entry_mark, NULL, error))
		return FALSE;

	self->info->word_count++;
	return TRUE;
}

/** Destroys the generator object, freeing up system resources. */
void
generator_free (Generator *self)
{
	stardict_info_free (self->info);

	g_object_unref (self->dict_data);
	g_object_unref (self->idx_data);

	g_object_unref (self->dict_stream);
	g_object_unref (self->idx_stream);
}
