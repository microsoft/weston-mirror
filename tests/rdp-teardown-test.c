#include "config.h"

#include "zunitc/zunitc.h"

#include "../libweston/backend-rdp/rdp.c"

ZUC_TEST(rdp_teardown, activation_error_cleanup)
{
	struct weston_compositor compositor = { 0 };
	struct weston_rdprail_shell_api shell_api = { 0 };
	struct rdp_backend backend = { 0 };
	RdpPeerContext context = { 0 };
	rdpSettings settings = { 0 };
	freerdp_peer peer = { 0 };

	compositor.state = WESTON_COMPOSITOR_SLEEPING;
	backend.compositor = &compositor;
	backend.compositor_tid = rdp_get_tid();
	backend.rdprail_shell_api = &shell_api;
	backend.rdp_peer = &peer;
	context.rdpBackend = &backend;
	context._p.settings = &settings;
	peer.context = (rdpContext *)&context;
	settings.SurfaceCommandsEnabled = TRUE;
	settings.RemoteApplicationMode = TRUE;
	settings.HiDefRemoteApp = TRUE;
	settings.DesktopWidth = 800;
	settings.DesktopHeight = 600;
	ZUC_ASSERT_TRUE(rdp_peer_context_new(&peer, &context));
	wl_list_init(&context.item.link);
	ZUC_ASSERT_EQ(0, pthread_mutex_init(&context.loop_task_list_mutex, NULL));
	ZUC_ASSERT_TRUE(rdp_rail_peer_init(&peer, &context));

	ZUC_ASSERT_FALSE(xf_peer_activate(&peer));
	rdp_peer_context_free(&peer, &context);
	ZUC_ASSERT_FALSE(backend.rdp_peer);
}
