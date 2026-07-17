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
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
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

#include <stdint.h>

#include <libweston/backend-rdp.h>
#include <libweston/libweston.h>

#include "compositor/weston.h"
#include "weston-rdprail-test-server-protocol.h"

static void
get_window_state(struct wl_client *client, struct wl_resource *resource,
		 struct wl_resource *surface_resource,
		 struct wl_resource *owner_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct weston_surface *owner = owner_resource ?
		wl_resource_get_user_data(owner_resource) : NULL;
	struct weston_surface_rail_state *rail_state;
	uint32_t owner_matches = 0;
	uint32_t show_state_requested = RDP_WINDOW_HIDE;

	if (!surface) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface is unavailable");
		return;
	}

	rail_state = surface->backend_state;
	if (rail_state) {
		owner_matches = rail_state->parent_surface == owner;
		show_state_requested = rail_state->showState_requested;
	}

	weston_rdprail_test_send_window_state(resource, owner_matches,
					      show_state_requested);

	(void)client;
}

static const struct weston_rdprail_test_interface test_implementation = {
	get_window_state,
};

static void
bind_rdprail_test(struct wl_client *client, void *data,
		  uint32_t version, uint32_t id)
{
	struct wl_resource *resource;

	resource = wl_resource_create(client, &weston_rdprail_test_interface,
				      1, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &test_implementation,
				       data, NULL);

	(void)version;
}

WL_EXPORT int
wet_module_init(struct weston_compositor *compositor,
		int *argc, char *argv[])
{
	if (!weston_rdprail_get_api(compositor)) {
		weston_log("RDPRAIL test requires the RDP backend.\n");
		return -1;
	}

	if (!wl_global_create(compositor->wl_display,
			      &weston_rdprail_test_interface, 1,
			      compositor, bind_rdprail_test))
		return -1;

	(void)argc;
	(void)argv;

	return 0;
}
