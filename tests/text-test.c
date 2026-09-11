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

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

#include "weston-test-client-helper.h"
#include "text-input-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "test-config.h"
#include "weston-test-fixture-compositor.h"

#define INPUT_METHOD_BARRIER_KEY 0x7d
#define INPUT_METHOD_CACHE_FLUSH_KEY 0x7e

static enum test_result_code
fixture_setup(struct weston_test_harness *harness)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.config_file = TESTSUITE_TEXT_CONFIG_PATH;
	setup.shell = SHELL_TEST_DESKTOP;

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP(fixture_setup);

static struct client *
create_text_test_client(void)
{
	struct client *client;

	client = create_client();
	client->surface = create_test_surface(client);
	return client;
}

struct text_input_state {
	int activated;
	int deactivated;
	int preedit_count;
	int commit_count;
	int delete_count;
	unsigned int done_count;
	char preedit[64];
	char committed[64];
	int32_t preedit_cursor_begin;
	int32_t preedit_cursor_end;
	uint32_t delete_before;
	uint32_t delete_after;
	uint32_t done_serial[8];
};

static void
text_input_commit_string(void *data,
			 struct zwp_text_input_v1 *text_input,
			 uint32_t serial,
			 const char *text)
{
}

static void
text_input_preedit_string(void *data,
			  struct zwp_text_input_v1 *text_input,
			  uint32_t serial,
			  const char *text,
			  const char *commit)
{
}

static void
text_input_delete_surrounding_text(void *data,
				   struct zwp_text_input_v1 *text_input,
				   int32_t index,
				   uint32_t length)
{
}

static void
text_input_cursor_position(void *data,
			   struct zwp_text_input_v1 *text_input,
			   int32_t index,
			   int32_t anchor)
{
}

static void
text_input_preedit_styling(void *data,
			   struct zwp_text_input_v1 *text_input,
			   uint32_t index,
			   uint32_t length,
			   uint32_t style)
{
}

static void
text_input_preedit_cursor(void *data,
			  struct zwp_text_input_v1 *text_input,
			  int32_t index)
{
}

static void
text_input_modifiers_map(void *data,
			 struct zwp_text_input_v1 *text_input,
			 struct wl_array *map)
{
}

static void
text_input_keysym(void *data,
		  struct zwp_text_input_v1 *text_input,
		  uint32_t serial,
		  uint32_t time,
		  uint32_t sym,
		  uint32_t state,
		  uint32_t modifiers)
{
}

static void
text_input_enter(void *data,
		 struct zwp_text_input_v1 *text_input,
		 struct wl_surface *surface)

{
	struct text_input_state *state = data;

	testlog("%s\n", __FUNCTION__);

	state->activated += 1;
}

static void
text_input_leave(void *data,
		 struct zwp_text_input_v1 *text_input)
{
	struct text_input_state *state = data;

	state->deactivated += 1;
}

static void
text_input_input_panel_state(void *data,
			     struct zwp_text_input_v1 *text_input,
			     uint32_t state)
{
}

static void
text_input_language(void *data,
		    struct zwp_text_input_v1 *text_input,
		    uint32_t serial,
		    const char *language)
{
}

static void
text_input_text_direction(void *data,
			  struct zwp_text_input_v1 *text_input,
			  uint32_t serial,
			  uint32_t direction)
{
}

static const struct zwp_text_input_v1_listener text_input_listener = {
	text_input_enter,
	text_input_leave,
	text_input_modifiers_map,
	text_input_input_panel_state,
	text_input_preedit_string,
	text_input_preedit_styling,
	text_input_preedit_cursor,
	text_input_commit_string,
	text_input_cursor_position,
	text_input_delete_surrounding_text,
	text_input_keysym,
	text_input_language,
	text_input_text_direction
};

