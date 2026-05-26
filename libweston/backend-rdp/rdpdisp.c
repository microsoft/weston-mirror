/*
 * Copyright © 2020 Microsoft
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

#include <stdio.h>
#include <wchar.h>
#include <strings.h>

#include "rdp.h"

#include "shared/xalloc.h"

float
disp_get_client_scale_from_monitor(struct rdp_backend *b, const rdpMonitor *config)
{
	if (config->attributes.desktopScaleFactor == 0.0)
		return 1.0f;

	if (b->enable_hi_dpi_support) {
		if (b->debug_desktop_scaling_factor)
			return (float)b->debug_desktop_scaling_factor / 100.f;
		else if (b->enable_fractional_hi_dpi_support)
			return (float)config->attributes.desktopScaleFactor / 100.0f;
		else if (b->enable_fractional_hi_dpi_roundup)
			return (float)(int)((config->attributes.desktopScaleFactor + 50) / 100);
		else
			return (float)(int)(config->attributes.desktopScaleFactor / 100);
	} else {
		return 1.0f;
	}
}

int
disp_get_output_scale_from_monitor(struct rdp_backend *b, const rdpMonitor *config)
{
	return (int) disp_get_client_scale_from_monitor(b, config);
}

static bool
match_primary(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b)
{
	if (a->is_primary && b->is_primary)
		return true;

	return false;
}

static bool
match_dimensions(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b)
{
	int scale_a = disp_get_output_scale_from_monitor(rdp, a);
	int scale_b = disp_get_output_scale_from_monitor(rdp, b);

	if (a->width != b->width ||
	    a->height != b->height ||
	    scale_a != scale_b)
		return false;

	return true;
}

static bool
match_position(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b)
{
	if (a->x != b->x ||
	    a->y != b->y)
		return false;

	return true;
}

static bool
match_exact(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b)
{
	if (match_dimensions(rdp, a, b) &&
	    match_position(rdp, a, b))
		return true;

	return false;
}

static bool
match_any(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b)
{
	return true;
}

static void
update_head(struct rdp_backend *rdp, struct rdp_head *head, rdpMonitor *config)
{
	struct weston_mode mode = {};
	int scale;
	bool changed = false;

	head->matched = true;
	scale = disp_get_output_scale_from_monitor(rdp, config);

	if (!match_position(rdp, &head->config, config))
		changed = true;

	if (!match_dimensions(rdp, &head->config, config)) {
		mode.flags = WL_OUTPUT_MODE_PREFERRED;
		mode.width = config->width;
		mode.height = config->height;
		mode.refresh = rdp->rdp_monitor_refresh_rate;
		weston_output_mode_set_native(head->base.output,
					      &mode, scale);
		changed = true;
	}

	if (changed) {
		weston_head_set_device_changed(&head->base);
	}
	head->config = *config;
}

static void
match_heads(struct rdp_backend *rdp, rdpMonitor *config, uint32_t count,
	    int *done,
	    bool (*cmp)(struct rdp_backend *rdp, rdpMonitor *a, rdpMonitor *b))
{
	struct weston_head *iter;
	struct rdp_head *current;
	uint32_t i;

	wl_list_for_each(iter, &rdp->compositor->head_list, compositor_link) {
		current = to_rdp_head(iter);
		if (current->matched)
			continue;

		for (i = 0; i < count; i++) {
			if (*done & (1 << i))
				continue;

			if (cmp(rdp, &current->config, &config[i])) {
				*done |= 1 << i;
				update_head(rdp, current, &config[i]);
				break;
			}
		}
	}
}

static void
disp_start_monitor_layout_change(freerdp_peer *client, rdpMonitor *config, UINT32 monitorCount)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;
	rdpSettings *settings = client->context->settings;
	struct rdp_backend *b = peerCtx->rdpBackend;
	struct rdp_head *current;
	struct weston_head *iter, *tmp;
	pixman_region32_t desktop;
	int done = 0;

	assert_compositor_thread(b);

	pixman_region32_init(&desktop);

	/* Prune heads that were never enabled, and flag heads as unmatched  */
	wl_list_for_each_safe(iter, tmp, &b->compositor->head_list, compositor_link) {
		current = to_rdp_head(iter);
		if (!iter->output) {
			rdp_head_destroy(b->compositor, to_rdp_head(iter));
			continue;
		}
		current->matched = false;
	}

	/* We want the primary head to remain primary - it
	 * should always be rdp-0.
	  */
	match_heads(b, config, monitorCount, &done, match_primary);

	/* Look for any exact match */
	match_heads(b, config, monitorCount, &done, match_exact);

	/* Match first head with the same dimensions */
	match_heads(b, config, monitorCount, &done, match_dimensions);

	/* Match head with the same position */
	match_heads(b, config, monitorCount, &done, match_position);

	/* Pick any available head */
	match_heads(b, config, monitorCount, &done, match_any);

	/* Destroy any heads we won't be using */
	wl_list_for_each_safe(iter, tmp, &b->compositor->head_list, compositor_link) {
		current = to_rdp_head(iter);
		if (!current->matched)
			rdp_head_destroy(b->compositor, to_rdp_head(iter));
	}


	for (uint32_t i = 0; i < monitorCount; i++) {
		/* accumulate monitor layout */
		if (config[i].is_primary) {
			/* it looks settings's desktopWidth/Height only represents primary */
			(void)freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, config[i].width);
			(void)freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, config[i].height);
		}
		pixman_region32_union_rect(&desktop, &desktop,
					   config[i].x,
					   config[i].y,
					   config[i].width,
					   config[i].height);

		/* Create new heads for any without matches */
		if (!(done & (1 << i)))
			rdp_head_create(b->compositor, config[i].is_primary, &config[i]);
	}
	peerCtx->desktop_left = desktop.extents.x1;
	peerCtx->desktop_top = desktop.extents.y1;
	peerCtx->desktop_width = desktop.extents.x2 - desktop.extents.x1;
	peerCtx->desktop_height = desktop.extents.y2 - desktop.extents.y1;
	pixman_region32_fini(&desktop);
}

