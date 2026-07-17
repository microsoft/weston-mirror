/*
 * Copyright © 2012 Openismus GmbH
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

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <libweston/libweston.h>
#include "weston.h"
#include "text-input-unstable-v1-server-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"
#include "input-method-unstable-v1-server-protocol.h"
#include "shared/helpers.h"
#include "shared/timespec-util.h"

struct text_input_manager;
struct input_method;
struct input_method_context;
struct text_backend;

enum text_input_protocol {
	TEXT_INPUT_PROTOCOL_V1,
	TEXT_INPUT_PROTOCOL_V3,
};

enum text_input_v3_activation {
	TEXT_INPUT_V3_ACTIVATION_NONE,
	TEXT_INPUT_V3_ACTIVATION_ENABLE,
	TEXT_INPUT_V3_ACTIVATION_DISABLE,
};

struct text_input_v3_state {
	char *surrounding_text;
	int32_t surrounding_cursor;
	int32_t surrounding_anchor;
	uint32_t content_hint;
	uint32_t content_purpose;
	uint32_t change_cause;
	pixman_box32_t cursor_rectangle;
	bool surrounding_text_set;
	bool content_type_set;
	bool cursor_rectangle_set;
	bool enabled;
};

struct text_input {
	struct wl_resource *resource;
	enum text_input_protocol protocol;

	struct weston_compositor *ec;

	struct wl_list input_methods;
	struct wl_list seat_link;
	struct weston_seat *seat;

	struct weston_surface *surface;

	pixman_box32_t cursor_rectangle;

	bool input_panel_visible;

	struct text_input_manager *manager;

	struct text_input_v3_state current_state;
	struct text_input_v3_state pending_state;
	enum text_input_v3_activation pending_activation;
	uint32_t commit_serial;
	int32_t preedit_cursor;
	uint32_t delete_before;
	uint32_t delete_after;
	bool delete_pending;
};

struct text_input_manager {
	struct wl_global *text_input_manager_v1_global;
	struct wl_global *text_input_manager_v3_global;
	struct wl_listener destroy_listener;

	struct text_input *current_text_input;

	struct weston_compositor *ec;
};

struct input_method {
	struct wl_resource *input_method_binding;
	struct wl_global *input_method_global;
	struct wl_listener destroy_listener;

	struct weston_seat *seat;
	struct text_input *input;

	struct wl_list link;

	struct wl_listener keyboard_focus_listener;

	bool focus_listener_initialized;

	struct input_method_context *context;
	struct wl_list text_inputs_v3;

	struct text_backend *text_backend;
};

struct input_method_context {
	struct wl_resource *resource;

	struct text_input *input;
	struct input_method *input_method;

	struct wl_resource *keyboard;
};

struct text_backend {
	struct weston_compositor *compositor;

	struct {
		char *path;
		struct wl_client *client;

		unsigned deathcount;
		struct timespec deathstamp;
	} input_method;

	struct wl_listener client_listener;
	struct wl_listener seat_created_listener;
};

static void
input_method_context_create(struct text_input *input,
			    struct input_method *input_method);
static void
input_method_context_end_keyboard_grab(struct input_method_context *context);

static void
input_method_init_seat(struct weston_seat *seat);

static void
text_input_v3_state_reset(struct text_input_v3_state *state)
{
	free(state->surrounding_text);
	memset(state, 0, sizeof *state);
	state->change_cause = ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD;
}

static bool
text_input_v3_state_copy(struct text_input_v3_state *dst,
			 const struct text_input_v3_state *src)
{
	char *surrounding_text = NULL;

	if (src->surrounding_text) {
		surrounding_text = strdup(src->surrounding_text);
		if (!surrounding_text)
			return false;
	}

	text_input_v3_state_reset(dst);
	*dst = *src;
	dst->surrounding_text = surrounding_text;
	return true;
}

static uint32_t
text_input_v3_map_content_purpose(uint32_t purpose)
{
	switch (purpose) {
	case ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PIN:
		return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_PASSWORD;
	case ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DATE:
		return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_DATE;
	case ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TIME:
		return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_TIME;
	case ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_DATETIME:
		return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_DATETIME;
	case ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_TERMINAL:
		return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_TERMINAL;
	default:
		if (purpose <= ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD)
			return purpose;
		return ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_NORMAL;
	}
}

static void
deactivate_input_method(struct input_method *input_method)
{
	struct text_input *text_input = input_method->input;
	struct weston_compositor *ec = text_input->ec;

	if (input_method->context && input_method->input_method_binding) {
		input_method_context_end_keyboard_grab(input_method->context);
		zwp_input_method_v1_send_deactivate(
			input_method->input_method_binding,
			input_method->context->resource);
		input_method->context->input = NULL;
	}

	wl_list_remove(&input_method->link);
	input_method->input = NULL;
	input_method->context = NULL;

	if (wl_list_empty(&text_input->input_methods) &&
	    text_input->input_panel_visible &&
	    text_input->manager->current_text_input == text_input) {
		wl_signal_emit(&ec->hide_input_panel_signal, ec);
		text_input->input_panel_visible = false;
	}

	if (text_input->manager->current_text_input == text_input)
		text_input->manager->current_text_input = NULL;

	if (text_input->protocol == TEXT_INPUT_PROTOCOL_V1)
		zwp_text_input_v1_send_leave(text_input->resource);
}

static void
destroy_text_input(struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next,
			      &text_input->input_methods, link)
		deactivate_input_method(input_method);

	if (text_input->protocol == TEXT_INPUT_PROTOCOL_V3) {
		wl_list_remove(&text_input->seat_link);
		text_input_v3_state_reset(&text_input->current_state);
		text_input_v3_state_reset(&text_input->pending_state);
	}

	free(text_input);
}

static void
text_input_set_surrounding_text(struct wl_client *client,
				struct wl_resource *resource,
				const char *text,
				uint32_t cursor,
				uint32_t anchor)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next,
			      &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		zwp_input_method_context_v1_send_surrounding_text(
			input_method->context->resource, text, cursor, anchor);
	}
}

static void
activate_input_method(struct text_input *text_input,
		      struct input_method *input_method,
		      struct weston_surface *surface)
{
	struct weston_compositor *ec = text_input->ec;
	struct text_input *current;

	if (input_method->input == text_input)
		return;

	if (input_method->input)
		deactivate_input_method(input_method);

	input_method->input = text_input;
	wl_list_insert(&text_input->input_methods, &input_method->link);
	input_method_init_seat(input_method->seat);

	text_input->surface = surface;

	input_method_context_create(text_input, input_method);

	current = text_input->manager->current_text_input;

	if (current && current != text_input) {
		current->input_panel_visible = false;
		wl_signal_emit(&ec->hide_input_panel_signal, ec);
	}

	if (text_input->protocol == TEXT_INPUT_PROTOCOL_V3)
		text_input->input_panel_visible = true;

	if (text_input->input_panel_visible && text_input->surface) {
		wl_signal_emit(&ec->show_input_panel_signal,
			       text_input->surface);
		wl_signal_emit(&ec->update_input_panel_signal,
			       &text_input->cursor_rectangle);
	}
	text_input->manager->current_text_input = text_input;
}

static void
text_input_activate(struct wl_client *client,
		    struct wl_resource *resource,
		    struct wl_resource *seat,
		    struct wl_resource *surface)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct weston_seat *weston_seat = wl_resource_get_user_data(seat);
	struct weston_surface *weston_surface = wl_resource_get_user_data(surface);

	if (!weston_seat)
		return;
	if (weston_seat->input_method->input == text_input)
		return;

	activate_input_method(text_input, weston_seat->input_method,
			      weston_surface);

	zwp_text_input_v1_send_enter(text_input->resource,
				     weston_surface->resource);
}

static void
text_input_deactivate(struct wl_client *client,
		      struct wl_resource *resource,
		      struct wl_resource *seat)
{
	struct weston_seat *weston_seat = wl_resource_get_user_data(seat);

	if (weston_seat && weston_seat->input_method->input)
		deactivate_input_method(weston_seat->input_method);
}

static void
text_input_reset(struct wl_client *client,
		 struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next,
			      &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		zwp_input_method_context_v1_send_reset(
			input_method->context->resource);
	}
}

static void
text_input_set_cursor_rectangle(struct wl_client *client,
				struct wl_resource *resource,
				int32_t x,
				int32_t y,
				int32_t width,
				int32_t height)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct weston_compositor *ec = text_input->ec;

	text_input->cursor_rectangle.x1 = x;
	text_input->cursor_rectangle.y1 = y;
	text_input->cursor_rectangle.x2 = x + width;
	text_input->cursor_rectangle.y2 = y + height;

	wl_signal_emit(&ec->update_input_panel_signal,
		       &text_input->cursor_rectangle);
}

static void
text_input_set_content_type(struct wl_client *client,
			    struct wl_resource *resource,
			    uint32_t hint,
			    uint32_t purpose)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next,
			      &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		zwp_input_method_context_v1_send_content_type(
			input_method->context->resource, hint, purpose);
	}
}

static void
text_input_invoke_action(struct wl_client *client,
			 struct wl_resource *resource,
			 uint32_t button,
			 uint32_t index)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next,
			      &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		zwp_input_method_context_v1_send_invoke_action(
			input_method->context->resource, button, index);
	}
}

static void
text_input_commit_state(struct wl_client *client,
			struct wl_resource *resource,
			uint32_t serial)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next,
			      &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		zwp_input_method_context_v1_send_commit_state(
			input_method->context->resource, serial);
	}
}

static void
text_input_show_input_panel(struct wl_client *client,
			    struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct weston_compositor *ec = text_input->ec;

	text_input->input_panel_visible = true;

	if (!wl_list_empty(&text_input->input_methods) &&
	    text_input == text_input->manager->current_text_input) {
		wl_signal_emit(&ec->show_input_panel_signal,
			       text_input->surface);
		wl_signal_emit(&ec->update_input_panel_signal,
			       &text_input->cursor_rectangle);
	}
}

static void
text_input_hide_input_panel(struct wl_client *client,
			    struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct weston_compositor *ec = text_input->ec;

	text_input->input_panel_visible = false;

	if (!wl_list_empty(&text_input->input_methods) &&
	    text_input == text_input->manager->current_text_input)
		wl_signal_emit(&ec->hide_input_panel_signal, ec);
}

static void
text_input_set_preferred_language(struct wl_client *client,
				  struct wl_resource *resource,
				  const char *language)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method, *next;

	wl_list_for_each_safe(input_method, next,
			      &text_input->input_methods, link) {
		if (!input_method->context)
			continue;
		zwp_input_method_context_v1_send_preferred_language(
			input_method->context->resource, language);
	}
}

static const struct zwp_text_input_v1_interface text_input_implementation = {
	text_input_activate,
	text_input_deactivate,
	text_input_show_input_panel,
	text_input_hide_input_panel,
	text_input_reset,
	text_input_set_surrounding_text,
	text_input_set_content_type,
	text_input_set_cursor_rectangle,
	text_input_set_preferred_language,
	text_input_commit_state,
	text_input_invoke_action
};

static void text_input_manager_create_text_input(struct wl_client *client,
						 struct wl_resource *resource,
						 uint32_t id)
{
	struct text_input_manager *text_input_manager =
		wl_resource_get_user_data(resource);
	struct text_input *text_input;

	text_input = zalloc(sizeof *text_input);
	if (text_input == NULL)
		return;

	text_input->resource =
		wl_resource_create(client, &zwp_text_input_v1_interface, 1, id);
	wl_resource_set_implementation(text_input->resource,
				       &text_input_implementation,
				       text_input, destroy_text_input);

	text_input->ec = text_input_manager->ec;
	text_input->manager = text_input_manager;

	wl_list_init(&text_input->input_methods);
};

static const struct zwp_text_input_manager_v1_interface manager_implementation = {
	text_input_manager_create_text_input
};

static void
bind_text_input_manager(struct wl_client *client,
			void *data,
			uint32_t version,
			uint32_t id)
{
	struct text_input_manager *text_input_manager = data;
	struct wl_resource *resource;

	/* No checking for duplicate binding necessary.  */
	resource =
		wl_resource_create(client,
				   &zwp_text_input_manager_v1_interface, 1, id);
	if (resource)
		wl_resource_set_implementation(resource,
					       &manager_implementation,
					       text_input_manager, NULL);
}

