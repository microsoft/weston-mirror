/*
 * Copyright © 2016 Quentin "Sardem FF7" Glidic
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

#include <string.h>
#include <assert.h>
#include <math.h>
#include <limits.h>

#include <wayland-server.h>

#include <libweston/libweston.h>
#include <libweston/zalloc.h>

#include <libweston-desktop/libweston-desktop.h>
#include "internal.h"
#include "shared/popup-constraint.h"

struct weston_desktop_view {
	struct wl_list link;
	struct weston_view *view;
	struct weston_desktop_view *parent;
	struct wl_list children_list;
	struct wl_list children_link;
};

struct weston_desktop_surface {
	struct weston_desktop *desktop;
	struct weston_desktop_client *client;
	struct wl_list client_link;
	const struct weston_desktop_surface_implementation *implementation;
	void *implementation_data;
	void *user_data;
	struct weston_surface *surface;
	struct wl_list view_list;
	struct weston_position buffer_move;
	struct wl_listener surface_commit_listener;
	struct wl_listener surface_destroy_listener;
	struct wl_listener client_destroy_listener;
	struct wl_listener transform_listener;
	struct wl_listener output_resize_listener;
	bool updating_position;
	bool popup;
	bool popup_mapped;
	bool popup_dismissed;
	bool popup_unmapped;
	struct wl_list children_list;

	struct wl_list resource_list;
	bool has_geometry;
	struct weston_geometry geometry;
	struct {
		char *title;
		char *app_id;
		pid_t pid;
		struct wl_signal metadata_signal;
	};
	struct {
		struct weston_desktop_surface *parent;
		struct wl_list children_link;
		struct weston_position position;
		bool use_geometry;
	};
	struct {
		struct wl_list grab_link;
	};
};

static void
weston_desktop_surface_update_view_position(struct weston_desktop_surface *surface)
{
	struct weston_desktop_view *view;
	int32_t x, y;

	x = surface->position.x;
	y = surface->position.y;

	if (surface->use_geometry) {
		struct weston_desktop_surface *parent =
			weston_desktop_surface_get_parent(surface);
		struct weston_geometry geometry, parent_geometry;

		geometry = weston_desktop_surface_get_geometry(surface);
		parent_geometry = weston_desktop_surface_get_geometry(parent);

		x += parent_geometry.x - geometry.x;
		y += parent_geometry.y - geometry.y;
	}
	wl_list_for_each(view, &surface->view_list, link)
		weston_view_set_position(view->view, x, y);
}


static void
weston_desktop_view_propagate_layer(struct weston_desktop_view *view);

WL_EXPORT void
weston_desktop_surface_update_popup_positions(struct weston_desktop_surface *surface)
{
	struct weston_desktop_surface *child;

	if (surface->updating_position || surface->popup_dismissed ||
	    surface->popup_unmapped)
		return;
	surface->updating_position = true;
	if (surface->popup && surface->parent &&
	    surface->implementation->update_position)
		surface->implementation->update_position(surface,
						 surface->implementation_data);
	wl_list_for_each(child, &surface->children_list, children_link)
		weston_desktop_surface_update_popup_positions(child);
	surface->updating_position = false;
}

static void
desktop_surface_transform_changed(struct wl_listener *listener, void *data)
{
	struct weston_desktop_surface *surface =
		wl_container_of(listener, surface, transform_listener);
	struct weston_desktop_surface *child;

	if (data != surface->surface || surface->updating_position)
		return;
	/* A popup's own transform is the result, not an input, of placement. */
	wl_list_for_each(child, &surface->children_list, children_link)
		weston_desktop_surface_update_popup_positions(child);
}

static void
desktop_surface_output_resized(struct wl_listener *listener, void *data)
{
	struct weston_desktop_surface *surface =
		wl_container_of(listener, surface, output_resize_listener);

	if (!surface->parent)
		weston_desktop_surface_update_popup_positions(surface);
}

void
weston_desktop_surface_set_popup(struct weston_desktop_surface *surface)
{
	surface->popup = true;
}