static bool
disp_monitor_sanity_check_layout(RdpPeerContext *peerCtx, rdpMonitor *config, uint32_t count)
{
	struct rdp_backend *b = peerCtx->rdpBackend;
	uint32_t primaryCount = 0;
	uint32_t i;

	/* dump client monitor topology */
	rdp_debug(b, "%s:---INPUT---\n", __func__);
	for (i = 0; i < count; i++) {
		float client_scale = disp_get_client_scale_from_monitor(b, &config[i]);
		int scale = disp_get_output_scale_from_monitor(b, &config[i]);

		rdp_debug(b, "	rdpMonitor[%d]: x:%d, y:%d, width:%d, height:%d, is_primary:%d\n",
			i, config[i].x, config[i].y,
			   config[i].width, config[i].height,
			   config[i].is_primary);
		rdp_debug(b, "	rdpMonitor[%d]: physicalWidth:%d, physicalHeight:%d, orientation:%d\n",
			i, config[i].attributes.physicalWidth,
			   config[i].attributes.physicalHeight,
			   config[i].attributes.orientation);
		rdp_debug(b, "	rdpMonitor[%d]: desktopScaleFactor:%d, deviceScaleFactor:%d\n",
			i, config[i].attributes.desktopScaleFactor,
			   config[i].attributes.deviceScaleFactor);
		rdp_debug(b, "	rdpMonitor[%d]: scale:%d, client scale :%3.2f\n",
			i, scale, client_scale);
	}

	for (i = 0; i < count; i++) {
		/* make sure there is only one primary and its position at client */
		if (config[i].is_primary) {
			/* count number of primary */
			if (++primaryCount > 1) {
				rdp_debug_error(b, "%s: RDP client reported unexpected primary count (%d)\n",__func__, primaryCount);
				return false;
			}
			/* primary must be at (0,0) in client space */
			if (config[i].x != 0 || config[i].y != 0) {
				rdp_debug_error(b, "%s: RDP client reported primary is not at (0,0) but (%d,%d).\n",
					__func__, config[i].x, config[i].y);
				return false;
			}
		}
	}
	return true;
}

