/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <cairo/cairo.h>

#include <libweston/libweston.h>
#include "xwayland.h"
#include "shared/helpers.h"

#ifdef WM_DEBUG
#define wm_log(...) weston_log(__VA_ARGS__)
#else
#define wm_log(...) do {} while (0)
#endif

struct xwm_selection_format {
	const char *atom_name;
	int atom_offset;
	const char *mime_type;
};

static const struct xwm_selection_format xwm_selection_formats[] = {
	{ "UTF8_STRING", offsetof(struct weston_wm, atom.utf8_string), "text/plain;charset=utf-8" },
	{ "TEXT",        offsetof(struct weston_wm, atom.text),         "text/plain;charset=utf-8" },
	{ "STRING",      offsetof(struct weston_wm, atom.string),       "STRING"                   },
	{ "image/bmp",   offsetof(struct weston_wm, atom.image_bmp),    "image/bmp"                },
	{ "image/png",   offsetof(struct weston_wm, atom.image_png),    "image/png"                },
	{ "text/rtf",    offsetof(struct weston_wm, atom.text_rtf),     "text/rtf"                 },
	{ "text/html",   offsetof(struct weston_wm, atom.text_html),    "text/html"                },
};

static xcb_atom_t
xwm_atom_at_offset(struct weston_wm *wm, int offset)
{
	return *(xcb_atom_t *)((char *)wm + offset);
}

static const struct xwm_selection_format *
xwm_selection_format_for_atom(struct weston_wm *wm, xcb_atom_t atom)
{
	unsigned i;

	for (i = 0; i < ARRAY_LENGTH(xwm_selection_formats); i++) {
		if (xwm_atom_at_offset(wm, xwm_selection_formats[i].atom_offset) == atom)
			return &xwm_selection_formats[i];
	}
	return NULL;
}

static const struct xwm_selection_format *
xwm_selection_format_for_mime_type(const char *mime_type)
{
	unsigned i;

	for (i = 0; i < ARRAY_LENGTH(xwm_selection_formats); i++) {
		if (strcmp(xwm_selection_formats[i].mime_type, mime_type) == 0)
			return &xwm_selection_formats[i];
	}
	return NULL;
}

struct png_write_closure {
	struct wl_array *data;
};

static cairo_status_t
png_write_func(void *closure, const unsigned char *data, unsigned int length)
{
	struct png_write_closure *wc = closure;
	void *p = wl_array_add(wc->data, length);

	if (!p)
		return CAIRO_STATUS_WRITE_ERROR;
	memcpy(p, data, length);
	return CAIRO_STATUS_SUCCESS;
}

static int
source_offers_mime_type(struct weston_wm *wm, const char *mime_type)
{
	struct weston_seat *seat = weston_wm_pick_seat(wm);
	struct weston_data_source *source;
	char **p;

	if (!seat || !seat->selection_data_source)
		return 0;

	source = seat->selection_data_source;
	wl_array_for_each(p, &source->mime_types) {
		if (strcmp(*p, mime_type) == 0)
			return 1;
	}
	return 0;
}

