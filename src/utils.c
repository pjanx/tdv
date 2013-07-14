/*
 * utils.c: miscellaneous utilities
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

#include <glib.h>
#include <gio/gio.h>

#include "utils.h"


/** Read the whole stream into a byte array. */
gboolean
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
gchar *
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
