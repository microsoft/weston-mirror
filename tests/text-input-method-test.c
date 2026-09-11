/*
 * Copyright 2026 huyuliang
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "input-method-unstable-v1-client-protocol.h"

#define INPUT_METHOD_BARRIER_KEY 0x7d
#define INPUT_METHOD_CACHE_FLUSH_KEY 0x7e

struct test_input_method {
	struct wl_display *display;
	struct zwp_input_method_v1 *input_method;
	struct zwp_input_method_context_v1 *context;
	struct wl_keyboard *keyboard;
	char surrounding_text[64];
	uint32_t surrounding_cursor;
	uint32_t surrounding_anchor;
	uint32_t content_hint;
	uint32_t content_purpose;
	uint32_t barrier_serial;
	uint32_t deferred_serial;
	unsigned int reset_count;
	bool surrounding_text_set;
	bool content_type_set;
	bool cache_flush_pending;
};

static void
send_barrier(struct test_input_method *input_method)
{
	input_method->barrier_serial++;
	zwp_input_method_context_v1_key(
		input_method->context, 0, input_method->barrier_serial,
		INPUT_METHOD_BARRIER_KEY, WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void
context_surrounding_text(void *data,
			 struct zwp_input_method_context_v1 *context,
			 const char *text,
			 uint32_t cursor,
			 uint32_t anchor)
{
	struct test_input_method *input_method = data;

	snprintf(input_method->surrounding_text,
		 sizeof input_method->surrounding_text, "%s", text);
	input_method->surrounding_cursor = cursor;
	input_method->surrounding_anchor = anchor;
	input_method->surrounding_text_set = true;
}

static void
context_reset(void *data, struct zwp_input_method_context_v1 *context)
{
	struct test_input_method *input_method = data;

	input_method->reset_count++;
}

static void
context_content_type(void *data,
		     struct zwp_input_method_context_v1 *context,
		     uint32_t hint,
		     uint32_t purpose)
{
	struct test_input_method *input_method = data;

	input_method->content_hint = hint;
	input_method->content_purpose = purpose;
	input_method->content_type_set = true;
}

static void
context_invoke_action(void *data,
		      struct zwp_input_method_context_v1 *context,
		      uint32_t button,
		      uint32_t index)
{
}

static void
send_bridge_result(struct test_input_method *input_method, uint32_t serial)
{
	struct zwp_input_method_context_v1 *context = input_method->context;

	if (!input_method->surrounding_text_set ||
	    !input_method->content_type_set)
		return;

	if (strcmp(input_method->surrounding_text, "bridge") == 0 &&
	    input_method->surrounding_cursor == 6 &&
	    input_method->surrounding_anchor == 6 &&
	    input_method->content_hint == 0 &&
	    input_method->content_purpose == 0) {
		zwp_input_method_context_v1_preedit_cursor(context, 1);
		zwp_input_method_context_v1_preedit_string(
			context, serial - 1, "preedit", "");
		zwp_input_method_context_v1_delete_surrounding_text(
			context, -1, 2);
		zwp_input_method_context_v1_commit_string(
			context, serial - 1, "X");
	} else if (strcmp(input_method->surrounding_text, "cause") == 0 &&
		   input_method->reset_count > 0) {
		zwp_input_method_context_v1_commit_string(
			context, serial, "reset");
	} else if (strcmp(input_method->surrounding_text, "prime-cache") == 0) {
		zwp_input_method_context_v1_preedit_cursor(context, 2);
		zwp_input_method_context_v1_delete_surrounding_text(
			context, -1, 2);
		input_method->deferred_serial = serial;
		input_method->cache_flush_pending = true;
	} else if (strcmp(input_method->surrounding_text, "clean") == 0) {
		zwp_input_method_context_v1_preedit_string(
			context, serial, "clean-preedit", "");
		zwp_input_method_context_v1_commit_string(
			context, serial, "clean");
	} else if (strcmp(input_method->surrounding_text, "second") == 0) {
		zwp_input_method_context_v1_commit_string(
			context, serial, "stolen");
	} else if (strcmp(input_method->surrounding_text, "overflow") == 0 &&
		   input_method->reset_count != 1) {
		zwp_input_method_context_v1_commit_string(
			context, serial, "reset-leaked");
	} else {
		zwp_input_method_context_v1_commit_string(
			context, serial, input_method->surrounding_text);
	}
}

static void
context_commit_state(void *data,
		     struct zwp_input_method_context_v1 *context,
		     uint32_t serial)
{
	struct test_input_method *input_method = data;

	send_bridge_result(input_method, serial);
	send_barrier(input_method);
}

static void
context_preferred_language(void *data,
			   struct zwp_input_method_context_v1 *context,
			   const char *language)
{
}

static const struct zwp_input_method_context_v1_listener context_listener = {
	context_surrounding_text,
	context_reset,
	context_content_type,
	context_invoke_action,
	context_commit_state,
	context_preferred_language,
};

static void
input_method_keyboard_keymap(void *data,
			     struct wl_keyboard *keyboard,
			     uint32_t format,
			     int fd,
			     uint32_t size)
{
	close(fd);
}

static void
input_method_keyboard_key(void *data,
			  struct wl_keyboard *keyboard,
			  uint32_t serial,
			  uint32_t time,
			  uint32_t key,
			  uint32_t state)
{
	struct test_input_method *input_method = data;

	if (key != INPUT_METHOD_CACHE_FLUSH_KEY ||
	    state != WL_KEYBOARD_KEY_STATE_PRESSED ||
	    !input_method->cache_flush_pending)
		return;

	zwp_input_method_context_v1_preedit_string(
		input_method->context, input_method->deferred_serial,
		"deferred-preedit", "");
	zwp_input_method_context_v1_commit_string(
		input_method->context, input_method->deferred_serial,
		"deferred");
	input_method->cache_flush_pending = false;
	send_barrier(input_method);
}

static void
input_method_keyboard_modifiers(void *data,
				struct wl_keyboard *keyboard,
				uint32_t serial,
				uint32_t mods_depressed,
				uint32_t mods_latched,
				uint32_t mods_locked,
				uint32_t group)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	input_method_keyboard_keymap,
	NULL,
	NULL,
	input_method_keyboard_key,
	input_method_keyboard_modifiers,
	NULL,
};

static void
input_method_activate(void *data,
		      struct zwp_input_method_v1 *input_method_proxy,
		      struct zwp_input_method_context_v1 *context)
{
	struct test_input_method *input_method = data;

	input_method->context = context;
	input_method->surrounding_text[0] = '\0';
	input_method->surrounding_text_set = false;
	input_method->content_type_set = false;
	input_method->reset_count = 0;
	input_method->cache_flush_pending = false;
	zwp_input_method_context_v1_add_listener(
		context, &context_listener, input_method);
	input_method->keyboard =
		zwp_input_method_context_v1_grab_keyboard(context);
	wl_keyboard_add_listener(input_method->keyboard,
				 &keyboard_listener, input_method);
}

static void
input_method_deactivate(void *data,
			struct zwp_input_method_v1 *input_method_proxy,
			struct zwp_input_method_context_v1 *context)
{
	struct test_input_method *input_method = data;
	struct wl_keyboard *inert_keyboard;

	if (input_method->context == context)
		input_method->context = NULL;

	zwp_input_method_context_v1_key(
		context, 0, 0, INPUT_METHOD_BARRIER_KEY,
		WL_KEYBOARD_KEY_STATE_PRESSED);
	zwp_input_method_context_v1_modifiers(context, 0, 0, 0, 0, 0);
	inert_keyboard =
		zwp_input_method_context_v1_grab_keyboard(context);
	wl_keyboard_destroy(inert_keyboard);

	if (input_method->keyboard) {
		wl_keyboard_destroy(input_method->keyboard);
		input_method->keyboard = NULL;
	}
	zwp_input_method_context_v1_destroy(context);
}

static const struct zwp_input_method_v1_listener input_method_listener = {
	input_method_activate,
	input_method_deactivate,
};

static void
registry_global(void *data,
		struct wl_registry *registry,
		uint32_t name,
		const char *interface,
		uint32_t version)
{
	struct test_input_method *input_method = data;

	if (strcmp(interface, "zwp_input_method_v1") != 0)
		return;

	if (input_method->input_method)
		wl_proxy_destroy((struct wl_proxy *) input_method->input_method);
	input_method->input_method = wl_registry_bind(
		registry, name, &zwp_input_method_v1_interface, 1);
	zwp_input_method_v1_add_listener(input_method->input_method,
					 &input_method_listener, input_method);
}

static void
registry_global_remove(void *data,
		       struct wl_registry *registry,
		       uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	registry_global,
	registry_global_remove,
};

int
main(int argc, char *argv[])
{
	struct test_input_method input_method = { 0 };
	struct wl_registry *registry;

	input_method.display = wl_display_connect(NULL);
	if (!input_method.display)
		return EXIT_FAILURE;

	registry = wl_display_get_registry(input_method.display);
	wl_registry_add_listener(registry, &registry_listener, &input_method);
	if (wl_display_roundtrip(input_method.display) < 0 ||
	    !input_method.input_method)
		return EXIT_FAILURE;

	while (wl_display_dispatch(input_method.display) >= 0)
		;

	if (input_method.keyboard)
		wl_proxy_destroy((struct wl_proxy *) input_method.keyboard);
	if (input_method.context)
		wl_proxy_destroy((struct wl_proxy *) input_method.context);
	if (input_method.input_method)
		wl_proxy_destroy((struct wl_proxy *) input_method.input_method);
	wl_registry_destroy(registry);
	wl_display_disconnect(input_method.display);
	return EXIT_SUCCESS;
}
