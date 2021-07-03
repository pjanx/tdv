/*
 * StarDict terminal UI
 *
 * Copyright (c) 2013 - 2021, Přemysl Eric Janouch <p@janouch.name>
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
#include <locale.h>
#include <stdarg.h>
#include <limits.h>
#include <string.h>

#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <pango/pango.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>
#include <pwd.h>

#include <termo.h> // input
#include <ncurses.h> // output

#include "config.h"
#include "stardict.h"
#include "utils.h"

#define CTRL_KEY(x)  ((x) - 'A' + 1)

#define TOP_BAR_CUTOFF  2               ///< How many lines are reserved on top
#define APP_TITLE  PROJECT_NAME " "     ///< Left top corner

// --- Utilities ---------------------------------------------------------------

static size_t
unichar_width (gunichar ch)
{
	if (g_unichar_iszerowidth (ch))
		return 0;
	return 1 + g_unichar_iswide (ch);
}

static guint
add_read_watch (int fd, GIOFunc func, gpointer user_data)
{
	GIOChannel *channel = g_io_channel_unix_new (fd);
	guint res = g_io_add_watch (channel, G_IO_IN, func, user_data);
	g_io_channel_unref (channel);
	return res;
}

// At times, GLib even with its sheer size is surprisingly useless,
// and I need to port some code over from "liberty".

static gchar **
get_xdg_config_dirs (void)
{
	GPtrArray *paths = g_ptr_array_new ();
	g_ptr_array_add (paths, (gpointer) g_get_user_config_dir ());
	for (const gchar *const *system = g_get_system_config_dirs ();
		*system; system++)
		g_ptr_array_add (paths, (gpointer) *system);
	g_ptr_array_add (paths, NULL);
	return (gchar **) g_ptr_array_free (paths, FALSE);
}

static gchar *
resolve_relative_filename_generic
	(gchar **paths, const gchar *tail, const gchar *filename)
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

static gchar *
resolve_relative_config_filename (const gchar *filename)
{
	gchar **paths = get_xdg_config_dirs ();
	gchar *result = resolve_relative_filename_generic
		(paths, PROJECT_NAME, filename);
	g_strfreev (paths);
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

static gchar *
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

// --- Application -------------------------------------------------------------

#define ATTRIBUTE_TABLE(XX)                               \
	XX( HEADER,    "header",        -1, -1, A_REVERSE   ) \
	XX( ACTIVE,    "header-active", -1, -1, A_UNDERLINE ) \
	XX( SEARCH,    "search",        -1, -1, A_UNDERLINE ) \
	XX( EVEN,      "even",          -1, -1, 0           ) \
	XX( ODD,       "odd",           -1, -1, 0           ) \
	XX( SELECTION, "selection",     -1, -1, A_REVERSE   ) \
	XX( DEFOCUSED, "defocused",     -1, -1, A_REVERSE   )

enum
{
#define XX(name, config, fg_, bg_, attrs_) ATTRIBUTE_ ## name,
	ATTRIBUTE_TABLE (XX)
#undef XX
	ATTRIBUTE_COUNT
};

struct attrs
{
	short fg;                           ///< Foreground color index
	short bg;                           ///< Background color index
	chtype attrs;                       ///< Other attributes
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Data relating to one entry within the dictionary.
typedef struct view_entry               ViewEntry;
/// Data relating to a dictionary file.
typedef struct dictionary               Dictionary;
/// Encloses application data.
typedef struct application              Application;

struct view_entry
{
	gchar   * word;                     ///< Word
	gchar  ** definitions;              ///< Word definition entries
	gsize     definitions_length;       ///< Length of the @a definitions array
};

struct dictionary
{
	gchar         * name;               ///< Visible identifier
	gsize           name_width;         ///< Visible width of the name
	gchar         * filename;           ///< Path to the dictionary
	StardictDict  * dict;               ///< Dictionary
};

struct application
{
	GMainLoop     * loop;               ///< Main loop
	termo_t       * tk;                 ///< termo handle
	guint           tk_timer;           ///< termo timeout timer
	GIConv          ucs4_to_locale;     ///< UTF-32 -> locale conversion
	gboolean        locale_is_utf8;     ///< The locale is Unicode
	gboolean        focused;            ///< Whether the terminal has focus

	GArray        * dictionaries;       ///< All loaded dictionaries

	StardictDict  * dict;               ///< The current dictionary
	guint           show_help : 1;      ///< Whether help can be shown
	guint           center_search : 1;  ///< Whether to center the search
	guint           underline_last : 1; ///< Underline the last definition
	guint           hl_prefix : 1;      ///< Highlight the common prefix
	guint           watch_x11_sel : 1;  ///< Requested X11 selection watcher

	guint32         top_position;       ///< Index of the topmost dict. entry
	guint           top_offset;         ///< Offset into the top entry
	guint           selected;           ///< Offset to the selected definition
	GPtrArray     * entries;            ///< ViewEntry's within the view

	gchar         * search_label;       ///< Text of the "Search" label
	GArray        * input;              ///< The current search input
	guint           input_pos;          ///< Cursor position within input
	gboolean        input_confirmed;    ///< Input has been confirmed

	gfloat          division;           ///< Position of the division column

	struct attrs    attrs[ATTRIBUTE_COUNT];
};

/// Shortcut to retrieve named terminal attributes
#define APP_ATTR(name) self->attrs[ATTRIBUTE_ ## name].attrs

/// Returns if the Unicode character is representable in the current locale.
static gboolean
app_is_character_in_locale (Application *self, gunichar ch)
{
	// Avoid the overhead joined with calling iconv() for all characters
	if (self->locale_is_utf8)
		return TRUE;

	gchar *tmp = g_convert_with_iconv ((const gchar *) &ch, sizeof ch,
		self->ucs4_to_locale, NULL, NULL, NULL);
	if (!tmp)
		return FALSE;
	g_free (tmp);
	return TRUE;
}

static gsize
app_char_width (Application *app, gunichar c)
{
	if (!app_is_character_in_locale (app, c))
		return 1;
	return unichar_width (c);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Splits the entry and adds it to a pointer array.
static void
view_entry_split_add (GPtrArray *out, const gchar *text)
{
	const gchar *p = text, *nl;
	for (; (nl = strchr (p, '\n')); p = nl + 1)
		if (nl != p)
			g_ptr_array_add (out, g_strndup (p, nl - p));
	if (*p)
		g_ptr_array_add (out, g_strdup (p));
}

/// Decomposes a dictionary entry into the format we want.
static ViewEntry *
view_entry_new (StardictIterator *iterator)
{
	g_return_val_if_fail (stardict_iterator_is_valid (iterator), NULL);

	ViewEntry *ve = g_slice_alloc (sizeof *ve);
	GString *word = g_string_new (stardict_iterator_get_word (iterator));

	StardictEntry *entry = stardict_iterator_get_entry (iterator);
	g_return_val_if_fail (entry != NULL, NULL);

	GPtrArray *definitions = g_ptr_array_new ();
	const GList *fields = stardict_entry_get_fields (entry);
	gboolean found_anything_displayable = FALSE;
	while (fields)
	{
		const StardictEntryField *field = fields->data;
		switch (field->type)
		{
		case STARDICT_FIELD_MEANING:
			view_entry_split_add (definitions, field->data);
			found_anything_displayable = TRUE;
			break;
		case STARDICT_FIELD_PANGO:
		{
			char *text;
			if (!pango_parse_markup (field->data, -1,
				0, NULL, &text, NULL, NULL))
				text = g_strdup_printf ("<%s>", _("error in entry"));
			view_entry_split_add (definitions, text);
			found_anything_displayable = TRUE;
			g_free (text);
			break;
		}
		case STARDICT_FIELD_PHONETIC:
			g_string_append_printf (word, " /%s/", (const gchar *) field->data);
			break;
		default:
			// TODO support more of them
			break;
		}
		fields = fields->next;
	}
	g_object_unref (entry);

	if (!found_anything_displayable)
		g_ptr_array_add (definitions,
			g_strdup_printf ("<%s>", _("no usable field found")));

	ve->word = g_string_free (word, FALSE);
	ve->definitions_length = definitions->len;
	g_ptr_array_add (definitions, NULL);
	ve->definitions = (gchar **) g_ptr_array_free (definitions, FALSE);
	return ve;
}

/// Release resources associated with the view entry.
static void
view_entry_free (ViewEntry *ve)
{
	g_free (ve->word);
	g_strfreev (ve->definitions);
	g_slice_free1 (sizeof *ve, ve);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gboolean
dictionary_load (Dictionary *self, Application *app, GError **e)
{
	if (!(self->dict = stardict_dict_new (self->filename, e)))
		return FALSE;

	if (!self->name)
	{
		self->name = g_strdup (stardict_info_get_book_name
			(stardict_dict_get_info (self->dict)));
	}

	// Add some padding for decorative purposes
	gchar *tmp = g_strdup_printf (" %s ", self->name);
	g_free (self->name);
	self->name = tmp;

	gunichar *ucs4 = g_utf8_to_ucs4_fast (self->name, -1, NULL);
	for (gunichar *it = ucs4; *it; it++)
		self->name_width += app_char_width (app, *it);
	g_free (ucs4);
	return TRUE;
}

static void
dictionary_free (Dictionary *self)
{
	g_free (self->name);
	g_free (self->filename);

	if (self->dict)
		g_object_unref (self->dict);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Reload view items.
static void
app_reload_view (Application *self)
{
	if (self->entries->len != 0)
		g_ptr_array_remove_range (self->entries, 0, self->entries->len);

	gint remains = LINES - TOP_BAR_CUTOFF + self->top_offset;
	StardictIterator *iterator =
		stardict_iterator_new (self->dict, self->top_position);
	while (remains > 0 && stardict_iterator_is_valid (iterator))
	{
		ViewEntry *entry = view_entry_new (iterator);
		remains -= entry->definitions_length;
		g_ptr_array_add (self->entries, entry);
		stardict_iterator_next (iterator);
	}
	g_object_unref (iterator);
}

/// Load configuration for a color using a subset of git config colors.
static void
app_load_color (Application *self, GKeyFile *kf, const gchar *name, int id)
{
	gchar *value = g_key_file_get_string (kf, "Colors", name, NULL);
	if (!value)
		return;

	struct attrs attrs = { -1, -1, 0 };
	gchar **values = g_strsplit (value, " ", 0);
	gint colors = 0;
	for (gchar **it = values; *it; it++)
	{
		gchar *end = NULL;
		gint64 n = g_ascii_strtoll (*it, &end, 10);
		if (*it != end && !*end && n >= G_MINSHORT && n <= G_MAXSHORT)
		{
			if (colors == 0) attrs.fg = n;
			if (colors == 1) attrs.bg = n;
			colors++;
		}
		else if (!strcmp (*it, "bold"))    attrs.attrs |= A_BOLD;
		else if (!strcmp (*it, "dim"))     attrs.attrs |= A_DIM;
		else if (!strcmp (*it, "ul"))      attrs.attrs |= A_UNDERLINE;
		else if (!strcmp (*it, "blink"))   attrs.attrs |= A_BLINK;
		else if (!strcmp (*it, "reverse")) attrs.attrs |= A_REVERSE;
#ifdef A_ITALIC
		else if (!strcmp (*it, "italic"))  attrs.attrs |= A_ITALIC;
#endif  // A_ITALIC
	}
	g_strfreev (values);

	g_free (value);
	self->attrs[id] = attrs;
}

static gboolean
app_load_bool (GKeyFile *kf, const gchar *name, gboolean def)
{
	GError *e = NULL;
	bool value = g_key_file_get_boolean (kf, "Settings", name, &e);
	if (e)
	{
		g_error_free (e);
		return def;
	}
	return value;
}

static void
app_load_config_values (Application *self, GKeyFile *kf)
{
	self->center_search =
		app_load_bool (kf, "center-search", self->center_search);
	self->underline_last =
		app_load_bool (kf, "underline-last", self->underline_last);
	self->hl_prefix =
		app_load_bool (kf, "hl-common-prefix", self->hl_prefix);
	self->watch_x11_sel =
		app_load_bool (kf, "watch-selection", self->watch_x11_sel);

#define XX(name, config, fg_, bg_, attrs_) \
	app_load_color (self, kf, config, ATTRIBUTE_ ## name);
	ATTRIBUTE_TABLE (XX)
#undef XX

	const gchar *dictionaries = "Dictionaries";
	gchar **names = g_key_file_get_keys (kf, dictionaries, NULL, NULL);
	if (!names)
		return;

	for (gchar **it = names; *it; it++)
	{
		gchar *path = g_key_file_get_string (kf, dictionaries, *it, NULL);
		if (!path)
			continue;

		// Try to resolve relative paths and expand tildes
		gchar *resolved = resolve_filename
			(path, resolve_relative_config_filename);
		if (resolved)
			g_free (path);
		else
			resolved = path;

		Dictionary dict = { .name = g_strdup (*it), .filename = resolved };
		g_array_append_val (self->dictionaries, dict);
	}
	g_strfreev (names);
}

static void
app_load_config (Application *self, GError **e)
{
	GKeyFile *kf = g_key_file_new ();
	gchar **paths = get_xdg_config_dirs ();

	// XXX: if there are dashes in the final path component,
	//   the function tries to replace them with directory separators,
	//   which is completely undocumented
	GError *error = NULL;
	g_key_file_load_from_dirs (kf,
		PROJECT_NAME G_DIR_SEPARATOR_S PROJECT_NAME ".conf",
		(const gchar **) paths, NULL, 0, &error);
	g_strfreev (paths);

	// TODO: proper error handling showing all relevant information;
	//   we can afford that here since the terminal hasn't been initialized yet
	if (!error)
		app_load_config_values (self, kf);
	else if (error->code == G_KEY_FILE_ERROR_NOT_FOUND)
		g_error_free (error);
	else
		g_propagate_error (e, error);

	g_key_file_free (kf);
}

static void
app_init_attrs (Application *self)
{
#define XX(name, config, fg_, bg_, attrs_)          \
	self->attrs[ATTRIBUTE_ ## name].fg    = fg_;    \
	self->attrs[ATTRIBUTE_ ## name].bg    = bg_;    \
	self->attrs[ATTRIBUTE_ ## name].attrs = attrs_;
	ATTRIBUTE_TABLE (XX)
#undef XX
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gboolean
app_load_dictionaries (Application *self, GError **e)
{
	for (guint i = 0; i < self->dictionaries->len; i++)
		if (!dictionary_load (&g_array_index (self->dictionaries,
			Dictionary, i), self, e))
			return FALSE;
	return TRUE;
}

// Parallelize dictionary loading if possible, because of collation reindexing
#if GLIB_CHECK_VERSION (2, 36, 0)
struct load_ctx
{
	Application *self;                  ///< Application context
	GAsyncQueue *error_queue;           ///< Errors
};

static void
app_load_worker (gpointer data, gpointer user_data)
{
	struct load_ctx *ctx = user_data;
	GError *e = NULL;
	dictionary_load (&g_array_index (ctx->self->dictionaries, Dictionary,
		GPOINTER_TO_UINT (data) - 1), ctx->self, &e);
	if (e)
		g_async_queue_push (ctx->error_queue, e);
}

static gboolean
app_load_dictionaries_parallel (Application *self, GError **e)
{
	struct load_ctx ctx;
	GThreadPool *pool = g_thread_pool_new (app_load_worker, &ctx,
		g_get_num_processors (), TRUE, NULL);
	if G_UNLIKELY (!g_thread_pool_get_num_threads (pool))
	{
		g_thread_pool_free (pool, TRUE, TRUE);
		return app_load_dictionaries (self, e);
	}

	ctx.self = self;
	ctx.error_queue = g_async_queue_new_full ((GDestroyNotify) g_error_free);
	for (guint i = 0; i < self->dictionaries->len; i++)
		g_thread_pool_push (pool, GUINT_TO_POINTER (i + 1), NULL);

	g_thread_pool_free (pool, FALSE, TRUE);

	gboolean result = TRUE;
	if ((*e = g_async_queue_try_pop (ctx.error_queue)))
		result = FALSE;

	g_async_queue_unref (ctx.error_queue);
	return result;
}

#define app_load_dictionaries app_load_dictionaries_parallel
#endif  // GLib >= 2.36

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Initialize the application core.
static void
app_init (Application *self, char **filenames)
{
	self->loop = NULL;

	self->tk = NULL;
	self->tk_timer = 0;

	self->show_help = TRUE;
	self->center_search = TRUE;
	self->underline_last = TRUE;
	self->hl_prefix = TRUE;
	self->watch_x11_sel = FALSE;

	self->top_position = 0;
	self->top_offset = 0;
	self->selected = 0;
	self->entries = g_ptr_array_new_with_free_func
		((GDestroyNotify) view_entry_free);

	self->search_label = g_strdup_printf ("%s: ", _("Search"));

	self->input = g_array_new (TRUE, FALSE, sizeof (gunichar));
	self->input_pos = 0;
	self->input_confirmed = FALSE;

	self->division = 0.5;

	const char *charset;
	self->locale_is_utf8 = g_get_charset (&charset);
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
	self->ucs4_to_locale = g_iconv_open (charset, "UTF-32LE");
#else // G_BYTE_ORDER != G_LITTLE_ENDIAN
	self->ucs4_to_locale = g_iconv_open (charset, "UTF-32BE");
#endif // G_BYTE_ORDER != G_LITTLE_ENDIAN
	self->focused = TRUE;

	app_init_attrs (self);
	self->dictionaries = g_array_new (FALSE, TRUE, sizeof (Dictionary));
	g_array_set_clear_func
		(self->dictionaries, (GDestroyNotify) dictionary_free);

	GError *error = NULL;
	app_load_config (self, &error);
	if (error)
	{
		g_printerr ("%s: %s\n", _("Cannot load configuration"), error->message);
		exit (EXIT_FAILURE);
	}

	self->loop = g_main_loop_new (NULL, FALSE);

	// Dictionaries given on the command line override the configuration
	if (*filenames)
	{
		g_array_set_size (self->dictionaries, 0);
		while (*filenames)
		{
			Dictionary dict = { .filename = g_strdup (*filenames++) };
			g_array_append_val (self->dictionaries, dict);
		}
	}

	if (!app_load_dictionaries (self, &error))
	{
		g_printerr ("%s: %s\n", _("Error loading dictionary"), error->message);
		exit (EXIT_FAILURE);
	}
	if (!self->dictionaries->len)
	{
		g_printerr ("%s\n", _("No dictionaries found either in "
			"the configuration or on the command line"));
		exit (EXIT_FAILURE);
	}
	self->dict = g_array_index (self->dictionaries, Dictionary, 0).dict;
	app_reload_view (self);
}

static void
app_init_terminal (Application *self)
{
	TERMO_CHECK_VERSION;
	if (!(self->tk = termo_new (STDIN_FILENO, NULL, 0)))
		abort ();
	if (!initscr () || nonl () == ERR)
		abort ();

	// By default we don't use any colors so they're not required...
	if (start_color () == ERR
	 || use_default_colors () == ERR
	 || COLOR_PAIRS <= ATTRIBUTE_COUNT)
		return;

	gboolean failed = FALSE;
	for (int a = 0; a < ATTRIBUTE_COUNT; a++)
	{
		if (self->attrs[a].fg == -1 &&
			self->attrs[a].bg == -1)
			continue;

		if (self->attrs[a].fg >= COLORS || self->attrs[a].fg < -1
		 || self->attrs[a].bg >= COLORS || self->attrs[a].bg < -1)
		{
			failed = TRUE;
			continue;
		}

		init_pair (a + 1, self->attrs[a].fg, self->attrs[a].bg);
		self->attrs[a].attrs |= COLOR_PAIR (a + 1);
	}

	// ...thus we can reset back to defaults even after initializing some
	if (failed)
		app_init_attrs (self);
}

/// Free any resources used by the application.
static void
app_destroy (Application *self)
{
	if (self->loop)
		g_main_loop_unref (self->loop);
	if (self->tk)
		termo_destroy (self->tk);
	if (self->tk_timer)
		g_source_remove (self->tk_timer);

	g_ptr_array_free (self->entries, TRUE);
	g_free (self->search_label);
	g_array_free (self->input, TRUE);
	g_array_free (self->dictionaries, TRUE);

	g_iconv_close (self->ucs4_to_locale);
}

/// Run the main event dispatch loop.
static void
app_run (Application *self)
{
	g_main_loop_run (self->loop);
}

/// Quit the main event dispatch loop.
static void
app_quit (Application *self)
{
	g_main_loop_quit (self->loop);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Necessary abstraction to simplify aligned, formatted character output

typedef struct row_char                 RowChar;
typedef struct row_buffer               RowBuffer;

struct row_char
{
	gunichar c;                         ///< Unicode codepoint
	chtype attrs;                       ///< Special attributes
	int width;                          ///< How many cells this takes
};

struct row_buffer
{
	Application *app;                   ///< Reference to Application
	GArray *chars;                      ///< Characters
	int total_width;                    ///< Total width of all characters
};

static void
row_buffer_init (RowBuffer *self, Application *app)
{
	self->app = app;
	self->chars = g_array_new (FALSE, TRUE, sizeof (RowChar));
	self->total_width = 0;
}

#define row_buffer_free(self) g_array_unref ((self)->chars)

/// Replace invalid chars and push all codepoints to the array w/ attributes.
static void
row_buffer_append (RowBuffer *self, const gchar *str, chtype attrs)
{
	glong ucs4_len;
	gunichar *ucs4 = g_utf8_to_ucs4_fast (str, -1, &ucs4_len);
	for (glong i = 0; i < ucs4_len; i++)
	{
		// XXX: this is very crude as it disrespects combining marks
		gunichar c =
			app_is_character_in_locale (self->app, ucs4[i]) ? ucs4[i] : '?';
		struct row_char rc = { c, attrs, unichar_width (c) };
		g_array_append_val (self->chars, rc);
		self->total_width += rc.width;
	}
	g_free (ucs4);
}

/// Pop as many codepoints as needed to free up "space" character cells.
/// Given the suffix nature of combining marks, this should work pretty fine.
static gint
row_buffer_pop_cells (RowBuffer *self, gint space)
{
	int made = 0;
	while (self->chars->len && made < space)
	{
		guint last = self->chars->len - 1;
		made += g_array_index (self->chars, RowChar, last).width;
		g_array_remove_index (self->chars, last);
	}
	self->total_width -= made;
	return made;
}

static void
row_buffer_ellipsis (RowBuffer *self, int target, chtype attrs)
{
	row_buffer_pop_cells (self, self->total_width - target);

	gunichar ellipsis = L'…';
	if (app_is_character_in_locale (self->app, ellipsis))
	{
		if (self->total_width >= target)
			row_buffer_pop_cells (self, 1);
		if (self->total_width + 1 <= target)
			row_buffer_append (self, "…", attrs);
	}
	else if (target >= 3)
	{
		if (self->total_width >= target)
			row_buffer_pop_cells (self, 3);
		if (self->total_width + 3 <= target)
			row_buffer_append (self, "...", attrs);
	}
}

static void
row_buffer_print (RowBuffer *self, gunichar *ucs4, size_t len, chtype attrs)
{
	gsize locale_str_len;
	gchar *str = g_convert_with_iconv ((const gchar *) ucs4, len * sizeof *ucs4,
		self->app->ucs4_to_locale, NULL, &locale_str_len, NULL);
	g_return_if_fail (str != NULL);

	attrset (attrs);
	addstr (str);
	attrset (0);
	g_free (str);
}

static void
row_buffer_flush (RowBuffer *self)
{
	if (!self->chars->len)
		return;

	gunichar ucs4[self->chars->len];
	for (guint i = 0; i < self->chars->len; i++)
		ucs4[i] = g_array_index (self->chars, RowChar, i).c;

	guint mark = 0;
	for (guint i = 1; i < self->chars->len; i++)
	{
		chtype attrs = g_array_index (self->chars, RowChar, i - 1).attrs;
		if (attrs != g_array_index (self->chars, RowChar, i).attrs)
		{
			row_buffer_print (self, ucs4 + mark, i - mark, attrs);
			mark = i;
		}
	}
	row_buffer_print (self, ucs4 + mark, self->chars->len - mark,
		g_array_index (self->chars, RowChar, self->chars->len - 1).attrs);
}

/// Align the buffer to @a width (if not lesser than zero) using @a attrs
static void
row_buffer_finish (RowBuffer *self, int width, chtype attrs)
{
	if (width >= 0 && self->total_width > width)
		row_buffer_ellipsis (self, width, attrs);
	while (self->total_width < width)
	{
		struct row_char rc = { ' ', attrs, 1 };
		g_array_append_val (self->chars, rc);
		self->total_width += rc.width;
	}

	row_buffer_flush (self);
	row_buffer_free (self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/// Render the top bar.
static void
app_redraw_top (Application *self)
{
	RowBuffer buf;
	row_buffer_init (&buf, self);
	row_buffer_append (&buf, APP_TITLE, APP_ATTR (HEADER) | A_BOLD);

	for (guint i = 0; i < self->dictionaries->len; i++)
	{
		Dictionary *dict = &g_array_index (self->dictionaries, Dictionary, i);
		row_buffer_append (&buf, dict->name,
			(self->dictionaries->len > 1 && self->dict == dict->dict)
				? APP_ATTR (ACTIVE) : APP_ATTR (HEADER));
	}
	move (0, 0);
	row_buffer_finish (&buf, COLS, APP_ATTR (HEADER));

	row_buffer_init (&buf, self);
	row_buffer_append (&buf, self->search_label, APP_ATTR (SEARCH));
	gsize indent = buf.total_width;

	int word_attrs = APP_ATTR (SEARCH);
	if (self->input_confirmed)
		word_attrs |= A_BOLD;

	gchar *input_utf8 = g_ucs4_to_utf8
		((gunichar *) self->input->data, -1, NULL, NULL, NULL);
	g_return_if_fail (input_utf8 != NULL);
	row_buffer_append (&buf, input_utf8, word_attrs);
	g_free (input_utf8);

	row_buffer_finish (&buf, COLS, APP_ATTR (SEARCH));

	guint offset, i;
	for (offset = i = 0; i < self->input_pos; i++)
		offset += app_char_width (self,
			g_array_index (self->input, gunichar, i));

	move (1, MIN ((gint) (indent + offset), COLS - 1));
	refresh ();
}

/// Computes width for the left column.
static guint
app_get_left_column_width (Application *self)
{
	gint width = COLS * self->division + 0.5;
	if (width < 1)
		width = 1;
	else if (width > COLS - 2)
		width = COLS - 2;
	return width;
}

/// Display a message in the view area.
static void
app_show_message (Application *self, const gchar *lines[], gsize len)
{
	gint top = (LINES - TOP_BAR_CUTOFF - len) / 2;

	gint i;
	for (i = 0; i < top; i++)
	{
		move (TOP_BAR_CUTOFF + i, 0);
		clrtoeol ();
	}

	while (len-- && i < LINES - TOP_BAR_CUTOFF)
	{
		move (TOP_BAR_CUTOFF + i, 0);
		clrtoeol ();

		gint x = (COLS - g_utf8_strlen (*lines, -1)) / 2;
		if (x < 0)
			x = 0;

		RowBuffer buf;
		row_buffer_init (&buf, self);
		row_buffer_append (&buf, *lines, 0);
		move (TOP_BAR_CUTOFF + i, x);
		row_buffer_finish (&buf, -1, 0);

		lines++;
		i++;
	}

	clrtobot ();
	refresh ();
}

/// Show some information about the program.
static void
app_show_help (Application *self)
{
	const gchar *lines[] =
	{
		PROJECT_NAME " " PROJECT_VERSION,
		_("Terminal UI for StarDict dictionaries"),
		"Copyright (c) 2013 - 2021, Přemysl Eric Janouch",
		"",
		_("Type to search")
	};

	app_show_message (self, lines, G_N_ELEMENTS (lines));
}

/// Combine attributes, taking care to replace colour bits entirely
static void
app_merge_attributes (int *target, int merged)
{
	if (merged & A_COLOR)
		*target = (*target & ~A_COLOR) | merged;
	else
		*target |= merged;
}

/// Redraw the dictionary view.
static void
app_redraw_view (Application *self)
{
	if (self->show_help)
	{
		app_show_help (self);
		return;
	}

	move (TOP_BAR_CUTOFF, 0);
	clrtobot ();

	// TODO: clean this stuff up a bit, it's all rather ugly
	gchar *input_utf8 = g_ucs4_to_utf8
		((gunichar *) self->input->data, -1, NULL, NULL, NULL);
	gint left_width = app_get_left_column_width (self);

	guint i, k = self->top_offset, shown = 0;
	for (i = 0; i < self->entries->len; i++)
	{
		ViewEntry *ve = g_ptr_array_index (self->entries, i);
		for (; k < ve->definitions_length; k++)
		{
			int attrs = ((self->top_position + i) & 1)
				? APP_ATTR (ODD) : APP_ATTR (EVEN);

			if (shown == self->selected)
				app_merge_attributes (&attrs, self->focused
					? APP_ATTR (SELECTION) : APP_ATTR (DEFOCUSED));

			gboolean last = k + 1 == ve->definitions_length;
			if (last && self->underline_last)
				attrs |= A_UNDERLINE;

			RowBuffer buf;
			row_buffer_init (&buf, self);

			size_t common = 0;
			if (self->hl_prefix)
			{
				common = stardict_longest_common_collation_prefix
					(self->dict, ve->word, input_utf8);

				gchar *prefix = g_strndup (ve->word, common);
				row_buffer_append (&buf, prefix, attrs | A_BOLD);
				g_free (prefix);
			}
			row_buffer_append (&buf, ve->word + common, attrs);
			row_buffer_finish (&buf, left_width, attrs);

			row_buffer_init (&buf, self);
			row_buffer_append (&buf, " ", attrs);
			row_buffer_append (&buf, ve->definitions[k], attrs);
			row_buffer_finish (&buf, COLS - left_width, attrs);

			if ((gint) ++shown == LINES - TOP_BAR_CUTOFF)
				goto done;
		}

		k = 0;
	}

done:
	free (input_utf8);
	refresh ();
}

static ViewEntry *
entry_for_position (Application *self, guint32 position)
{
	StardictIterator *iterator = stardict_iterator_new (self->dict, position);
	ViewEntry *ve = NULL;
	if (stardict_iterator_is_valid (iterator))
		ve = view_entry_new (iterator);
	g_object_unref (iterator);
	return ve;
}

/// Just prepends a new view entry into the entries array.
static ViewEntry *
prepend_entry (Application *self, guint32 position)
{
	ViewEntry *ve = entry_for_position (self, position);
	g_ptr_array_add (self->entries, NULL);
	memmove (self->entries->pdata + 1, self->entries->pdata,
		sizeof ve * (self->entries->len - 1));
	return g_ptr_array_index (self->entries, 0) = ve;
}

/// Counts the number of definitions available for seeing.
static guint
app_count_view_items (Application *self)
{
	guint i, n_definitions = 0;
	for (i = 0; i < self->entries->len; i++)
	{
		ViewEntry *entry = g_ptr_array_index (self->entries, i);
		n_definitions += entry->definitions_length;
	}
	return n_definitions;
}

/// Scroll up @a n entries.  Doesn't redraw.
static gint
app_scroll_up (Application *self, guint n)
{
	guint n_definitions = app_count_view_items (self);
	gint scrolled = 0;
	for (; n--; scrolled++)
	{
		if (self->top_offset > 0)
		{
			self->top_offset--;
			continue;
		}

		// We've reached the top
		if (self->top_position == 0)
			break;

		ViewEntry *ve = prepend_entry (self, --self->top_position);
		self->top_offset = ve->definitions_length - 1;
		n_definitions += ve->definitions_length;

		// Remove the last entry if not shown
		ViewEntry *last_entry =
			g_ptr_array_index (self->entries, self->entries->len - 1);
		if ((gint) (n_definitions - self->top_offset
			- last_entry->definitions_length) >= LINES - TOP_BAR_CUTOFF)
		{
			n_definitions -= last_entry->definitions_length;
			g_ptr_array_remove_index_fast
				(self->entries, self->entries->len - 1);
		}
	}
	return scrolled;
}

/// Scroll down @a n entries.  Doesn't redraw.
static gint
app_scroll_down (Application *self, guint n)
{
	guint n_definitions = app_count_view_items (self);
	gint scrolled = 0;
	for (; n--; scrolled++)
	{
		if (self->entries->len == 0)
			break;

		// Simulate the movement first to disallow scrolling past the end
		guint to_be_offset = self->top_offset;
		guint to_be_definitions = n_definitions;

		ViewEntry *first_entry = g_ptr_array_index (self->entries, 0);
		if (++to_be_offset >= first_entry->definitions_length)
		{
			to_be_definitions -= first_entry->definitions_length;
			to_be_offset = 0;
		}
		if ((gint) (to_be_definitions - to_be_offset) < LINES - TOP_BAR_CUTOFF)
		{
			ViewEntry *new_entry = entry_for_position
				(self, self->top_position + self->entries->len);
			if (!new_entry)
				break;

			g_ptr_array_add (self->entries, new_entry);
			to_be_definitions += new_entry->definitions_length;
		}
		if (to_be_offset == 0)
		{
			g_ptr_array_remove_index (self->entries, 0);
			self->top_position++;
		}

		self->top_offset = to_be_offset;
		n_definitions = to_be_definitions;
	}
	return scrolled;
}

/// Moves the selection one entry up.
static gboolean
app_one_entry_up (Application *self)
{
	if (self->selected == 0 && self->top_offset == 0)
	{
		if (self->top_position == 0)
			return FALSE;
		prepend_entry (self, --self->top_position);
	}

	// Find the last entry that starts above the selection
	gint first = -self->top_offset;
	guint i;
	for (i = 0; i < self->entries->len; i++)
	{
		ViewEntry *ve = g_ptr_array_index (self->entries, i);
		gint new_first = first + ve->definitions_length;
		if (new_first >= (gint) self->selected)
			break;
		first = new_first;
	}

	if (first < 0)
	{
		self->selected = 0;
		app_scroll_up (self, -first);
	}
	else
		self->selected = first;

	app_redraw_view (self);
	return TRUE;
}

/// Moves the selection one entry down.
static void
app_one_entry_down (Application *self)
{
	// Find the first entry that starts below the selection
	gint first = -self->top_offset;
	guint i;
	for (i = 0; i < self->entries->len; i++)
	{
		ViewEntry *ve = g_ptr_array_index (self->entries, i);
		first += ve->definitions_length;
		if (first > (gint) self->selected)
			break;
	}

	// FIXME: selection can still get past the end
	if (first >= LINES - TOP_BAR_CUTOFF)
	{
		app_scroll_down (self, first - (LINES - TOP_BAR_CUTOFF - 1));
		self->selected = LINES - TOP_BAR_CUTOFF - 1;
	}
	else
		self->selected = first;

	app_redraw_view (self);
}

/// Redraw everything.
static void
app_redraw (Application *self)
{
	app_redraw_view (self);
	app_redraw_top (self);
}

/// When dictionary contents are longer than the screen, eliminate empty space
static void
app_fill_view (Application *self)
{
	gint missing = LINES - TOP_BAR_CUTOFF
		- (gint) (app_count_view_items (self) - self->top_offset);
	if (missing > 0)
		self->selected += app_scroll_up (self, missing);
}

/// Search for the current entry.
static void
app_search_for_entry (Application *self)
{
	gchar *input_utf8 = g_ucs4_to_utf8
		((gunichar *) self->input->data, -1, NULL, NULL, NULL);
	g_return_if_fail (input_utf8 != NULL);

	StardictIterator *iterator =
		stardict_dict_search (self->dict, input_utf8, NULL);
	g_free (input_utf8);

	self->top_position = stardict_iterator_get_offset (iterator);
	self->top_offset = 0;
	self->selected = 0;
	g_object_unref (iterator);

	self->show_help = FALSE;
	app_reload_view (self);

	// Don't let the iterator get past the end of the dictionary
	if (!self->entries->len)
		(void) app_scroll_up (self, 1);

	// If the user wants it centered, just move the view up half a screen;
	// actually, one third seems to be a better guess
	if (self->center_search)
	{
		gint half = (LINES - TOP_BAR_CUTOFF) / 3;
		if (half > 0)
			self->selected += app_scroll_up (self, half);
	}

	app_fill_view (self);
	app_redraw_view (self);
}

/// Switch to a different dictionary by number.
static gboolean
app_goto_dictionary (Application *self, guint n)
{
	if (n >= self->dictionaries->len)
		return FALSE;

	Dictionary *dict = &g_array_index (self->dictionaries, Dictionary, n);
	self->dict = dict->dict;
	app_search_for_entry (self);
	app_redraw_top (self);
	return TRUE;
}

/// Switch to a different dictionary by delta.
static gboolean
app_goto_dictionary_delta (Application *self, gint n)
{
	GArray *dicts = self->dictionaries;
	if (dicts->len <= 1)
		return FALSE;

	guint i = 0;
	while (i < dicts->len &&
		g_array_index (dicts, Dictionary, i).dict != self->dict)
		i++;

	return app_goto_dictionary (self, (i + dicts->len + n) % dicts->len);
}

/// The terminal has been resized, make appropriate changes.
static gboolean
app_process_resize (Application *self)
{
	app_reload_view (self);
	app_fill_view (self);

	guint n_visible = app_count_view_items (self) - self->top_offset;
	if ((gint) n_visible > LINES - TOP_BAR_CUTOFF)
		n_visible = LINES - TOP_BAR_CUTOFF;
	if (self->selected >= n_visible)
	{
		app_scroll_down (self, self->selected - n_visible + 1);
		self->selected = n_visible - 1;
	}

	app_redraw (self);
	return TRUE;
}

// --- User input handling -----------------------------------------------------

/// All the actions that can be performed by the user.
typedef enum user_action                UserAction;

enum user_action
{
	USER_ACTION_NONE,

	USER_ACTION_QUIT,
	USER_ACTION_REDRAW,

	USER_ACTION_MOVE_SPLITTER_LEFT,
	USER_ACTION_MOVE_SPLITTER_RIGHT,

	USER_ACTION_GOTO_ENTRY_PREVIOUS,
	USER_ACTION_GOTO_ENTRY_NEXT,
	USER_ACTION_GOTO_DEFINITION_PREVIOUS,
	USER_ACTION_GOTO_DEFINITION_NEXT,
	USER_ACTION_GOTO_PAGE_PREVIOUS,
	USER_ACTION_GOTO_PAGE_NEXT,
	USER_ACTION_GOTO_DICTIONARY_PREVIOUS,
	USER_ACTION_GOTO_DICTIONARY_NEXT,

	USER_ACTION_INPUT_CONFIRM,
	USER_ACTION_INPUT_HOME,
	USER_ACTION_INPUT_END,
	USER_ACTION_INPUT_LEFT,
	USER_ACTION_INPUT_RIGHT,
	USER_ACTION_INPUT_DELETE_PREVIOUS,
	USER_ACTION_INPUT_DELETE_NEXT,
	USER_ACTION_INPUT_DELETE_TO_HOME,
	USER_ACTION_INPUT_DELETE_TO_END,
	USER_ACTION_INPUT_DELETE_PREVIOUS_WORD,
	USER_ACTION_INPUT_TRANSPOSE,

	USER_ACTION_COUNT
};

#define SAVE_CURSOR                 \
	int last_x, last_y;             \
	getyx (stdscr, last_y, last_x);

#define RESTORE_CURSOR              \
	move (last_y, last_x);          \
	refresh ();

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gboolean
app_process_user_action (Application *self, UserAction action)
{
	SAVE_CURSOR
	switch (action)
	{
	case USER_ACTION_QUIT:
		return FALSE;
	case USER_ACTION_REDRAW:
		clear ();
		app_redraw (self);
		return TRUE;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_MOVE_SPLITTER_LEFT:
		self->division = (app_get_left_column_width (self) - 1.) / COLS;
		app_redraw_view (self);
		RESTORE_CURSOR
		return TRUE;
	case USER_ACTION_MOVE_SPLITTER_RIGHT:
		self->division = (app_get_left_column_width (self) + 1.) / COLS;
		app_redraw_view (self);
		RESTORE_CURSOR
		return TRUE;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_GOTO_DEFINITION_PREVIOUS:
		if (self->selected > 0)
			self->selected--;
		else
			app_scroll_up (self, 1);
		app_redraw_view (self);
		RESTORE_CURSOR
		return TRUE;
	case USER_ACTION_GOTO_DEFINITION_NEXT:
		if ((gint) self->selected < LINES - TOP_BAR_CUTOFF - 1 &&
			self->selected < app_count_view_items (self) - self->top_offset - 1)
			self->selected++;
		else
			app_scroll_down (self, 1);
		app_redraw_view (self);
		RESTORE_CURSOR
		return TRUE;

	case USER_ACTION_GOTO_ENTRY_PREVIOUS:
		app_one_entry_up (self);
		RESTORE_CURSOR
		return TRUE;
	case USER_ACTION_GOTO_ENTRY_NEXT:
		app_one_entry_down (self);
		RESTORE_CURSOR
		return TRUE;

	case USER_ACTION_GOTO_PAGE_PREVIOUS:
		app_scroll_up (self, LINES - TOP_BAR_CUTOFF);
		app_redraw_view (self);
		RESTORE_CURSOR
		return TRUE;
	case USER_ACTION_GOTO_PAGE_NEXT:
		app_scroll_down (self, LINES - TOP_BAR_CUTOFF);
		app_redraw_view (self);
		RESTORE_CURSOR
		return TRUE;

	case USER_ACTION_GOTO_DICTIONARY_PREVIOUS:
		if (!app_goto_dictionary_delta (self, -1))
			beep ();
		return TRUE;
	case USER_ACTION_GOTO_DICTIONARY_NEXT:
		if (!app_goto_dictionary_delta (self, +1))
			beep ();
		return TRUE;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_INPUT_HOME:
		self->input_pos = 0;
		app_redraw_top (self);
		return TRUE;
	case USER_ACTION_INPUT_END:
		self->input_pos = self->input->len;
		app_redraw_top (self);
		return TRUE;
	case USER_ACTION_INPUT_LEFT:
		if (self->input_pos > 0)
		{
			do self->input_pos--;
			while (self->input_pos > 0 && g_unichar_ismark
				(g_array_index (self->input, gunichar, self->input_pos)));
			app_redraw_top (self);
		}
		return TRUE;
	case USER_ACTION_INPUT_RIGHT:
		if (self->input_pos < self->input->len)
		{
			do self->input_pos++;
			while (self->input_pos < self->input->len && g_unichar_ismark
				(g_array_index (self->input, gunichar, self->input_pos)));
			app_redraw_top (self);
		}
		return TRUE;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_INPUT_CONFIRM:
		self->input_confirmed = TRUE;
		app_redraw_top (self);
		return TRUE;

	case USER_ACTION_INPUT_TRANSPOSE:
	{
		if (!self->input_pos || self->input->len < 2)
			break;

		guint start = self->input_pos - 1;
		if (self->input_pos >= self->input->len)
			start--;

		gunichar tmp = g_array_index (self->input, gunichar, start);
		g_array_index (self->input, gunichar, start)
			= g_array_index (self->input, gunichar, start + 1);
		g_array_index (self->input, gunichar, start + 1) = tmp;

		if (self->input_pos < self->input->len)
			self->input_pos++;

		app_search_for_entry (self);
		app_redraw_top (self);
		return TRUE;
	}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_INPUT_DELETE_PREVIOUS:
		if (self->input_pos > 0)
		{
			g_array_remove_index (self->input, --self->input_pos);
			app_search_for_entry (self);
			app_redraw_top (self);
		}
		return TRUE;
	case USER_ACTION_INPUT_DELETE_NEXT:
		if (self->input_pos < self->input->len)
		{
			g_array_remove_index (self->input, self->input_pos);
			app_search_for_entry (self);
			app_redraw_top (self);
		}
		return TRUE;
	case USER_ACTION_INPUT_DELETE_TO_HOME:
		if (self->input->len != 0)
		{
			g_array_remove_range (self->input, 0, self->input_pos);
			self->input_pos = 0;

			app_search_for_entry (self);
			app_redraw_top (self);
		}
		return TRUE;
	case USER_ACTION_INPUT_DELETE_TO_END:
		if (self->input_pos < self->input->len)
		{
			g_array_remove_range (self->input,
				self->input_pos, self->input->len - self->input_pos);
			app_search_for_entry (self);
			app_redraw_top (self);
		}
		return TRUE;
	case USER_ACTION_INPUT_DELETE_PREVIOUS_WORD:
	{
		if (self->input_pos == 0)
			return TRUE;

		gint i = self->input_pos;
		while (i)
			if (g_array_index (self->input, gunichar, --i) != L' ')
				break;
		while (i--)
			if (g_array_index (self->input, gunichar,   i) == L' ')
				break;

		i++;
		g_array_remove_range (self->input, i, self->input_pos - i);
		self->input_pos = i;

		app_search_for_entry (self);
		app_redraw_top (self);
		return TRUE;
	}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	case USER_ACTION_NONE:
		return TRUE;
	default:
		g_assert_not_reached ();
	}
	return TRUE;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static gboolean
app_process_keysym (Application *self, termo_key_t *event)
{
	UserAction action = USER_ACTION_NONE;
	typedef const UserAction ActionMap[TERMO_N_SYMS];

	static ActionMap actions =
	{
		[TERMO_SYM_ESCAPE]    = USER_ACTION_QUIT,

		[TERMO_SYM_UP]        = USER_ACTION_GOTO_DEFINITION_PREVIOUS,
		[TERMO_SYM_DOWN]      = USER_ACTION_GOTO_DEFINITION_NEXT,
		[TERMO_SYM_PAGEUP]    = USER_ACTION_GOTO_PAGE_PREVIOUS,
		[TERMO_SYM_PAGEDOWN]  = USER_ACTION_GOTO_PAGE_NEXT,

		[TERMO_SYM_ENTER]     = USER_ACTION_INPUT_CONFIRM,

		[TERMO_SYM_HOME]      = USER_ACTION_INPUT_HOME,
		[TERMO_SYM_END]       = USER_ACTION_INPUT_END,
		[TERMO_SYM_LEFT]      = USER_ACTION_INPUT_LEFT,
		[TERMO_SYM_RIGHT]     = USER_ACTION_INPUT_RIGHT,

		[TERMO_SYM_BACKSPACE] = USER_ACTION_INPUT_DELETE_PREVIOUS,
		[TERMO_SYM_DEL]       = USER_ACTION_INPUT_DELETE_PREVIOUS,
		[TERMO_SYM_DELETE]    = USER_ACTION_INPUT_DELETE_NEXT,
	};
	static ActionMap actions_alt =
	{
		[TERMO_SYM_LEFT]      = USER_ACTION_MOVE_SPLITTER_LEFT,
		[TERMO_SYM_RIGHT]     = USER_ACTION_MOVE_SPLITTER_RIGHT,
	};
	static ActionMap actions_ctrl =
	{
		[TERMO_SYM_UP]        = USER_ACTION_GOTO_ENTRY_PREVIOUS,
		[TERMO_SYM_DOWN]      = USER_ACTION_GOTO_ENTRY_NEXT,
		[TERMO_SYM_LEFT]      = USER_ACTION_GOTO_DICTIONARY_PREVIOUS,
		[TERMO_SYM_RIGHT]     = USER_ACTION_GOTO_DICTIONARY_NEXT,
		[TERMO_SYM_PAGEUP]    = USER_ACTION_GOTO_DICTIONARY_PREVIOUS,
		[TERMO_SYM_PAGEDOWN]  = USER_ACTION_GOTO_DICTIONARY_NEXT,
	};

	if (!event->modifiers)
		action = actions[event->code.sym];
	else if (event->modifiers == TERMO_KEYMOD_ALT)
		action = actions_alt[event->code.sym];
	else if (event->modifiers == TERMO_KEYMOD_CTRL)
		action = actions_ctrl[event->code.sym];

	return app_process_user_action (self, action);
}

static gboolean
app_process_ctrl_key (Application *self, termo_key_t *event)
{
	static const UserAction actions[32] =
	{
		[CTRL_KEY ('L')]      = USER_ACTION_REDRAW,

		[CTRL_KEY ('P')]      = USER_ACTION_GOTO_DEFINITION_PREVIOUS,
		[CTRL_KEY ('N')]      = USER_ACTION_GOTO_DEFINITION_NEXT,
		[CTRL_KEY ('B')]      = USER_ACTION_GOTO_PAGE_PREVIOUS,
		[CTRL_KEY ('F')]      = USER_ACTION_GOTO_PAGE_NEXT,

		[CTRL_KEY ('A')]      = USER_ACTION_INPUT_HOME,
		[CTRL_KEY ('E')]      = USER_ACTION_INPUT_END,

		[CTRL_KEY ('H')]      = USER_ACTION_INPUT_DELETE_PREVIOUS,
		[CTRL_KEY ('K')]      = USER_ACTION_INPUT_DELETE_TO_END,
		[CTRL_KEY ('W')]      = USER_ACTION_INPUT_DELETE_PREVIOUS_WORD,
		[CTRL_KEY ('U')]      = USER_ACTION_INPUT_DELETE_TO_HOME,
		[CTRL_KEY ('T')]      = USER_ACTION_INPUT_TRANSPOSE,
	};

	gint64 i = (gint64) event->code.codepoint - 'a' + 1;
	if (i > 0 && i < (gint64) G_N_ELEMENTS (actions))
		return app_process_user_action (self, actions[i]);

	return TRUE;
}

static gboolean
app_process_alt_key (Application *self, termo_key_t *event)
{
	if (event->code.codepoint == 'c')
		self->center_search = !self->center_search;

	if (event->code.codepoint >= '0'
	 && event->code.codepoint <= '9')
	{
		gint n = event->code.codepoint - '0';
		if (!app_goto_dictionary (self, (n == 0 ? 10 : n) - 1))
			beep ();
	}
	return TRUE;
}

static gboolean
app_process_key (Application *self, termo_key_t *event)
{
	if (event->modifiers == TERMO_KEYMOD_CTRL)
		return app_process_ctrl_key (self, event);
	if (event->modifiers == TERMO_KEYMOD_ALT)
		return app_process_alt_key (self, event);
	if (event->modifiers)
		return TRUE;

	gunichar c = event->code.codepoint;
	if (!g_unichar_isprint (c))
	{
		beep ();
		return TRUE;
	}

	if (self->input_confirmed)
	{
		if (self->input->len != 0)
			g_array_remove_range (self->input, 0, self->input->len);
		self->input_pos = 0;
		self->input_confirmed = FALSE;
	}

	g_array_insert_val (self->input, self->input_pos++, c);
	app_search_for_entry (self);
	app_redraw_top (self);
	return TRUE;
}

static void
app_process_left_mouse_click (Application *self, int line, int column)
{
	SAVE_CURSOR
	if (line == 0)
	{
		int indent = strlen (APP_TITLE);
		if (column < indent)
			return;

		Dictionary *dicts = (Dictionary *) self->dictionaries->data;
		for (guint i = 0; i < self->dictionaries->len; i++)
		{
			if (column < (indent += dicts[i].name_width))
			{
				app_goto_dictionary (self, i);
				return;
			}
		}
	}
	else if (line == 1)
	{
		// FIXME: this is only an approximation
		gsize label_len = g_utf8_strlen (self->search_label, -1);
		gint pos = column - label_len;
		if (pos >= 0)
		{
			guint offset, i;
			for (offset = i = 0; i < self->input->len; i++)
			{
				size_t width = app_char_width
					(self, g_array_index (self->input, gunichar, i));
				if ((offset += width) > (guint) pos)
					break;
			}

			self->input_pos = i;
			app_redraw_top (self);
		}
	}
	else if (line <= (int) (app_count_view_items (self) - self->top_offset))
	{
		self->selected = line - TOP_BAR_CUTOFF;
		app_redraw_view (self);
		RESTORE_CURSOR
	}
}

/// Process mouse input.
static gboolean
app_process_mouse (Application *self, termo_key_t *event)
{
	int line, column, button;
	termo_mouse_event_t type;
	termo_interpret_mouse (self->tk, event, &type, &button, &line, &column);

	if (type != TERMO_MOUSE_PRESS)
		return TRUE;

	if (button == 1)
		app_process_left_mouse_click (self, line, column);
	else if (button == 4)
		app_process_user_action (self, USER_ACTION_GOTO_DEFINITION_PREVIOUS);
	else if (button == 5)
		app_process_user_action (self, USER_ACTION_GOTO_DEFINITION_NEXT);

	return TRUE;
}

/// Process input events from the terminal.
static gboolean
app_process_termo_event (Application *self, termo_key_t *event)
{
	switch (event->type)
	{
	case TERMO_TYPE_MOUSE:
		return app_process_mouse (self, event);
	case TERMO_TYPE_KEY:
		return app_process_key (self, event);
	case TERMO_TYPE_KEYSYM:
		return app_process_keysym (self, event);
	case TERMO_TYPE_FOCUS:
	{
		SAVE_CURSOR
		self->focused = !!event->code.focused;
		app_redraw_view (self);
		RESTORE_CURSOR
		return TRUE;
	}
	default:
		return TRUE;
	}
}

// --- SIGWINCH ----------------------------------------------------------------

static int g_winch_pipe[2];            ///< SIGWINCH signalling pipe.

static void
winch_handler (int signum)
{
	(void) signum;
	write (g_winch_pipe[1], "x", 1);
}

static void
install_winch_handler (void)
{
	struct sigaction act, oldact;

	if (pipe (g_winch_pipe) == -1)
		abort ();

	act.sa_handler = winch_handler;
	act.sa_flags = SA_RESTART;
	sigemptyset (&act.sa_mask);
	sigaction (SIGWINCH, &act, &oldact);
}

// --- X11 selection watcher ---------------------------------------------------

#ifdef WITH_X11

static void
app_set_input (Application *self, const gchar *text, gsize text_len)
{
	glong size;
	gunichar *output = g_utf8_to_ucs4 (text, text_len, NULL, &size, NULL);

	// XXX: signal invalid data?
	if (!output)
		return;

	g_array_free (self->input, TRUE);
	self->input = g_array_new (TRUE, FALSE, sizeof (gunichar));
	self->input_pos = 0;

	gunichar *p = output;
	gboolean last_was_space = false;
	while (size--)
	{
		// Normalize whitespace, to cover OCR anomalies
		gunichar c = *p++;
		if (!g_unichar_isspace (c))
			last_was_space = FALSE;
		else if (last_was_space)
			continue;
		else
		{
			c = ' ';
			last_was_space = TRUE;
		}

		// XXX: skip?  Might be some binary nonsense.
		if (!g_unichar_isprint (c))
			break;

		g_array_insert_val (self->input, self->input_pos++, c);
	}
	g_free (output);

	self->input_confirmed = FALSE;
	app_search_for_entry (self);
	app_redraw_top (self);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#include <xcb/xcb.h>
#include <xcb/xfixes.h>

/// Data relating to one entry within the dictionary.
typedef struct selection_watch          SelectionWatch;

struct selection_watch
{
	Application *app;
	xcb_connection_t *X;
	const xcb_query_extension_reply_t *xfixes;

	guint           watch;              ///< X11 connection watcher
	xcb_window_t    wid;                ///< Withdrawn communications window
	xcb_atom_t      atom_incr;          ///< INCR
	xcb_atom_t      atom_utf8_string;   ///< UTF8_STRING
	xcb_timestamp_t in_progress;        ///< Timestamp of last processed event
	GString       * buffer;             ///< UTF-8 text buffer

	gboolean        incr;               ///< INCR running
	gboolean        incr_failure;       ///< INCR failure indicator
};

static gboolean
is_xcb_ok (xcb_connection_t *X)
{
	int xcb_error = xcb_connection_has_error (X);
	if (xcb_error)
	{
		g_warning (_("X11 connection failed (error code %d)"), xcb_error);
		return FALSE;
	}
	return TRUE;
}

static xcb_atom_t
resolve_atom (xcb_connection_t *X, const char *atom)
{
	xcb_intern_atom_reply_t *iar = xcb_intern_atom_reply (X,
		xcb_intern_atom (X, false, strlen (atom), atom), NULL);
	xcb_atom_t result = iar ? iar->atom : XCB_NONE;
	free (iar);
	return result;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void
on_selection_text_received (SelectionWatch *self, const gchar *text)
{
	// Strip ASCII whitespace: this is compatible with UTF-8
	while (g_ascii_isspace (*text))
		text++;
	gsize text_len = strlen (text);
	while (text_len && g_ascii_isspace (text[text_len - 1]))
		text_len--;

	if (text_len)
		app_set_input (self->app, text, text_len);
}

static gboolean
read_utf8_property (SelectionWatch *self, xcb_window_t wid, xcb_atom_t property,
	gboolean *empty)
{
	guint32 offset = 0;
	gboolean more_data = TRUE, ok = TRUE;
	xcb_get_property_reply_t *gpr;
	while (ok && more_data)
	{
		if (!(gpr = xcb_get_property_reply (self->X,
			xcb_get_property (self->X, FALSE /* delete */, wid,
			property, XCB_GET_PROPERTY_TYPE_ANY, offset, 0x8000), NULL)))
			return FALSE;

		int len = xcb_get_property_value_length (gpr);
		if (offset == 0 && len == 0 && empty)
			*empty = TRUE;

		ok = gpr->type == self->atom_utf8_string && gpr->format == 8;
		more_data = gpr->bytes_after != 0;
		if (ok)
		{
			offset += len >> 2;
			g_string_append_len (self->buffer,
				xcb_get_property_value (gpr), len);
		}
		free (gpr);
	}
	return ok;
}