static int
convert_bmp_to_png_data(struct wl_array *source_data)
{
	unsigned char *bmp = source_data->data;
	size_t bmp_size = source_data->size;
	uint32_t data_offset;
	int32_t width, height;
	uint16_t bpp;
	uint32_t compression;
	int top_down, stride, x, y;
	size_t bmp_row_size;
	unsigned char *pixels;
	cairo_surface_t *surface;
	struct wl_array png_data;
	struct png_write_closure closure;
	cairo_status_t status;

	if (bmp_size < 54 || bmp[0] != 'B' || bmp[1] != 'M')
		return -1;

	memcpy(&data_offset, bmp + 10, 4);
	memcpy(&width, bmp + 18, 4);
	memcpy(&height, bmp + 22, 4);
	memcpy(&bpp, bmp + 28, 2);
	memcpy(&compression, bmp + 30, 4);

	top_down = height < 0;
	if (top_down)
		height = -height;

	if (width <= 0 || height <= 0)
		return -1;

	if (compression != 0 && compression != 3)
		return -1;

	if (bpp != 24 && bpp != 32)
		return -1;

	stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
	pixels = calloc(height, stride);
	if (!pixels)
		return -1;

	bmp_row_size = (((size_t)width * bpp / 8) + 3) & ~3;

	for (y = 0; y < height; y++) {
		int src_y = top_down ? y : (height - 1 - y);
		unsigned char *src_row = bmp + data_offset + src_y * bmp_row_size;
		uint32_t *dst_row = (uint32_t *)(pixels + y * stride);

		if (data_offset + (size_t)(src_y + 1) * bmp_row_size > bmp_size) {
			free(pixels);
			return -1;
		}

		for (x = 0; x < width; x++) {
			unsigned char b, g, r, a;

			if (bpp == 24) {
				b = src_row[x * 3];
				g = src_row[x * 3 + 1];
				r = src_row[x * 3 + 2];
				a = 255;
			} else {
				b = src_row[x * 4];
				g = src_row[x * 4 + 1];
				r = src_row[x * 4 + 2];
				a = src_row[x * 4 + 3];
			}
			dst_row[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
				     ((uint32_t)g << 8) | b;
		}
	}

	surface = cairo_image_surface_create_for_data(
		pixels, CAIRO_FORMAT_ARGB32, width, height, stride);

	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		free(pixels);
		return -1;
	}

	wl_array_init(&png_data);
	closure.data = &png_data;
	status = cairo_surface_write_to_png_stream(surface, png_write_func,
						   &closure);
	cairo_surface_destroy(surface);
	free(pixels);

	if (status != CAIRO_STATUS_SUCCESS) {
		wl_array_release(&png_data);
		return -1;
	}

	wl_array_release(source_data);
	*source_data = png_data;
	return 0;
}

static int
writable_callback(int fd, uint32_t mask, void *data)
{
	struct weston_wm *wm = data;
	unsigned char *property;
	int len, remainder;

	property = xcb_get_property_value(wm->property_reply);
	remainder = xcb_get_property_value_length(wm->property_reply) -
		wm->property_start;

	len = write(fd, property + wm->property_start, remainder);
	if (len == -1) {
		free(wm->property_reply);
		wm->property_reply = NULL;
		if (wm->property_source)
			wl_event_source_remove(wm->property_source);
		wm->property_source = NULL;
		weston_log("write error to target fd:%d start:%d reminder:%d %s\n",
			   fd, wm->property_start, remainder, strerror(errno));
		close(fd);
		wm->data_source_fd = -1;
		return 1;
	}

	weston_log("wrote fd:%d %d (chunk size %d) of %d bytes\n",
		fd, wm->property_start + len,
		len, xcb_get_property_value_length(wm->property_reply));

	wm->property_start += len;
	if (len == remainder) {
		free(wm->property_reply);
		wm->property_reply = NULL;
		if (wm->property_source)
			wl_event_source_remove(wm->property_source);
		wm->property_source = NULL;

		if (wm->incr) {
			xcb_delete_property(wm->conn,
					    wm->selection_window,
					    wm->atom.wl_selection);
		} else {
			weston_log("transfer write complete\n");
			close(fd);
			wm->data_source_fd = -1;
		}
	}

	return 1;
}

static void
weston_wm_write_property(struct weston_wm *wm, xcb_get_property_reply_t *reply)
{
	wm->property_start = 0;
	wm->property_reply = reply;
	writable_callback(wm->data_source_fd, WL_EVENT_WRITABLE, wm);

	if (wm->property_reply)
		wm->property_source =
			wl_event_loop_add_fd(wm->server->loop,
					     wm->data_source_fd,
					     WL_EVENT_WRITABLE,
					     writable_callback, wm);
}

