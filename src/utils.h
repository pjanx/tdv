/*
 * utils.h: miscellaneous utilities
 *
 * Copyright (c) 2013 - 2015, Přemysl Janouch <p.janouch@gmail.com>
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

#ifndef UTILS_H
#define UTILS_H

/// After this statement, the element has been found and its index is stored
/// in the variable "imid".
#define BINARY_SEARCH_BEGIN(max, compare)                                     \
	gint imin = 0, imax = max, imid;                                          \
	while (imin <= imax) {                                                    \
		imid = imin + (imax - imin) / 2;                                      \
		gint cmp = compare;                                                   \
		if      (cmp > 0) imin = imid + 1;                                    \
		else if (cmp < 0) imax = imid - 1;                                    \
		else {

/// After this statement, the binary search has failed and "imin" stores
/// the position where the element can be inserted.
#define BINARY_SEARCH_END                                                     \
		}                                                                     \
	}

gboolean stream_read_all (GByteArray *ba, GInputStream *is, GError **error);
gchar *stream_read_string (GDataInputStream *dis, GError **error);
gboolean xstrtoul (unsigned long *out, const char *s, int base);
void update_curses_terminal_size (void);

#endif  // ! UTILS_H