static void
text_input_v3_send_state(struct text_input *text_input)
{
	struct input_method *input_method, *next;
	struct text_input_v3_state *state = &text_input->current_state;

	wl_list_for_each_safe(input_method, next,
			      &text_input->input_methods, link) {
		if (!input_method->context)
			continue;

		if (state->surrounding_text_set)
			zwp_input_method_context_v1_send_surrounding_text(
				input_method->context->resource,
				state->surrounding_text,
				state->surrounding_cursor,
				state->surrounding_anchor);

		if (state->content_type_set)
			zwp_input_method_context_v1_send_content_type(
				input_method->context->resource,
				state->content_hint,
				text_input_v3_map_content_purpose(
					state->content_purpose));

		zwp_input_method_context_v1_send_commit_state(
			input_method->context->resource,
			text_input->commit_serial);
	}

	if (state->cursor_rectangle_set) {
		text_input->cursor_rectangle = state->cursor_rectangle;
		wl_signal_emit(&text_input->ec->update_input_panel_signal,
			       &text_input->cursor_rectangle);
	}
}

static void
text_input_v3_destroy(struct wl_client *client,
		      struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
text_input_v3_enable(struct wl_client *client,
		     struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);

	text_input_v3_state_reset(&text_input->pending_state);
	text_input->pending_state.enabled = true;
	text_input->pending_activation = TEXT_INPUT_V3_ACTIVATION_ENABLE;
}

