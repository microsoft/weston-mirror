/*
 * Copyright 2026 Microsoft Corporation
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
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <libweston/backend-rdp.h>

#include "input-method-unstable-v1-client-protocol.h"
#include "test-config.h"
#include "text-input-unstable-v1-client-protocol.h"
#include "weston-rdprail-test-client-protocol.h"
#include "weston-test-client-helper.h"
#include "weston-test-fixture-compositor.h"

static enum test_result_code
fixture_setup(struct weston_test_harness *harness)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.backend = WESTON_BACKEND_RDP;
	setup.shell = SHELL_RDPRAIL;
	setup.config_file = TESTSUITE_RDPRAIL_INPUT_PANEL_CONFIG_PATH;
	setup.extra_module = TESTSUITE_RDPRAIL_TEST_PLUGIN_PATH;

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP(fixture_setup);

struct input_method_state {
	struct zwp_input_method_context_v1 *context;
};

struct rdprail_test_state {
	bool received;
	bool owner_matches;
	uint32_t show_state_requested;
};

static void
rdprail_window_state(void *data, struct weston_rdprail_test *rdprail_test,
		     uint32_t owner_matches,
		     uint32_t show_state_requested)
{
	struct rdprail_test_state *state = data;

	state->received = true;
	state->owner_matches = owner_matches;
	state->show_state_requested = show_state_requested;

	(void)rdprail_test;
}

static const struct weston_rdprail_test_listener rdprail_test_listener = {
	rdprail_window_state,
};

static void
context_surrounding_text(void *data,
			 struct zwp_input_method_context_v1 *context,
			 const char *text, uint32_t cursor, uint32_t anchor)
{
}

static void
context_reset(void *data, struct zwp_input_method_context_v1 *context)
{
}

static void
context_content_type(void *data,
		     struct zwp_input_method_context_v1 *context,
		     uint32_t hint, uint32_t purpose)
{
}

static void
context_invoke_action(void *data,
		      struct zwp_input_method_context_v1 *context,
		      uint32_t button, uint32_t index)
{
}

static void
context_commit_state(void *data,
		     struct zwp_input_method_context_v1 *context,
		     uint32_t serial)
{
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
input_method_activate(void *data,
		      struct zwp_input_method_v1 *input_method,
		      struct zwp_input_method_context_v1 *context)
{
	struct input_method_state *state = data;

	state->context = context;
	zwp_input_method_context_v1_add_listener(context, &context_listener,
					 state);

	(void)input_method;
}

static void
input_method_deactivate(void *data,
			struct zwp_input_method_v1 *input_method,
			struct zwp_input_method_context_v1 *context)
{
	struct input_method_state *state = data;

	if (state->context == context)
		state->context = NULL;
	zwp_input_method_context_v1_destroy(context);

	(void)input_method;
}

static const struct zwp_input_method_v1_listener input_method_listener = {
	input_method_activate,
	input_method_deactivate,
};

static void
text_input_commit_string(void *data, struct zwp_text_input_v1 *text_input,
			 uint32_t serial, const char *text)
{
}

static void
text_input_preedit_string(void *data, struct zwp_text_input_v1 *text_input,
			  uint32_t serial, const char *text, const char *commit)
{
}

static void
text_input_delete_surrounding_text(void *data,
				   struct zwp_text_input_v1 *text_input,
				   int32_t index, uint32_t length)
{
}

static void
text_input_cursor_position(void *data, struct zwp_text_input_v1 *text_input,
			   int32_t index, int32_t anchor)
{
}

static void
text_input_preedit_styling(void *data, struct zwp_text_input_v1 *text_input,
			   uint32_t index, uint32_t length, uint32_t style)
{
}

static void
text_input_preedit_cursor(void *data, struct zwp_text_input_v1 *text_input,
			  int32_t index)
{
}

static void
text_input_modifiers_map(void *data, struct zwp_text_input_v1 *text_input,
			 struct wl_array *map)
{
}

static void
text_input_keysym(void *data, struct zwp_text_input_v1 *text_input,
		  uint32_t serial, uint32_t time, uint32_t sym,
		  uint32_t state, uint32_t modifiers)
{
}

static void
text_input_enter(void *data, struct zwp_text_input_v1 *text_input,
		 struct wl_surface *surface)
{
}

static void
text_input_leave(void *data, struct zwp_text_input_v1 *text_input)
{
}

static void
text_input_input_panel_state(void *data,
			     struct zwp_text_input_v1 *text_input,
			     uint32_t state)
{
}

static void
text_input_language(void *data, struct zwp_text_input_v1 *text_input,
		    uint32_t serial, const char *language)
{
}

static void
text_input_text_direction(void *data, struct zwp_text_input_v1 *text_input,
			  uint32_t serial, uint32_t direction)
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
	text_input_text_direction,
};

static struct global *
find_global(struct client *client, const char *interface)
{
	struct global *global;

	wl_list_for_each(global, &client->global_list, link) {
		if (strcmp(global->interface, interface) == 0)
			return global;
	}

	return NULL;
}

static struct zwp_input_panel_v1 *
bind_input_panel(struct client *client)
{
	struct global *global;

	global = find_global(client, "zwp_input_panel_v1");
	assert(global);
	return wl_registry_bind(client->wl_registry, global->name,
				&zwp_input_panel_v1_interface, 1);
}

static struct zwp_input_method_v1 *
bind_input_method(struct client *client, struct input_method_state *state)
{
	struct global *global;
	struct zwp_input_method_v1 *input_method;

	global = find_global(client, "zwp_input_method_v1");
	assert(global);
	input_method = wl_registry_bind(client->wl_registry, global->name,
					&zwp_input_method_v1_interface, 1);
	zwp_input_method_v1_add_listener(input_method, &input_method_listener,
					 state);
	return input_method;
}

static struct zwp_text_input_manager_v1 *
bind_text_input_manager(struct client *client)
{
	struct global *global;

	global = find_global(client, "zwp_text_input_manager_v1");
	assert(global);
	return wl_registry_bind(client->wl_registry, global->name,
				&zwp_text_input_manager_v1_interface, 1);
}

static struct weston_rdprail_test *
bind_rdprail_test(struct client *client, struct rdprail_test_state *state)
{
	struct global *global;
	struct weston_rdprail_test *rdprail_test;

	global = find_global(client, "weston_rdprail_test");
	assert(global);
	rdprail_test = wl_registry_bind(client->wl_registry, global->name,
					&weston_rdprail_test_interface, 1);
	weston_rdprail_test_add_listener(rdprail_test, &rdprail_test_listener,
					 state);
	return rdprail_test;
}

static void
get_rdprail_window_state(struct client *client,
			 struct weston_rdprail_test *rdprail_test,
			 struct rdprail_test_state *state,
			 struct wl_surface *surface,
			 struct wl_surface *owner)
{
	state->received = false;
	weston_rdprail_test_get_window_state(rdprail_test, surface, owner);
	client_roundtrip(client);
	assert(state->received);
}

static void
set_output_mode(struct client *client)
{
	weston_test_set_output_mode(client->test->weston_test,
				    client->output->wl_output, 320, 240);
	client_roundtrip(client);
	assert(client->output->width == 320);
	assert(client->output->height == 240);
}

static void
setup_client_surface(struct client *client, int x, int y)
{
	client->surface = create_test_surface(client);
	client->surface->width = 20;
	client->surface->height = 20;
	client->surface->buffer = create_shm_buffer_a8r8g8b8(client, 20, 20);
	weston_test_move_surface(client->test->weston_test,
				 client->surface->wl_surface, x, y);
	wl_surface_attach(client->surface->wl_surface,
			  client->surface->buffer->proxy, 0, 0);
	wl_surface_damage(client->surface->wl_surface, 0, 0, 20, 20);
	wl_surface_commit(client->surface->wl_surface);
	client_roundtrip(client);
}

static struct zwp_input_panel_surface_v1 *
create_overlay_panel(struct client *client,
			     struct zwp_input_panel_v1 *input_panel,
			     struct wl_surface **surface_out,
			     struct buffer **buffer_out)
{
	struct wl_surface *surface;
	struct zwp_input_panel_surface_v1 *panel_surface;
	struct buffer *buffer;
	pixman_color_t color;

	surface = wl_compositor_create_surface(client->wl_compositor);
	assert(surface);
	panel_surface = zwp_input_panel_v1_get_input_panel_surface(input_panel,
							      surface);
	zwp_input_panel_surface_v1_set_overlay_panel(panel_surface);

	buffer = create_shm_buffer_a8r8g8b8(client, 8, 8);
	color_rgb888(&color, 255, 0, 0);
	fill_image_with_color(buffer->image, &color);
	wl_surface_attach(surface, buffer->proxy, 0, 0);
	wl_surface_damage(surface, 0, 0, 8, 8);
	wl_surface_commit(surface);

	*surface_out = surface;
	*buffer_out = buffer;
	return panel_surface;
}

static void
check_repeated_role(void)
{
	struct client *client;
	struct zwp_input_panel_v1 *input_panel;
	struct wl_surface *surface;
	struct zwp_input_panel_surface_v1 *panel_surface;

	client = create_client();
	input_panel = bind_input_panel(client);
	surface = wl_compositor_create_surface(client->wl_compositor);
	panel_surface = zwp_input_panel_v1_get_input_panel_surface(input_panel,
							      surface);
	zwp_input_panel_surface_v1_set_overlay_panel(panel_surface);
	zwp_input_panel_surface_v1_set_overlay_panel(panel_surface);
	expect_protocol_error(client, &zwp_input_panel_surface_v1_interface,
			      WL_DISPLAY_ERROR_INVALID_OBJECT);
}

static void
check_mixed_roles(void)
{
	struct client *client;
	struct zwp_input_panel_v1 *input_panel;
	struct wl_surface *surface;
	struct zwp_input_panel_surface_v1 *panel_surface;

	client = create_client();
	input_panel = bind_input_panel(client);
	surface = wl_compositor_create_surface(client->wl_compositor);
	panel_surface = zwp_input_panel_v1_get_input_panel_surface(input_panel,
							      surface);
	zwp_input_panel_surface_v1_set_toplevel(panel_surface,
						 client->output->wl_output,
						 ZWP_INPUT_PANEL_SURFACE_V1_POSITION_CENTER_BOTTOM);
	zwp_input_panel_surface_v1_set_overlay_panel(panel_surface);
	expect_protocol_error(client, &zwp_input_panel_surface_v1_interface,
			      WL_DISPLAY_ERROR_INVALID_OBJECT);
}

static void
check_position_and_hide(void)
{
	struct client *client;
	struct input_method_state input_method_state = {};
	struct zwp_input_method_v1 *input_method;
	struct zwp_input_panel_v1 *input_panel;
	struct zwp_input_panel_surface_v1 *panel_surface;
	struct zwp_text_input_manager_v1 *text_input_manager;
	struct zwp_text_input_v1 *text_input;
	struct weston_rdprail_test *rdprail_test;
	struct rdprail_test_state rdprail_state = {};
	struct wl_surface *panel;
	struct buffer *panel_buffer;
	struct weston_test_surface_state state;
	bool owner_matches;
	uint32_t show_state_requested;

	client = create_client();
	set_output_mode(client);
	setup_client_surface(client, 100, 50);
	input_method = bind_input_method(client, &input_method_state);
	input_panel = bind_input_panel(client);
	rdprail_test = bind_rdprail_test(client, &rdprail_state);
	panel_surface = create_overlay_panel(client, input_panel, &panel,
					     &panel_buffer);
	text_input_manager = bind_text_input_manager(client);
	text_input = zwp_text_input_manager_v1_create_text_input(text_input_manager);
	zwp_text_input_v1_add_listener(text_input, &text_input_listener, client);

	weston_test_activate_surface(client->test->weston_test,
				     client->surface->wl_surface);
	zwp_text_input_v1_activate(text_input, client->input->wl_seat,
				   client->surface->wl_surface);
	zwp_text_input_v1_set_cursor_rectangle(text_input, 7, 11, 1, 1);
	zwp_text_input_v1_show_input_panel(text_input);
	client_roundtrip(client);
	assert(input_method_state.context);

	state = get_surface_state(client, panel);
	assert(state.mapped);
	assert(wl_fixed_to_int(state.x) == 108);
	assert(wl_fixed_to_int(state.y) == 62);
	get_rdprail_window_state(client, rdprail_test, &rdprail_state, panel,
				 client->surface->wl_surface);
	owner_matches = rdprail_state.owner_matches;
	show_state_requested = rdprail_state.show_state_requested;
	assert(owner_matches);
	assert(show_state_requested == RDP_WINDOW_SHOW);

	zwp_text_input_v1_hide_input_panel(text_input);
	client_roundtrip(client);
	state = get_surface_state(client, panel);
	assert(!state.mapped);
	get_rdprail_window_state(client, rdprail_test, &rdprail_state, panel,
				 client->surface->wl_surface);
	owner_matches = rdprail_state.owner_matches;
	show_state_requested = rdprail_state.show_state_requested;
	assert(owner_matches);
	assert(show_state_requested == RDP_WINDOW_HIDE);

	zwp_text_input_v1_show_input_panel(text_input);
	client_roundtrip(client);
	state = get_surface_state(client, panel);
	assert(state.mapped);

	surface_destroy(client->surface);
	client->surface = NULL;
	client_roundtrip(client);
	state = get_surface_state(client, panel);
	assert(!state.mapped);
	wl_surface_commit(panel);
	client_roundtrip(client);
	state = get_surface_state(client, panel);
	assert(!state.mapped);

	zwp_text_input_v1_destroy(text_input);
	zwp_text_input_manager_v1_destroy(text_input_manager);
	weston_rdprail_test_destroy(rdprail_test);
	wl_surface_destroy(panel);
	wl_proxy_destroy((struct wl_proxy *) panel_surface);
	wl_proxy_destroy((struct wl_proxy *) input_panel);
	wl_proxy_destroy((struct wl_proxy *) input_method);
	buffer_destroy(panel_buffer);
	client_destroy(client);
}

static void
check_surface_destroy(void)
{
	struct client *client;
	struct zwp_input_panel_v1 *input_panel;
	struct zwp_input_panel_surface_v1 *panel_surface;
	struct wl_surface *panel;
	struct buffer *panel_buffer;

	client = create_client();
	input_panel = bind_input_panel(client);
	panel_surface = create_overlay_panel(client, input_panel, &panel,
					     &panel_buffer);
	wl_surface_destroy(panel);
	client_roundtrip(client);

	wl_proxy_destroy((struct wl_proxy *) panel_surface);
	wl_proxy_destroy((struct wl_proxy *) input_panel);
	buffer_destroy(panel_buffer);
	client_destroy(client);
}

static void
check_output_destroy(void)
{
	struct client *client;
	struct input_method_state input_method_state = {};
	struct zwp_input_method_v1 *input_method;
	struct zwp_input_panel_v1 *input_panel;
	struct zwp_input_panel_surface_v1 *panel_surface;
	struct zwp_text_input_manager_v1 *text_input_manager;
	struct zwp_text_input_v1 *text_input;
	struct wl_surface *panel;
	struct buffer *buffer;
	struct weston_test_surface_state state;

	client = create_client();
	setup_client_surface(client, 0, 0);
	input_method = bind_input_method(client, &input_method_state);
	input_panel = bind_input_panel(client);
	panel = wl_compositor_create_surface(client->wl_compositor);
	panel_surface = zwp_input_panel_v1_get_input_panel_surface(input_panel,
							      panel);
	zwp_input_panel_surface_v1_set_toplevel(panel_surface,
						 client->output->wl_output,
						 ZWP_INPUT_PANEL_SURFACE_V1_POSITION_CENTER_BOTTOM);
	buffer = create_shm_buffer_a8r8g8b8(client, 8, 8);
	wl_surface_attach(panel, buffer->proxy, 0, 0);
	wl_surface_damage(panel, 0, 0, 8, 8);
	wl_surface_commit(panel);
	text_input_manager = bind_text_input_manager(client);
	text_input = zwp_text_input_manager_v1_create_text_input(text_input_manager);
	zwp_text_input_v1_add_listener(text_input, &text_input_listener, client);
	weston_test_activate_surface(client->test->weston_test,
				     client->surface->wl_surface);
	zwp_text_input_v1_activate(text_input, client->input->wl_seat,
				   client->surface->wl_surface);
	zwp_text_input_v1_show_input_panel(text_input);
	client_roundtrip(client);
	assert(input_method_state.context);
	state = get_surface_state(client, panel);
	assert(state.mapped);

	weston_test_destroy_output(client->test->weston_test,
				  client->output->wl_output);
	client_roundtrip(client);
	state = get_surface_state(client, panel);
	assert(!state.mapped);
	wl_surface_commit(panel);
	client_roundtrip(client);
	state = get_surface_state(client, panel);
	assert(!state.mapped);

	zwp_text_input_v1_destroy(text_input);
	zwp_text_input_manager_v1_destroy(text_input_manager);
	wl_surface_destroy(panel);
	wl_proxy_destroy((struct wl_proxy *) panel_surface);
	wl_proxy_destroy((struct wl_proxy *) input_panel);
	wl_proxy_destroy((struct wl_proxy *) input_method);
	buffer_destroy(buffer);
	client_destroy(client);
}

TEST(input_panel)
{
	check_repeated_role();
	check_mixed_roles();
	check_position_and_hide();
	check_surface_destroy();
	check_output_destroy();
}