static void
on_x11_selection_change (SelectionWatch *self,
	xcb_xfixes_selection_notify_event_t *e)
{
	// Not checking whether we should give up when this interrupts our
	// current retrieval attempt--the timeout mostly solves this for all cases
	if (e->owner == XCB_NONE)
		return;

	// Don't try to process two things at once.  Each request gets a few seconds
	// to finish, then we move on, hoping that a property race doesn't commence.
	// Ideally we'd set up a separate queue for these skipped requests and
	// process them later.
	if (self->in_progress != 0 && e->timestamp - self->in_progress < 5000)
		return;

	// ICCCM says we should ensure the named property doesn't exist
	(void) xcb_delete_property (self->X, self->wid, XCB_ATOM_PRIMARY);

	(void) xcb_convert_selection (self->X, self->wid, e->selection,
		self->atom_utf8_string, XCB_ATOM_PRIMARY, e->timestamp);

	self->in_progress = e->timestamp;
	self->incr = FALSE;
}

static void
on_x11_selection_receive (SelectionWatch *self,
	xcb_selection_notify_event_t *e)
{
	if (e->requestor != self->wid
	 || e->time != self->in_progress)
		return;

	self->in_progress = 0;
	if (e->property == XCB_ATOM_NONE)
		return;

	xcb_get_property_reply_t *gpr = xcb_get_property_reply (self->X,
		xcb_get_property (self->X, FALSE /* delete */, e->requestor,
		e->property, XCB_GET_PROPERTY_TYPE_ANY, 0, 0), NULL);
	if (!gpr)
		return;

	// Garbage collection, GString only ever expands in size
	g_string_free (self->buffer, TRUE);
	self->buffer = g_string_new (NULL);

	// When you select a lot of text in VIM, it starts the ICCCM INCR mechanism,
	// from which there is no opt-out
	if (gpr->type == self->atom_incr)
	{
		self->in_progress = e->time;
		self->incr = TRUE;
		self->incr_failure = FALSE;
	}
	else if (read_utf8_property (self, e->requestor, e->property, NULL))
		on_selection_text_received (self, self->buffer->str);

	free (gpr);
	(void) xcb_delete_property (self->X, self->wid, e->property);
}