static void
text_input_v3_disable(struct wl_client *client,
		      struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);

	text_input_v3_state_reset(&text_input->pending_state);
	text_input->pending_activation = TEXT_INPUT_V3_ACTIVATION_DISABLE;
}

static void
text_input_v3_set_surrounding_text(struct wl_client *client,
				   struct wl_resource *resource,
				   const char *text,
				   int32_t cursor,
				   int32_t anchor)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	char *surrounding_text;
	size_t length = strlen(text);

	if (cursor < 0 || anchor < 0 ||
	    (size_t) cursor > length || (size_t) anchor > length)
		return;

	surrounding_text = strdup(text);
	if (!surrounding_text) {
		wl_client_post_no_memory(client);
		return;
	}

	free(text_input->pending_state.surrounding_text);
	text_input->pending_state.surrounding_text = surrounding_text;
	text_input->pending_state.surrounding_cursor = cursor;
	text_input->pending_state.surrounding_anchor = anchor;
	text_input->pending_state.surrounding_text_set = true;
}

static void
text_input_v3_set_text_change_cause(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t cause)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);

	text_input->pending_state.change_cause = cause;
}

static void
text_input_v3_set_content_type(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t hint,
			       uint32_t purpose)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);

	text_input->pending_state.content_hint = hint;
	text_input->pending_state.content_purpose = purpose;
	text_input->pending_state.content_type_set = true;
}