void
weston_desktop_surface_constrain_popup(struct weston_desktop_surface *surface,
				       struct weston_desktop_surface *parent,
				       struct weston_geometry *geometry,
				       struct weston_geometry anchor,
				       struct weston_position offset,
				       uint32_t adjustment)
{
	struct weston_view *view;
	struct weston_geometry pg;
	pixman_rectangle32_t area;
	float x1, y1, x2, y2;
	int64_t x = geometry->x, y = geometry->y;
	int64_t width = geometry->width, height = geometry->height;
	int64_t flip_x, flip_y;

	if (!adjustment || wl_list_empty(&parent->surface->views))
		return;
	view = wl_container_of(parent->surface->views.next, view, surface_link);
	weston_view_update_transform(view);
	if (!view->output)
		return;
	weston_desktop_api_get_work_area(surface->desktop, view->output, &area);
	if (!area.width || !area.height)
		return;
	pg = weston_desktop_surface_get_geometry(parent);
	weston_view_from_global_float(view, area.x, area.y, &x1, &y1);
	weston_view_from_global_float(view, (double)area.x + area.width,
				     (double)area.y + area.height, &x2, &y2);
	/* Reflect anchor and gravity, but preserve the requested offset. */
	flip_x = 2 * (int64_t)anchor.x + anchor.width - x - width +
		 2 * (int64_t)offset.x;
	flip_y = 2 * (int64_t)anchor.y + anchor.height - y - height +
		 2 * (int64_t)offset.y;
	/* stable and v6 use the same constraint-adjustment bit values. */
	popup_constrain_axis(&x, &width, flip_x,
		ceil(fmin(x1, x2)) - pg.x, floor(fmax(x1, x2)) - pg.x,
		adjustment & 4, adjustment & 1, adjustment & 16);
	popup_constrain_axis(&y, &height, flip_y,
		ceil(fmin(y1, y2)) - pg.y, floor(fmax(y1, y2)) - pg.y,
		adjustment & 8, adjustment & 2, adjustment & 32);
	if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX)
		return;
	geometry->x = x;
	geometry->y = y;
	geometry->width = width;
	geometry->height = height;
}

static void
weston_desktop_view_destroy(struct weston_desktop_view *view)
{
	struct weston_desktop_view *child_view, *tmp;

	wl_list_for_each_safe(child_view, tmp, &view->children_list, children_link)
		weston_desktop_view_destroy(child_view);

	wl_list_remove(&view->children_link);
	wl_list_remove(&view->link);

	weston_view_damage_below(view->view);
	if (view->parent != NULL)
		weston_view_destroy(view->view);

	free(view);
}

void
weston_desktop_surface_destroy(struct weston_desktop_surface *surface)
{
	struct weston_desktop_view *view, *next_view;
	struct weston_desktop_surface *child, *next_child;

	weston_desktop_surface_unmap_popup(surface);
	wl_list_for_each_reverse(child, &surface->children_list, children_link)
		weston_desktop_surface_unmap_popup(child);
	wl_list_remove(&surface->transform_listener.link);
	wl_list_remove(&surface->output_resize_listener.link);

	wl_list_remove(&surface->surface_commit_listener.link);
	wl_list_remove(&surface->surface_destroy_listener.link);
	wl_list_remove(&surface->client_destroy_listener.link);

	if (!wl_list_empty(&surface->resource_list)) {
		struct wl_resource *resource, *tmp;
		wl_resource_for_each_safe(resource, tmp, &surface->resource_list) {
			wl_resource_set_user_data(resource, NULL);
			wl_list_remove(wl_resource_get_link(resource));
		}
	}

	surface->implementation->destroy(surface, surface->implementation_data);

	surface->surface->committed = NULL;
	surface->surface->committed_private = NULL;

	weston_desktop_surface_unset_relative_to(surface);
	wl_list_remove(&surface->client_link);

	wl_list_for_each_safe(child, next_child,
			      &surface->children_list,
			      children_link)
		weston_desktop_surface_unset_relative_to(child);

	wl_list_for_each_safe(view, next_view, &surface->view_list, link)
		weston_desktop_view_destroy(view);

	free(surface->title);
	free(surface->app_id);

	free(surface);
}