static void
on_x11_property_notify (SelectionWatch *self, xcb_property_notify_event_t *e)
{
	if (!self->incr
	 || e->window != self->wid
	 || e->state != XCB_PROPERTY_NEW_VALUE
	 || e->atom != XCB_ATOM_PRIMARY)
		return;

	gboolean empty = FALSE;
	if (!read_utf8_property (self, e->window, e->atom, &empty))
		// We need to keep deleting the property
		self->incr_failure = TRUE;

	// Once it's empty, we've consumed everything and can move on undisturbed
	if (empty)
	{
		if (!self->incr_failure)
			on_selection_text_received (self, self->buffer->str);

		self->in_progress = 0;
		self->incr = FALSE;
	}

	(void) xcb_delete_property (self->X, e->window, e->atom);
}

static void
process_x11_event (SelectionWatch *self, xcb_generic_event_t *event)
{
	int event_code = event->response_type & 0x7f;
	if (event_code == 0)
	{
		xcb_generic_error_t *err = (xcb_generic_error_t *) event;
		g_warning (_("X11 request error (%d, major %d, minor %d)"),
			err->error_code, err->major_code, err->minor_code);
	}
	else if (event_code ==
		self->xfixes->first_event + XCB_XFIXES_SELECTION_NOTIFY)
		on_x11_selection_change (self,
			(xcb_xfixes_selection_notify_event_t *) event);
	else if (event_code == XCB_SELECTION_NOTIFY)
		on_x11_selection_receive (self,
			(xcb_selection_notify_event_t *) event);
	else if (event_code == XCB_PROPERTY_NOTIFY)
		on_x11_property_notify (self,
			(xcb_property_notify_event_t *) event);
}