static void
text_input_v3_set_cursor_rectangle(struct wl_client *client,
				   struct wl_resource *resource,
				   int32_t x,
				   int32_t y,
				   int32_t width,
				   int32_t height)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct text_input_v3_state *state = &text_input->pending_state;

	state->cursor_rectangle.x1 = x;
	state->cursor_rectangle.y1 = y;
	state->cursor_rectangle.x2 = x + width;
	state->cursor_rectangle.y2 = y + height;
	state->cursor_rectangle_set = true;
}

static void
text_input_v3_commit(struct wl_client *client,
		     struct wl_resource *resource)
{
	struct text_input *text_input = wl_resource_get_user_data(resource);
	struct input_method *input_method = text_input->seat->input_method;
	enum text_input_v3_activation activation =
		text_input->pending_activation;

	text_input->commit_serial++;
	text_input_v3_state_reset(&text_input->current_state);
	text_input->current_state = text_input->pending_state;
	memset(&text_input->pending_state, 0,
	       sizeof text_input->pending_state);
	if (!text_input_v3_state_copy(&text_input->pending_state,
				      &text_input->current_state)) {
		wl_client_post_no_memory(client);
		return;
	}
	text_input->pending_state.change_cause =
		ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_INPUT_METHOD;
	text_input->pending_activation = TEXT_INPUT_V3_ACTIVATION_NONE;

	if (activation != TEXT_INPUT_V3_ACTIVATION_NONE &&
	    input_method->input == text_input)
		deactivate_input_method(input_method);

	if (activation == TEXT_INPUT_V3_ACTIVATION_ENABLE &&
	    text_input->surface)
		activate_input_method(text_input, input_method,
				      text_input->surface);

	if (input_method->input == text_input)
		text_input_v3_send_state(text_input);
}

static const struct zwp_text_input_v3_interface text_input_v3_implementation = {
	text_input_v3_destroy,
	text_input_v3_enable,
	text_input_v3_disable,
	text_input_v3_set_surrounding_text,
	text_input_v3_set_text_change_cause,
	text_input_v3_set_content_type,
	text_input_v3_set_cursor_rectangle,
	text_input_v3_commit,
};

static void
text_input_manager_v3_destroy(struct wl_client *client,
			      struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
text_input_manager_v3_get_text_input(struct wl_client *client,
				     struct wl_resource *resource,
				     uint32_t id,
				     struct wl_resource *seat_resource)
{
	struct text_input_manager *manager = wl_resource_get_user_data(resource);
	struct weston_seat *seat = wl_resource_get_user_data(seat_resource);
	struct weston_keyboard *keyboard;
	struct text_input *text_input;

	if (!seat)
		return;

	text_input = zalloc(sizeof *text_input);
	if (!text_input) {
		wl_client_post_no_memory(client);
		return;
	}

	text_input->resource = wl_resource_create(
		client, &zwp_text_input_v3_interface, 1, id);
	if (!text_input->resource) {
		free(text_input);
		return;
	}

	text_input->protocol = TEXT_INPUT_PROTOCOL_V3;
	text_input->ec = manager->ec;
	text_input->manager = manager;
	text_input->seat = seat;
	text_input->preedit_cursor = -1;
	wl_list_init(&text_input->input_methods);
	wl_list_insert(&seat->input_method->text_inputs_v3,
		       &text_input->seat_link);
	text_input_v3_state_reset(&text_input->current_state);
	text_input_v3_state_reset(&text_input->pending_state);
	wl_resource_set_implementation(text_input->resource,
				       &text_input_v3_implementation,
				       text_input, destroy_text_input);

	input_method_init_seat(seat);
	keyboard = weston_seat_get_keyboard(seat);
	if (keyboard && keyboard->focus && keyboard->focus->resource &&
	    wl_resource_get_client(keyboard->focus->resource) == client) {
		text_input->surface = keyboard->focus;
		zwp_text_input_v3_send_enter(text_input->resource,
					     text_input->surface->resource);
	}
}

static const struct zwp_text_input_manager_v3_interface
text_input_manager_v3_implementation = {
	text_input_manager_v3_destroy,
	text_input_manager_v3_get_text_input,
};

static void
bind_text_input_manager_v3(struct wl_client *client,
			   void *data,
			   uint32_t version,
			   uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &zwp_text_input_manager_v3_interface,
				      1, id);
	if (resource)
		wl_resource_set_implementation(
			resource, &text_input_manager_v3_implementation,
			data, NULL);
}

