/*
 * Copyright © 2010-2012 Intel Corporation
 * Copyright © 2011-2012 Collabora, Ltd.
 * Copyright © 2013 Raspberry Pi Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libweston/backend-rdp.h>

#include "shell.h"
#include "input-method-unstable-v1-server-protocol.h"
#include "shared/helpers.h"

struct input_panel_surface {
	struct wl_resource *resource;
	struct wl_signal destroy_signal;

	struct desktop_shell *shell;

	struct wl_list link;
	struct weston_surface *surface;
	struct weston_view *view;
	struct wl_listener surface_destroy_listener;
	struct wl_listener output_destroy_listener;
	struct wl_event_source *hide_idle;

	struct weston_output *output;
	uint32_t panel;
	bool role_set;
};

static void
hide_input_panels_now(struct desktop_shell *shell);

static void
input_panel_handle_owner_destroy(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell,
			     text_input.surface_destroy_listener);

	shell->text_input.surface = NULL;
	shell->text_input.surface_destroy_listener.notify = NULL;
	hide_input_panels_now(shell);

	(void)data;
}

static void
input_panel_set_owner(struct desktop_shell *shell,
		      struct weston_surface *surface)
{
	if (shell->text_input.surface_destroy_listener.notify) {
		wl_list_remove(&shell->text_input.surface_destroy_listener.link);
		shell->text_input.surface_destroy_listener.notify = NULL;
	}

	shell->text_input.surface = surface;
	if (!surface)
		return;

	shell->text_input.surface_destroy_listener.notify =
		input_panel_handle_owner_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &shell->text_input.surface_destroy_listener);
}

static void
input_panel_surface_set_rail_visibility(struct input_panel_surface *ipsurf,
					bool visible)
{
	const struct weston_rdprail_api *api = ipsurf->shell->rdprail_api;

	if (api->set_window_visible)
		api->set_window_visible(ipsurf->surface, visible);
}

static void
hide_input_panel_surface(struct input_panel_surface *ipsurf)
{
	if (ipsurf->hide_idle) {
		wl_event_source_remove(ipsurf->hide_idle);
		ipsurf->hide_idle = NULL;
	}

	if (!weston_surface_is_mapped(ipsurf->surface))
		return;

	/* RAIL needs the hide update before unmapping clears output_mask. */
	input_panel_surface_set_rail_visibility(ipsurf, false);
	weston_surface_unmap(ipsurf->surface);
}

static void
hide_input_panel_surface_idle(void *data)
{
	struct input_panel_surface *ipsurf = data;

	ipsurf->hide_idle = NULL;
	hide_input_panel_surface(ipsurf);
}

static void
input_panel_handle_output_destroy(struct wl_listener *listener, void *data)
{
	struct input_panel_surface *ipsurf =
		container_of(listener, struct input_panel_surface,
			     output_destroy_listener);

	ipsurf->output = NULL;
	ipsurf->output_destroy_listener.notify = NULL;
	input_panel_surface_set_rail_visibility(ipsurf, false);
	if (weston_surface_is_mapped(ipsurf->surface))
		ipsurf->hide_idle = wl_event_loop_add_idle(
			wl_display_get_event_loop(ipsurf->shell->compositor->wl_display),
			hide_input_panel_surface_idle, ipsurf);

	(void)data;
}

static void
input_panel_surface_set_output(struct input_panel_surface *ipsurf,
				       struct weston_output *output)
{
	if (ipsurf->output_destroy_listener.notify) {
		wl_list_remove(&ipsurf->output_destroy_listener.link);
		ipsurf->output_destroy_listener.notify = NULL;
	}

	ipsurf->output = output;
	if (!output)
		return;

	ipsurf->output_destroy_listener.notify = input_panel_handle_output_destroy;
	wl_signal_add(&output->destroy_signal,
		      &ipsurf->output_destroy_listener);
}

static void
position_input_panel_surface(struct input_panel_surface *ipsurf)
{
	struct desktop_shell *shell = ipsurf->shell;
	struct weston_view *view;
	float x, y;

	if (ipsurf->panel) {
		if (!shell->text_input.surface)
			return;

		view = get_default_view(shell->text_input.surface);
		if (!view)
			return;

		weston_view_to_global_float(
			view,
			shell->text_input.cursor_rectangle.x2,
			shell->text_input.cursor_rectangle.y2,
			&x, &y);
	} else {
		if (!ipsurf->output)
			return;

		x = ipsurf->output->x +
			(ipsurf->output->width - ipsurf->surface->width) / 2;
		y = ipsurf->output->y + ipsurf->output->height -
			ipsurf->surface->height;
	}

	weston_view_set_position(ipsurf->view, x, y);
}