bool
handle_adjust_monitor_layout(freerdp_peer *client, int monitor_count, rdpMonitor *monitors)
{
	RdpPeerContext *peerCtx = (RdpPeerContext *)client->context;

	if (!disp_monitor_sanity_check_layout(peerCtx, monitors, monitor_count))
		return true;

	disp_start_monitor_layout_change(client, monitors, monitor_count);

	return true;
}

static bool
rdp_monitor_contains(rdpMonitor *monitor, int32_t x, int32_t y)
{
	if (x < monitor->x)
		return FALSE;
	if (y < monitor->y)
		return FALSE;
	if (x >= monitor->x + monitor->width)
		return FALSE;
	if (y >= monitor->y + monitor->height)
		return FALSE;

	return TRUE;
}

/* Input x/y in client space, output x/y in weston space */
struct weston_output *
to_weston_coordinate(RdpPeerContext *peerContext, int32_t *x, int32_t *y, uint32_t *width, uint32_t *height)
{
	struct rdp_backend *b = peerContext->rdpBackend;
	int sx = *x, sy = *y;
	struct weston_head *head_iter;

	/* First, find which monitor contains this x/y. */
	wl_list_for_each(head_iter, &b->compositor->head_list, compositor_link) {
		struct rdp_head *head = to_rdp_head(head_iter);

		if (rdp_monitor_contains(&head->config, sx, sy)) {
			struct weston_output *output = head->base.output;
			float client_scale = disp_get_client_scale_from_monitor(b, &head->config);
			float scale = 1.0f / client_scale;

			/* translate x/y to offset from this output on client space. */
			sx -= head->config.x;
			sy -= head->config.y;
			/* scale x/y to client output space. */
			sx *= scale;
			sy *= scale;
			if (width && height) {
				*width *= scale;
				*height *= scale;
			}
			/* translate x/y to offset from this output on weston space. */
			sx += output->x;
			sy += output->y;
			rdp_debug_verbose(b, "%s: (x:%d, y:%d) -> (sx:%d, sy:%d) at head:%s\n",
					  __func__, *x, *y, sx, sy, head->base.name);
			*x = sx;
			*y = sy;
			return output; // must be only 1 head per output.
		}
	}
	/* x/y is outside of any monitors. */
	return NULL;
}

/* Input x/y in weston space, output x/y in client space */
void
to_client_coordinate(RdpPeerContext *peerContext, struct weston_output *output, int32_t *x, int32_t *y, uint32_t *width, uint32_t *height)
{
	struct rdp_backend *b = peerContext->rdpBackend;
	int sx = *x, sy = *y;
	struct weston_head *head_iter;

	/* Pick first head from output. */
	wl_list_for_each(head_iter, &output->head_list, output_link) {
		struct rdp_head *head = to_rdp_head(head_iter);
		float scale = disp_get_client_scale_from_monitor(b, &head->config);

		/* translate x/y to offset from this output on weston space. */
		sx -= output->x;
		sy -= output->y;
		/* scale x/y to client output space. */
		sx *= scale;
		sy *= scale;

		if (width && height) {
			*width *= scale;
			*height *= scale;
		}
		/* translate x/y to offset from this output on client space. */
		sx += head->config.x;
		sy += head->config.y;
		rdp_debug_verbose(b, "%s: (x:%d, y:%d) -> (sx:%d, sy:%d) at head:%s\n",
				  __func__, *x, *y, sx, sy, head_iter->name);
		*x = sx;
		*y = sy;
		return; // must be only 1 head per output.
	}
}