static gboolean
process_x11 (G_GNUC_UNUSED GIOChannel *source,
	G_GNUC_UNUSED GIOCondition condition, gpointer data)
{
	SelectionWatch *self = data;

	xcb_generic_event_t *event;
	while ((event = xcb_poll_for_event (self->X)))
	{
		process_x11_event (self, event);
		free (event);
	}
	(void) xcb_flush (self->X);
	return is_xcb_ok (self->X);
}

static void
selection_watch_init (SelectionWatch *self, Application *app)
{
	memset (self, 0, sizeof *self);
	if (!app->watch_x11_sel)
		return;
	self->app = app;

	int which_screen = -1;
	self->X = xcb_connect (NULL, &which_screen);
	if (!is_xcb_ok (self->X))
		return;

	// Most modern applications support this, though an XCB_ATOM_STRING
	// fallback might be good to add (COMPOUND_TEXT is complex)
	g_return_if_fail
		((self->atom_utf8_string = resolve_atom (self->X, "UTF8_STRING")));
	g_return_if_fail
		((self->atom_incr = resolve_atom (self->X, "INCR")));

	self->xfixes = xcb_get_extension_data (self->X, &xcb_xfixes_id);
	g_return_if_fail (self->xfixes->present);

	(void) xcb_xfixes_query_version_unchecked (self->X,
		XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);

	const xcb_setup_t *setup = xcb_get_setup (self->X);
	xcb_screen_iterator_t setup_iter = xcb_setup_roots_iterator (setup);
	while (which_screen--)
		xcb_screen_next (&setup_iter);

	xcb_screen_t *screen = setup_iter.data;
	self->wid = xcb_generate_id (self->X);
	const uint32_t values[] = {XCB_EVENT_MASK_PROPERTY_CHANGE};
	(void) xcb_create_window (self->X, screen->root_depth, self->wid,
		screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		screen->root_visual, XCB_CW_EVENT_MASK, values);

	(void) xcb_xfixes_select_selection_input (self->X, self->wid,
		XCB_ATOM_PRIMARY, XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);

	(void) xcb_flush (self->X);
	self->watch = add_read_watch
		(xcb_get_file_descriptor (self->X), process_x11, self);

	// Never NULL so that we don't need to care about pointer validity
	self->buffer = g_string_new (NULL);
}