static void
weston_desktop_surface_surface_committed(struct wl_listener *listener,
					 void *data)
{
	struct weston_desktop_surface *surface =
		wl_container_of(listener, surface, surface_commit_listener);

	if (surface->popup) {
		if (surface->popup_dismissed || surface->popup_unmapped)
			return;
		if (surface->popup_mapped && !surface->surface->buffer_ref.buffer) {
			weston_desktop_surface_unmap_popup(surface);
			return;
		}
	}

	if (surface->implementation->committed != NULL)
		surface->implementation->committed(surface,
						   surface->implementation_data,
						   surface->buffer_move.x,
						   surface->buffer_move.y);

	if (surface->parent != NULL) {
		struct weston_desktop_view *view;

		/* Apply committed window geometry (including shadow offsets) before
		 * mapping or emitting transforms used to position child popups. */
		weston_desktop_surface_update_view_position(surface);
		wl_list_for_each(view, &surface->view_list, link) {
			weston_view_set_transform_parent(view->view,
							 view->parent->view);
			weston_view_update_transform(view->view);
			weston_desktop_view_propagate_layer(view->parent);
		}
	}

	if (!wl_list_empty(&surface->children_list)) {
		struct weston_desktop_surface *child;

		wl_list_for_each(child, &surface->children_list, children_link)
			weston_desktop_surface_update_view_position(child);
	}

	surface->buffer_move.x = 0;
	surface->buffer_move.y = 0;
	weston_desktop_surface_update_popup_positions(surface);
	if (surface->popup && !surface->popup_mapped &&
	    surface->surface->buffer_ref.buffer) {
		surface->popup_mapped = true;
		weston_desktop_api_popup_state_changed(surface->desktop,
						 surface, true);
	}
}

static void
weston_desktop_surface_surface_destroyed(struct wl_listener *listener,
					 void *data)
{
	struct weston_desktop_surface *surface =
		wl_container_of(listener, surface, surface_destroy_listener);

	weston_desktop_surface_destroy(surface);
}

void
weston_desktop_surface_resource_destroy(struct wl_resource *resource)
{
	struct weston_desktop_surface *surface =
		wl_resource_get_user_data(resource);

	if (surface != NULL)
		weston_desktop_surface_destroy(surface);
}

static void
weston_desktop_surface_committed(struct weston_surface *wsurface,
				 int32_t sx, int32_t sy)
{
	struct weston_desktop_surface *surface = wsurface->committed_private;

	surface->buffer_move.x = sx;
	surface->buffer_move.y = sy;
}

static void
weston_desktop_surface_client_destroyed(struct wl_listener *listener,
					void *data)
{
	struct weston_desktop_surface *surface =
		wl_container_of(listener, surface, client_destroy_listener);

	weston_desktop_surface_destroy(surface);
}

struct weston_desktop_surface *
weston_desktop_surface_create(struct weston_desktop *desktop,
			      struct weston_desktop_client *client,
			      struct weston_surface *wsurface,
			      const struct weston_desktop_surface_implementation *implementation,
			      void *implementation_data)
{
	assert(implementation->destroy != NULL);

	struct weston_desktop_surface *surface;

	surface = zalloc(sizeof(struct weston_desktop_surface));
	if (surface == NULL) {
		if (client != NULL)
			wl_client_post_no_memory(weston_desktop_client_get_client(client));
		return NULL;
	}

	surface->desktop = desktop;
	surface->implementation = implementation;
	surface->implementation_data = implementation_data;
	surface->surface = wsurface;
	surface->transform_listener.notify = desktop_surface_transform_changed;
	wl_signal_add(&wsurface->compositor->transform_signal,
		      &surface->transform_listener);
	surface->output_resize_listener.notify = desktop_surface_output_resized;
	wl_signal_add(&wsurface->compositor->output_resized_signal,
		      &surface->output_resize_listener);

	surface->client = client;
	surface->client_destroy_listener.notify =
		weston_desktop_surface_client_destroyed;
	weston_desktop_client_add_destroy_listener(
		client, &surface->client_destroy_listener);

	wsurface->committed = weston_desktop_surface_committed;
	wsurface->committed_private = surface;

	surface->pid = -1;

	surface->surface_commit_listener.notify =
		weston_desktop_surface_surface_committed;
	wl_signal_add(&surface->surface->commit_signal,
		      &surface->surface_commit_listener);
	surface->surface_destroy_listener.notify =
		weston_desktop_surface_surface_destroyed;
	wl_signal_add(&surface->surface->destroy_signal,
		      &surface->surface_destroy_listener);

	wl_list_init(&surface->client_link);
	wl_list_init(&surface->resource_list);
	wl_list_init(&surface->children_list);
	wl_list_init(&surface->children_link);
	wl_list_init(&surface->view_list);
	wl_list_init(&surface->grab_link);

	wl_signal_init(&surface->metadata_signal);

	return surface;
}