static void
show_input_panel_surface(struct input_panel_surface *ipsurf)
{
	struct desktop_shell *shell = ipsurf->shell;
	struct weston_surface *owner;
	const struct weston_rdprail_api *api = shell->rdprail_api;

	owner = weston_surface_get_main_surface(shell->text_input.surface);
	if (api->set_window_owner)
		api->set_window_owner(ipsurf->surface, owner);
	position_input_panel_surface(ipsurf);
	weston_layer_entry_insert(&shell->input_panel_layer.view_list,
				  &ipsurf->view->layer_link);
	weston_view_geometry_dirty(ipsurf->view);
	weston_view_update_transform(ipsurf->view);
	ipsurf->surface->is_mapped = true;
	ipsurf->view->is_mapped = true;
	input_panel_surface_set_rail_visibility(ipsurf, true);
	weston_surface_damage(ipsurf->surface);
}

static void
show_input_panels(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell,
			     show_input_panel_listener);
	struct input_panel_surface *ipsurf, *next;

	input_panel_set_owner(shell, data);
	if (shell->showing_input_panels)
		return;

	shell->showing_input_panels = true;
	weston_layer_set_position(&shell->input_panel_layer,
				  WESTON_LAYER_POSITION_TOP_UI);

	wl_list_for_each_safe(ipsurf, next,
			      &shell->input_panel.surfaces, link) {
		if (ipsurf->surface->width == 0 ||
		    (!ipsurf->panel && !ipsurf->output))
			continue;

		show_input_panel_surface(ipsurf);
	}
}

static void
hide_input_panels_now(struct desktop_shell *shell)
{
	struct input_panel_surface *ipsurf;

	if (!shell->showing_input_panels)
		return;

	shell->showing_input_panels = false;
	weston_layer_unset_position(&shell->input_panel_layer);

	wl_list_for_each(ipsurf, &shell->input_panel.surfaces, link)
		hide_input_panel_surface(ipsurf);
}

static void
hide_input_panels(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell,
			     hide_input_panel_listener);

	hide_input_panels_now(shell);
	input_panel_set_owner(shell, NULL);

	(void)data;
}

static void
update_input_panels(struct wl_listener *listener, void *data)
{
	struct desktop_shell *shell =
		container_of(listener, struct desktop_shell,
			     update_input_panel_listener);
	struct input_panel_surface *ipsurf;

	memcpy(&shell->text_input.cursor_rectangle, data,
	       sizeof shell->text_input.cursor_rectangle);

	wl_list_for_each(ipsurf, &shell->input_panel.surfaces, link) {
		if (ipsurf->panel && weston_surface_is_mapped(ipsurf->surface)) {
			position_input_panel_surface(ipsurf);
			weston_view_update_transform(ipsurf->view);
			weston_surface_damage(ipsurf->surface);
		}
	}
}

static int
input_panel_get_label(struct weston_surface *surface, char *buf, size_t len)
{
	return snprintf(buf, len, "rdprail-shell input panel");
}

static void
input_panel_committed(struct weston_surface *surface, int32_t sx, int32_t sy)
{
	struct input_panel_surface *ipsurf = surface->committed_private;

	if (surface->width == 0)
		return;
	if (!ipsurf->panel && !ipsurf->output)
		return;

	position_input_panel_surface(ipsurf);
	if (!weston_surface_is_mapped(surface) &&
	    ipsurf->shell->showing_input_panels)
		show_input_panel_surface(ipsurf);

	(void)sx;
	(void)sy;
}

static void
destroy_input_panel_surface(struct input_panel_surface *input_panel_surface)
{
	wl_signal_emit(&input_panel_surface->destroy_signal, input_panel_surface);

	if (input_panel_surface->hide_idle)
		wl_event_source_remove(input_panel_surface->hide_idle);
	wl_list_remove(&input_panel_surface->surface_destroy_listener.link);
	input_panel_surface_set_output(input_panel_surface, NULL);
	wl_list_remove(&input_panel_surface->link);

	input_panel_surface->surface->committed = NULL;
	input_panel_surface->surface->committed_private = NULL;
	weston_surface_set_label_func(input_panel_surface->surface, NULL);
	weston_view_destroy(input_panel_surface->view);

	free(input_panel_surface);
}

static void
input_panel_handle_surface_destroy(struct wl_listener *listener, void *data)
{
	struct input_panel_surface *ipsurface = container_of(listener,
							     struct input_panel_surface,
							     surface_destroy_listener);

	if (ipsurface->resource) {
		wl_resource_destroy(ipsurface->resource);
	} else {
		destroy_input_panel_surface(ipsurface);
	}
}

static struct input_panel_surface *
create_input_panel_surface(struct desktop_shell *shell,
			   struct weston_surface *surface)
{
	struct input_panel_surface *input_panel_surface;

	input_panel_surface = calloc(1, sizeof *input_panel_surface);
	if (!input_panel_surface)
		return NULL;

	input_panel_surface->shell = shell;
	input_panel_surface->surface = surface;
	input_panel_surface->view = weston_view_create(surface);
	if (!input_panel_surface->view) {
		free(input_panel_surface);
		return NULL;
	}

	surface->committed = input_panel_committed;
	surface->committed_private = input_panel_surface;
	weston_surface_set_label_func(surface, input_panel_get_label);

	wl_signal_init(&input_panel_surface->destroy_signal);
	input_panel_surface->surface_destroy_listener.notify = input_panel_handle_surface_destroy;
	wl_signal_add(&surface->destroy_signal,
		      &input_panel_surface->surface_destroy_listener);
	wl_list_init(&input_panel_surface->link);

