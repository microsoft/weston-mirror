/* SPDX-License-Identifier: MIT */
#include "config.h"

#include "zunitc/zunitc.h"
#include "../libweston/backend-rdp/rdprail.c"

static void
acknowledge(freerdp_peer *peer, uint32_t frame, uint32_t queue_depth,
	    bool free_only)
{
	struct rdp_rail_dispatch_data *data = calloc(1, sizeof(*data));

	ZUC_ASSERT_TRUE(data != NULL);
	data->client = peer;
	data->frame_ack.frameId = frame;
	data->frame_ack.queueDepth = queue_depth;
	rail_grfx_frame_acknowledge_callback(free_only, &data->task_base);
}

ZUC_TEST(rdp_repaint, acknowledgement_releases_deferred_repaint)
{
	struct weston_compositor compositor = { 0 };
	struct rdp_backend backend = { 0 };
	struct weston_output output = { 0 };
	RdpPeerContext context = { 0 };
	freerdp_peer peer = { 0 };

	wl_list_init(&compositor.output_list);
	compositor.wl_display = wl_display_create();
	ZUC_ASSERT_TRUE(compositor.wl_display != NULL);
	pixman_region32_init(&compositor.primary_plane.damage);
	pixman_region32_init_rect(&output.region, 0, 0, 640, 480);
	output.compositor = &compositor;
	output.repaint_status = REPAINT_AWAITING_COMPLETION;
	wl_list_insert(&compositor.output_list, &output.link);
	backend.compositor = &compositor;
	backend.compositor_tid = rdp_get_tid();
	context.rdpBackend = &backend;
	peer.context = (rdpContext *)&context;
	context.currentFrameId = 3;
	context.acknowledgedFrameId = 1;
	context.repaint_deferred = true;
	acknowledge(&peer, 1, 0, false);
	ZUC_ASSERT_TRUE(context.repaint_deferred);
	ZUC_ASSERT_FALSE(output.repaint_needed);
	ZUC_ASSERT_FALSE(pixman_region32_not_empty(&compositor.primary_plane.damage));
	acknowledge(&peer, 2, 0, false);
	ZUC_ASSERT_FALSE(context.repaint_deferred);
	ZUC_ASSERT_EQ(2, context.acknowledgedFrameId);
	ZUC_ASSERT_TRUE(output.repaint_needed);
	ZUC_ASSERT_TRUE(pixman_region32_not_empty(&compositor.primary_plane.damage));

	/* An acknowledgement without deferred work does not start a redraw. */
	output.repaint_needed = false;
	pixman_region32_clear(&compositor.primary_plane.damage);
	acknowledge(&peer, 3, 0, false);
	ZUC_ASSERT_FALSE(output.repaint_needed);

	/* Teardown-only dispatch must not touch counters or schedule repaint. */
	context.repaint_deferred = true;
	acknowledge(&peer, 4, 0, true);
	ZUC_ASSERT_TRUE(context.repaint_deferred);
	ZUC_ASSERT_EQ(3, context.acknowledgedFrameId);
	ZUC_ASSERT_FALSE(output.repaint_needed);

	/* Suspended acknowledgements lift the frame limit immediately. */
	context.currentFrameId = 10;
	acknowledge(&peer, 3, UINT32_MAX, false);
	ZUC_ASSERT_FALSE(context.repaint_deferred);
	ZUC_ASSERT_TRUE(output.repaint_needed);
	ZUC_ASSERT_TRUE(context.isAcknowledgedSuspended);
	pixman_region32_fini(&output.region);
	pixman_region32_fini(&compositor.primary_plane.damage);
	wl_display_destroy(compositor.wl_display);
}