struct wl_resource *
weston_desktop_surface_add_resource(struct weston_desktop_surface *surface,
				    const struct wl_interface *interface,
				    const void *implementation, uint32_t id,
				    wl_resource_destroy_func_t destroy)
{
	struct wl_resource *client_resource =
		weston_desktop_client_get_resource(surface->client);
	struct wl_client *wl_client  =
		weston_desktop_client_get_client(surface->client);
	struct wl_resource *resource;

	resource = wl_resource_create(wl_client,
				      interface,
				      wl_resource_get_version(client_resource),
				      id);
	if (resource == NULL) {
		wl_client_post_no_memory(wl_client);
		weston_desktop_surface_destroy(surface);
		return NULL;
	}
	if (destroy == NULL)
		destroy = weston_desktop_surface_resource_destroy;
	wl_resource_set_implementation(resource, implementation, surface, destroy);
	wl_list_insert(&surface->resource_list, wl_resource_get_link(resource));

	return resource;
}

struct weston_desktop_surface *
weston_desktop_surface_from_grab_link(struct wl_list *grab_link)
{
	struct weston_desktop_surface *surface =
		wl_container_of(grab_link, surface, grab_link);

	return surface;
}

WL_EXPORT bool
weston_surface_is_desktop_surface(struct weston_surface *wsurface)
{
	return wsurface->committed == weston_desktop_surface_committed;
}

WL_EXPORT struct weston_desktop_surface *
weston_surface_get_desktop_surface(struct weston_surface *wsurface)
{
	if (!weston_surface_is_desktop_surface(wsurface))
		return NULL;
	return wsurface->committed_private;
}

WL_EXPORT void
weston_desktop_surface_set_user_data(struct weston_desktop_surface *surface,
				     void *user_data)
{
	surface->user_data = user_data;
}

static struct weston_desktop_view *
weston_desktop_surface_create_desktop_view(struct weston_desktop_surface *surface)
{
	struct wl_client *wl_client=
		weston_desktop_client_get_client(surface->client);
	struct weston_desktop_view *view, *child_view;
	struct weston_view *wview;
	struct weston_desktop_surface *child;

	wview = weston_view_create(surface->surface);
	if (wview == NULL) {
		if (wl_client != NULL)
			wl_client_post_no_memory(wl_client);
		return NULL;
	}

	view = zalloc(sizeof(struct weston_desktop_view));
	if (view == NULL) {
		if (wl_client != NULL)
			wl_client_post_no_memory(wl_client);
		return NULL;
	}

	view->view = wview;
	wl_list_init(&view->children_list);
	wl_list_init(&view->children_link);
	wl_list_insert(surface->view_list.prev, &view->link);

	wl_list_for_each(child, &surface->children_list, children_link) {
		child_view =
			weston_desktop_surface_create_desktop_view(child);
		if (child_view == NULL) {
			weston_desktop_view_destroy(view);
			return NULL;
		}

		child_view->parent = view;
		wl_list_insert(view->children_list.prev,
			       &child_view->children_link);
	}

	return view;
}

WL_EXPORT struct weston_view *
weston_desktop_surface_create_view(struct weston_desktop_surface *surface)
{
	struct weston_desktop_view *view;

	view = weston_desktop_surface_create_desktop_view(surface);
	if (view == NULL)
		return NULL;

	return view->view;
}

WL_EXPORT void
weston_desktop_surface_unlink_view(struct weston_view *wview)
{
	struct weston_desktop_surface *surface;
	struct weston_desktop_view *view;

	if (!weston_surface_is_desktop_surface(wview->surface))
		return;

	surface = weston_surface_get_desktop_surface(wview->surface);
	wl_list_for_each(view, &surface->view_list, link) {
		if (view->view == wview) {
			weston_desktop_view_destroy(view);
			return;
		}
	}
}