static void
weston_wm_get_incr_chunk(struct weston_wm *wm)
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;
	FILE *fp;
	char *logstr;
	size_t logsize;

	cookie = xcb_get_property(wm->conn,
				  0, /* delete */
				  wm->selection_window,
				  wm->atom.wl_selection,
				  XCB_GET_PROPERTY_TYPE_ANY,
				  0, /* offset */
				  0x1fffffff /* length */);

	reply = xcb_get_property_reply(wm->conn, cookie, NULL);
	if (reply == NULL)
		return;

	fp = open_memstream(&logstr, &logsize);
	if (fp) {
		dump_property(fp, wm, wm->atom.wl_selection, reply);
		if (fclose(fp) == 0)
			wm_log("%s", logstr);
		free(logstr);
	}

	if (xcb_get_property_value_length(reply) > 0) {
		/* reply's ownership is transferred to wm, which is responsible
		 * for freeing it */
		weston_wm_write_property(wm, reply);
	} else {
		weston_log("transfer write complete (zero size reply)\n");
		close(wm->data_source_fd);
		wm->data_source_fd = -1;
		free(reply);
	}
}

struct x11_data_source {
	struct weston_data_source base;
	struct weston_wm *wm;
};

static void
data_source_accept(struct weston_data_source *source,
		   uint32_t time, const char *mime_type)
{
}

static void
data_source_send(struct weston_data_source *base,
		 const char *mime_type, int32_t fd)
{
	struct x11_data_source *source = (struct x11_data_source *) base;
	struct weston_wm *wm = source->wm;
	const struct xwm_selection_format *fmt;
	xcb_atom_t target;

	fmt = xwm_selection_format_for_mime_type(mime_type);
	if (!fmt) {
		weston_log("unhandled mime type for send: %s\n", mime_type);
		close(fd);
		return;
	}

	target = xwm_atom_at_offset(wm, fmt->atom_offset);
	xcb_convert_selection(wm->conn,
			      wm->selection_window,
			      wm->atom.clipboard,
			      target,
			      wm->atom.wl_selection,
			      XCB_TIME_CURRENT_TIME);

	xcb_flush(wm->conn);

	fcntl(fd, F_SETFL, O_WRONLY | O_NONBLOCK);
	wm->data_source_fd = fd;
}

static void
data_source_cancel(struct weston_data_source *source)
{
}

static void
weston_wm_get_selection_targets(struct weston_wm *wm)
{
	struct x11_data_source *source;
	struct weston_compositor *compositor;
	struct weston_seat *seat = weston_wm_pick_seat(wm);
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;
	xcb_atom_t *value;
	char **p;
	uint32_t i;
	FILE *fp;
	char *logstr;
	size_t logsize;

	if (!seat)
		return;

	cookie = xcb_get_property(wm->conn,
				  1, /* delete */
				  wm->selection_window,
				  wm->atom.wl_selection,
				  XCB_GET_PROPERTY_TYPE_ANY,
				  0, /* offset */
				  4096 /* length */);

	reply = xcb_get_property_reply(wm->conn, cookie, NULL);
	if (reply == NULL)
		return;

	fp = open_memstream(&logstr, &logsize);
	if (fp) {
		dump_property(fp, wm, wm->atom.wl_selection, reply);
		if (fclose(fp) == 0)
			wm_log("%s", logstr);
		free(logstr);
	}

	if (reply->type != XCB_ATOM_ATOM) {
		free(reply);
		return;
	}

	source = zalloc(sizeof *source);
	if (source == NULL) {
		free(reply);
		return;
	}

	wl_signal_init(&source->base.destroy_signal);
	source->base.accept = data_source_accept;
	source->base.send = data_source_send;
	source->base.cancel = data_source_cancel;
	source->wm = wm;

	wl_array_init(&source->base.mime_types);
	value = xcb_get_property_value(reply);
	for (i = 0; i < reply->value_len; i++) {
		const struct xwm_selection_format *fmt;
		int already_added = 0;

		fmt = xwm_selection_format_for_atom(wm, value[i]);
		if (!fmt)
			continue;

		wl_array_for_each(p, &source->base.mime_types) {
			if (strcmp(*p, fmt->mime_type) == 0) {
				already_added = 1;
				break;
			}
		}

		if (!already_added) {
			p = wl_array_add(&source->base.mime_types, sizeof *p);
			if (p)
				*p = strdup(fmt->mime_type);
		}
	}

	compositor = wm->server->compositor;
	weston_seat_set_selection(seat, &source->base,
				  wl_display_next_serial(compositor->wl_display));

	free(reply);
}