static void
text_input_manager_notifier_destroy(struct wl_listener *listener, void *data)
{
	struct text_input_manager *text_input_manager =
		container_of(listener,
			     struct text_input_manager,
			     destroy_listener);

	wl_list_remove(&text_input_manager->destroy_listener.link);
	wl_global_destroy(text_input_manager->text_input_manager_v1_global);
	wl_global_destroy(text_input_manager->text_input_manager_v3_global);

	free(text_input_manager);
}

static void
text_input_manager_create(struct weston_compositor *ec)
{
	struct text_input_manager *text_input_manager;

	text_input_manager = zalloc(sizeof *text_input_manager);
	if (text_input_manager == NULL)
		return;

	text_input_manager->ec = ec;

	text_input_manager->text_input_manager_v1_global =
		wl_global_create(ec->wl_display,
				 &zwp_text_input_manager_v1_interface, 1,
				 text_input_manager, bind_text_input_manager);
	text_input_manager->text_input_manager_v3_global =
		wl_global_create(ec->wl_display,
				 &zwp_text_input_manager_v3_interface, 1,
				 text_input_manager, bind_text_input_manager_v3);

	text_input_manager->destroy_listener.notify =
		text_input_manager_notifier_destroy;
	wl_signal_add(&ec->destroy_signal,
		      &text_input_manager->destroy_listener);
}

static void
input_method_context_destroy(struct wl_client *client,
			     struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
input_method_context_commit_string(struct wl_client *client,
				   struct wl_resource *resource,
				   uint32_t serial,
				   const char *text)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (!context->input)
		return;

	if (context->input->protocol == TEXT_INPUT_PROTOCOL_V1) {
		zwp_text_input_v1_send_commit_string(context->input->resource,
						     serial, text);
		return;
	}

	if (serial != context->input->commit_serial) {
		context->input->delete_pending = false;
		context->input->preedit_cursor = -1;
		return;
	}

	if (context->input->delete_pending) {
		zwp_text_input_v3_send_delete_surrounding_text(
			context->input->resource,
			context->input->delete_before,
			context->input->delete_after);
		context->input->delete_pending = false;
	}
	zwp_text_input_v3_send_commit_string(context->input->resource, text);
	zwp_text_input_v3_send_done(context->input->resource, serial);
	context->input->preedit_cursor = -1;
}

static void
input_method_context_preedit_string(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t serial,
				    const char *text,
				    const char *commit)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (!context->input)
		return;

	if (context->input->protocol == TEXT_INPUT_PROTOCOL_V1) {
		zwp_text_input_v1_send_preedit_string(context->input->resource,
						      serial, text, commit);
		return;
	}

	if (serial != context->input->commit_serial) {
		context->input->delete_pending = false;
		context->input->preedit_cursor = -1;
		return;
	}

	zwp_text_input_v3_send_preedit_string(context->input->resource, text,
					      context->input->preedit_cursor,
					      context->input->preedit_cursor);
	zwp_text_input_v3_send_done(context->input->resource, serial);
	context->input->preedit_cursor = -1;
}

static void
input_method_context_preedit_styling(struct wl_client *client,
				     struct wl_resource *resource,
				     uint32_t index,
				     uint32_t length,
				     uint32_t style)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (context->input &&
	    context->input->protocol == TEXT_INPUT_PROTOCOL_V1)
		zwp_text_input_v1_send_preedit_styling(context->input->resource,
						       index, length, style);
}

static void
input_method_context_preedit_cursor(struct wl_client *client,
				    struct wl_resource *resource,
				    int32_t cursor)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (!context->input)
		return;

	if (context->input->protocol == TEXT_INPUT_PROTOCOL_V3) {
		context->input->preedit_cursor = cursor;
		return;
	}

	if (context->input)
		zwp_text_input_v1_send_preedit_cursor(context->input->resource,
						      cursor);
}

static void
input_method_context_delete_surrounding_text(struct wl_client *client,
					     struct wl_resource *resource,
					     int32_t index,
					     uint32_t length)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (!context->input)
		return;

	if (context->input->protocol == TEXT_INPUT_PROTOCOL_V3) {
		int64_t end = (int64_t) index + length;
		int64_t before = -(int64_t) index;

		if (index <= 0 && end >= 0 &&
		    before <= UINT32_MAX && end <= UINT32_MAX) {
			context->input->delete_before = before;
			context->input->delete_after = end;
			context->input->delete_pending = true;
		}
		return;
	}

	if (context->input)
		zwp_text_input_v1_send_delete_surrounding_text(
			context->input->resource, index, length);
}

static void
input_method_context_cursor_position(struct wl_client *client,
				     struct wl_resource *resource,
				     int32_t index,
				     int32_t anchor)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (context->input &&
	    context->input->protocol == TEXT_INPUT_PROTOCOL_V1)
		zwp_text_input_v1_send_cursor_position(context->input->resource,
						       index, anchor);
}