static void
weston_desktop_view_propagate_layer(struct weston_desktop_view *view)
{
	struct weston_desktop_view *child;
	struct wl_list *link = &view->view->layer_link.link;

	wl_list_for_each_reverse(child, &view->children_list, children_link) {
		struct weston_layer_entry *prev =
			wl_container_of(link->prev, prev, link);
		struct weston_desktop_surface *surface =
			weston_surface_get_desktop_surface(child->view->surface);

		/* The initial, bufferless xdg commit only requests configure. */
		if (surface && surface->popup &&
		    !child->view->surface->buffer_ref.buffer)
			continue;

		if (prev == &child->view->layer_link)
			continue;

		child->view->is_mapped = true;
		weston_view_damage_below(child->view);
		weston_view_geometry_dirty(child->view);
		weston_layer_entry_remove(&child->view->layer_link);
		weston_layer_entry_insert(prev, &child->view->layer_link);
		weston_view_geometry_dirty(child->view);
		weston_surface_damage(child->view->surface);
		weston_view_update_transform(child->view);

		weston_desktop_view_propagate_layer(child);
	}
}

WL_EXPORT void
weston_desktop_surface_propagate_layer(struct weston_desktop_surface *surface)
{
	struct weston_desktop_view *view;

	wl_list_for_each(view, &surface->view_list, link)
		weston_desktop_view_propagate_layer(view);
}

WL_EXPORT void
weston_desktop_surface_set_activated(struct weston_desktop_surface *surface, bool activated)
{
	if (surface->implementation->set_activated != NULL)
		surface->implementation->set_activated(surface,
						       surface->implementation_data,
						       activated);
}

WL_EXPORT void
weston_desktop_surface_set_fullscreen(struct weston_desktop_surface *surface, bool fullscreen)
{
	if (surface->implementation->set_fullscreen != NULL)
		surface->implementation->set_fullscreen(surface,
							surface->implementation_data,
							fullscreen);
}

WL_EXPORT void
weston_desktop_surface_set_maximized(struct weston_desktop_surface *surface, bool maximized)
{
	if (surface->implementation->set_maximized != NULL)
		surface->implementation->set_maximized(surface,
						       surface->implementation_data,
						       maximized);
}

WL_EXPORT void
weston_desktop_surface_set_resizing(struct weston_desktop_surface *surface, bool resizing)
{
	if (surface->implementation->set_resizing != NULL)
		surface->implementation->set_resizing(surface,
						      surface->implementation_data,
						      resizing);
}

WL_EXPORT void
weston_desktop_surface_set_size(struct weston_desktop_surface *surface, int32_t width, int32_t height)
{
	if (surface->implementation->set_size != NULL)
		surface->implementation->set_size(surface,
						  surface->implementation_data,
						  width, height);
}

WL_EXPORT void
weston_desktop_surface_close(struct weston_desktop_surface *surface)
{
	if (surface->implementation->close != NULL)
		surface->implementation->close(surface,
					       surface->implementation_data);
}

WL_EXPORT void
weston_desktop_surface_add_metadata_listener(struct weston_desktop_surface *surface,
					     struct wl_listener *listener)
{
	wl_signal_add(&surface->metadata_signal, listener);
}

struct weston_desktop_surface *
weston_desktop_surface_from_client_link(struct wl_list *link)
{
	struct weston_desktop_surface *surface;

	surface = wl_container_of(link, surface, client_link);
	return surface;
}

struct wl_list *
weston_desktop_surface_get_client_link(struct weston_desktop_surface *surface)
{
	return &surface->client_link;
}

bool
weston_desktop_surface_has_implementation(struct weston_desktop_surface *surface,
					  const struct weston_desktop_surface_implementation *implementation)
{
	return surface->implementation == implementation;
}

const struct weston_desktop_surface_implementation *
weston_desktop_surface_get_implementation(struct weston_desktop_surface *surface)
{
	return surface->implementation;
}

void *
weston_desktop_surface_get_implementation_data(struct weston_desktop_surface *surface)
{
	return surface->implementation_data;
}

struct weston_desktop_surface *
weston_desktop_surface_get_parent(struct weston_desktop_surface *surface)
{
	return surface->parent;
}

