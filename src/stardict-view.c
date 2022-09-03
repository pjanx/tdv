/*
 * StarDict GTK+ UI - dictionary view component
 *
 * Copyright (c) 2021 - 2022, Přemysl Eric Janouch <p@janouch.name>
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

typedef struct view_entry_render_ctx ViewEntryRenderCtx;

// TODO: see if we can't think of a cleaner way of doing this
struct view_entry_render_ctx
{
	GtkStyleContext *style;
	cairo_t *cr;
	int width;
	int height;

	// Forwarded from StardictView
	PangoLayout *selection_layout;
	int selection_begin;
	int selection_end;
	PangoLayout *hover_layout;
	int hover_begin;
	int hover_end;
};

static PangoLayout *
view_entry_adjust_layout (ViewEntryRenderCtx *ctx, PangoLayout *layout)
{
	if (layout != ctx->hover_layout)
		return g_object_ref (layout);

	layout = pango_layout_copy (layout);
	PangoAttrList *attrs = pango_layout_get_attributes (layout);
	attrs = attrs
		? pango_attr_list_copy (attrs)
		: pango_attr_list_new ();

	PangoAttribute *u = pango_attr_underline_new (PANGO_UNDERLINE_SINGLE);
	u->start_index = ctx->hover_begin;
	u->end_index = ctx->hover_end;
	pango_attr_list_change (attrs, u);

	PangoAttribute *uc = pango_attr_underline_color_new (0, 0, 0xffff);
	uc->start_index = ctx->hover_begin;
	uc->end_index = ctx->hover_end;
	pango_attr_list_change (attrs, uc);

	PangoAttribute *c = pango_attr_foreground_new (0, 0, 0xffff);
	c->start_index = ctx->hover_begin;
	c->end_index = ctx->hover_end;
	pango_attr_list_change (attrs, c);

	pango_layout_set_attributes (layout, attrs);
	pango_attr_list_unref (attrs);
	return layout;
}

static void
view_entry_render (ViewEntryRenderCtx *ctx, gdouble x, gdouble y,
	PangoLayout *layout)
{
	PangoLayout *adjusted = view_entry_adjust_layout (ctx, layout);
	gtk_render_layout (ctx->style, ctx->cr, x, y, adjusted);
	if (layout != ctx->selection_layout)
		goto out;

	gtk_style_context_save (ctx->style);
	gtk_style_context_set_state (ctx->style, GTK_STATE_FLAG_SELECTED);
	cairo_save (ctx->cr);

	int ranges[2] = { MIN (ctx->selection_begin, ctx->selection_end),
		MAX (ctx->selection_begin, ctx->selection_end) };
	cairo_region_t *region
		= gdk_pango_layout_get_clip_region (adjusted, x, y, ranges, 1);
	gdk_cairo_region (ctx->cr, region);
	cairo_clip (ctx->cr);
	cairo_region_destroy (region);

	gtk_render_background (ctx->style, ctx->cr, 0, 0, ctx->width, ctx->height);
	gtk_render_layout (ctx->style, ctx->cr, x, y, adjusted);

	cairo_restore (ctx->cr);
	gtk_style_context_restore (ctx->style);
out:
	g_object_unref (adjusted);
}

static gint
view_entry_draw (ViewEntry *ve, ViewEntryRenderCtx *ctx)
{
	gint word_y = 0, defn_y = 0;
	ctx->height = view_entry_height (ve, &word_y, &defn_y);

	gtk_render_background (ctx->style, ctx->cr, 0, 0, ctx->width, ctx->height);
	gtk_render_frame (ctx->style, ctx->cr, 0, 0, ctx->width, ctx->height);

	// Top/bottom and left/right-dependent padding will not work, too much code
	GtkBorder padding = view_entry_get_padding (ctx->style);

	gtk_style_context_save (ctx->style);
	gtk_style_context_add_class (ctx->style, GTK_STYLE_CLASS_RIGHT);
	view_entry_render (ctx, ctx->width / 2 + padding.left, defn_y,
		ve->definition_layout);
	gtk_style_context_restore (ctx->style);

	gtk_style_context_save (ctx->style);
	gtk_style_context_add_class (ctx->style, GTK_STYLE_CLASS_LEFT);
	PangoLayoutIter *iter = pango_layout_get_iter (ve->definition_layout);
	do
	{
		if (!pango_layout_iter_get_line_readonly (iter)->is_paragraph_start)
			continue;

		PangoRectangle logical = {};
		pango_layout_iter_get_line_extents (iter, NULL, &logical);
		view_entry_render (ctx, padding.left, word_y + PANGO_PIXELS (logical.y),
			ve->word_layout);
	}
	while (pango_layout_iter_next_line (iter));
	pango_layout_iter_free (iter);
	gtk_style_context_restore (ctx->style);
	return ctx->height;
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
	gdouble drag_last_offset;           ///< Last offset when dragging
	GList *entries;                     ///< ViewEntry-s within the view

	GtkGesture *selection_gesture;      ///< Selection gesture
	GWeakRef selection;                 ///< Selected PangoLayout, if any
	int selection_begin;                ///< Start index within `selection`
	int selection_end;                  ///< End index within `selection`

	GWeakRef hover;                     ///< Hovered PangoLayout, if any
	int hover_begin;                    ///< Word start index within `hover`
	int hover_end;                      ///< Word end index within `hover`
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
reset_hover (StardictView *self)
{
	GtkWidget *widget = GTK_WIDGET (self);
	PangoLayout *hover = g_weak_ref_get (&self->hover);
	if (hover)
	{
		g_object_unref (hover);
		g_weak_ref_set (&self->hover, NULL);
		self->hover_begin = self->hover_end = -1;
		gtk_widget_queue_draw (widget);
	}

	if (gtk_widget_get_realized (widget))
		gdk_window_set_cursor (gtk_widget_get_window (widget), NULL);
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

	// Also handling this for adjust_for_offset(), which calls this.
	PangoLayout *selection = g_weak_ref_get (&self->selection);
	if (selection)
		g_object_unref (selection);
	else
		self->selection_begin = self->selection_end = -1;

	reset_hover (self);
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

	// For consistency, and the check in make_context_menu()
	self->selection_begin = self->selection_end = -1;
	reset_hover (self);

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

// --- Figuring out where stuff is----------------------------------------------

/// Figure out which layout is at given widget coordinates, and translate them.
static PangoLayout *
layout_at (StardictView *self, int *x, int *y)
{
	GtkWidget *widget = GTK_WIDGET (self);
	int width = gtk_widget_get_allocated_width (widget);

	// The algorithm here is a simplification of stardict_view_draw().
	GtkStyleContext *style = gtk_widget_get_style_context (widget);
	GtkBorder padding = view_entry_get_padding (style);
	gint offset = -self->top_offset;
	for (GList *iter = self->entries; iter; iter = iter->next)
	{
		ViewEntry *ve = iter->data;
		if (G_UNLIKELY (*y < offset))
			break;

		gint top_y = offset, word_y = 0, defn_y = 0;
		offset += view_entry_height (ve, &word_y, &defn_y);
		if (*y >= offset)
			continue;

		if (*x >= width / 2)
		{
			*x -= width / 2 + padding.left;
			*y -= top_y + defn_y;
			return ve->definition_layout;
		}
		else
		{
			*x -= padding.left;
			*y -= top_y + word_y;
			return ve->word_layout;
		}
	}
	return NULL;
}

/// Figure out a layout's coordinates.
static gboolean
layout_coords (StardictView *self, PangoLayout *layout, int *x, int *y)
{
	GtkWidget *widget = GTK_WIDGET (self);
	int width = gtk_widget_get_allocated_width (widget);

	// The algorithm here is a simplification of stardict_view_draw().
	GtkStyleContext *style = gtk_widget_get_style_context (widget);
	GtkBorder padding = view_entry_get_padding (style);
	gint offset = -self->top_offset;
	for (GList *iter = self->entries; iter; iter = iter->next)
	{
		ViewEntry *ve = iter->data;
		gint top_y = offset, word_y = 0, defn_y = 0;
		offset += view_entry_height (ve, &word_y, &defn_y);

		if (layout == ve->definition_layout)
		{
			*x = width / 2 + padding.left;
			*y = top_y + defn_y;
			return TRUE;
		}
		if (layout == ve->word_layout)
		{
			*x = padding.left;
			*y = top_y + word_y;
			return TRUE;
		}
	}
	return FALSE;
}

static int
layout_index_at (PangoLayout *layout, int x, int y)
{
	int index = 0, trailing = 0;
	(void) pango_layout_xy_to_index (layout,
		x * PANGO_SCALE,
		y * PANGO_SCALE,
		&index,
		&trailing);

	const char *text = pango_layout_get_text (layout) + index;
	while (trailing--)
	{
		int len = g_utf8_next_char (text) - text;
		text += len;
		index += len;
	}
	return index;
}

static PangoLayout *
locate_word_at (StardictView *self, int x, int y, int *beginpos, int *endpos)
{
	*beginpos = -1;
	*endpos = -1;
	PangoLayout *layout = layout_at (self, &x, &y);
	if (!layout)
		return NULL;

	const char *text = pango_layout_get_text (layout), *p = NULL;
	const char *begin = text + layout_index_at (layout, x, y), *end = begin;
	while ((p = g_utf8_find_prev_char (text, begin))
		&& !g_unichar_isspace (g_utf8_get_char (p)))
		begin = p;
	gunichar c;
	while ((c = g_utf8_get_char (end)) && !g_unichar_isspace (c))
		end = g_utf8_next_char (end);

	*beginpos = begin - text;
	*endpos = end - text;
	return layout;
}

// --- Boilerplate -------------------------------------------------------------

G_DEFINE_TYPE (StardictView, stardict_view, GTK_TYPE_WIDGET)

enum {
	SEND,
	LAST_SIGNAL,
};

static guint view_signals[LAST_SIGNAL];

static void
stardict_view_finalize (GObject *gobject)
{
	StardictView *self = STARDICT_VIEW (gobject);
	g_clear_object (&self->dict);

	g_list_free_full (self->entries, (GDestroyNotify) view_entry_destroy);
	self->entries = NULL;

	g_object_unref (self->selection_gesture);
	g_weak_ref_clear (&self->selection);

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
		.event_mask  = gtk_widget_get_events (widget) | GDK_SCROLL_MASK
			| GDK_SMOOTH_SCROLL_MASK | GDK_BUTTON_PRESS_MASK
			| GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK,
	};

	// We need this window to receive input events at all.
	GdkWindow *window = gdk_window_new (gtk_widget_get_parent_window (widget),
		&attributes, GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);

	// The default background colour of the GDK window is transparent
	gtk_widget_register_window (widget, window);
	gtk_widget_set_window (widget, window);
	gtk_widget_set_realized (widget, TRUE);
}

static void
reset_hover_for_event (StardictView *self, guint state, int x, int y)
{
	reset_hover (self);
	if ((state &= gtk_accelerator_get_default_mod_mask ()) != GDK_CONTROL_MASK)
		return;

	g_weak_ref_set (&self->hover,
		locate_word_at (self, x, y, &self->hover_begin, &self->hover_end));
	gtk_widget_queue_draw (GTK_WIDGET (self));

	GdkWindow *window = gtk_widget_get_window (GTK_WIDGET (self));
	GdkCursor *cursor = gdk_cursor_new_from_name
		(gdk_window_get_display (window), "pointer");
	gdk_window_set_cursor (window, cursor);
	g_object_unref (cursor);
}

static void
on_keymap_state_changed (G_GNUC_UNUSED GdkKeymap *keymap, StardictView *self)
{
	GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (self));
	GdkSeat *seat = gdk_display_get_default_seat (display);
	GdkDevice *pointer = gdk_seat_get_pointer (seat);

	int x = -1, y = -1;
	GdkModifierType state = 0;
	GdkWindow *window = gtk_widget_get_window (GTK_WIDGET (self));
	gdk_window_get_device_position (window, pointer, &x, &y, &state);
	reset_hover_for_event (self, state, x, y);
}

static void
stardict_view_map (GtkWidget *widget)
{
	GTK_WIDGET_CLASS (stardict_view_parent_class)->map (widget);

	GdkWindow *window = gtk_widget_get_window (widget);
	GdkDisplay *display = gdk_window_get_display (window);
	GdkKeymap *keymap = gdk_keymap_get_for_display (display);
	g_signal_connect (keymap, "state-changed",
		G_CALLBACK (on_keymap_state_changed), widget);
}

static void
stardict_view_unmap (GtkWidget *widget)
{
	GdkWindow *window = gtk_widget_get_window (widget);
	GdkDisplay *display = gdk_window_get_display (window);
	GdkKeymap *keymap = gdk_keymap_get_for_display (display);
	g_signal_handlers_disconnect_by_data (keymap, widget);

	GTK_WIDGET_CLASS (stardict_view_parent_class)->unmap (widget);
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

	ViewEntryRenderCtx ctx =
	{
		.style = style,
		.cr = cr,
		.width = allocation.width,
		.height = 0,

		.selection_layout = g_weak_ref_get (&self->selection),
		.selection_begin = self->selection_begin,
		.selection_end = self->selection_end,
		.hover_layout = g_weak_ref_get (&self->hover),
		.hover_begin = self->hover_begin,
		.hover_end = self->hover_end,
	};

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
		offset += view_entry_draw (iter->data, &ctx);
		cairo_restore (cr);

		gtk_style_context_restore (style);
	}
	g_clear_object (&ctx.selection_layout);
	g_clear_object (&ctx.hover_layout);
	return TRUE;
}

static void
stardict_view_size_allocate (GtkWidget *widget, GtkAllocation *allocation)
{
	GTK_WIDGET_CLASS (stardict_view_parent_class)
		->size_allocate (widget, allocation);

	StardictView *self = STARDICT_VIEW (widget);
	if (!gtk_widget_get_realized (widget) || !self->dict)
		return;

	PangoLayout *selection = g_weak_ref_get (&self->selection), **origin = NULL;
	for (GList *iter = self->entries; iter; iter = iter->next)
	{
		ViewEntry *ve = iter->data;
		if (selection && selection == ve->word_layout)
			origin = &ve->word_layout;
		if (selection && selection == ve->definition_layout)
			origin = &ve->definition_layout;
	}
	if (selection)
		g_object_unref (selection);

	for (GList *iter = self->entries; iter; iter = iter->next)
		view_entry_rebuild_layouts (iter->data, widget);
	if (origin)
		g_weak_ref_set (&self->selection, *origin);

	adjust_for_offset (self);
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
	{
		// On GDK/Wayland, the mouse wheel will typically create 1.5 deltas,
		// after dividing a 15 degree click angle from libinput by 10.
		// (Noticed on Arch + Sway, cannot reproduce on Ubuntu 22.04.)
		// On X11, as libinput(4) indicates, the delta will always be 1.0.
		double delta = CLAMP (event->delta_y, -1, +1);
		stardict_view_scroll (self, GTK_SCROLL_STEPS, 3 * delta);
		return TRUE;
	}
	default:
		return FALSE;
	}
}

static void
publish_selection (StardictView *self, GdkAtom target)
{
	PangoLayout *layout = g_weak_ref_get (&self->selection);
	if (!layout)
		return;

	// Unlike GtkLabel, we don't place the selection in PRIMARY immediately.
	const char *text = pango_layout_get_text (layout);
	int len = strlen (text),
		s1 = MIN (self->selection_begin, self->selection_end),
		s2 = MAX (self->selection_begin, self->selection_end);
	if (s1 != s2 && s1 >= 0 && s1 <= len && s2 >= 0 && s2 <= len)
		gtk_clipboard_set_text (gtk_clipboard_get (target), text + s1, s2 - s1);
	g_object_unref (layout);
}

static void
select_word_at (StardictView *self, int x, int y)
{
	g_weak_ref_set (&self->selection, locate_word_at (self,
		x, y, &self->selection_begin, &self->selection_end));

	gtk_widget_queue_draw (GTK_WIDGET (self));
	publish_selection (self, GDK_SELECTION_PRIMARY);
}

static void
select_all_at (StardictView *self, int x, int y)
{
	PangoLayout *layout = layout_at (self, &x, &y);
	if (!layout)
		return;

	g_weak_ref_set (&self->selection, layout);
	self->selection_begin = 0;
	self->selection_end = strlen (pango_layout_get_text (layout));
	gtk_widget_queue_draw (GTK_WIDGET (self));
	publish_selection (self, GDK_SELECTION_PRIMARY);
}

static void
on_copy_activate (G_GNUC_UNUSED GtkMenuItem *item, gpointer user_data)
{
	publish_selection (STARDICT_VIEW (user_data), GDK_SELECTION_CLIPBOARD);
}

static gboolean
destroy_widget_idle_source_func (GtkWidget *widget)
{
	// The whole menu is deactivated /before/ any item is activated,
	// and a destroyed child item will not activate.
	gtk_widget_destroy (widget);
	return FALSE;
}

static GtkMenu *
make_context_menu (StardictView *self)
{
	GtkWidget *copy = gtk_menu_item_new_with_mnemonic ("_Copy");
	gtk_widget_set_sensitive (copy,
		self->selection_begin != self->selection_end);
	g_signal_connect_data (copy, "activate",
		G_CALLBACK (on_copy_activate), g_object_ref (self),
		(GClosureNotify) g_object_unref, 0);

	GtkWidget *menu = gtk_menu_new ();
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), copy);

	// As per GTK+ 3 Common Questions, 1.5.
	g_object_ref_sink (menu);
	g_signal_connect_swapped (menu, "deactivate",
		G_CALLBACK (g_idle_add), destroy_widget_idle_source_func);
	g_signal_connect (menu, "destroy",
		G_CALLBACK (g_object_unref), NULL);

	gtk_widget_show_all (menu);
	return GTK_MENU (menu);
}

static gboolean
stardict_view_button_press_event (GtkWidget *widget, GdkEventButton *event)
{
	StardictView *self = STARDICT_VIEW (widget);
	if (gdk_event_triggers_context_menu ((const GdkEvent *) event))
	{
		gtk_menu_popup_at_pointer (make_context_menu (self),
			(const GdkEvent *) event);
		return GDK_EVENT_STOP;
	}

	if (event->type == GDK_2BUTTON_PRESS && event->button == GDK_BUTTON_PRIMARY)
	{
		gtk_event_controller_reset (
			GTK_EVENT_CONTROLLER (self->selection_gesture));
		select_word_at (self, event->x, event->y);
		return GDK_EVENT_STOP;
	}

	if (event->type == GDK_3BUTTON_PRESS && event->button == GDK_BUTTON_PRIMARY)
	{
		gtk_event_controller_reset (
			GTK_EVENT_CONTROLLER (self->selection_gesture));
		select_all_at (self, event->x, event->y);
		return GDK_EVENT_STOP;
	}

	return GTK_WIDGET_CLASS (stardict_view_parent_class)
		->button_press_event (widget, event);
}

static gboolean
stardict_view_motion_notify_event (GtkWidget *widget, GdkEventMotion *event)
{
	StardictView *self = STARDICT_VIEW (widget);
	reset_hover_for_event (self, event->state, event->x, event->y);
	return GTK_WIDGET_CLASS (stardict_view_parent_class)
		->motion_notify_event (widget, event);
}

static gboolean
stardict_view_leave_notify_event
	(GtkWidget *widget, G_GNUC_UNUSED GdkEventCrossing *event)
{
	reset_hover (STARDICT_VIEW (widget));
	return GDK_EVENT_PROPAGATE;
}

static void
on_drag_begin (GtkGestureDrag *drag, G_GNUC_UNUSED gdouble start_x,
	G_GNUC_UNUSED gdouble start_y, gpointer user_data)
{
	GtkGesture *gesture = GTK_GESTURE (drag);
	GdkEventSequence *sequence
		= gtk_gesture_get_last_updated_sequence (gesture);

	GdkModifierType state = 0;
	const GdkEvent *last_event = gtk_gesture_get_last_event (gesture, sequence);
	(void) gdk_event_get_state (last_event, &state);
	if (state & gtk_accelerator_get_default_mod_mask ())
		gtk_gesture_set_sequence_state (gesture, sequence,
			GTK_EVENT_SEQUENCE_DENIED);
	else
	{
		gtk_gesture_set_sequence_state (gesture, sequence,
			GTK_EVENT_SEQUENCE_CLAIMED);
		STARDICT_VIEW (user_data)->drag_last_offset = 0;
	}
}

static void
on_drag_update (G_GNUC_UNUSED GtkGestureDrag *drag,
	G_GNUC_UNUSED gdouble offset_x, gdouble offset_y, gpointer user_data)
{
	StardictView *self = STARDICT_VIEW (user_data);
	self->top_offset += self->drag_last_offset - offset_y;
	adjust_for_offset (self);
	self->drag_last_offset = offset_y;
}

static gboolean
send_hover (StardictView *self)
{
	PangoLayout *layout = g_weak_ref_get (&self->hover);
	if (!layout)
		return FALSE;

	const char *text = pango_layout_get_text (layout);
	int len = strlen (text),
		s1 = MIN (self->hover_begin, self->hover_end),
		s2 = MAX (self->hover_begin, self->hover_end);
	if (s1 != s2 && s1 >= 0 && s1 <= len && s2 >= 0 && s2 <= len)
	{
		gchar *word = g_strndup (text + s1, s2 - s1);
		g_signal_emit (self, view_signals[SEND], 0, word);
		g_free (word);
	}
	g_object_unref (layout);
	return TRUE;
}

static void
on_select_begin (GtkGestureDrag *drag, gdouble start_x, gdouble start_y,
	gpointer user_data)
{
	// We probably don't need to check modifiers and mouse position again.
	StardictView *self = STARDICT_VIEW (user_data);
	GtkGesture *gesture = GTK_GESTURE (drag);
	if (send_hover (self))
	{
		gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_DENIED);
		return;
	}

	// Despite our two gestures not being grouped up, claiming one doesn't
	// deny the other, and :exclusive isn't the opposite of :touch-only.
	// A non-NULL sequence indicates a touch event.
	if (gtk_gesture_get_last_updated_sequence (gesture))
	{
		gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_DENIED);
		return;
	}

	g_weak_ref_set (&self->selection, NULL);
	self->selection_begin = -1;
	self->selection_end = -1;
	gtk_widget_queue_draw (GTK_WIDGET (self));

	int layout_x = start_x, layout_y = start_y;
	PangoLayout *layout = layout_at (self, &layout_x, &layout_y);
	if (!layout)
	{
		gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_DENIED);
		return;
	}

	g_weak_ref_set (&self->selection, layout);
	self->selection_end = self->selection_begin
		= layout_index_at (layout, layout_x, layout_y);
	gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_select_update (GtkGestureDrag *drag, gdouble offset_x, gdouble offset_y,
	gpointer user_data)
{
	GtkGesture *gesture = GTK_GESTURE (drag);
	StardictView *self = STARDICT_VIEW (user_data);
	PangoLayout *layout = g_weak_ref_get (&self->selection);
	if (!layout)
	{
		gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_DENIED);
		return;
	}

	double start_x = 0, start_y = 0;
	(void) gtk_gesture_drag_get_start_point (drag, &start_x, &start_y);

	int x = 0, y = 0;
	if (!layout_coords (self, layout, &x, &y))
	{
		g_warning ("internal error: weakly referenced layout not found");
		gtk_gesture_set_state (gesture, GTK_EVENT_SEQUENCE_DENIED);
		goto out;
	}

	self->selection_end = layout_index_at (layout,
		start_x + offset_x - x,
		start_y + offset_y - y);
	gtk_widget_queue_draw (GTK_WIDGET (self));

out:
	g_object_unref (layout);
}

static void
on_select_end (G_GNUC_UNUSED GtkGestureDrag *drag,
	G_GNUC_UNUSED gdouble offset_x, G_GNUC_UNUSED gdouble offset_y,
	gpointer user_data)
{
	publish_selection (STARDICT_VIEW (user_data), GDK_SELECTION_PRIMARY);
}

static void
stardict_view_class_init (StardictViewClass *klass)
{
	view_signals[SEND] = g_signal_new ("send",
		G_TYPE_FROM_CLASS (klass), 0, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_STRING);

	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = stardict_view_finalize;

	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
	widget_class->get_preferred_height = stardict_view_get_preferred_height;
	widget_class->get_preferred_width = stardict_view_get_preferred_width;
	widget_class->realize = stardict_view_realize;
	widget_class->map = stardict_view_map;
	widget_class->unmap = stardict_view_unmap;
	widget_class->draw = stardict_view_draw;
	widget_class->size_allocate = stardict_view_size_allocate;
	widget_class->screen_changed = stardict_view_screen_changed;
	widget_class->scroll_event = stardict_view_scroll_event;
	widget_class->button_press_event = stardict_view_button_press_event;
	widget_class->motion_notify_event = stardict_view_motion_notify_event;
	widget_class->leave_notify_event = stardict_view_leave_notify_event;

	gtk_widget_class_set_css_name (widget_class, "stardict-view");
}

static void
stardict_view_init (StardictView *self)
{
	g_weak_ref_init (&self->selection, NULL);
	self->selection_begin = -1;
	self->selection_end = -1;

	GtkGesture *drag = gtk_gesture_drag_new (GTK_WIDGET (self));
	gtk_gesture_single_set_touch_only (GTK_GESTURE_SINGLE (drag), TRUE);
	gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (drag),
		GTK_PHASE_TARGET);
	g_object_set_data_full (G_OBJECT (self), "stardict-view-drag-gesture",
		drag, g_object_unref);
	g_signal_connect (drag, "drag-begin",
		G_CALLBACK (on_drag_begin), self);
	g_signal_connect (drag, "drag-update",
		G_CALLBACK (on_drag_update), self);

	self->selection_gesture = gtk_gesture_drag_new (GTK_WIDGET (self));
	gtk_gesture_single_set_exclusive (
		GTK_GESTURE_SINGLE (self->selection_gesture), TRUE);
	gtk_event_controller_set_propagation_phase (
		GTK_EVENT_CONTROLLER (self->selection_gesture), GTK_PHASE_TARGET);
	g_signal_connect (self->selection_gesture, "drag-begin",
		G_CALLBACK (on_select_begin), self);
	g_signal_connect (self->selection_gesture, "drag-update",
		G_CALLBACK (on_select_update), self);
	g_signal_connect (self->selection_gesture, "drag-end",
		G_CALLBACK (on_select_end), self);
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
	g_return_if_fail (dict == NULL || STARDICT_IS_DICT (dict));

	// Update the minimum size, if appropriate (almost never)
	if (!self->dict != !dict)
		gtk_widget_queue_resize (GTK_WIDGET (self));

	g_clear_object (&self->dict);
	self->dict = dict ? g_object_ref (dict) : NULL;
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