static void
weston_wm_get_selection_data(struct weston_wm *wm)
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;
	FILE *fp;
	char *logstr;
	size_t logsize;

	cookie = xcb_get_property(wm->conn,
				  1, /* delete */
				  wm->selection_window,
				  wm->atom.wl_selection,
				  XCB_GET_PROPERTY_TYPE_ANY,
				  0, /* offset */
				  0x1fffffff /* length */);

	reply = xcb_get_property_reply(wm->conn, cookie, NULL);

	fp = open_memstream(&logstr, &logsize);
	if (fp) {
		dump_property(fp, wm, wm->atom.wl_selection, reply);
		if (fclose(fp) == 0)
			wm_log("%s", logstr);
		free(logstr);
	}

	if (reply == NULL) {
		return;
	} else if (reply->type == wm->atom.incr) {
		wm->incr = 1;
		free(reply);
	} else {
		wm->incr = 0;
		/* reply's ownership is transferred to wm, which is responsible
		 * for freeing it */
		weston_wm_write_property(wm, reply);
	}
}

static void
weston_wm_handle_selection_notify(struct weston_wm *wm,
				xcb_generic_event_t *event)
{
	xcb_selection_notify_event_t *selection_notify =
		(xcb_selection_notify_event_t *) event;

	if (selection_notify->property == XCB_ATOM_NONE) {
		/* convert selection failed */
	} else if (selection_notify->target == wm->atom.targets) {
		weston_wm_get_selection_targets(wm);
	} else {
		weston_wm_get_selection_data(wm);
	}
}

static const size_t incr_chunk_size = 64 * 1024;

static void
weston_wm_send_selection_notify(struct weston_wm *wm, xcb_atom_t property)
{
	xcb_selection_notify_event_t selection_notify;

	memset(&selection_notify, 0, sizeof selection_notify);
	selection_notify.response_type = XCB_SELECTION_NOTIFY;
	selection_notify.sequence = 0;
	selection_notify.time = wm->selection_request.time;
	selection_notify.requestor = wm->selection_request.requestor;
	selection_notify.selection = wm->selection_request.selection;
	selection_notify.target = wm->selection_request.target;
	selection_notify.property = property;

	xcb_send_event(wm->conn, 0, /* propagate */
		       wm->selection_request.requestor,
		       XCB_EVENT_MASK_NO_EVENT, (char *) &selection_notify);

	xcb_flush(wm->conn);
}

static void
weston_wm_send_targets(struct weston_wm *wm)
{
	struct weston_seat *seat = weston_wm_pick_seat(wm);
	struct weston_data_source *source;
	xcb_atom_t targets[ARRAY_LENGTH(xwm_selection_formats) + 3];
	uint32_t num_targets = 0;
	char **mime_p;
	unsigned i, j;
	int has_bmp = 0, has_png = 0;

	targets[num_targets++] = wm->atom.timestamp;
	targets[num_targets++] = wm->atom.targets;

	if (seat && seat->selection_data_source) {
		source = seat->selection_data_source;
		wl_array_for_each(mime_p, &source->mime_types) {
			if (strcmp(*mime_p, "image/bmp") == 0)
				has_bmp = 1;
			if (strcmp(*mime_p, "image/png") == 0)
				has_png = 1;

			for (i = 0; i < ARRAY_LENGTH(xwm_selection_formats); i++) {
				xcb_atom_t atom;
				int duplicate = 0;

				if (strcmp(xwm_selection_formats[i].mime_type, *mime_p) != 0)
					continue;

				atom = xwm_atom_at_offset(wm, xwm_selection_formats[i].atom_offset);
				for (j = 0; j < num_targets; j++) {
					if (targets[j] == atom) {
						duplicate = 1;
						break;
					}
				}
				if (!duplicate && num_targets < ARRAY_LENGTH(targets))
					targets[num_targets++] = atom;
			}
		}

		if (has_bmp && !has_png && num_targets < ARRAY_LENGTH(targets))
			targets[num_targets++] = wm->atom.image_png;
	}

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->selection_request.requestor,
			    wm->selection_request.property,
			    XCB_ATOM_ATOM,
			    32,
			    num_targets, targets);

	weston_wm_send_selection_notify(wm, wm->selection_request.property);
}

