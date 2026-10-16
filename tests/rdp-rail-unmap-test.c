#include "config.h"

#include "zunitc/zunitc.h"

#include "../libweston/backend-rdp/rdprail.c"

#define WINDOW_HIDE_FIELDS (WINDOW_ORDER_TYPE_WINDOW | \
			    WINDOW_ORDER_FIELD_SHOW | \
			    WINDOW_ORDER_FIELD_TASKBAR_BUTTON)

struct window_update_record {
	int count;
	int order;
	UINT32 window_id;
	UINT32 field_flags;
	UINT32 show_state;
	BYTE taskbar_button;
};

static struct window_update_record window_updates;

struct zorder_record {
	int count;
	int order;
	UINT32 active_window_id;
	UINT32 num_window_ids;
};

static struct zorder_record zorder_updates;
static int order_counter;

static BOOL
stub_paint(rdpContext *context)
{
	return TRUE;
}

static BOOL
stub_window_update(rdpContext *context, const WINDOW_ORDER_INFO *order_info,
		   const WINDOW_STATE_ORDER *window_state)
{
	window_updates.count++;
	window_updates.order = ++order_counter;
	window_updates.window_id = order_info->windowId;
	window_updates.field_flags = order_info->fieldFlags;
	window_updates.show_state = window_state->showState;
	window_updates.taskbar_button = window_state->TaskbarButton;
	return TRUE;
}

static BOOL
stub_monitored_desktop(rdpContext *context, const WINDOW_ORDER_INFO *order_info,
		       const MONITORED_DESKTOP_ORDER *monitored_desktop)
{
	zorder_updates.count++;
	zorder_updates.order = ++order_counter;
	zorder_updates.active_window_id = monitored_desktop->activeWindowId;
	zorder_updates.num_window_ids = monitored_desktop->numWindowIds;
	return TRUE;
}

static int
stub_drain_output_buffer(freerdp_peer *peer)
{
	return 0;
}

struct fixture {
	struct weston_compositor compositor;
	struct rdp_backend backend;
	struct weston_output output;
	freerdp_peer peer;
	RdpPeerContext context;
	rdpUpdate update;
	rdpWindowUpdate window_update;
};

static void
fixture_init(struct fixture *f)
{
	memset(f, 0, sizeof(*f));
	memset(&window_updates, 0, sizeof(window_updates));
	memset(&zorder_updates, 0, sizeof(zorder_updates));
	order_counter = 0;

	f->backend.compositor = &f->compositor;
	f->backend.compositor_tid = rdp_get_tid();
	f->backend.rdp_peer = &f->peer;
	f->compositor.backend = &f->backend.base;
	f->output.compositor = &f->compositor;

	f->peer.context = (rdpContext *)&f->context;
	f->context.rdpBackend = &f->backend;
	f->context._p.update = &f->update;
	f->update.context = (rdpContext *)&f->context;
	f->update.BeginPaint = stub_paint;
	f->update.EndPaint = stub_paint;
	f->update.window = &f->window_update;
	f->window_update.WindowUpdate = stub_window_update;
	f->window_update.MonitoredDesktop = stub_monitored_desktop;
	f->peer.DrainOutputBuffer = stub_drain_output_buffer;
	wl_list_init(&f->compositor.layer_list);

	/* Leave the client more than one frame behind, so the regular
	 * window update pass is throttled and only the hide pass runs. */
	f->context.currentFrameId = 5;
	f->context.acknowledgedFrameId = 0;
	f->context.isAcknowledgedSuspended = false;

	ZUC_ASSERT_TRUE(rdp_id_manager_init(&f->backend, &f->context.windowId,
					    0x1, 0x7FFFFFFF));
}

static void
fixture_fini(struct fixture *f)
{
	rdp_id_manager_free(&f->context.windowId);
}

/* Set up a surface whose RAIL window has been created and shown. */
static void
shown_window_init(struct fixture *f, struct weston_surface *surface,
		  struct weston_surface_rail_state *rail_state)
{
	memset(surface, 0, sizeof(*surface));
	memset(rail_state, 0, sizeof(*rail_state));

	surface->compositor = &f->compositor;
	surface->backend_state = rail_state;
	surface->is_mapped = true;
	wl_list_init(&surface->views);
	wl_list_init(&surface->subsurface_list);

	ZUC_ASSERT_TRUE(rdp_id_manager_allocate_id(&f->context.windowId,
						   surface,
						   &rail_state->window_id));
	rail_state->isWindowCreated = true;
	rail_state->output = &f->output;
	rail_state->showState = RDP_WINDOW_SHOW_MAXIMIZED;
}

ZUC_TEST(rdp_rail_unmap, unmapped_window_hidden_once_while_throttled)
{
	struct fixture f;
	struct weston_surface surface;
	struct weston_surface_rail_state rail_state;

	fixture_init(&f);
	shown_window_init(&f, &surface, &rail_state);

	/* Xwayland unmapped the window but has not destroyed its surface. */
	surface.is_mapped = false;
	rdp_rail_output_repaint(&f.output, NULL);

	ZUC_ASSERT_EQ(1, window_updates.count);
	ZUC_ASSERT_EQ(rail_state.window_id, window_updates.window_id);
	ZUC_ASSERT_EQ(WINDOW_HIDE_FIELDS, window_updates.field_flags);
	ZUC_ASSERT_EQ(WINDOW_HIDE, window_updates.show_state);
	ZUC_ASSERT_EQ(1, window_updates.taskbar_button);

	/* A remap must go through rdp_rail_update_window() again. */
	ZUC_ASSERT_TRUE(rail_state.output == NULL);
	ZUC_ASSERT_TRUE(rail_state.forceUpdateWindowState);
	ZUC_ASSERT_EQ(RDP_WINDOW_HIDE, rail_state.showState);
	ZUC_ASSERT_EQ(1, rail_state.taskbarButton);

	/* The hide is sent only once. */
	rdp_rail_output_repaint(&f.output, NULL);
	ZUC_ASSERT_EQ(1, window_updates.count);

	rdp_id_manager_free_id(&f.context.windowId, rail_state.window_id);
	fixture_fini(&f);
}