bool
weston_desktop_surface_get_grab(struct weston_desktop_surface *surface)
{
	return !wl_list_empty(&surface->grab_link);
}

WL_EXPORT struct weston_desktop_client *
weston_desktop_surface_get_client(struct weston_desktop_surface *surface)
{
	return surface->client;
}

WL_EXPORT void *
weston_desktop_surface_get_user_data(struct weston_desktop_surface *surface)
{
	return surface->user_data;
}

WL_EXPORT struct weston_surface *
weston_desktop_surface_get_surface(struct weston_desktop_surface *surface)
{
	return surface->surface;
}

WL_EXPORT const char *
weston_desktop_surface_get_title(struct weston_desktop_surface *surface)
{
	return surface->title;
}

WL_EXPORT const char *
weston_desktop_surface_get_app_id(struct weston_desktop_surface *surface)
{
	return surface->app_id;
}

WL_EXPORT pid_t
weston_desktop_surface_get_pid(struct weston_desktop_surface *surface)
{
	pid_t pid;

	if (surface->pid != -1) {
		pid = surface->pid;
	} else {
		struct weston_desktop_client *client =
			weston_desktop_surface_get_client(surface);
		struct wl_client *wl_client =
			weston_desktop_client_get_client(client);

		/* wl_client should always be valid, because only in the
		 * xwayland case it wouldn't be, but in that case we won't
		 * reach here, as the pid is initialized to 0. */
		assert(wl_client);
		wl_client_get_credentials(wl_client, &pid, NULL, NULL);
	}
	return pid;
}

WL_EXPORT bool
weston_desktop_surface_get_activated(struct weston_desktop_surface *surface)
{
	if (surface->implementation->get_activated == NULL)
		return false;
	return surface->implementation->get_activated(surface,
						      surface->implementation_data);
}

WL_EXPORT bool
weston_desktop_surface_get_resizing(struct weston_desktop_surface *surface)
{
	if (surface->implementation->get_resizing == NULL)
		return false;
	return surface->implementation->get_resizing(surface,
						     surface->implementation_data);
}

WL_EXPORT bool
weston_desktop_surface_get_maximized(struct weston_desktop_surface *surface)
{
	if (surface->implementation->get_maximized == NULL)
		return false;
	return surface->implementation->get_maximized(surface,
						      surface->implementation_data);
}

WL_EXPORT bool
weston_desktop_surface_get_fullscreen(struct weston_desktop_surface *surface)
{
	if (surface->implementation->get_fullscreen == NULL)
		return false;
	return surface->implementation->get_fullscreen(surface,
						       surface->implementation_data);
}

WL_EXPORT struct weston_geometry
weston_desktop_surface_get_geometry(struct weston_desktop_surface *surface)
{
	if (surface->has_geometry)
		return surface->geometry;
	return weston_surface_get_bounding_box(surface->surface);
}

WL_EXPORT struct weston_size
weston_desktop_surface_get_max_size(struct weston_desktop_surface *surface)
{
	struct weston_size size = { 0, 0 };

	if (surface->implementation->get_max_size == NULL)
		return size;
	return surface->implementation->get_max_size(surface,
						     surface->implementation_data);
}

WL_EXPORT struct weston_size
weston_desktop_surface_get_min_size(struct weston_desktop_surface *surface)
{
	struct weston_size size = { 0, 0 };

	if (surface->implementation->get_min_size == NULL)
		return size;
	return surface->implementation->get_min_size(surface,
						     surface->implementation_data);
}

void
weston_desktop_surface_set_title(struct weston_desktop_surface *surface,
				 const char *title)
{
	char *tmp, *old;

	tmp = strdup(title);
	if (tmp == NULL)
		return;

	old = surface->title;
	surface->title = tmp;
	wl_signal_emit(&surface->metadata_signal, surface);
	free(old);
}

void
weston_desktop_surface_set_app_id(struct weston_desktop_surface *surface,
				  const char *app_id)
{
	char *tmp, *old;

	tmp = strdup(app_id);
	if (tmp == NULL)
		return;

	old = surface->app_id;
	surface->app_id = tmp;
	wl_signal_emit(&surface->metadata_signal, surface);
	free(old);
}

void
weston_desktop_surface_set_pid(struct weston_desktop_surface *surface,
			       pid_t pid)
{
	surface->pid = pid;
}

