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
#include <string.h>

#include "input-method-unstable-v1-client-protocol.h"
#include "test-config.h"
#include "weston-test-client-helper.h"
#include "weston-test-fixture-compositor.h"

struct setup_args {
	const char *config_file;
	bool allow_external;
};

static const struct setup_args setup_args[] = {
	{ TESTSUITE_INPUT_METHOD_DENY_CONFIG_PATH, false },
	{ TESTSUITE_INPUT_METHOD_ALLOW_CONFIG_PATH, true },
};

static enum test_result_code
fixture_setup(struct weston_test_harness *harness,
	      const struct setup_args *args)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.config_file = args->config_file;
	setup.shell = SHELL_DESKTOP;

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP_WITH_ARG(fixture_setup, setup_args);

static struct zwp_input_method_v1 *
bind_input_method(struct client *client)
{
	struct global *global;

	wl_list_for_each(global, &client->global_list, link) {
		if (strcmp(global->interface, "zwp_input_method_v1") == 0)
			return wl_registry_bind(
				client->wl_registry, global->name,
				&zwp_input_method_v1_interface, 1);
	}

	return NULL;
}

static struct zwp_input_method_v1 *
bind_input_method_successfully(struct client *client)
{
	struct zwp_input_method_v1 *input_method;

	input_method = bind_input_method(client);
	assert(input_method);
	client_roundtrip(client);
	assert(wl_display_get_error(client->wl_display) == 0);

	return input_method;
}

TEST(input_method_external_bind)
{
	const struct setup_args *args;
	struct zwp_input_method_v1 *input_method;
	struct client *client;
	struct client *other_client;

	args = &setup_args[get_test_fixture_index()];
	client = create_client();
	input_method = bind_input_method(client);
	assert(input_method);

	if (!args->allow_external) {
		expect_protocol_error(client, &zwp_input_method_v1_interface,
				      WL_DISPLAY_ERROR_INVALID_OBJECT);
		return;
	}

	client_roundtrip(client);
	assert(wl_display_get_error(client->wl_display) == 0);
	wl_proxy_destroy((struct wl_proxy *) input_method);
	client_destroy(client);

	client = create_client();
	input_method = bind_input_method_successfully(client);

	other_client = create_client();
	assert(bind_input_method(other_client));
	expect_protocol_error(other_client, &zwp_input_method_v1_interface,
			      WL_DISPLAY_ERROR_INVALID_OBJECT);

	wl_proxy_destroy((struct wl_proxy *) input_method);
}