TEST(text_test)
{
	struct client *client;
	struct global *global;
	struct zwp_text_input_manager_v1 *factory;
	struct zwp_text_input_v1 *text_input;
	struct text_input_state state;

	client = create_text_test_client();
	assert(client);

	factory = NULL;
	wl_list_for_each(global, &client->global_list, link) {
		if (strcmp(global->interface, "zwp_text_input_manager_v1") == 0)
			factory = wl_registry_bind(client->wl_registry,
						   global->name,
						   &zwp_text_input_manager_v1_interface, 1);
	}

	assert(factory);

	memset(&state, 0, sizeof state);
	text_input = zwp_text_input_manager_v1_create_text_input(factory);
	zwp_text_input_v1_add_listener(text_input,
				       &text_input_listener,
				       &state);

	/* Make sure our test surface has keyboard focus. */
	weston_test_activate_surface(client->test->weston_test,
				 client->surface->wl_surface);
	client_roundtrip(client);
	assert(client->input->keyboard->focus == client->surface);

	/* Activate test model and make sure we get enter event. */
	zwp_text_input_v1_activate(text_input, client->input->wl_seat,
				   client->surface->wl_surface);
	client_roundtrip(client);
	assert(state.activated == 1 && state.deactivated == 0);

	/* Deactivate test model and make sure we get leave event. */
	zwp_text_input_v1_deactivate(text_input, client->input->wl_seat);
	client_roundtrip(client);
	assert(state.activated == 1 && state.deactivated == 1);

	/* Activate test model again. */
	zwp_text_input_v1_activate(text_input, client->input->wl_seat,
				   client->surface->wl_surface);
	client_roundtrip(client);
	assert(state.activated == 2 && state.deactivated == 1);

	/* Take keyboard focus away and verify we get leave event. */
	weston_test_activate_surface(client->test->weston_test, NULL);
	client_roundtrip(client);
	assert(state.activated == 2 && state.deactivated == 2);
}

static void
text_input_v3_enter(void *data,
		    struct zwp_text_input_v3 *text_input,
		    struct wl_surface *surface)
{
	struct text_input_state *state = data;

	state->activated++;
}

static void
text_input_v3_leave(void *data,
		    struct zwp_text_input_v3 *text_input,
		    struct wl_surface *surface)
{
	struct text_input_state *state = data;

	state->deactivated++;
}

static void
text_input_v3_preedit_string(void *data,
			     struct zwp_text_input_v3 *text_input,
			     const char *text,
			     int32_t cursor_begin,
			     int32_t cursor_end)
{
	struct text_input_state *state = data;

	state->preedit_count++;
	snprintf(state->preedit, sizeof state->preedit,
		 "%s", text ? text : "");
	state->preedit_cursor_begin = cursor_begin;
	state->preedit_cursor_end = cursor_end;
}

static void
text_input_v3_commit_string(void *data,
			    struct zwp_text_input_v3 *text_input,
			    const char *text)
{
	struct text_input_state *state = data;

	state->commit_count++;
	snprintf(state->committed, sizeof state->committed,
		 "%s", text ? text : "");
}

static void
text_input_v3_delete_surrounding_text(void *data,
				      struct zwp_text_input_v3 *text_input,
				      uint32_t before_length,
				      uint32_t after_length)
{
	struct text_input_state *state = data;

	state->delete_count++;
	state->delete_before = before_length;
	state->delete_after = after_length;
}

static void
text_input_v3_done(void *data,
		   struct zwp_text_input_v3 *text_input,
		   uint32_t serial)
{
	struct text_input_state *state = data;

	assert(state->done_count < ARRAY_LENGTH(state->done_serial));
	state->done_serial[state->done_count++] = serial;
}

static const struct zwp_text_input_v3_listener text_input_v3_listener = {
	text_input_v3_enter,
	text_input_v3_leave,
	text_input_v3_preedit_string,
	text_input_v3_commit_string,
	text_input_v3_delete_surrounding_text,
	text_input_v3_done,
};

static struct zwp_text_input_manager_v3 *
bind_text_input_manager_v3(struct client *client)
{
	struct zwp_text_input_manager_v3 *manager = NULL;
	struct global *global;

	wl_list_for_each(global, &client->global_list, link) {
		if (strcmp(global->interface,
			   "zwp_text_input_manager_v3") == 0)
			manager = wl_registry_bind(
				client->wl_registry, global->name,
				&zwp_text_input_manager_v3_interface, 1);
	}

	assert(manager);
	return manager;
}

static struct zwp_text_input_v3 *
create_text_input_v3(struct client *client,
		     struct zwp_text_input_manager_v3 *manager,
		     struct text_input_state *state)
{
	struct zwp_text_input_v3 *text_input;

	text_input = zwp_text_input_manager_v3_get_text_input(
		manager, client->input->wl_seat);
	zwp_text_input_v3_add_listener(text_input,
				       &text_input_v3_listener, state);
	return text_input;
}

static void
wait_for_input_method(struct client *client)
{
	struct keyboard *keyboard = client->input->keyboard;
	uint32_t previous_barrier = keyboard->key_time_msec;

	assert(wl_display_flush(client->wl_display) >= 0);
	while (keyboard->key_time_msec == previous_barrier)
		assert(wl_display_dispatch(client->wl_display) >= 0);
	assert(keyboard->key == INPUT_METHOD_BARRIER_KEY);
}

