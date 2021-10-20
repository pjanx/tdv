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
	gchar *word;                        ///< The word
	gsize word_matched;                 ///< Initial matching bytes of the word
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

	GString *adjusted_word = g_string_new (word);
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
			g_string_append_printf (adjusted_word,
				" /%s/", (const char *) field->data);
		default:
			// TODO: support more of them
			break;
		}
		fields = fields->next;
	}
	g_object_unref (entry);

	ViewEntry *ve = g_slice_alloc0 (sizeof *ve);
	ve->word = g_string_free (adjusted_word, FALSE);
	ve->word_matched = stardict_longest_common_collation_prefix
		(iterator->owner, word, matched);
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

static GtkBorder
view_entry_get_padding (GtkStyleContext *style)
{
	GtkBorder padding = {};
	GtkStateFlags state = gtk_style_context_get_state (style);
	gtk_style_context_get_padding (style, state, &padding);
	return padding;
}

static gint
view_entry_draw (ViewEntry *ve, cairo_t *cr, gint full_width,
	GtkStyleContext *style)
{
	gint word_y = 0, defn_y = 0,
		height = view_entry_height (ve, &word_y, &defn_y);

	gtk_render_background (style, cr, 0, 0, full_width, height);
	gtk_render_frame (style, cr, 0, 0, full_width, height);

	// Top/bottom and left/right-dependent padding will not work, too much code
	GtkBorder padding = view_entry_get_padding (style);

	gtk_style_context_save (style);
	gtk_style_context_add_class (style, GTK_STYLE_CLASS_RIGHT);
	gtk_render_layout (style, cr,
		full_width / 2 + padding.left, defn_y, ve->definition_layout);
	gtk_style_context_restore (style);

	gtk_style_context_save (style);
	gtk_style_context_add_class (style, GTK_STYLE_CLASS_LEFT);
	PangoLayoutIter *iter = pango_layout_get_iter (ve->definition_layout);
	do
	{
		if (!pango_layout_iter_get_line_readonly (iter)->is_paragraph_start)
			continue;

		PangoRectangle logical = {};
		pango_layout_iter_get_line_extents (iter, NULL, &logical);
		gtk_render_layout (style, cr,
			padding.left, word_y + PANGO_PIXELS (logical.y), ve->word_layout);
	}
	while (pango_layout_iter_next_line (iter));
	pango_layout_iter_free (iter);
	gtk_style_context_restore (style);
	return height;
}