static void
selection_watch_destroy (SelectionWatch *self)
{
	if (self->X)
		xcb_disconnect (self->X);
	if (self->watch)
		g_source_remove (self->watch);
	if (self->buffer)
		g_string_free (self->buffer, TRUE);
}

#endif  // WITH_X11

// --- Initialisation, event handling ------------------------------------------

static gboolean on_stdin_input_timeout (gpointer data);

static gboolean
process_stdin_input (G_GNUC_UNUSED GIOChannel *source,
	G_GNUC_UNUSED GIOCondition condition, gpointer data)
{
	Application *app = data;
	if (app->tk_timer)
	{
		g_source_remove (app->tk_timer);
		app->tk_timer = 0;
	}

	termo_advisereadable (app->tk);

	termo_key_t event;
	termo_result_t res;
	while ((res = termo_getkey (app->tk, &event)) == TERMO_RES_KEY)
		if (!app_process_termo_event (app, &event))
			goto quit;

	if (res == TERMO_RES_AGAIN)
		app->tk_timer = g_timeout_add (termo_get_waittime (app->tk),
			on_stdin_input_timeout, app);
	else if (res == TERMO_RES_ERROR || res == TERMO_RES_EOF)
		goto quit;

	return TRUE;

quit:
	app_quit (app);
	return FALSE;
}