ZUC_TEST(rdp_rail_unmap, mapped_and_ineligible_windows_left_alone)
{
	struct fixture f;
	struct weston_surface mapped, cursor, not_created, never_shown, failed;
	struct weston_surface_rail_state mapped_rs, cursor_rs, not_created_rs;
	struct weston_surface_rail_state never_shown_rs, failed_rs;

	fixture_init(&f);

	/* Minimized windows stay mapped. */
	shown_window_init(&f, &mapped, &mapped_rs);
	mapped_rs.showState = RDP_WINDOW_SHOW_MINIMIZED;

	shown_window_init(&f, &cursor, &cursor_rs);
	cursor.is_mapped = false;
	cursor_rs.isCursor = true;

	shown_window_init(&f, &not_created, &not_created_rs);
	not_created.is_mapped = false;
	not_created_rs.isWindowCreated = false;

	shown_window_init(&f, &never_shown, &never_shown_rs);
	never_shown.is_mapped = false;
	never_shown_rs.output = NULL;

	shown_window_init(&f, &failed, &failed_rs);
	failed.is_mapped = false;
	failed_rs.error = true;

	rdp_rail_output_repaint(&f.output, NULL);

	ZUC_ASSERT_EQ(0, window_updates.count);
	ZUC_ASSERT_TRUE(mapped_rs.output == &f.output);
	ZUC_ASSERT_EQ(RDP_WINDOW_SHOW_MINIMIZED, mapped_rs.showState);

	rdp_id_manager_free_id(&f.context.windowId, mapped_rs.window_id);
	rdp_id_manager_free_id(&f.context.windowId, cursor_rs.window_id);
	rdp_id_manager_free_id(&f.context.windowId, not_created_rs.window_id);
	rdp_id_manager_free_id(&f.context.windowId, never_shown_rs.window_id);
	rdp_id_manager_free_id(&f.context.windowId, failed_rs.window_id);
	fixture_fini(&f);
}

ZUC_TEST(rdp_rail_unmap, transient_child_with_mapped_view_left_alone)
{
	struct fixture f;
	struct weston_surface surface;
	struct weston_surface_rail_state rail_state;
	struct weston_view view = { 0 };

	fixture_init(&f);
	shown_window_init(&f, &surface, &rail_state);

	/* libweston-desktop maps the view of a transient child, such as an
	 * X11 tooltip, without setting is_mapped on its surface. */
	surface.is_mapped = false;
	view.surface = &surface;
	view.is_mapped = true;
	wl_list_insert(&surface.views, &view.surface_link);

	rdp_rail_output_repaint(&f.output, NULL);

	ZUC_ASSERT_EQ(0, window_updates.count);
	ZUC_ASSERT_TRUE(rail_state.output == &f.output);

	/* Once its view is unmapped as well, the window is hidden. */
	view.is_mapped = false;
	rdp_rail_output_repaint(&f.output, NULL);

	ZUC_ASSERT_EQ(1, window_updates.count);
	ZUC_ASSERT_EQ(WINDOW_HIDE, window_updates.show_state);

	wl_list_remove(&view.surface_link);
	rdp_id_manager_free_id(&f.context.windowId, rail_state.window_id);
	fixture_fini(&f);
}

ZUC_TEST(rdp_rail_unmap, zorder_sent_before_hide)
{
	struct fixture f;
	struct weston_surface main_surface, popup;
	struct weston_surface_rail_state main_rs, popup_rs;
	struct weston_layer layer = { 0 };
	struct weston_view main_view = { 0 };

	fixture_init(&f);
	f.backend.enable_window_zorder_sync = true;

	shown_window_init(&f, &main_surface, &main_rs);
	main_rs.showState = RDP_WINDOW_SHOW;
	shown_window_init(&f, &popup, &popup_rs);

	/* Only the main window is still in the scene graph: the popup was
	 * unmapped, which removed its view from the layer. */
	wl_list_init(&layer.view_list.link);
	wl_list_insert(&f.compositor.layer_list, &layer.link);
	main_view.surface = &main_surface;
	main_view.is_mapped = true;
	wl_list_insert(&layer.view_list.link, &main_view.layer_link.link);
	wl_list_insert(&main_surface.views, &main_view.surface_link);

	popup.is_mapped = false;
	rdp_rail_output_repaint(&f.output, NULL);

	/* The client learns that the main window is active before the popup,
	 * which may be its active window, is hidden. */
	ZUC_ASSERT_EQ(1, zorder_updates.count);
	ZUC_ASSERT_EQ(main_rs.window_id, zorder_updates.active_window_id);
	ZUC_ASSERT_EQ(1, zorder_updates.num_window_ids);
	ZUC_ASSERT_EQ(1, window_updates.count);
	ZUC_ASSERT_EQ(popup_rs.window_id, window_updates.window_id);
	ZUC_ASSERT_TRUE(zorder_updates.order < window_updates.order);
	ZUC_ASSERT_FALSE(f.context.is_window_zorder_dirty);

	wl_list_remove(&main_view.surface_link);
	rdp_id_manager_free_id(&f.context.windowId, main_rs.window_id);
	rdp_id_manager_free_id(&f.context.windowId, popup_rs.window_id);
	fixture_fini(&f);
}
