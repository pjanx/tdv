/*
 * dictzip-input-stream.c: dictzip GIO stream reader
 *
 * Copyright (c) 2013, Přemysl Janouch <p@janouch.name>
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
#include <assert.h>

#include <glib.h>
#include <gio/gio.h>

#include <zlib.h>

#include "utils.h"
#include "dictzip-input-stream.h"


// --- Errors ------------------------------------------------------------------

GQuark
dictzip_error_quark (void)
{
	return g_quark_from_static_string ("dictzip-error-quark");
}

// --- dictzip utilities -------------------------------------------------------

static void
free_gzip_header (gz_header *gzh)
{
	g_free (gzh->comment);  gzh->comment = NULL;
	g_free (gzh->extra);    gzh->extra   = NULL;
	g_free (gzh->name);     gzh->name    = NULL;
}

// Reading the header in manually due to stupidity of the ZLIB API.
static gboolean
read_gzip_header (GInputStream *is, gz_header *gzh,
	goffset *first_block_offset, GError **error)
{
	assert (is != NULL);
	assert (gzh != NULL);

	GDataInputStream *dis = g_data_input_stream_new (is);
	g_data_input_stream_set_byte_order (dis,
		G_DATA_STREAM_BYTE_ORDER_LITTLE_ENDIAN);
	g_filter_input_stream_set_close_base_stream
		(G_FILTER_INPUT_STREAM (dis), FALSE);

	GError *err = NULL;
	memset (gzh, 0, sizeof *gzh);

	// File header identification
	if (g_data_input_stream_read_byte (dis, NULL, &err) != 31
	 || g_data_input_stream_read_byte (dis, NULL, &err) != 139)
	{
		if (err)
			g_propagate_error (error, err);
		else
			g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
				"wrong header magic");
		goto error_own;
	}

	// Compression method, only "deflate" is supported here
	if (g_data_input_stream_read_byte (dis, NULL, &err) != Z_DEFLATED)
	{
		if (err)
			g_propagate_error (error, err);
		else
			g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
				"unsupported compression method");
		goto error_own;
	}

	guint flags = g_data_input_stream_read_byte (dis, NULL, &err);
	if (err) goto error;

	gzh->text = ((flags & 1) != 0);
	gzh->hcrc = ((flags & 2) != 0);

	gzh->time = g_data_input_stream_read_uint32 (dis, NULL, &err);
	if (err) goto error;

	gzh->xflags = g_data_input_stream_read_byte (dis, NULL, &err);
	if (err) goto error;

	gzh->os = g_data_input_stream_read_byte (dis, NULL, &err);
	if (err) goto error;

	if (flags & 4)
	{
		gzh->extra_len = g_data_input_stream_read_uint16 (dis, NULL, &err);
		if (err) goto error;
		gzh->extra_max = gzh->extra_len;

		gzh->extra = g_malloc (gzh->extra_len);
		gssize read = g_input_stream_read (G_INPUT_STREAM (dis),
			gzh->extra, gzh->extra_len, NULL, &err);
		if (err) goto error;

		if (read != gzh->extra_len)
		{
			g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
				"unexpected end of file");
			goto error_own;
		}
	}

	if (flags & 8)
	{
		gzh->name = (Bytef *) stream_read_string (dis, &err);
		if (err) goto error;
		gzh->name_max = strlen ((char *) gzh->name) + 1;
	}

	if (flags & 16)
	{
		gzh->comment = (Bytef *) stream_read_string (dis, &err);
		if (err) goto error;
		gzh->comm_max = strlen ((char *) gzh->comment) + 1;
	}

	goffset header_size_sans_crc = g_seekable_tell (G_SEEKABLE (dis));

	if (!gzh->hcrc)
		*first_block_offset = header_size_sans_crc;
	else
	{
		*first_block_offset = header_size_sans_crc + 2;
		uLong header_crc = g_data_input_stream_read_uint16 (dis, NULL, &err);
		if (err) goto error;

		g_seekable_seek (G_SEEKABLE (is), 0, G_SEEK_SET, NULL, &err);
		if (err) goto error;

		gpointer buf = g_malloc (header_size_sans_crc);
		g_input_stream_read (is, buf, header_size_sans_crc, NULL, &err);
		if (err) goto error;

		uLong crc = crc32 (0, NULL, 0);
		crc = crc32 (crc, buf, header_size_sans_crc);
		g_free (buf);

		if (header_crc != (guint16) crc)
		{
			g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
				"header checksum mismatch");
			goto error_own;
		}
	}

	gzh->done = 1;
	g_object_unref (dis);
	return TRUE;

error:
	g_propagate_error (error, err);
error_own:
	free_gzip_header (gzh);
	g_object_unref (dis);
	return FALSE;
}

static guint16 *
read_random_access_field (const gz_header *gzh,
	gsize *chunk_length, gsize *n_chunks, GError **error)
{
	if (!gzh->extra)
	{
		g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
			"no 'extra' field within the header");
		return NULL;
	}

	guchar *extra_iterator = gzh->extra;
	guchar *extra_end = gzh->extra + gzh->extra_len;

	guint16 *chunks = NULL;

	while (extra_iterator <= extra_end - 4)
	{
		guchar *f = extra_iterator;

		guint16 length = f[2] | (f[3] << 8);
		extra_iterator += length + 4;
		if (extra_iterator > extra_end)
		{
			g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
				"overflowing header subfield");
			g_free (chunks);
			return NULL;
		}

		if (f[0] != 'R' || f[1] != 'A')
			continue;

		if (chunks != NULL)
		{
			g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
				"multiple RA subfields present in the header");
			g_free (chunks);
			return NULL;
		}

		guint16 version = f[4] | (f[5] << 8);
		if (version != 1)
		{
			g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
				"unsupported RA subfield version");
			return NULL;
		}

		*chunk_length = f[6] | (f[7] << 8);
		if (*chunk_length == 0)
		{
			g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
				"invalid RA chunk length");
			return NULL;
		}

		*n_chunks = f[8] | (f[9] << 8);
		if ((gulong) (extra_iterator - f) < 10 + *n_chunks * 2)
		{
			g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
				"RA subfield overflow");
			return NULL;
		}

		chunks = g_malloc_n (*n_chunks, sizeof *chunks);

		guint i;
		for (i = 0; i < *n_chunks; i++)
			chunks[i] = f[10 + i * 2] + (f[10 + i * 2 + 1] << 8);
	}

	if (extra_iterator < extra_end - 4)
	{
		g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
			"invalid 'extra' field, subfield too short");
		g_free (chunks);
		return NULL;
	}

	return chunks;
}

// --- DictzipInputStream ------------------------------------------------------

static void dictzip_input_stream_finalize (GObject *gobject);

static void dictzip_input_stream_seekable_init
	(GSeekableIface *iface, gpointer iface_data);
static goffset dictzip_input_stream_tell (GSeekable *seekable);
static gboolean dictzip_input_stream_seek (GSeekable *seekable, goffset offset,
	GSeekType type, GCancellable *cancellable, GError **error);

static gssize dictzip_input_stream_read (GInputStream *stream, void *buffer,
	gsize count, GCancellable *cancellable, GError **error);
static gssize dictzip_input_stream_skip (GInputStream *stream, gsize count,
	GCancellable *cancellable, GError **error);

struct dictzip_input_stream_private
{
	GFileInfo  * file_info;            ///< File information from gzip header

	goffset      first_block_offset;   ///< Offset to the first block/chunk
	gsize        chunk_length;         ///< Uncompressed chunk length
	gsize        n_chunks;             ///< Number of chunks in file
	guint16    * chunks;               ///< Chunk sizes after compression

	z_stream     zs;                   ///< zlib decompression context
	gpointer     input_buffer;         ///< Input buffer

	goffset      offset;               ///< Current offset
	gpointer   * decompressed;         ///< Array of decompressed chunks
	gsize        last_chunk_length;    ///< Size of the last chunk
};

G_DEFINE_TYPE_EXTENDED (DictzipInputStream, dictzip_input_stream,
	G_TYPE_FILTER_INPUT_STREAM, 0,
	G_IMPLEMENT_INTERFACE (G_TYPE_SEEKABLE, dictzip_input_stream_seekable_init))

static gboolean seekable_true  (G_GNUC_UNUSED GSeekable *x) { return TRUE;  }
static gboolean seekable_false (G_GNUC_UNUSED GSeekable *x) { return FALSE; }

static void
dictzip_input_stream_seekable_init
	(GSeekableIface *iface, G_GNUC_UNUSED gpointer iface_data)
{
	iface->tell            = dictzip_input_stream_tell;
	iface->can_seek        = seekable_true;
	iface->seek            = dictzip_input_stream_seek;
	iface->can_truncate    = seekable_false;
}

static void
dictzip_input_stream_class_init (DictzipInputStreamClass *klass)
{
	g_type_class_add_private (klass, sizeof (DictzipInputStreamPrivate));

	GInputStreamClass *stream_class = G_INPUT_STREAM_CLASS (klass);
	stream_class->read_fn  = dictzip_input_stream_read;
	stream_class->skip     = dictzip_input_stream_skip;

	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = dictzip_input_stream_finalize;
}

static void
dictzip_input_stream_init (DictzipInputStream *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
		DICTZIP_TYPE_INPUT_STREAM, DictzipInputStreamPrivate);
}

static void
dictzip_input_stream_finalize (GObject *gobject)
{
	DictzipInputStreamPrivate *priv = DICTZIP_INPUT_STREAM (gobject)->priv;

	if (priv->file_info)
		g_object_unref (priv->file_info);
	g_free (priv->chunks);
	g_free (priv->input_buffer);
	inflateEnd (&priv->zs);

	guint i;
	for (i = 0; i < priv->n_chunks; i++)
		g_free (priv->decompressed[i]);
	g_free (priv->decompressed);

	G_OBJECT_CLASS (dictzip_input_stream_parent_class)->finalize (gobject);
}

static goffset
dictzip_input_stream_tell (GSeekable *seekable)
{
	return DICTZIP_INPUT_STREAM (seekable)->priv->offset;
}

static gpointer
inflate_chunk (DictzipInputStream *self,
	guint chunk_id, gsize *inflated_length, GError **error)
{
	DictzipInputStreamPrivate *priv = self->priv;
	g_return_val_if_fail (chunk_id < priv->n_chunks, NULL);

	GInputStream *base_stream = G_FILTER_INPUT_STREAM (self)->base_stream;

	guint i;
	goffset offset = priv->first_block_offset;
	for (i = 0; i < chunk_id; i++)
		offset += priv->chunks[i];

	if (!g_seekable_seek (G_SEEKABLE (base_stream),
		offset, G_SEEK_SET, NULL, error))
		return NULL;

	gssize read = g_input_stream_read (base_stream, priv->input_buffer,
		priv->chunks[chunk_id], NULL, error);
	if (read == -1)
		return NULL;

	if (read != priv->chunks[chunk_id])
	{
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"premature end of file");
		return NULL;
	}

	int z_err;
	gpointer chunk_data = g_malloc (priv->chunk_length);

	priv->zs.next_in   = (Bytef *) priv->input_buffer;
	priv->zs.avail_in  = read;
	priv->zs.total_in  = 0;

	priv->zs.next_out  = (Bytef *) chunk_data;
	priv->zs.avail_out = priv->chunk_length;
	priv->zs.total_out = 0;

	z_err = inflateReset (&priv->zs);
	if (z_err != Z_OK)
		goto error_zlib;

	z_err = inflate (&priv->zs, Z_BLOCK);
	if (z_err != Z_OK)
		goto error_zlib;

	*inflated_length = priv->zs.total_out;
	return chunk_data;

error_zlib:
	g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		"failed to inflate the requested block: %s", zError (z_err));
	g_free (chunk_data);
	return NULL;
}

static gpointer
get_chunk (DictzipInputStream *self, guint chunk_id, GError **error)
{
	DictzipInputStreamPrivate *priv = self->priv;
	gpointer chunk = priv->decompressed[chunk_id];
	if (!chunk)
	{
		// Just inflating the file piece by piece as needed.
		gsize chunk_size;
		chunk = inflate_chunk (self, chunk_id, &chunk_size, error);
		if (!chunk)
			return NULL;

		if (chunk_id + 1 == priv->n_chunks)
			priv->last_chunk_length = chunk_size;
		else if (chunk_size < priv->chunk_length)
		{
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				"inflated dictzip chunk is too short");
			g_free (chunk);
			return NULL;
		}

		priv->decompressed[chunk_id] = chunk;
	}
	return chunk;
}

static gboolean
dictzip_input_stream_seek (GSeekable *seekable, goffset offset,
	GSeekType type, GCancellable *cancellable, GError **error)
{
	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return FALSE;

	if (type == G_SEEK_END)
	{
		// This could be implemented by retrieving the last chunk
		// and deducing the filesize, should the functionality be needed.
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"I don't know where the stream ends, cannot seek there");
		return FALSE;
	}

	DictzipInputStream *self = DICTZIP_INPUT_STREAM (seekable);
	goffset new_offset;

	if (type == G_SEEK_SET)
		new_offset = offset;
	else if (type == G_SEEK_CUR)
		new_offset = self->priv->offset + offset;
	else
		g_assert_not_reached ();

	if (new_offset < 0)
	{
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"cannot seek before the start of data");
		return FALSE;
	}

	self->priv->offset = new_offset;
	return TRUE;
}

static gssize
dictzip_input_stream_read (GInputStream *stream, void *buffer,
	gsize count, GCancellable *cancellable, GError **error)
{
	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return -1;

	DictzipInputStream *self = DICTZIP_INPUT_STREAM (stream);
	DictzipInputStreamPrivate *priv = self->priv;
	gssize read = 0;

	guint chunk_id     = priv->offset / priv->chunk_length;
	guint chunk_offset = priv->offset % priv->chunk_length;

	do
	{
		if (chunk_id >= priv->n_chunks)
			return read;

		gpointer chunk = get_chunk (self, chunk_id, error);
		if (!chunk)
			return -1;

		glong to_copy;
		if (chunk_id + 1 == priv->n_chunks)
			// Set by the call to get_chunk().
			to_copy = priv->last_chunk_length - chunk_offset;
		else
			to_copy = priv->chunk_length - chunk_offset;

		if (to_copy > (glong) count)
			to_copy = count;

		if (to_copy > 0)
		{
			memcpy (buffer, chunk + chunk_offset, to_copy);
			buffer += to_copy;
			priv->offset += to_copy;
			count -= to_copy;
			read += to_copy;
		}

		chunk_id++;
		chunk_offset = 0;
	}
	while (count);

	return read;
}

static gssize
dictzip_input_stream_skip (GInputStream *stream, gsize count,
	GCancellable *cancellable, GError **error)
{
	if (!dictzip_input_stream_seek (G_SEEKABLE (stream), count,
		G_SEEK_CUR, cancellable, error))
		return -1;

	return count;
}

/// Create an input stream for the underlying dictzip file.
DictzipInputStream *
dictzip_input_stream_new (GInputStream *base_stream, GError **error)
{
	g_return_val_if_fail (G_IS_INPUT_STREAM (base_stream), NULL);

	if (!G_IS_SEEKABLE (base_stream)
	 || !g_seekable_can_seek (G_SEEKABLE (base_stream)))
	{
		g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_NOT_SEEKABLE,
			"the underlying stream isn't seekable");
		return NULL;
	}

	GError *err = NULL;
	DictzipInputStream *self = g_object_new (DICTZIP_TYPE_INPUT_STREAM,
		"base-stream", base_stream, "close-base-stream", FALSE, NULL);
	DictzipInputStreamPrivate *priv = self->priv;

	// Decode the header.
	gz_header gzh;
	if (!read_gzip_header (G_INPUT_STREAM (base_stream),
		&gzh, &priv->first_block_offset, &err))
	{
		g_propagate_error (error, err);
		goto error;
	}

	priv->chunks = read_random_access_field (&gzh,
		&priv->chunk_length, &priv->n_chunks, &err);
	if (err)
	{
		g_propagate_error (error, err);
		goto error;
	}

	if (!priv->chunks)
	{
		g_set_error (error, DICTZIP_ERROR, DICTZIP_ERROR_INVALID_HEADER,
			"not a dictzip file");
		goto error;
	}

	// Store file information.
	priv->file_info = g_file_info_new ();

	if (gzh.time != 0)
	{
		GTimeVal m_time = { gzh.time, 0 };
		g_file_info_set_modification_time (priv->file_info, &m_time);
	}

	if (gzh.name && *gzh.name)
		g_file_info_set_name (priv->file_info, (gchar *) gzh.name);

	// Initialise zlib.
	int z_err;
	z_err = inflateInit2 (&priv->zs, -15);
	if (z_err != Z_OK)
	{
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			"zlib initialisation failed: %s", zError (z_err));
		goto error;
	}

	priv->input_buffer = g_malloc (65536);
	priv->decompressed = g_new0 (gpointer, priv->n_chunks);
	priv->last_chunk_length = -1;  // We don't know yet.

	free_gzip_header (&gzh);
	return self;

error:
	free_gzip_header (&gzh);
	g_object_unref (self);
	return NULL;
}

/// Return file information for the compressed file.
GFileInfo *
dictzip_input_stream_get_file_info (DictzipInputStream *self)
{
	g_return_val_if_fail (DICTZIP_IS_INPUT_STREAM (self), NULL);

	DictzipInputStreamPrivate *priv = self->priv;
	return priv->file_info;
}