static void
weston_wm_send_timestamp(struct weston_wm *wm)
{
	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->selection_request.requestor,
			    wm->selection_request.property,
			    XCB_ATOM_INTEGER,
			    32, /* format */
			    1, &wm->selection_timestamp);

	weston_wm_send_selection_notify(wm, wm->selection_request.property);
}

static int
weston_wm_flush_source_data(struct weston_wm *wm)
{
	int length;

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->selection_request.requestor,
			    wm->selection_request.property,
			    wm->selection_target,
			    8, /* format */
			    wm->source_data.size,
			    wm->source_data.data);
	wm->selection_property_set = 1;
	length = wm->source_data.size;
	wm->source_data.size = 0;

	return length;
}

static int
weston_wm_read_data_source(int fd, uint32_t mask, void *data)
{
	struct weston_wm *wm = data;
	int len, current, available;
	void *p;

	current = wm->source_data.size;
	if (wm->source_data.size < incr_chunk_size || wm->convert_bmp_to_png)
		p = wl_array_add(&wm->source_data, incr_chunk_size);
	else
		p = (char *) wm->source_data.data + wm->source_data.size;
	available = wm->source_data.alloc - current;

	len = read(fd, p, available);
	if (len == -1) {
		weston_log("read error from data source fd:%d %s\n",
			   fd, strerror(errno));
		weston_wm_send_selection_notify(wm, XCB_ATOM_NONE);
		if (wm->property_source)
			wl_event_source_remove(wm->property_source);
		wm->property_source = NULL;
		close(fd);
		wm->data_source_fd = -1;
		wl_array_release(&wm->source_data);
		wm->convert_bmp_to_png = 0;
		return 1;
	}

	weston_log("read fd:%d %d (available %d, mask 0x%x)\n",
		fd, len, available, mask);

	wm->source_data.size = current + len;

	if (wm->convert_bmp_to_png) {
		if (len == 0) {
			weston_log("bmp-to-png conversion: %zu bytes received\n",
				   wm->source_data.size);
			if (convert_bmp_to_png_data(&wm->source_data) != 0) {
				weston_log("BMP to PNG conversion failed\n");
				weston_wm_send_selection_notify(wm, XCB_ATOM_NONE);
			} else {
				weston_log("bmp-to-png conversion: %zu bytes png\n",
					   wm->source_data.size);
				weston_wm_flush_source_data(wm);
				weston_wm_send_selection_notify(wm,
					wm->selection_request.property);
			}
			if (wm->property_source)
				wl_event_source_remove(wm->property_source);
			wm->property_source = NULL;
			close(fd);
			wm->data_source_fd = -1;
			wl_array_release(&wm->source_data);
			wm->selection_request.requestor = XCB_NONE;
			wm->convert_bmp_to_png = 0;
		}
		return 1;
	}

	if (wm->source_data.size >= incr_chunk_size) {
		if (!wm->incr) {
			weston_log("got %zu bytes, starting incr\n",
				wm->source_data.size);
			wm->incr = 1;
			xcb_change_property(wm->conn,
					    XCB_PROP_MODE_REPLACE,
					    wm->selection_request.requestor,
					    wm->selection_request.property,
					    wm->atom.incr,
					    32, /* format */
					    1, &incr_chunk_size);
			wm->selection_property_set = 1;
			wm->flush_property_on_delete = 1;
			if (wm->property_source)
				wl_event_source_remove(wm->property_source);
			wm->property_source = NULL;
			weston_wm_send_selection_notify(wm, wm->selection_request.property);
		} else if (wm->selection_property_set) {
			weston_log("got %zu bytes, waiting for "
				"property delete\n", wm->source_data.size);

			wm->flush_property_on_delete = 1;
			if (wm->property_source)
				wl_event_source_remove(wm->property_source);
			wm->property_source = NULL;
		} else {
			weston_log("got %zu bytes, "
				"property deleted, setting new property\n",
				wm->source_data.size);
			weston_wm_flush_source_data(wm);
		}
	} else if (len == 0 && !wm->incr) {
		weston_log("non-incr transfer read complete\n");
		/* Non-incr transfer all done. */
		weston_wm_flush_source_data(wm);
		weston_wm_send_selection_notify(wm, wm->selection_request.property);
		if (wm->property_source)
			wl_event_source_remove(wm->property_source);
		wm->property_source = NULL;
		close(fd);
		wm->data_source_fd = -1;
		wl_array_release(&wm->source_data);
		wm->selection_request.requestor = XCB_NONE;
	} else if (len == 0 && wm->incr) {
		weston_log("incr transfer read complete\n");

		wm->flush_property_on_delete = 1;
		if (wm->selection_property_set) {
			weston_log("got %zu bytes, waiting for "
				"property delete\n", wm->source_data.size);
		} else {
			weston_log("got %zu bytes, "
				"property deleted, setting new property\n",
				wm->source_data.size);
			weston_wm_flush_source_data(wm);
		}
		xcb_flush(wm->conn);
		if (wm->property_source)
			wl_event_source_remove(wm->property_source);
		wm->property_source = NULL;
		close(fd);
		wm->data_source_fd = -1;
	} else {
		weston_log("nothing happened, buffered the bytes\n");
	}

	return 1;
}