static void
flush_deferred_input_method_state(struct client *client)
{
	weston_test_send_key(client->test->weston_test, 0, 0, 1,
			     INPUT_METHOD_CACHE_FLUSH_KEY,
			     WL_KEYBOARD_KEY_STATE_PRESSED);
	wait_for_input_method(client);
}

TEST(text_v3_test)
{
	struct client *client;
	struct zwp_text_input_manager_v3 *manager;
	struct zwp_text_input_v3 *text_input;
	struct text_input_state state = { 0 };
	int commits;

	client = create_text_test_client();
	assert(client);

	manager = bind_text_input_manager_v3(client);
	text_input = create_text_input_v3(client, manager, &state);

	weston_test_activate_surface(client->test->weston_test,
				 client->surface->wl_surface);
	client_roundtrip(client);
	assert(state.activated == 1 && state.deactivated == 0);

	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_set_surrounding_text(text_input, "bridge", 6, 6);
	zwp_text_input_v3_set_cursor_rectangle(text_input, 10, 10, 1, 20);
	zwp_text_input_v3_commit(text_input);
	wait_for_input_method(client);

	assert(state.preedit_count == 1);
	assert(strcmp(state.preedit, "preedit") == 0);
	assert(state.preedit_cursor_begin == 1);
	assert(state.preedit_cursor_end == 1);
	assert(state.delete_count == 1);
	assert(state.delete_before == 1 && state.delete_after == 1);
	assert(state.commit_count == 1);
	assert(strcmp(state.committed, "X") == 0);
	assert(state.done_count == 2);
	assert(state.done_serial[0] == 0 && state.done_serial[1] == 0);

	zwp_text_input_v3_set_text_change_cause(
		text_input, ZWP_TEXT_INPUT_V3_CHANGE_CAUSE_OTHER);
	zwp_text_input_v3_set_surrounding_text(text_input, "cause", 5, 5);
	zwp_text_input_v3_commit(text_input);
	wait_for_input_method(client);
	assert(state.commit_count == 2);
	assert(strcmp(state.committed, "reset") == 0);
	assert(state.done_serial[state.done_count - 1] == 2);

	zwp_text_input_v3_set_cursor_rectangle(
		text_input, INT32_MAX, 0, 1, 1);
	zwp_text_input_v3_set_surrounding_text(text_input, "overflow", 8, 8);
	zwp_text_input_v3_commit(text_input);
	wait_for_input_method(client);
	assert(state.commit_count == 3);
	assert(strcmp(state.committed, "overflow") == 0);
	assert(state.done_serial[state.done_count - 1] == 3);

	weston_test_activate_surface(client->test->weston_test, NULL);
	client_roundtrip(client);
	assert(state.activated == 1 && state.deactivated == 1);

	commits = state.commit_count;
	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_set_surrounding_text(text_input, "ignored", 7, 7);
	zwp_text_input_v3_commit(text_input);
	client_roundtrip(client);
	assert(state.commit_count == commits);

	weston_test_activate_surface(client->test->weston_test,
				 client->surface->wl_surface);
	client_roundtrip(client);
	assert(state.activated == 2 && state.deactivated == 1);

	zwp_text_input_v3_commit(text_input);
	client_roundtrip(client);
	assert(state.commit_count == commits);

	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_set_surrounding_text(
		text_input, "after-leave", 11, 11);
	zwp_text_input_v3_commit(text_input);
	wait_for_input_method(client);
	assert(state.commit_count == commits + 1);
	assert(strcmp(state.committed, "after-leave") == 0);
	assert(state.done_serial[state.done_count - 1] == 6);
}

TEST(text_v3_enable_clears_input_method_state)
{
	struct client *client;
	struct zwp_text_input_manager_v3 *manager;
	struct zwp_text_input_v3 *text_input;
	struct text_input_state state = { 0 };

	client = create_text_test_client();
	assert(client);
	manager = bind_text_input_manager_v3(client);
	text_input = create_text_input_v3(client, manager, &state);

	weston_test_activate_surface(client->test->weston_test,
				 client->surface->wl_surface);
	client_roundtrip(client);

	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_set_surrounding_text(
		text_input, "prime-cache", 11, 11);
	zwp_text_input_v3_commit(text_input);
	wait_for_input_method(client);
	assert(state.preedit_count == 0);
	assert(state.delete_count == 0);
	assert(state.commit_count == 0);

	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_set_surrounding_text(text_input, "clean", 5, 5);
	zwp_text_input_v3_commit(text_input);
	wait_for_input_method(client);
	assert(state.preedit_count == 1);
	assert(state.preedit_cursor_begin == -1);
	assert(state.preedit_cursor_end == -1);
	assert(state.delete_count == 0);
	assert(state.commit_count == 1);
	assert(strcmp(state.committed, "clean") == 0);
}

