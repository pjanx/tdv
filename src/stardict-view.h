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

#ifndef STARDICT_VIEW_H
#define STARDICT_VIEW_H

#include <gtk/gtk.h>

#include "stardict.h"

#define STARDICT_TYPE_VIEW  (stardict_view_get_type ())
G_DECLARE_FINAL_TYPE (StardictView, stardict_view, STARDICT, VIEW, GtkWidget)

GtkWidget *stardict_view_new (void);
void stardict_view_set_position (StardictView *view,
	StardictDict *dict, guint position);
void stardict_view_set_matched (StardictView *view, const gchar *matched);

#endif  // ! STARDICT_VIEW_H