static void
input_method_context_modifiers_map(struct wl_client *client,
				   struct wl_resource *resource,
				   struct wl_array *map)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (context->input &&
	    context->input->protocol == TEXT_INPUT_PROTOCOL_V1)
		zwp_text_input_v1_send_modifiers_map(context->input->resource,
						     map);
}

static void
input_method_context_keysym(struct wl_client *client,
			    struct wl_resource *resource,
			    uint32_t serial,
			    uint32_t time,
			    uint32_t sym,
			    uint32_t state,
			    uint32_t modifiers)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (context->input &&
	    context->input->protocol == TEXT_INPUT_PROTOCOL_V1)
		zwp_text_input_v1_send_keysym(context->input->resource,
					      serial, time,
					      sym, state, modifiers);
}

static void
unbind_keyboard(struct wl_resource *resource)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	input_method_context_end_keyboard_grab(context);
	context->keyboard = NULL;
}

static void
input_method_context_grab_key(struct weston_keyboard_grab *grab,
			      const struct timespec *time, uint32_t key,
			      uint32_t state_w)
{
	struct weston_keyboard *keyboard = grab->keyboard;
	struct wl_display *display;
	uint32_t serial;
	uint32_t msecs;

	if (!keyboard->input_method_resource)
		return;

	display = wl_client_get_display(
		wl_resource_get_client(keyboard->input_method_resource));
	serial = wl_display_next_serial(display);
	msecs = timespec_to_msec(time);
	wl_keyboard_send_key(keyboard->input_method_resource,
			     serial, msecs, key, state_w);
}

static void
input_method_context_grab_modifier(struct weston_keyboard_grab *grab,
				   uint32_t serial,
				   uint32_t mods_depressed,
				   uint32_t mods_latched,
				   uint32_t mods_locked,
				   uint32_t group)
{
	struct weston_keyboard *keyboard = grab->keyboard;

	if (!keyboard->input_method_resource)
		return;

	wl_keyboard_send_modifiers(keyboard->input_method_resource,
				   serial, mods_depressed, mods_latched,
				   mods_locked, group);
}

static void
input_method_context_grab_cancel(struct weston_keyboard_grab *grab)
{
	weston_keyboard_end_grab(grab->keyboard);
}

static const struct weston_keyboard_grab_interface input_method_context_grab = {
	input_method_context_grab_key,
	input_method_context_grab_modifier,
	input_method_context_grab_cancel,
};

static void
input_method_context_grab_keyboard(struct wl_client *client,
				   struct wl_resource *resource,
				   uint32_t id)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);
	struct wl_resource *cr;
	struct weston_seat *seat = context->input_method->seat;
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);

	cr = wl_resource_create(client, &wl_keyboard_interface, 1, id);
	wl_resource_set_implementation(cr, NULL, context, unbind_keyboard);

	context->keyboard = cr;

	weston_keyboard_send_keymap(keyboard, cr);

	if (keyboard->grab != &keyboard->default_grab) {
		weston_keyboard_end_grab(keyboard);
	}
	weston_keyboard_start_grab(keyboard, &keyboard->input_method_grab);
	keyboard->input_method_resource = cr;
}

static void
input_method_context_key(struct wl_client *client,
			 struct wl_resource *resource,
			 uint32_t serial,
			 uint32_t time,
			 uint32_t key,
			 uint32_t state_w)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);
	struct weston_seat *seat = context->input_method->seat;
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);
	struct weston_keyboard_grab *default_grab = &keyboard->default_grab;
	struct timespec ts;

	timespec_from_msec(&ts, time);

	default_grab->interface->key(default_grab, &ts, key, state_w);
}

static void
input_method_context_modifiers(struct wl_client *client,
			       struct wl_resource *resource,
			       uint32_t serial,
			       uint32_t mods_depressed,
			       uint32_t mods_latched,
			       uint32_t mods_locked,
			       uint32_t group)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	struct weston_seat *seat = context->input_method->seat;
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);
	struct weston_keyboard_grab *default_grab = &keyboard->default_grab;

	default_grab->interface->modifiers(default_grab,
					   serial, mods_depressed,
					   mods_latched, mods_locked,
					   group);
}

static void
input_method_context_language(struct wl_client *client,
			      struct wl_resource *resource,
			      uint32_t serial,
			      const char *language)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (context->input &&
	    context->input->protocol == TEXT_INPUT_PROTOCOL_V1)
		zwp_text_input_v1_send_language(context->input->resource,
						serial, language);
}

static void
input_method_context_text_direction(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t serial,
				    uint32_t direction)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (context->input &&
	    context->input->protocol == TEXT_INPUT_PROTOCOL_V1)
		zwp_text_input_v1_send_text_direction(context->input->resource,
						      serial, direction);
}


