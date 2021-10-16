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
#include <glib/gstdio.h>

#include <stdlib.h>
#include <errno.h>
#include <stdarg.h>

#include <pwd.h>

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

// At times, GLib even with its sheer size is surprisingly useless,
// and I need to port some code over from "liberty".

static const gchar **
get_xdg_config_dirs (void)
{
	GPtrArray *paths = g_ptr_array_new ();
	g_ptr_array_add (paths, (gpointer) g_get_user_config_dir ());
	for (const gchar *const *system = g_get_system_config_dirs ();
		*system; system++)
		g_ptr_array_add (paths, (gpointer) *system);
	g_ptr_array_add (paths, NULL);
	return (const gchar **) g_ptr_array_free (paths, FALSE);
}

gchar *
resolve_relative_filename_generic
	(const gchar **paths, const gchar *tail, const gchar *filename)
{
	for (; *paths; paths++)
	{
		// As per XDG spec, relative paths are ignored
		if (**paths != '/')
			continue;

		gchar *file = g_build_filename (*paths, tail, filename, NULL);
		GStatBuf st;
		if (!g_stat (file, &st))
			return file;
		g_free (file);
	}
	return NULL;
}

gchar *
resolve_relative_config_filename (const gchar *filename)
{
	const gchar **paths = get_xdg_config_dirs ();
	gchar *result =
		resolve_relative_filename_generic (paths, PROJECT_NAME, filename);
	g_free (paths);
	return result;
}

static gchar *
try_expand_tilde (const gchar *filename)
{
	size_t until_slash = strcspn (filename, "/");
	if (!until_slash)
		return g_build_filename (g_get_home_dir () ?: "", filename, NULL);

	long buf_len = sysconf (_SC_GETPW_R_SIZE_MAX);
	if (buf_len < 0)
		buf_len = 1024;
	struct passwd pwd, *success = NULL;

	gchar *user = g_strndup (filename, until_slash);
	gchar *buf = g_malloc (buf_len);
	while (getpwnam_r (user, &pwd, buf, buf_len, &success) == ERANGE)
		buf = g_realloc (buf, buf_len <<= 1);
	g_free (user);

	gchar *result = NULL;
	if (success)
		result = g_strdup_printf ("%s%s", pwd.pw_dir, filename + until_slash);
	g_free (buf);
	return result;
}

gchar *
resolve_filename (const gchar *filename, gchar *(*relative_cb) (const char *))
{
	// Absolute path is absolute
	if (*filename == '/')
		return g_strdup (filename);

	// We don't want to use wordexp() for this as it may execute /bin/sh
	if (*filename == '~')
	{
		// Paths to home directories ought to be absolute
		char *expanded = try_expand_tilde (filename + 1);
		if (expanded)
			return expanded;
		g_debug ("failed to expand the home directory in `%s'", filename);
	}
	return relative_cb (filename);
}

GKeyFile *
load_project_config_file (GError **error)
{
	GKeyFile *key_file = g_key_file_new ();
	const gchar **paths = get_xdg_config_dirs ();
	GError *e = NULL;

	// XXX: if there are dashes in the final path component,
	//   the function tries to replace them with directory separators,
	//   which is completely undocumented
	g_key_file_load_from_dirs (key_file,
		PROJECT_NAME G_DIR_SEPARATOR_S PROJECT_NAME ".conf",
		paths, NULL, 0, &e);
	g_free (paths);
	if (!e)
		return key_file;

	if (e->code == G_KEY_FILE_ERROR_NOT_FOUND)
		g_error_free (e);
	else
		g_propagate_error (error, e);

	g_key_file_free (key_file);
	return NULL;
}

// --- Loading -----------------------------------------------------------------

void
dictionary_destroy (Dictionary *self)
{
	g_free (self->name);
	g_free (self->filename);

	if (self->dict)
		g_object_unref (self->dict);

	g_free (self);
}

static gboolean
dictionary_load (Dictionary *self, GError **e)
{
	if (!(self->dict = stardict_dict_new (self->filename, e)))
		return FALSE;

	if (!self->name)
	{
		self->name = g_strdup (stardict_info_get_book_name
			(stardict_dict_get_info (self->dict)));
	}
	return TRUE;
}

static gboolean
load_dictionaries_sequentially (GPtrArray *dictionaries, GError **e)
{
	for (guint i = 0; i < dictionaries->len; i++)
		if (!dictionary_load (g_ptr_array_index (dictionaries, i), e))
			return FALSE;
	return TRUE;
}

// Parallelize dictionary loading if possible, because of collation reindexing
#if GLIB_CHECK_VERSION (2, 36, 0)
static void
load_worker (gpointer data, gpointer user_data)
{
	GError *e = NULL;
	dictionary_load (data, &e);
	if (e)
		g_async_queue_push (user_data, e);
}

gboolean
load_dictionaries (GPtrArray *dictionaries, GError **e)
{
	GAsyncQueue *error_queue =
		g_async_queue_new_full ((GDestroyNotify) g_error_free);
	GThreadPool *pool = g_thread_pool_new (load_worker, error_queue,
		g_get_num_processors (), TRUE, NULL);
	if G_UNLIKELY (!g_thread_pool_get_num_threads (pool))
	{
		g_thread_pool_free (pool, TRUE, TRUE);
		g_async_queue_unref (error_queue);
		return load_dictionaries_sequentially (dictionaries, e);
	}

	for (guint i = 0; i < dictionaries->len; i++)
		g_thread_pool_push (pool, g_ptr_array_index (dictionaries, i), NULL);
	g_thread_pool_free (pool, FALSE, TRUE);

	gboolean result = TRUE;
	if ((*e = g_async_queue_try_pop (error_queue)))
		result = FALSE;

	g_async_queue_unref (error_queue);
	return result;
}
#else  // GLib < 2.36
gboolean
load_dictionaries (GPtrArray *dictionaries, GError **e)
{
	return load_dictionaries_sequentially (dictionaries, e);
}
#endif  // GLib < 2.36
