/*
 * A tool to query multiple dictionaries for the specified word
 *
 * Intended for use in IRC bots and similar silly things---words go in,
 * one per each line, and entries come out, one dictionary at a time,
 * finalised with an empty line.  Newlines are escaped with `\n',
 * backslashes with `\\'.
 *
 * So far only the `m', `g`, and `x` fields are supported, as in sdtui.
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
#include <string.h>
#include <errno.h>

#include <glib.h>
#include <gio/gio.h>
#include <pango/pango.h>

#include "stardict.h"
#include "stardict-private.h"
#include "generator.h"
#include "utils.h"


// --- Output formatting -------------------------------------------------------

/// Transform Pango attributes to in-line formatting sequences (non-reentrant)
typedef const gchar *(*FormatterFunc) (PangoAttrIterator *);

static const gchar *
pango_attrs_ignore (G_GNUC_UNUSED PangoAttrIterator *iterator)
{
	return "";
}

static const gchar *
pango_attrs_to_irc (PangoAttrIterator *iterator)
{
	static gchar buf[5];
	gchar *p = buf;
	*p++ = 0x0f;

	if (!iterator)
		goto reset_formatting;

	PangoAttrInt *attr = NULL;
	if ((attr = (PangoAttrInt *) pango_attr_iterator_get (iterator,
			PANGO_ATTR_WEIGHT)) && attr->value >= PANGO_WEIGHT_BOLD)
		*p++ = 0x02;
	if ((attr = (PangoAttrInt *) pango_attr_iterator_get (iterator,
			PANGO_ATTR_UNDERLINE)) && attr->value == PANGO_UNDERLINE_SINGLE)
		*p++ = 0x1f;
	if ((attr = (PangoAttrInt *) pango_attr_iterator_get (iterator,
			PANGO_ATTR_STYLE)) && attr->value == PANGO_STYLE_ITALIC)
		*p++ = 0x1d;

reset_formatting:
	*p++ = 0;
	return buf;
}

static const gchar *
pango_attrs_to_ansi (PangoAttrIterator *iterator)
{
	static gchar buf[16];
	g_strlcpy (buf, "\x1b[0", sizeof buf);
	if (!iterator)
		goto reset_formatting;

	PangoAttrInt *attr = NULL;
	if ((attr = (PangoAttrInt *) pango_attr_iterator_get (iterator,
			PANGO_ATTR_WEIGHT)) && attr->value >= PANGO_WEIGHT_BOLD)
		g_strlcat (buf, ";1", sizeof buf);
	if ((attr = (PangoAttrInt *) pango_attr_iterator_get (iterator,
			PANGO_ATTR_UNDERLINE)) && attr->value == PANGO_UNDERLINE_SINGLE)
		g_strlcat (buf, ";4", sizeof buf);
	if ((attr = (PangoAttrInt *) pango_attr_iterator_get (iterator,
			PANGO_ATTR_STYLE)) && attr->value == PANGO_STYLE_ITALIC)
		g_strlcat (buf, ";3", sizeof buf);

reset_formatting:
	g_strlcat (buf, "m", sizeof buf);
	return buf;
}

static gchar *
pango_to_output_text (const gchar *markup, FormatterFunc formatter)
{
	// This function skips leading whitespace, but it's the canonical one
	gchar *text = NULL;
	PangoAttrList *attrs = NULL;
	if (!pango_parse_markup (markup, -1, 0, &attrs, &text, NULL, NULL))
		return g_strdup_printf ("<%s>", ("error in entry"));

	PangoAttrIterator *iterator = pango_attr_list_get_iterator (attrs);
	GString *result = g_string_new ("");
	do
	{
		gint start = 0, end = 0;
		pango_attr_iterator_range (iterator, &start, &end);
		if (end == G_MAXINT)
			end = strlen (text);

		g_string_append (result, formatter (iterator));
		g_string_append_len (result, text + start, end - start);
	}
	while (pango_attr_iterator_next (iterator));
	g_string_append (result, formatter (NULL));

	g_free (text);
	pango_attr_iterator_destroy (iterator);
	pango_attr_list_unref (attrs);
	return g_string_free (result, FALSE);
}

static gchar *
field_to_output_text (const StardictEntryField *field, FormatterFunc formatter)
{
	const gchar *definition = field->data;
	if (field->type == STARDICT_FIELD_MEANING)
		return g_strdup (definition);
	if (field->type == STARDICT_FIELD_PANGO)
		return pango_to_output_text (definition, formatter);
	if (field->type == STARDICT_FIELD_XDXF)
	{
		gchar *markup = xdxf_to_pango_markup_with_reduced_effort (definition);
		gchar *result = pango_to_output_text (markup, formatter);
		g_free (markup);
		return result;
	}
	return NULL;
}

// --- Main --------------------------------------------------------------------

static guint
count_equal_chars (const gchar *a, const gchar *b)
{
	guint count = 0;
	while (*a && *b)
		if (*a++ == *b++)
			count++;
	return count;
}

static void
do_dictionary (StardictDict *dict, const gchar *word, FormatterFunc formatter)
{
	gboolean found;
	StardictIterator *iter = stardict_dict_search (dict, word, &found);
	if (!found)
		goto out;

	// Default Stardict ordering is ASCII case-insensitive,
	// which may be further exacerbated by our own collation feature.
	// Try to find a better matching entry:

	gint64 best_offset = stardict_iterator_get_offset (iter);
	guint best_score = count_equal_chars
		(stardict_iterator_get_word (iter), word);

	while (TRUE)
	{
		stardict_iterator_next (iter);
		if (!stardict_iterator_is_valid (iter))
			break;

		const gchar *iter_word = stardict_iterator_get_word (iter);
		if (g_ascii_strcasecmp (iter_word, word))
			break;

		guint score = count_equal_chars (iter_word, word);
		if (score > best_score)
		{
			best_offset = stardict_iterator_get_offset (iter);
			best_score = score;
		}
	}

	stardict_iterator_set_offset (iter, best_offset, FALSE);

	StardictEntry *entry = stardict_iterator_get_entry (iter);
	StardictInfo *info = stardict_dict_get_info (dict);
	const GList *list = stardict_entry_get_fields (entry);
	for (; list; list = list->next)
	{
		StardictEntryField *field = list->data;
		gchar *definitions = field_to_output_text (field, formatter);
		if (!definitions)
			continue;

		printf ("%s\t", info->book_name);
		for (const gchar *p = definitions; *p; p++)
		{
			if (*p == '\\')
				printf ("\\\\");
			else if (*p == '\n')
				printf ("\\n");
			else
				putchar (*p);
		}
		putchar ('\n');
		g_free (definitions);
	}
	g_object_unref (entry);
out:
	g_object_unref (iter);
}

static FormatterFunc
parse_options (int *argc, char ***argv)
{
	GError *error = NULL;
	GOptionContext *ctx = g_option_context_new
		("DICTIONARY.ifo... - query multiple dictionaries");

	gboolean format_with_ansi = FALSE;
	gboolean format_with_irc = FALSE;
	GOptionEntry entries[] =
	{
		{ "ansi", 'a', 0, G_OPTION_ARG_NONE, &format_with_ansi,
		  "Format with ANSI sequences", NULL },
		{ "irc", 'i', 0, G_OPTION_ARG_NONE, &format_with_irc,
		  "Format with IRC codes", NULL },
		{ }
	};

	g_option_context_add_main_entries (ctx, entries, NULL);
	if (!g_option_context_parse (ctx, argc, argv, &error))
	{
		g_printerr ("Error: option parsing failed: %s\n", error->message);
		exit (EXIT_FAILURE);
	}
	if (*argc < 2)
	{
		g_printerr ("%s\n", g_option_context_get_help (ctx, TRUE, NULL));
		exit (EXIT_FAILURE);
	}
	g_option_context_free (ctx);

	if (format_with_ansi)
		return pango_attrs_to_ansi;
	if (format_with_irc)
		return pango_attrs_to_irc;

	return pango_attrs_ignore;
}

int
main (int argc, char *argv[])
{
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	if (glib_check_version (2, 36, 0))
		g_type_init ();
G_GNUC_END_IGNORE_DEPRECATIONS

	FormatterFunc formatter = parse_options (&argc, &argv);

	guint n_dicts = argc - 1;
	StardictDict **dicts = g_alloca (sizeof *dicts * n_dicts);

	guint i;
	for (i = 1; i <= n_dicts; i++)
	{
		GError *error = NULL;
		dicts[i - 1] = stardict_dict_new (argv[i], &error);
		if (error)
		{
			g_printerr ("Error: opening dictionary `%s' failed: %s\n",
				argv[i], error->message);
			exit (EXIT_FAILURE);
		}
	}

	gint c;
	do
	{
		GString *s = g_string_new (NULL);
		while ((c = getchar ()) != EOF && c != '\n')
			if (c != '\r')
				g_string_append_c (s, c);

		if (s->len)
			for (i = 0; i < n_dicts; i++)
				do_dictionary (dicts[i], s->str, formatter);

		printf ("\n");
		fflush (NULL);
		g_string_free (s, TRUE);
	}
	while (c != EOF);

	for (i = 0; i < n_dicts; i++)
		g_object_unref (dicts[i]);

	return 0;
}