static const struct zwp_input_method_context_v1_interface context_implementation = {
	input_method_context_destroy,
	input_method_context_commit_string,
	input_method_context_preedit_string,
	input_method_context_preedit_styling,
	input_method_context_preedit_cursor,
	input_method_context_delete_surrounding_text,
	input_method_context_cursor_position,
	input_method_context_modifiers_map,
	input_method_context_keysym,
	input_method_context_grab_keyboard,
	input_method_context_key,
	input_method_context_modifiers,
	input_method_context_language,
	input_method_context_text_direction
};

static void
destroy_input_method_context(struct wl_resource *resource)
{
	struct input_method_context *context =
		wl_resource_get_user_data(resource);

	if (context->keyboard)
		wl_resource_destroy(context->keyboard);

	if (context->input_method && context->input_method->context == context)
		context->input_method->context = NULL;

	free(context);
}

static void
input_method_context_create(struct text_input *input,
			    struct input_method *input_method)
{
	struct input_method_context *context;
	struct wl_resource *binding;

	if (!input_method->input_method_binding)
		return;

	context = zalloc(sizeof *context);
	if (context == NULL)
		return;

	binding = input_method->input_method_binding;
	context->resource =
		wl_resource_create(wl_resource_get_client(binding),
				   &zwp_input_method_context_v1_interface,
				   1, 0);
	if (!context->resource) {
		free(context);
		return;
	}
	wl_resource_set_implementation(context->resource,
				       &context_implementation,
				       context, destroy_input_method_context);

	context->input = input;
	context->input_method = input_method;
	input_method->context = context;


	zwp_input_method_v1_send_activate(binding, context->resource);
}

static void
input_method_context_end_keyboard_grab(struct input_method_context *context)
{
	struct weston_keyboard_grab *grab;
	struct weston_keyboard *keyboard;

	keyboard = weston_seat_get_keyboard(context->input_method->seat);
	if (!keyboard)
		return;

	grab = &keyboard->input_method_grab;
	keyboard = grab->keyboard;
	if (!keyboard)
		return;

	if (keyboard->grab == grab)
		weston_keyboard_end_grab(keyboard);

	keyboard->input_method_resource = NULL;
}

static void
unbind_input_method(struct wl_resource *resource)
{
	struct input_method *input_method = wl_resource_get_user_data(resource);

	input_method->input_method_binding = NULL;
	input_method->context = NULL;
}

static void
bind_input_method(struct wl_client *client,
		  void *data,
		  uint32_t version,
		  uint32_t id)
{
	struct input_method *input_method = data;
	struct text_backend *text_backend = input_method->text_backend;
	struct wl_resource *resource;

	resource =
		wl_resource_create(client,
				   &zwp_input_method_v1_interface, 1, id);

	if (input_method->input_method_binding != NULL) {
		wl_resource_post_error(resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "interface object already bound");
		return;
	}

	if (text_backend->input_method.client != client) {
		wl_resource_post_error(resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "permission to bind "
				       "input_method denied");
		return;
	}

	wl_resource_set_implementation(resource, NULL, input_method,
				       unbind_input_method);
	input_method->input_method_binding = resource;

	if (input_method->input) {
		input_method_context_create(input_method->input, input_method);
		if (input_method->input->protocol == TEXT_INPUT_PROTOCOL_V3)
			text_input_v3_send_state(input_method->input);
	}
}

static void
input_method_notifier_destroy(struct wl_listener *listener, void *data)
{
	struct input_method *input_method =
		container_of(listener, struct input_method, destroy_listener);
	struct text_input *text_input, *next;

	if (input_method->input)
		deactivate_input_method(input_method);
	wl_list_for_each_safe(text_input, next,
			      &input_method->text_inputs_v3, seat_link)
		wl_resource_destroy(text_input->resource);

	wl_global_destroy(input_method->input_method_global);
	wl_list_remove(&input_method->destroy_listener.link);

	free(input_method);
}

static void
handle_keyboard_focus(struct wl_listener *listener, void *data)
{
	struct weston_keyboard *keyboard = data;
	struct input_method *input_method =
		container_of(listener, struct input_method,
			     keyboard_focus_listener);
	struct weston_surface *surface = keyboard->focus;
	struct text_input *text_input, *next;
	struct wl_client *focus_client = NULL;

	if (surface && surface->resource)
		focus_client = wl_resource_get_client(surface->resource);

	if (input_method->input && input_method->input->surface != surface)
		deactivate_input_method(input_method);

	wl_list_for_each_safe(text_input, next,
			      &input_method->text_inputs_v3, seat_link) {
		struct wl_client *client =
			wl_resource_get_client(text_input->resource);

		if (text_input->surface && text_input->surface != surface) {
			zwp_text_input_v3_send_leave(
				text_input->resource,
				text_input->surface->resource);
			text_input->surface = NULL;
			text_input_v3_state_reset(&text_input->current_state);
			text_input_v3_state_reset(&text_input->pending_state);
			text_input->pending_activation =
				TEXT_INPUT_V3_ACTIVATION_NONE;
			text_input->delete_pending = false;
			text_input->preedit_cursor = -1;
		}

		if (!text_input->surface && focus_client == client) {
			text_input->surface = surface;
			zwp_text_input_v3_send_enter(text_input->resource,
						     surface->resource);
		}
	}
}