static void
view_entry_rebuild_layouts (ViewEntry *ve, GtkWidget *widget)
{
	PangoContext *pc = gtk_widget_get_pango_context (widget);
	GtkStyleContext *style = gtk_widget_get_style_context (widget);
	gint full_width = gtk_widget_get_allocated_width (widget);

	g_clear_object (&ve->word_layout);
	g_clear_object (&ve->definition_layout);

	GtkBorder padding = view_entry_get_padding (style);
	gint part_width = full_width / 2 - padding.left - padding.right;
	if (part_width < 1)
		return;

	// Left/right-dependent fonts aren't supported
	ve->word_layout = pango_layout_new (pc);
	pango_layout_set_text (ve->word_layout, ve->word, -1);
	pango_layout_set_ellipsize (ve->word_layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_single_paragraph_mode (ve->word_layout, TRUE);
	pango_layout_set_width (ve->word_layout, PANGO_SCALE * part_width);

	// gtk_css_style_get_pango_attributes() is completely inaccessible,
	// so the underline cannot be styled (GTK_STYLE_PROPERTY_FONT isn't enough)
	PangoAttrList *attrs = pango_attr_list_new ();
	PangoAttribute *u = pango_attr_underline_new (PANGO_UNDERLINE_SINGLE);
	u->end_index = ve->word_matched;
	pango_attr_list_insert (attrs, u);
	pango_layout_set_attributes (ve->word_layout, attrs);
	pango_attr_list_unref (attrs);

	// TODO: preferably pre-validate the layout with pango_parse_markup(),
	//   so that it doesn't warn without indication on the frontend
	ve->definition_layout = pango_layout_new (pc);
	pango_layout_set_markup (ve->definition_layout, ve->definition, -1);
	pango_layout_set_width (ve->definition_layout, PANGO_SCALE * part_width);
	pango_layout_set_wrap (ve->definition_layout, PANGO_WRAP_WORD_CHAR);
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

static ViewEntry *
make_entry (StardictView *self, StardictIterator *iterator)
{
	const gchar *matched = self->matched ? self->matched : "";
	ViewEntry *ve = view_entry_new (iterator, matched);
	view_entry_rebuild_layouts (ve, GTK_WIDGET (self));
	return ve;
}

static void
adjust_for_height (StardictView *self)
{
	GtkWidget *widget = GTK_WIDGET (self);
	StardictIterator *iterator =
		stardict_iterator_new (self->dict, self->top_position);

	gint missing = gtk_widget_get_allocated_height (widget) + self->top_offset;
	for (GList *iter = self->entries, *next;
		 next = g_list_next (iter), iter; iter = next)
	{
		if (missing > 0)
			missing -= view_entry_height (iter->data, NULL, NULL);
		else
		{
			view_entry_destroy (iter->data);
			self->entries = g_list_delete_link (self->entries, iter);
		}
		stardict_iterator_next (iterator);
	}

	GList *append = NULL;
	while (missing > 0 && stardict_iterator_is_valid (iterator))
	{
		ViewEntry *ve = make_entry (self, iterator);
		missing -= view_entry_height (ve, NULL, NULL);
		append = g_list_prepend (append, ve);
		stardict_iterator_next (iterator);
	}
	g_object_unref (iterator);

	self->entries = g_list_concat (self->entries, g_list_reverse (append));
	gtk_widget_queue_draw (widget);
}

static void
adjust_for_offset (StardictView *self)
{
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
		ViewEntry *ve = make_entry (self, iterator);
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
	adjust_for_height (self);
}

static void
reload (StardictView *self)
{
	GtkWidget *widget = GTK_WIDGET (self);

	g_list_free_full (self->entries, (GDestroyNotify) view_entry_destroy);
	self->entries = NULL;
	gtk_widget_queue_draw (widget);

	if (gtk_widget_get_realized (widget) && self->dict)
		adjust_for_height (self);
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
stardict_view_get_preferred_width (GtkWidget *widget,
	gint *minimum, gint *natural)
{
	GtkStyleContext *style = gtk_widget_get_style_context (widget);
	GtkBorder padding = view_entry_get_padding (style);
	*natural = *minimum = 2 * (padding.left + 1 * padding.right);
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

		// GDK_SMOOTH_SCROLL_MASK is useless, will stop sending UP/DOWN
		.event_mask  = gtk_widget_get_events (widget) | GDK_SCROLL_MASK,
	};

	// We need this window to receive input events at all.
	// TODO: see if we don't want GDK_WA_CURSOR for setting a text cursor
	GdkWindow *window = gdk_window_new (gtk_widget_get_parent_window (widget),
		&attributes, GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);

	// The default background colour of the GDK window is transparent
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

	GtkStyleContext *style = gtk_widget_get_style_context (widget);
	gtk_render_background (style, cr,
		0, 0, allocation.width, allocation.height);
	gtk_render_frame (style, cr,
		0, 0, allocation.width, allocation.height);

	gint offset = -self->top_offset;
	gint i = self->top_position;
	for (GList *iter = self->entries; iter; iter = iter->next)
	{
		// Style regions would be appropriate, if they weren't deprecated.
		// GTK+ CSS gadgets/nodes are an internal API.  We don't want to turn
		// this widget into a container, to avoid needless complexity.
		//
		// gtk_style_context_{get,set}_path() may be misused by adding the same
		// GType with gtk_widget_path_append_type() and changing its name
		// using gtk_widget_path_iter_set_name()... but that is ugly.
		gtk_style_context_save (style);
		gtk_style_context_add_class (style, (i++ & 1) ? "even" : "odd");

		cairo_save (cr);
		cairo_translate (cr, 0, offset);
		// TODO: later exclude clipped entries, but it's not that important
		offset += view_entry_draw (iter->data, cr, allocation.width, style);
		cairo_restore (cr);

		gtk_style_context_restore (style);
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
		stardict_view_scroll (self, GTK_SCROLL_STEPS, -3);
		return TRUE;
	case GDK_SCROLL_DOWN:
		stardict_view_scroll (self, GTK_SCROLL_STEPS, +3);
		return TRUE;
	case GDK_SCROLL_SMOOTH:
		self->top_offset += event->delta_y;
		adjust_for_offset (self);
		return TRUE;
	default:
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

	gtk_widget_class_set_css_name (widget_class, "stardict-view");
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

void
stardict_view_scroll (StardictView *self, GtkScrollStep step, gdouble amount)
{
	g_return_if_fail (STARDICT_IS_VIEW (self));

	GtkWidget *widget = GTK_WIDGET (self);
	switch (step)
	{
	case GTK_SCROLL_STEPS:
		self->top_offset += amount * natural_row_size (widget);
		break;
	case GTK_SCROLL_PAGES:
		self->top_offset += amount * gtk_widget_get_allocated_height (widget);
		break;
	default:
		break;
	}

	adjust_for_offset (self);
}
