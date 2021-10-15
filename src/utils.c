/*
 * utils.c: miscellaneous utilities
 *
 * Copyright (c) 2013 - 2020, Přemysl Eric Janouch <p@janouch.name>
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

#include <glib.h>
#include <glib/gprintf.h>
#include <gio/gio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>

#include "config.h"
#include "utils.h"


/// Trivially filter out all tags that aren't part of the Pango markup language,
/// or no frontend can quite handle--this seems to work well.
/// Given the nature of our display, also skip whole keyword elements.
gchar *
xdxf_to_pango_markup_with_reduced_effort (const gchar *xml)
{
	GString *filtered = g_string_new ("");
	while (*xml)
	{
		// GMarkup can read some of the wilder XML constructs, Pango skips them
		const gchar *p = NULL;
		if (*xml != '<' || xml[1] == '!' || xml[1] == '?'
		 || g_ascii_isspace (xml[1]) || !*(p = xml + 1 + (xml[1] == '/'))
		 || (strchr ("biu", *p) && p[1] == '>') || !(p = strchr (p, '>')))
			g_string_append_c (filtered, *xml++);
		else if (xml[1] != 'k' || xml[2] != '>' || !(xml = strstr (p, "</k>")))
			xml = ++p;
	}
	return g_string_free (filtered, FALSE);
}

/// Read the whole stream into a byte array.
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

/// Read a null-terminated string from a data input stream.
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

gboolean
xstrtoul (unsigned long *out, const char *s, int base)
{
	char *end;
	errno = 0;
	*out = strtoul (s, &end, base);
	return errno == 0 && !*end && end != s;
}

/// Print a fatal error message and terminate the process immediately.
void
fatal (const gchar *format, ...)
{
	va_list ap;
	va_start (ap, format);
	g_vfprintf (stderr, format, ap);
	exit (EXIT_FAILURE);
	va_end (ap);
}