static void
weston_wm_send_data(struct weston_wm *wm, xcb_atom_t target, const char *mime_type)
{
	struct weston_data_source *source;
	struct weston_seat *seat = weston_wm_pick_seat(wm);
	int p[2];

	if (wm->property_source) {
		weston_log("outstanding selection exist\n");
		return;
	}

	if (pipe2(p, O_CLOEXEC | O_NONBLOCK) == -1) {
		weston_log("pipe2 failed: %s\n", strerror(errno));
		weston_wm_send_selection_notify(wm, XCB_ATOM_NONE);
		return;
	}

	wl_array_init(&wm->source_data);
	wm->selection_target = target;
	wm->data_source_fd = p[0];
	wm->property_source = wl_event_loop_add_fd(wm->server->loop,
						   wm->data_source_fd,
						   WL_EVENT_READABLE,
						   weston_wm_read_data_source,
						   wm);

	source = seat->selection_data_source;
	source->send(source, mime_type, p[1]);
	//shouldn't closing pipe by done inside send()? See client_source_send()
	//close(p[1]);
}

static void
weston_wm_send_incr_chunk(struct weston_wm *wm)
{
	int length;

	weston_log("property deleted\n");

	wm->selection_property_set = 0;
	if (wm->flush_property_on_delete) {
		weston_log("setting new property, %zu bytes\n",
			wm->source_data.size);
		wm->flush_property_on_delete = 0;
		length = weston_wm_flush_source_data(wm);

		if (wm->data_source_fd >= 0) {
			wm->property_source =
				wl_event_loop_add_fd(wm->server->loop,
						     wm->data_source_fd,
						     WL_EVENT_READABLE,
						     weston_wm_read_data_source,
						     wm);
		} else if (length > 0) {
			/* Transfer is all done, but queue a flush for
			 * the delete of the last chunk so we can set
			 * the 0 sized property to signal the end of
			 * the transfer. */
			wm->flush_property_on_delete = 1;
			wl_array_release(&wm->source_data);
		} else {
			wm->selection_request.requestor = XCB_NONE;
		}
	}
}

static int
weston_wm_handle_selection_property_notify(struct weston_wm *wm,
					   xcb_generic_event_t *event)
{
	xcb_property_notify_event_t *property_notify =
		(xcb_property_notify_event_t *) event;

	if (property_notify->window == wm->selection_window) {
		if (property_notify->state == XCB_PROPERTY_NEW_VALUE &&
		    property_notify->atom == wm->atom.wl_selection &&
		    wm->incr)
			weston_wm_get_incr_chunk(wm);
		return 1;
	} else if (property_notify->window == wm->selection_request.requestor) {
		if (property_notify->state == XCB_PROPERTY_DELETE &&
		    property_notify->atom == wm->selection_request.property &&
		    wm->incr)
			weston_wm_send_incr_chunk(wm);
		return 1;
	}

	return 0;
}