void
weston_desktop_surface_set_geometry(struct weston_desktop_surface *surface,
				    struct weston_geometry geometry)
{
	surface->has_geometry = true;
	surface->geometry = geometry;
}

void
weston_desktop_surface_set_relative_to(struct weston_desktop_surface *surface,
				       struct weston_desktop_surface *parent,
				       int32_t x, int32_t y, bool use_geometry)
{
	struct weston_desktop_view *view, *parent_view;
	struct wl_list *link, *tmp;

	assert(parent);

	surface->position.x = x;
	surface->position.y = y;
	surface->use_geometry = use_geometry;

	if (surface->parent == parent) {
		weston_desktop_surface_update_view_position(surface);
		return;
	}

	surface->parent = parent;
	wl_list_remove(&surface->children_link);
	wl_list_insert(surface->parent->children_list.prev,
		       &surface->children_link);

	link = surface->view_list.next;
	tmp = link->next;
	wl_list_for_each(parent_view, &parent->view_list, link) {
		if (link == &surface->view_list) {
			view = weston_desktop_surface_create_desktop_view(surface);
			if (view == NULL)
				return;
			tmp = &surface->view_list;
		} else {
			view = wl_container_of(link, view, link);
			wl_list_remove(&view->children_link);
		}

		view->parent = parent_view;
		weston_view_set_transform_parent(view->view, parent_view->view);
		wl_list_insert(parent_view->children_list.prev,
			       &view->children_link);
		weston_desktop_view_propagate_layer(view);

		link = tmp;
		tmp = link->next;
	}
	for (; link != &surface->view_list; link = tmp, tmp = link->next) {
		view = wl_container_of(link, view, link);
		weston_desktop_view_destroy(view);
	}
	weston_desktop_surface_update_view_position(surface);
}

void
weston_desktop_surface_unset_relative_to(struct weston_desktop_surface *surface)
{
	struct weston_desktop_view *view, *tmp;

	if (surface->parent == NULL)
		return;

	surface->parent = NULL;
	wl_list_remove(&surface->children_link);
	wl_list_init(&surface->children_link);

	wl_list_for_each_safe(view, tmp, &surface->view_list, link)
		weston_desktop_view_destroy(view);
}

void
weston_desktop_surface_popup_grab(struct weston_desktop_surface *surface,
				  struct weston_desktop_seat *seat,
				  uint32_t serial)
{
	struct wl_client *wl_client =
		weston_desktop_client_get_client(surface->client);

	/* wl_shell can reuse a shell surface for another popup. */
	if (!surface->popup) {
		surface->popup_dismissed = false;
		surface->popup_unmapped = false;
	}
	if (weston_desktop_seat_popup_grab_start(seat, wl_client, serial))
		weston_desktop_seat_popup_grab_add_surface(seat, &surface->grab_link);
	else
		weston_desktop_surface_popup_dismiss(surface);
}

void
weston_desktop_surface_popup_ungrab(struct weston_desktop_surface *surface,
				   struct weston_desktop_seat *seat)
{
	weston_desktop_seat_popup_grab_remove_surface(seat, &surface->grab_link);
}

void
weston_desktop_surface_popup_dismiss(struct weston_desktop_surface *surface)
{
	if (surface->popup_dismissed)
		return;
	surface->popup_dismissed = true;
	weston_desktop_surface_unmap_popup(surface);
	wl_list_remove(&surface->grab_link);
	wl_list_init(&surface->grab_link);
	weston_desktop_surface_close(surface);
}

void
weston_desktop_surface_unmap_popup(struct weston_desktop_surface *surface)
{
	struct weston_desktop_view *view, *tmp;
	struct weston_desktop_surface *child;

	if ((!surface->popup && !surface->popup_dismissed) ||
	    surface->popup_unmapped)
		return;
	surface->popup_unmapped = true;
	surface->popup_mapped = false;
	wl_list_for_each_reverse(child, &surface->children_list, children_link)
		weston_desktop_surface_unmap_popup(child);

	wl_list_for_each_safe(view, tmp, &surface->view_list, link)
		weston_desktop_view_destroy(view);
	weston_desktop_api_popup_state_changed(surface->desktop, surface, false);
}