static gboolean
on_stdin_input_timeout (gpointer data)
{
	Application *app = data;
	termo_key_t event;
	if (termo_getkey_force (app->tk, &event) == TERMO_RES_KEY)
		if (!app_process_termo_event (app, &event))
			app_quit (app);

	app->tk_timer = 0;
	return FALSE;
}

static gboolean
process_winch_input (GIOChannel *source,
	G_GNUC_UNUSED GIOCondition condition, gpointer data)
 {
	char c;
	(void) read (g_io_channel_unix_get_fd (source), &c, 1);

	update_curses_terminal_size ();
	app_process_resize (data);
	return TRUE;
}

static gboolean
on_terminated (gpointer user_data)
{
	app_quit (user_data);
	return TRUE;
}

static void
log_handler_curses (Application *self, const gchar *message)
{
	// Beep, beep, I'm a jeep; let the user know
	beep ();

	// We certainly don't want to end up in a possibly infinite recursion
	static gboolean in_processing;
	if (in_processing)
		return;

	in_processing = TRUE;
	SAVE_CURSOR

	RowBuffer buf;
	row_buffer_init (&buf, self);
	row_buffer_append (&buf, message, A_REVERSE);
	move (0, 0);
	row_buffer_finish (&buf, COLS, A_REVERSE);

	RESTORE_CURSOR
	in_processing = FALSE;
}