static void
weston_wm_handle_selection_request(struct weston_wm *wm,
				 xcb_generic_event_t *event)
{
	xcb_selection_request_event_t *selection_request =
		(xcb_selection_request_event_t *) event;

	weston_log("selection request, %s, ",
		get_atom_name(wm->conn, selection_request->selection));
	weston_log_continue("target %s, ",
		get_atom_name(wm->conn, selection_request->target));
	weston_log_continue("property %s\n",
		get_atom_name(wm->conn, selection_request->property));

	assert(selection_request->requestor != wm->selection_window);
	wm->selection_request = *selection_request;
	wm->incr = 0;
	wm->flush_property_on_delete = 0;
	wm->convert_bmp_to_png = 0;

	if (selection_request->selection == wm->atom.clipboard_manager) {
		/* The weston clipboard should already have grabbed
		 * the first target, so just send selection notify
		 * now.  This isn't synchronized with the clipboard
		 * finishing getting the data, so there's a race here. */
		weston_wm_send_selection_notify(wm, wm->selection_request.property);
		return;
	}

	if (selection_request->target == wm->atom.targets) {
		weston_wm_send_targets(wm);
	} else if (selection_request->target == wm->atom.timestamp) {
		weston_wm_send_timestamp(wm);
	} else if (selection_request->target == wm->atom.image_png &&
		   !source_offers_mime_type(wm, "image/png") &&
		   source_offers_mime_type(wm, "image/bmp")) {
		wm->convert_bmp_to_png = 1;
		weston_wm_send_data(wm, wm->atom.image_png, "image/bmp");
	} else {
		const struct xwm_selection_format *fmt;

		fmt = xwm_selection_format_for_atom(wm, selection_request->target);
		if (fmt) {
			weston_wm_send_data(wm,
					    xwm_atom_at_offset(wm, fmt->atom_offset),
					    fmt->mime_type);
		} else {
			weston_log("unsupported selection target: %s\n",
				   get_atom_name(wm->conn, selection_request->target));
			weston_wm_send_selection_notify(wm, XCB_ATOM_NONE);
		}
	}
}

static int
weston_wm_handle_xfixes_selection_notify(struct weston_wm *wm,
				       xcb_generic_event_t *event)
{
	xcb_xfixes_selection_notify_event_t *xfixes_selection_notify =
		(xcb_xfixes_selection_notify_event_t *) event;
	struct weston_compositor *compositor;
	struct weston_seat *seat = weston_wm_pick_seat(wm);
	uint32_t serial;

	if (xfixes_selection_notify->selection != wm->atom.clipboard)
		return 0;

	weston_log("xfixes selection notify event: owner %d\n",
	       xfixes_selection_notify->owner);

	if (xfixes_selection_notify->owner == XCB_WINDOW_NONE) {
		if (!seat)
			return 1;

		if (wm->selection_owner != wm->selection_window) {
			/* A real X client selection went away, not our
			 * proxy selection.  Clear the wayland selection. */
			compositor = wm->server->compositor;
			serial = wl_display_next_serial(compositor->wl_display);
			weston_seat_set_selection(seat, NULL, serial);
		}

		wm->selection_owner = XCB_WINDOW_NONE;

		return 1;
	}

	wm->selection_owner = xfixes_selection_notify->owner;

	/* We have to use XCB_TIME_CURRENT_TIME when we claim the
	 * selection, so grab the actual timestamp here so we can
	 * answer TIMESTAMP conversion requests correctly. */
	if (xfixes_selection_notify->owner == wm->selection_window) {
		wm->selection_timestamp = xfixes_selection_notify->timestamp;
		weston_log("our window, skipping\n");
		return 1;
	}

	wm->incr = 0;
	xcb_convert_selection(wm->conn, wm->selection_window,
			      wm->atom.clipboard,
			      wm->atom.targets,
			      wm->atom.wl_selection,
			      xfixes_selection_notify->timestamp);

	xcb_flush(wm->conn);

	return 1;
}

int
weston_wm_handle_selection_event(struct weston_wm *wm,
				 xcb_generic_event_t *event)
{
	switch (event->response_type & ~0x80) {
	case XCB_SELECTION_NOTIFY:
		weston_wm_handle_selection_notify(wm, event);
		return 1;
	case XCB_PROPERTY_NOTIFY:
		return weston_wm_handle_selection_property_notify(wm, event);
	case XCB_SELECTION_REQUEST:
		weston_wm_handle_selection_request(wm, event);
		return 1;
	}

	switch (event->response_type - wm->xfixes->first_event) {
	case XCB_XFIXES_SELECTION_NOTIFY:
		return weston_wm_handle_xfixes_selection_notify(wm, event);
	}

	return 0;
}

