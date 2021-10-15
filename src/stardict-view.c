/*
 * StarDict GTK+ UI - dictionary view component
 *
 * Copyright (c) 2021, Přemysl Eric Janouch <p@janouch.name>
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

#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include "stardict-view.h"
#include "utils.h"


typedef struct view_entry ViewEntry;

struct view_entry
{
	gchar *word;                        ///< The word, in Pango markup
	gchar *definition;                  ///< Definition lines, in Pango markup

	PangoLayout *word_layout;           ///< Ellipsized one-line layout or NULL
	PangoLayout *definition_layout;     ///< Multiline layout or NULL
};

static void
view_entry_destroy (ViewEntry *self)
{
	g_free (self->word);
	g_free (self->definition);
	g_clear_object (&self->word_layout);
	g_clear_object (&self->definition_layout);
	g_slice_free1 (sizeof *self, self);
}

static ViewEntry *
view_entry_new (StardictIterator *iterator, const gchar *matched)
{
	g_return_val_if_fail (stardict_iterator_is_valid (iterator), NULL);

	StardictEntry *entry = stardict_iterator_get_entry (iterator);
	g_return_val_if_fail (entry != NULL, NULL);

	// Highlighting may change the rendition, so far it's easiest to recompute
	// it on each search field change by rebuilding the list of view entries.
	// The phonetics suffix would need to be stored separately.
	const gchar *word = stardict_iterator_get_word (iterator);
	gsize common_prefix = stardict_longest_common_collation_prefix
		(iterator->owner, word, matched);

	ViewEntry *ve = g_slice_alloc0 (sizeof *ve);

	GString *adjusted_word = g_string_new ("");
	gchar *pre = g_markup_escape_text (word, common_prefix);
	gchar *post = g_markup_escape_text (word + common_prefix, -1);
	g_string_printf (adjusted_word, "<u>%s</u>%s", pre, post);
	g_free (pre);
	g_free (post);

	GPtrArray *definitions = g_ptr_array_new_full (2, g_free);
	for (const GList *fields = stardict_entry_get_fields (entry); fields; )
	{
		const StardictEntryField *field = fields->data;
		switch (field->type)
		{
		case STARDICT_FIELD_MEANING:
			g_ptr_array_add (definitions,
				g_markup_escape_text (field->data, -1));
			break;
		case STARDICT_FIELD_PANGO:
			g_ptr_array_add (definitions, g_strdup (field->data));
			break;
		case STARDICT_FIELD_XDXF:
			g_ptr_array_add (definitions,
				xdxf_to_pango_markup_with_reduced_effort (field->data));
			break;
		case STARDICT_FIELD_PHONETIC:
		{
			gchar *escaped = g_markup_escape_text (field->data, -1);
			g_string_append_printf (adjusted_word, " /%s/", escaped);
			g_free (escaped);
			break;
		}
		default:
			// TODO: support more of them
			break;
		}
		fields = fields->next;
	}
	g_object_unref (entry);

	ve->word = g_string_free (adjusted_word, FALSE);
	if (!definitions->len)
	{
		gchar *message = g_markup_escape_text (_("no usable field found"), -1);
		g_ptr_array_add (definitions, g_strdup_printf ("&lt;%s&gt;", message));
		g_free (message);
	}

	g_ptr_array_add (definitions, NULL);
	ve->definition = g_strjoinv ("\n", (gchar **) definitions->pdata);
	g_ptr_array_free (definitions, TRUE);
	return ve;
}

static gint
view_entry_height (ViewEntry *ve, gint *word_offset, gint *defn_offset)
{
	gint word_w = 0, word_h = 0;
	gint defn_w = 0, defn_h = 0;
	pango_layout_get_pixel_size (ve->word_layout,       &word_w, &word_h);
	pango_layout_get_pixel_size (ve->definition_layout, &defn_w, &defn_h);

	// Align baselines, without further considerations
	gint wb = pango_layout_get_baseline (ve->word_layout);
	gint db = pango_layout_get_baseline (ve->definition_layout);
	gint word_y = MAX (0, PANGO_PIXELS (db - wb));
	gint defn_y = MAX (0, PANGO_PIXELS (wb - db));

	if (word_offset)
		*word_offset = word_y;
	if (defn_offset)
		*defn_offset = defn_y;

	return MAX (word_y + word_h, defn_y + defn_h);
}

#define PADDING 5

static gint
view_entry_draw (ViewEntry *ve, cairo_t *cr, gint full_width, gboolean even)
{
	// TODO: this shouldn't be hardcoded, read it out from somewhere
	gdouble g = even ? 1. : .95;

	gint word_y = 0, defn_y = 0,
		height = view_entry_height (ve, &word_y, &defn_y);
	cairo_rectangle (cr, 0, 0, full_width, height);
	cairo_set_source_rgb (cr, g, g, g);
	cairo_fill (cr);

	cairo_set_source_rgb (cr, 0, 0, 0);
	cairo_move_to (cr, full_width / 2 + PADDING, defn_y);
	pango_cairo_show_layout (cr, ve->definition_layout);

	PangoLayoutIter *iter = pango_layout_get_iter (ve->definition_layout);
	do
	{
		if (!pango_layout_iter_get_line_readonly (iter)->is_paragraph_start)
			continue;

		PangoRectangle logical = {};
		pango_layout_iter_get_line_extents (iter, NULL, &logical);
		cairo_move_to (cr, PADDING, word_y + PANGO_PIXELS (logical.y));
		pango_cairo_show_layout (cr, ve->word_layout);
	}
	while (pango_layout_iter_next_line (iter));
	pango_layout_iter_free (iter);
	return height;
}

static void
view_entry_rebuild_layout (ViewEntry *ve, PangoContext *pc, gint width)
{
	g_clear_object (&ve->word_layout);
	g_clear_object (&ve->definition_layout);

	int left_width = width / 2 - 2 * PADDING;
	int right_width = width - left_width - 2 * PADDING;
	if (left_width < 1 || right_width < 1)
		return;

	// TODO: preferably pre-validate the layouts with pango_parse_markup(),
	//   so that it doesn't warn without indication on the frontend
	ve->word_layout = pango_layout_new (pc);
	pango_layout_set_markup (ve->word_layout, ve->word, -1);
	pango_layout_set_ellipsize (ve->word_layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_single_paragraph_mode (ve->word_layout, TRUE);
	pango_layout_set_width (ve->word_layout, PANGO_SCALE * left_width);

	ve->definition_layout = pango_layout_new (pc);
	pango_layout_set_markup (ve->definition_layout, ve->definition, -1);
	pango_layout_set_width (ve->definition_layout, PANGO_SCALE * right_width);
}

// --- Widget ------------------------------------------------------------------

struct _StardictView
{
	GtkWidget parent_instance;

	StardictDict *dict;                 ///< The displayed dictionary
	guint top_position;                 ///< Index of the topmost dict. entry
	gchar *matched;                     ///< Highlight common word part of this

	gint top_offset;                    ///< Pixel offset into the entry
	// TODO: think about making it, e.g., a pair of (ViewEntry *, guint)
	// NOTE: this is the index of a Pango paragraph (a virtual entity)
	guint selected;                     ///< Offset to the selected definition
	GList *entries;                     ///< ViewEntry-s within the view
};

static void
adjust_for_offset (StardictView *self)
{
	// FIXME: lots of code duplication with reload(), could be refactored
	GtkWidget *widget = GTK_WIDGET (self);
	PangoContext *pc = gtk_widget_get_pango_context (widget);
	const gchar *matched = self->matched ? self->matched : "";

	GtkAllocation allocation = {};
	gtk_widget_get_allocation (widget, &allocation);

	// If scrolled way up, prepend entries so long as it's possible
	StardictIterator *iterator =
		stardict_iterator_new (self->dict, self->top_position);
	while (self->top_offset < 0)
	{
		stardict_iterator_prev (iterator);
		if (!stardict_iterator_is_valid (iterator))
		{
			self->top_offset = 0;
			break;
		}

		self->top_position = stardict_iterator_get_offset (iterator);
		ViewEntry *ve = view_entry_new (iterator, matched);
		view_entry_rebuild_layout (ve, pc, allocation.width);
		self->top_offset += view_entry_height (ve, NULL, NULL);
		self->entries = g_list_prepend (self->entries, ve);
	}
	g_object_unref (iterator);

	// If scrolled way down, drop leading entries so long as it's possible
	while (self->entries)
	{
		gint height = view_entry_height (self->entries->data, NULL, NULL);
		if (self->top_offset < height)
			break;

		self->top_offset -= height;
		view_entry_destroy (self->entries->data);
		self->entries = g_list_delete_link (self->entries, self->entries);
		self->top_position++;

	}
	if (self->top_offset && !self->entries)
		self->top_offset = 0;

	// Load replacement trailing entries, or drop those no longer visible
	iterator = stardict_iterator_new (self->dict, self->top_position);
	gint used = -self->top_offset;
	for (GList *iter = self->entries, *next;
		 next = g_list_next (iter), iter; iter = next)
	{
		if (used < allocation.height)
			used += view_entry_height (iter->data, NULL, NULL);
		else
		{
			view_entry_destroy (iter->data);
			self->entries = g_list_delete_link (self->entries, iter);
		}
		stardict_iterator_next (iterator);
	}
	while (used < allocation.height && stardict_iterator_is_valid (iterator))
	{
		ViewEntry *ve = view_entry_new (iterator, matched);
		view_entry_rebuild_layout (ve, pc, allocation.width);
		used += view_entry_height (ve, NULL, NULL);
		self->entries = g_list_append (self->entries, ve);
		stardict_iterator_next (iterator);
	}
	g_object_unref (iterator);

	gtk_widget_queue_draw (widget);
}

static void
reload (StardictView *self)
{
	g_list_free_full (self->entries, (GDestroyNotify) view_entry_destroy);
	self->entries = NULL;

	GtkWidget *widget = GTK_WIDGET (self);
	if (!gtk_widget_get_realized (widget) || !self->dict)
		return;

	GtkAllocation allocation = {};
	gtk_widget_get_allocation (widget, &allocation);

	PangoContext *pc = gtk_widget_get_pango_context (widget);
	StardictIterator *iterator =
		stardict_iterator_new (self->dict, self->top_position);

	gint used = 0;
	const gchar *matched = self->matched ? self->matched : "";
	while (used < allocation.height && stardict_iterator_is_valid (iterator))
	{
		ViewEntry *ve = view_entry_new (iterator, matched);
		view_entry_rebuild_layout (ve, pc, allocation.width);
		used += view_entry_height (ve, NULL, NULL);
		self->entries = g_list_prepend (self->entries, ve);
		stardict_iterator_next (iterator);
	}
	g_object_unref (iterator);
	self->entries = g_list_reverse (self->entries);

	// Right now, we're being lazy--this could be integrated here
	adjust_for_offset (self);

	gtk_widget_queue_draw (widget);
}

static gint
natural_row_size (GtkWidget *widget)
{
	PangoLayout *layout = gtk_widget_create_pango_layout (widget, "X");
	gint width = 0, height = 0;
	pango_layout_get_pixel_size (layout, &width, &height);
	g_object_unref (layout);
	return height;
}

// --- Boilerplate -------------------------------------------------------------

G_DEFINE_TYPE (StardictView, stardict_view, GTK_TYPE_WIDGET)

static void
stardict_view_finalize (GObject *gobject)
{
	StardictView *self = STARDICT_VIEW (gobject);
	g_clear_object (&self->dict);

	g_list_free_full (self->entries, (GDestroyNotify) view_entry_destroy);
	self->entries = NULL;

	g_free (self->matched);
	self->matched = NULL;

	G_OBJECT_CLASS (stardict_view_parent_class)->finalize (gobject);
}

static void
stardict_view_get_preferred_height (GtkWidget *widget,
	gint *minimum, gint *natural)
{
	// There isn't any value that would make any real sense
	if (!STARDICT_VIEW (widget)->dict)
		*natural = *minimum = 0;
	else
		*natural = *minimum = natural_row_size (widget);
}

static void
stardict_view_get_preferred_width (GtkWidget *widget G_GNUC_UNUSED,
	gint *minimum, gint *natural)
{
	*natural = *minimum = 4 * PADDING;
}

static void
stardict_view_realize (GtkWidget *widget)
{
	GtkAllocation allocation;
	gtk_widget_get_allocation (widget, &allocation);

	GdkWindowAttr attributes =
	{
		.window_type = GDK_WINDOW_CHILD,
		.x           = allocation.x,
		.y           = allocation.y,
		.width       = allocation.width,
		.height      = allocation.height,

		// Input-only would presumably also work (as in GtkPathBar, e.g.),
		// but it merely seems to involve more work.
		.wclass      = GDK_INPUT_OUTPUT,

		.visual      = gtk_widget_get_visual (widget),
		.event_mask  = gtk_widget_get_events (widget) | GDK_SCROLL_MASK,
	};

	// We need this window to receive input events at all.
	// TODO: see if we don't want GDK_WA_CURSOR for setting a text cursor
	GdkWindow *window = gdk_window_new (gtk_widget_get_parent_window (widget),
		&attributes, GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);

	// The default background colour of the GDK window is transparent,
	// we'll keep it that way, rather than apply the style context.

	gtk_widget_register_window (widget, window);
	gtk_widget_set_window (widget, window);
	gtk_widget_set_realized (widget, TRUE);
}

static gboolean
stardict_view_draw (GtkWidget *widget, cairo_t *cr)
{
	StardictView *self = STARDICT_VIEW (widget);

	GtkAllocation allocation;
	gtk_widget_get_allocation (widget, &allocation);

	gint offset = -self->top_offset;
	gint i = self->top_position;
	for (GList *iter = self->entries; iter; iter = iter->next)
	{
		cairo_save (cr);
		cairo_translate (cr, 0, offset);
		// TODO: later exclude clipped entries, but it's not that important
		offset += view_entry_draw (iter->data, cr, allocation.width, i++ & 1);
		cairo_restore (cr);
	}
	return TRUE;
}

static void
stardict_view_size_allocate (GtkWidget *widget, GtkAllocation *allocation)
{
	GTK_WIDGET_CLASS (stardict_view_parent_class)
		->size_allocate (widget, allocation);

	reload (STARDICT_VIEW (widget));
}

static void
stardict_view_screen_changed (GtkWidget *widget, G_GNUC_UNUSED GdkScreen *prev)
{
	// Update the minimum size
	gtk_widget_queue_resize (widget);

	// Recreate Pango layouts
	reload (STARDICT_VIEW (widget));
}

static gboolean
stardict_view_scroll_event (GtkWidget *widget, GdkEventScroll *event)
{
	// TODO: rethink the notes here to rather iterate over /definition lines/
	//  - iterate over selected lines, maybe one, maybe three
	StardictView *self = STARDICT_VIEW (widget);
	if (!self->dict)
		return FALSE;

	switch (event->direction)
	{
	case GDK_SCROLL_UP:
		self->top_offset -= 3 * natural_row_size (widget);
		adjust_for_offset (self);
		return TRUE;
	case GDK_SCROLL_DOWN:
		self->top_offset += 3 * natural_row_size (widget);
		adjust_for_offset (self);
		return TRUE;
	default:
		// GDK_SCROLL_SMOOTH doesn't fit the intended way of usage
		return FALSE;
	}
}

static void
stardict_view_class_init (StardictViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = stardict_view_finalize;

	// TODO: handle mouse events for text selection
	// See https://wiki.gnome.org/HowDoI/CustomWidgets for some guidelines.
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
	widget_class->get_preferred_height = stardict_view_get_preferred_height;
	widget_class->get_preferred_width = stardict_view_get_preferred_width;
	widget_class->realize = stardict_view_realize;
	widget_class->draw = stardict_view_draw;
	widget_class->size_allocate = stardict_view_size_allocate;
	widget_class->screen_changed = stardict_view_screen_changed;
	widget_class->scroll_event = stardict_view_scroll_event;
}

static void
stardict_view_init (G_GNUC_UNUSED StardictView *self)
{
}

// --- Public ------------------------------------------------------------------

GtkWidget *
stardict_view_new (void)
{
	return GTK_WIDGET (g_object_new (STARDICT_TYPE_VIEW, NULL));
}

void
stardict_view_set_position (StardictView *self,
	StardictDict *dict, guint position)
{
	g_return_if_fail (STARDICT_IS_VIEW (self));
	g_return_if_fail (STARDICT_IS_DICT (dict));

	// Update the minimum size, if appropriate (almost never)
	if (!self->dict != !dict)
		gtk_widget_queue_resize (GTK_WIDGET (self));

	g_clear_object (&self->dict);
	self->dict = g_object_ref (dict);
	self->top_position = position;
	self->top_offset = 0;

	reload (self);
}

void
stardict_view_set_matched (StardictView *self, const gchar *matched)
{
	g_return_if_fail (STARDICT_IS_VIEW (self));

	g_free (self->matched);
	self->matched = g_strdup (matched);
	reload (self);
}