static void
input_method_init_seat(struct weston_seat *seat)
{
	struct weston_keyboard *keyboard = weston_seat_get_keyboard(seat);

	if (seat->input_method->focus_listener_initialized)
		return;

	if (keyboard) {
		seat->input_method->keyboard_focus_listener.notify =
			handle_keyboard_focus;
		wl_signal_add(&keyboard->focus_signal,
			      &seat->input_method->keyboard_focus_listener);
		keyboard->input_method_grab.interface =
			&input_method_context_grab;
	}

	seat->input_method->focus_listener_initialized = true;
}

static void launch_input_method(struct text_backend *text_backend);

static void
respawn_input_method_process(struct text_backend *text_backend)
{
	struct timespec time;
	int64_t tdiff;

	/* if input_method dies more than 5 times in 10 seconds, give up */
	weston_compositor_get_time(&time);
	tdiff = timespec_sub_to_msec(&time,
				     &text_backend->input_method.deathstamp);
	if (tdiff > 10000) {
		text_backend->input_method.deathstamp = time;
		text_backend->input_method.deathcount = 0;
	}

	text_backend->input_method.deathcount++;
	if (text_backend->input_method.deathcount > 5) {
		weston_log("input_method disconnected, giving up.\n");
		return;
	}

	weston_log("input_method disconnected, respawning...\n");
	launch_input_method(text_backend);
}

static void
input_method_client_notifier(struct wl_listener *listener, void *data)
{
	struct text_backend *text_backend;

	text_backend = container_of(listener, struct text_backend,
				    client_listener);

	text_backend->input_method.client = NULL;
	respawn_input_method_process(text_backend);
}

static void
launch_input_method(struct text_backend *text_backend)
{
	if (!text_backend->input_method.path)
		return;

	if (strcmp(text_backend->input_method.path, "") == 0)
		return;

	text_backend->input_method.client =
		weston_client_start(text_backend->compositor,
				    text_backend->input_method.path);

	if (!text_backend->input_method.client) {
		weston_log("not able to start %s\n",
			   text_backend->input_method.path);
		return;
	}

	text_backend->client_listener.notify = input_method_client_notifier;
	wl_client_add_destroy_listener(text_backend->input_method.client,
				       &text_backend->client_listener);
}

static void
text_backend_seat_created(struct text_backend *text_backend,
			  struct weston_seat *seat)
{
	struct input_method *input_method;
	struct weston_compositor *ec = seat->compositor;

	input_method = zalloc(sizeof *input_method);
	if (input_method == NULL)
		return;

	input_method->seat = seat;
	input_method->input = NULL;
	input_method->focus_listener_initialized = false;
	input_method->context = NULL;
	input_method->text_backend = text_backend;
	wl_list_init(&input_method->text_inputs_v3);

	input_method->input_method_global =
		wl_global_create(ec->wl_display,
				 &zwp_input_method_v1_interface, 1,
				 input_method, bind_input_method);

	input_method->destroy_listener.notify = input_method_notifier_destroy;
	wl_signal_add(&seat->destroy_signal, &input_method->destroy_listener);

	seat->input_method = input_method;
}

static void
handle_seat_created(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = data;
	struct text_backend *text_backend =
		container_of(listener, struct text_backend,
			     seat_created_listener);

	text_backend_seat_created(text_backend, seat);
}

static void
text_backend_configuration(struct text_backend *text_backend)
{
	struct weston_config *config = wet_get_config(text_backend->compositor);
	struct weston_config_section *section;
	char *client;

	section = weston_config_get_section(config,
					    "input-method", NULL, NULL);
	client = wet_get_libexec_path("weston-keyboard");
	weston_config_section_get_string(section, "path",
					 &text_backend->input_method.path,
					 client);
	free(client);
}

WL_EXPORT void
text_backend_destroy(struct text_backend *text_backend)
{
	wl_list_remove(&text_backend->seat_created_listener.link);

	if (text_backend->input_method.client) {
		/* disable respawn */
		wl_list_remove(&text_backend->client_listener.link);
		wl_client_destroy(text_backend->input_method.client);
	}

	free(text_backend->input_method.path);
	free(text_backend);
}

WL_EXPORT struct text_backend *
text_backend_init(struct weston_compositor *ec)
{
	struct text_backend *text_backend;
	struct weston_seat *seat;

	text_backend = zalloc(sizeof(*text_backend));
	if (text_backend == NULL)
		return NULL;

	text_backend->compositor = ec;

	text_backend_configuration(text_backend);

	wl_list_for_each(seat, &ec->seat_list, link)
		text_backend_seat_created(text_backend, seat);
	text_backend->seat_created_listener.notify = handle_seat_created;
	wl_signal_add(&ec->seat_created_signal,
		      &text_backend->seat_created_listener);

	text_input_manager_create(ec);

	launch_input_method(text_backend);

	return text_backend;
}