static void
weston_wm_set_selection(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = data;
	struct weston_wm *wm =
		container_of(listener, struct weston_wm, selection_listener);
	struct weston_data_source *source = seat->selection_data_source;

	if (source == NULL) {
		if (wm->selection_owner == wm->selection_window)
			xcb_set_selection_owner(wm->conn,
						XCB_ATOM_NONE,
						wm->atom.clipboard,
						wm->selection_timestamp);
		return;
	}

	if (source->send == data_source_send)
		return;

	xcb_set_selection_owner(wm->conn,
				wm->selection_window,
				wm->atom.clipboard,
				XCB_TIME_CURRENT_TIME);

	xcb_flush(wm->conn);
}

static void
maybe_reassign_selection_seat(struct weston_wm *wm)
{
	struct weston_seat *seat;

	/* If we already have a seat, keep it */
	if (!wl_list_empty(&wm->selection_listener.link))
		return;

	seat = weston_wm_pick_seat(wm);
	if (!seat)
		return;

	wl_list_remove(&wm->selection_listener.link);
	wl_list_remove(&wm->seat_destroy_listener.link);

	wl_signal_add(&seat->selection_signal, &wm->selection_listener);
	wl_signal_add(&seat->destroy_signal, &wm->seat_destroy_listener);

	weston_wm_set_selection(&wm->selection_listener, seat);
}

static void
weston_wm_seat_created(struct wl_listener *listener, void *data)
{
	struct weston_wm *wm =
		container_of(listener, struct weston_wm, seat_create_listener);

	maybe_reassign_selection_seat(wm);
}

static void
weston_wm_seat_destroyed(struct wl_listener *listener, void *data)
{
	struct weston_wm *wm =
		container_of(listener, struct weston_wm, seat_destroy_listener);

	wl_list_remove(&wm->selection_listener.link);
	wl_list_init(&wm->selection_listener.link);

	wl_list_remove(&wm->seat_destroy_listener.link);
	wl_list_init(&wm->seat_destroy_listener.link);

	/* Try to pick another available seat to fall back to */
	maybe_reassign_selection_seat(wm);
}

void
weston_wm_selection_init(struct weston_wm *wm)
{
	uint32_t values[1], mask;

	wl_list_init(&wm->selection_listener.link);
	wl_list_init(&wm->seat_create_listener.link);
	wl_list_init(&wm->seat_destroy_listener.link);

	wm->selection_request.requestor = XCB_NONE;

	values[0] = XCB_EVENT_MASK_PROPERTY_CHANGE;
	wm->selection_window = xcb_generate_id(wm->conn);
	xcb_create_window(wm->conn,
			  XCB_COPY_FROM_PARENT,
			  wm->selection_window,
			  wm->screen->root,
			  0, 0,
			  10, 10,
			  0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  wm->screen->root_visual,
			  XCB_CW_EVENT_MASK, values);

	xcb_set_selection_owner(wm->conn,
				wm->selection_window,
				wm->atom.clipboard_manager,
				XCB_TIME_CURRENT_TIME);

	mask =
		XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
	xcb_xfixes_select_selection_input(wm->conn, wm->selection_window,
					  wm->atom.clipboard, mask);

	/* Try to set up a selection listener for any existing seat - we
	 * have a clipboard manager that can copy a subset of available
	 * selections so they don't disappear when the client owning
	 * them quits, but to make this work we need to have a seat
	 * to hang the selection off.
	 *
	 * If we have no seat or lose our seat we need to make sure we
	 * eventually assign a new one, so we listen for seat creation
	 * and destruction.
	 */
	wm->selection_listener.notify = weston_wm_set_selection;
	wm->seat_destroy_listener.notify = weston_wm_seat_destroyed;
	wm->seat_create_listener.notify = weston_wm_seat_created;
	wl_signal_add(&wm->server->compositor->seat_created_signal,
		      &wm->seat_create_listener);
	maybe_reassign_selection_seat(wm);
}