	return input_panel_surface;
}

static void
input_panel_surface_set_toplevel(struct wl_client *client,
				 struct wl_resource *resource,
				 struct wl_resource *output_resource,
				 uint32_t position)
{
	struct input_panel_surface *ipsurf =
		wl_resource_get_user_data(resource);
	struct weston_head *head;

	if (ipsurf->role_set) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "input panel surface role already set");
		return;
	}

	head = weston_head_from_resource(output_resource);
	if (!head || !head->output) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "input panel output is unavailable");
		return;
	}

	wl_list_insert(&ipsurf->shell->input_panel.surfaces, &ipsurf->link);
	input_panel_surface_set_output(ipsurf, head->output);
	ipsurf->panel = 0;
	ipsurf->role_set = true;
	input_panel_surface_set_rail_visibility(ipsurf, false);

	(void)client;
	(void)position;
}

static void
input_panel_surface_set_overlay_panel(struct wl_client *client,
				      struct wl_resource *resource)
{
	struct input_panel_surface *ipsurf =
		wl_resource_get_user_data(resource);

	if (ipsurf->role_set) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "input panel surface role already set");
		return;
	}

	wl_list_insert(&ipsurf->shell->input_panel.surfaces, &ipsurf->link);
	ipsurf->panel = 1;
	ipsurf->role_set = true;
	input_panel_surface_set_rail_visibility(ipsurf, false);

	(void)client;
}

static const struct zwp_input_panel_surface_v1_interface input_panel_surface_implementation = {
	input_panel_surface_set_toplevel,
	input_panel_surface_set_overlay_panel
};

static struct input_panel_surface *
get_input_panel_surface(struct weston_surface *surface)
{
	if (surface->committed == input_panel_committed) {
		return surface->committed_private;
	} else {
		return NULL;
	}
}

static void
destroy_input_panel_surface_resource(struct wl_resource *resource)
{
	struct input_panel_surface *ipsurf =
		wl_resource_get_user_data(resource);

	destroy_input_panel_surface(ipsurf);
}

static void
input_panel_get_input_panel_surface(struct wl_client *client,
				    struct wl_resource *resource,
				    uint32_t id,
				    struct wl_resource *surface_resource)
{
	struct weston_surface *surface =
		wl_resource_get_user_data(surface_resource);
	struct desktop_shell *shell = wl_resource_get_user_data(resource);
	struct input_panel_surface *ipsurf;

	if (get_input_panel_surface(surface)) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "wl_input_panel::get_input_panel_surface already requested");
		return;
	}

	ipsurf = create_input_panel_surface(shell, surface);
	if (!ipsurf) {
		wl_resource_post_error(surface_resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface->committed already set");
		return;
	}

	ipsurf->resource =
		wl_resource_create(client,
				   &zwp_input_panel_surface_v1_interface,
				   1,
				   id);
	wl_resource_set_implementation(ipsurf->resource,
				       &input_panel_surface_implementation,
				       ipsurf,
				       destroy_input_panel_surface_resource);
}

static const struct zwp_input_panel_v1_interface input_panel_implementation = {
	input_panel_get_input_panel_surface
};

static void
unbind_input_panel(struct wl_resource *resource)
{
	struct desktop_shell *shell = wl_resource_get_user_data(resource);

	shell->input_panel.binding = NULL;
}

static void
bind_input_panel(struct wl_client *client,
	      void *data, uint32_t version, uint32_t id)
{
	struct desktop_shell *shell = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &zwp_input_panel_v1_interface, 1, id);

	if (shell->input_panel.binding == NULL) {
		wl_resource_set_implementation(resource,
					       &input_panel_implementation,
					       shell, unbind_input_panel);
		shell->input_panel.binding = resource;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "interface object already bound");
}

void
input_panel_destroy(struct desktop_shell *shell)
{
	input_panel_set_owner(shell, NULL);
	wl_list_remove(&shell->show_input_panel_listener.link);
	wl_list_remove(&shell->hide_input_panel_listener.link);
	wl_list_remove(&shell->update_input_panel_listener.link);
}

int
input_panel_setup(struct desktop_shell *shell)
{
	struct weston_compositor *ec = shell->compositor;

	shell->show_input_panel_listener.notify = show_input_panels;
	wl_signal_add(&ec->show_input_panel_signal,
		      &shell->show_input_panel_listener);
	shell->hide_input_panel_listener.notify = hide_input_panels;
	wl_signal_add(&ec->hide_input_panel_signal,
		      &shell->hide_input_panel_listener);
	shell->update_input_panel_listener.notify = update_input_panels;
	wl_signal_add(&ec->update_input_panel_signal,
		      &shell->update_input_panel_listener);

	wl_list_init(&shell->input_panel.surfaces);

	if (wl_global_create(shell->compositor->wl_display,
			     &zwp_input_panel_v1_interface, 1,
			     shell, bind_input_panel) == NULL)
		return -1;

	return 0;
}