static void
log_handler (const gchar *domain, GLogLevelFlags level,
	const gchar *message, gpointer data)
{
	// There's probably no point in trying to display a fatal message nicely
	if (level & G_LOG_FLAG_FATAL)
		g_log_default_handler (domain, level, message, NULL);

	const gchar *prefix;
	switch (level & G_LOG_LEVEL_MASK)
	{
	case G_LOG_LEVEL_ERROR:    prefix = "E"; break;
	case G_LOG_LEVEL_CRITICAL: prefix = "C"; break;
	case G_LOG_LEVEL_WARNING:  prefix = "W"; break;
	case G_LOG_LEVEL_MESSAGE:  prefix = "M"; break;
	case G_LOG_LEVEL_INFO:     prefix = "I"; break;
	case G_LOG_LEVEL_DEBUG:    prefix = "D"; break;
	default:                   prefix = "?";
	}

	gchar *out;
	if (domain)
		out = g_strdup_printf ("%s: %s: %s", prefix, domain, message);
	else
		out = g_strdup_printf ("%s: %s", prefix, message);

	// If the standard error output isn't redirected, try our best at showing
	// the message to the user; it will probably get overdrawn soon
	if (isatty (STDERR_FILENO))
		log_handler_curses (data, out);
	else
		fprintf (stderr, "%s\n", out);

	g_free (out);
}