TEST(text_v3_enable_is_double_buffered)
{
	struct client *client;
	struct zwp_text_input_manager_v3 *manager;
	struct zwp_text_input_v3 *text_input;
	struct text_input_state state = { 0 };

	client = create_text_test_client();
	assert(client);
	manager = bind_text_input_manager_v3(client);
	text_input = create_text_input_v3(client, manager, &state);

	weston_test_activate_surface(client->test->weston_test,
				 client->surface->wl_surface);
	client_roundtrip(client);

	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_set_surrounding_text(
		text_input, "prime-cache", 11, 11);
	zwp_text_input_v3_commit(text_input);
	wait_for_input_method(client);
	assert(state.preedit_count == 0);
	assert(state.delete_count == 0);

	zwp_text_input_v3_enable(text_input);
	client_roundtrip(client);
	flush_deferred_input_method_state(client);

	assert(state.preedit_count == 1);
	assert(strcmp(state.preedit, "deferred-preedit") == 0);
	assert(state.preedit_cursor_begin == 2);
	assert(state.preedit_cursor_end == 2);
	assert(state.delete_count == 1);
	assert(state.delete_before == 1 && state.delete_after == 1);
	assert(state.commit_count == 1);
	assert(strcmp(state.committed, "deferred") == 0);
}

TEST(text_v3_second_enable_is_ignored)
{
	struct client *client;
	struct zwp_text_input_manager_v3 *manager;
	struct zwp_text_input_v3 *first;
	struct zwp_text_input_v3 *second;
	struct text_input_state first_state = { 0 };
	struct text_input_state second_state = { 0 };

	client = create_text_test_client();
	assert(client);
	manager = bind_text_input_manager_v3(client);
	first = create_text_input_v3(client, manager, &first_state);
	second = create_text_input_v3(client, manager, &second_state);

	weston_test_activate_surface(client->test->weston_test,
				 client->surface->wl_surface);
	client_roundtrip(client);
	assert(first_state.activated == 1 && second_state.activated == 1);

	zwp_text_input_v3_enable(first);
	zwp_text_input_v3_set_surrounding_text(first, "first", 5, 5);
	zwp_text_input_v3_commit(first);
	wait_for_input_method(client);
	assert(first_state.commit_count == 1);

	zwp_text_input_v3_enable(second);
	zwp_text_input_v3_set_surrounding_text(second, "second", 6, 6);
	zwp_text_input_v3_commit(second);

	zwp_text_input_v3_set_surrounding_text(
		first, "first-again", 11, 11);
	zwp_text_input_v3_commit(first);
	wait_for_input_method(client);
	assert(second_state.commit_count == 0);
	assert(first_state.commit_count == 2);
	assert(strcmp(first_state.committed, "first-again") == 0);
}

TEST(text_v3_resource_becomes_inert_with_seat)
{
	struct client *client;
	struct zwp_text_input_manager_v3 *manager;
	struct zwp_text_input_v3 *text_input;
	struct zwp_text_input_v3 *inert_text_input;
	struct text_input_state state = { 0 };
	struct text_input_state inert_state = { 0 };

	client = create_text_test_client();
	assert(client);
	manager = bind_text_input_manager_v3(client);
	text_input = create_text_input_v3(client, manager, &state);

	weston_test_device_release(client->test->weston_test, "seat");
	inert_text_input = create_text_input_v3(client, manager, &inert_state);
	client_roundtrip(client);
	assert(!client->input);

	zwp_text_input_v3_enable(text_input);
	zwp_text_input_v3_set_surrounding_text(text_input, "inert", 5, 5);
	zwp_text_input_v3_commit(text_input);
	zwp_text_input_v3_destroy(text_input);
	zwp_text_input_v3_enable(inert_text_input);
	zwp_text_input_v3_commit(inert_text_input);
	zwp_text_input_v3_destroy(inert_text_input);
	client_roundtrip(client);

	weston_test_device_add(client->test->weston_test, "seat");
	client_roundtrip(client);
	client_roundtrip(client);
	assert(client->input);
}
