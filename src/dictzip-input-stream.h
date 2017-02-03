/*
 * dictzip-input-stream.h: dictzip GIO stream reader
 *
 * Copyright (c) 2013, Přemysl Janouch <p.janouch@gmail.com>
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

#ifndef DICTZIP_INPUT_STREAM_H
#define DICTZIP_INPUT_STREAM_H

/// Random-access dictzip reader.
typedef struct dictzip_input_stream          DictzipInputStream;
typedef struct dictzip_input_stream_class    DictzipInputStreamClass;
typedef struct dictzip_input_stream_private  DictzipInputStreamPrivate;

// GObject boilerplate.
#define DICTZIP_TYPE_INPUT_STREAM  (dictzip_input_stream_get_type ())
#define DICTZIP_INPUT_STREAM(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST ((obj), \
	DICTZIP_TYPE_INPUT_STREAM, DictzipInputStream))
#define DICTZIP_IS_INPUT_STREAM(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE ((obj), \
	DICTZIP_TYPE_INPUT_STREAM))
#define DICTZIP_INPUT_STREAM_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_CAST ((klass), \
	DICTZIP_TYPE_INPUT_STREAM, DictzipInputStreamClass))
#define DICTZIP_IS_INPUT_STREAM_CLASS(klass) \
	(G_TYPE_CHECK_CLASS_TYPE ((klass), \
	DICTZIP_TYPE_INPUT_STREAM))
#define DICTZIP_INPUT_STREAM_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS ((obj), \
	DICTZIP_TYPE_INPUT_STREAM, DictzipInputStreamClass))

// --- Errors ------------------------------------------------------------------

typedef enum {
	DICTZIP_ERROR_NOT_SEEKABLE,        //!< Underlying stream isn't seekable
	DICTZIP_ERROR_INVALID_HEADER       //!< Error occured while parsing header
} DictzipError;

#define DICTZIP_ERROR  (dictzip_error_quark ())

GQuark dictzip_error_quark (void);

// --- DictzipInputStream ------------------------------------------------------

struct dictzip_input_stream
{
	GFilterInputStream parent_instance;
	DictzipInputStreamPrivate *priv;
};

struct dictzip_input_stream_class
{
	GFilterInputStreamClass parent_class;
};

GType dictzip_input_stream_get_type (void);
DictzipInputStream *dictzip_input_stream_new
	(GInputStream *base_stream, GError **error);
GFileInfo *dictzip_input_stream_get_file_info (DictzipInputStream *self);


#endif  // ! DICTZIP_INPUT_STREAM_H