int
main (int argc, char *argv[])
{
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	if (glib_check_version (2, 36, 0))
		g_type_init ();
G_GNUC_END_IGNORE_DEPRECATIONS

	gboolean show_version = FALSE;
	GOptionEntry entries[] =
	{
		{ "version", 0, G_OPTION_FLAG_IN_MAIN,
		  G_OPTION_ARG_NONE, &show_version,
		  N_("Output version information and exit"), NULL },
		{ NULL }
	};

	if (!setlocale (LC_ALL, ""))
		g_printerr ("%s: %s\n", _("Warning"), _("failed to set the locale"));

	bindtextdomain (GETTEXT_PACKAGE, GETTEXT_DIRNAME);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	GError *error = NULL;
	GOptionContext *ctx = g_option_context_new
		(N_("[dictionary.ifo...] - StarDict terminal UI"));
	GOptionGroup *group = g_option_group_new ("", "", "", NULL, NULL);
	g_option_group_add_entries (group, entries);
	g_option_group_set_translation_domain (group, GETTEXT_PACKAGE);
	g_option_context_add_group (ctx, group);
	g_option_context_set_translation_domain (ctx, GETTEXT_PACKAGE);
	if (!g_option_context_parse (ctx, &argc, &argv, &error))
	{
		g_printerr ("%s: %s: %s\n", _("Error"), _("option parsing failed"),
			error->message);
		exit (EXIT_FAILURE);
	}
	g_option_context_free (ctx);

	if (show_version)
	{
		g_print (PROJECT_NAME " " PROJECT_VERSION "\n");
		exit (EXIT_SUCCESS);
	}

	Application app;
	app_init (&app, argv + 1);
	app_init_terminal (&app);
	app_redraw (&app);

	// g_unix_signal_add() cannot handle SIGWINCH
	install_winch_handler ();

	// Avoid disruptive warnings
	g_log_set_default_handler (log_handler, &app);

	// Message loop
	guint watch_term  = g_unix_signal_add (SIGTERM, on_terminated, &app);
	guint watch_int   = g_unix_signal_add (SIGINT,  on_terminated, &app);
	guint watch_stdin = add_read_watch
		(STDIN_FILENO, process_stdin_input, &app);
	guint watch_winch = add_read_watch
		(g_winch_pipe[0], process_winch_input, &app);

#ifdef WITH_X11
	SelectionWatch sw;
	selection_watch_init (&sw, &app);
#endif  // WITH_X11

	app_run (&app);

#ifdef WITH_X11
	selection_watch_destroy (&sw);
#endif  // WITH_X11

	g_source_remove (watch_term);
	g_source_remove (watch_int);
	g_source_remove (watch_stdin);
	g_source_remove (watch_winch);

	endwin ();
	g_log_set_default_handler (g_log_default_handler, NULL);
	app_destroy (&app);

	if (close (g_winch_pipe[0]) == -1
	 || close (g_winch_pipe[1]) == -1)
		abort ();

	return 0;
}

