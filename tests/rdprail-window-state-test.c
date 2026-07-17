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

#include <libweston/backend-rdp.h>

#include "libweston/backend-rdp/rdprail-window-state.h"
#include "shared/helpers.h"
#include "zunitc/zunitc.h"

struct dispatch_state {
	unsigned int count;
	unsigned int calls[3];
	WINDOW_ORDER_INFO order_info;
	WINDOW_STATE_ORDER window_state;
};

static BOOL
record_begin_paint(rdpContext *context)
{
	struct dispatch_state *state = (struct dispatch_state *)context;

	state->calls[state->count++] = 1;
	return TRUE;
}

static BOOL
record_window_update(rdpContext *context,
		     const WINDOW_ORDER_INFO *order_info,
		     const WINDOW_STATE_ORDER *window_state)
{
	struct dispatch_state *state = (struct dispatch_state *)context;

	state->calls[state->count++] = 2;
	state->order_info = *order_info;
	state->window_state = *window_state;
	return TRUE;
}

static BOOL
record_end_paint(rdpContext *context)
{
	struct dispatch_state *state = (struct dispatch_state *)context;

	state->calls[state->count++] = 3;
	return TRUE;
}

ZUC_TEST(rdprail_window_state_test, leaves_unchanged_state_alone)
{
	WINDOW_ORDER_INFO order_info = {
		.fieldFlags = WINDOW_ORDER_TYPE_WINDOW,
	};
	WINDOW_STATE_ORDER window_state = {};
	uint32_t fields;

	fields = rdp_rail_prepare_window_state_order(
		10, 10, RDP_WINDOW_SHOW, RDP_WINDOW_SHOW,
		&order_info, &window_state);

	ZUC_ASSERT_EQ(0, fields);
	ZUC_ASSERT_EQ(WINDOW_ORDER_TYPE_WINDOW, order_info.fieldFlags);
}

ZUC_TEST(rdprail_window_state_test, writes_owner_and_show_fields)
{
	WINDOW_ORDER_INFO order_info = {
		.fieldFlags = WINDOW_ORDER_TYPE_WINDOW,
	};
	WINDOW_STATE_ORDER window_state = {};
	uint32_t fields;

	fields = rdp_rail_prepare_window_state_order(
		10, 20, RDP_WINDOW_HIDE, RDP_WINDOW_SHOW,
		&order_info, &window_state);

	ZUC_ASSERT_EQ(WINDOW_ORDER_FIELD_OWNER | WINDOW_ORDER_FIELD_SHOW,
		      fields);
	ZUC_ASSERT_EQ(WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_FIELD_OWNER |
		      WINDOW_ORDER_FIELD_SHOW, order_info.fieldFlags);
	ZUC_ASSERT_EQ(20, window_state.ownerWindowId);
	ZUC_ASSERT_EQ(WINDOW_SHOW, window_state.showState);
}

ZUC_TEST(rdprail_window_state_test, maps_all_show_states)
{
	static const struct {
		uint32_t requested;
		uint32_t expected;
	} cases[] = {
		{ RDP_WINDOW_HIDE, WINDOW_HIDE },
		{ RDP_WINDOW_SHOW, WINDOW_SHOW },
		{ RDP_WINDOW_SHOW_MINIMIZED, WINDOW_SHOW_MINIMIZED },
		{ RDP_WINDOW_SHOW_MAXIMIZED, WINDOW_SHOW_MAXIMIZED },
		{ RDP_WINDOW_SHOW_FULLSCREEN, WINDOW_SHOW },
	};
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(cases); i++) {
		WINDOW_ORDER_INFO order_info = {};
		WINDOW_STATE_ORDER window_state = {};
		uint32_t previous;
		uint32_t fields;

		previous = cases[i].requested == RDP_WINDOW_HIDE ?
			RDP_WINDOW_SHOW : RDP_WINDOW_HIDE;
		fields = rdp_rail_prepare_window_state_order(
			0, 0, previous, cases[i].requested,
			&order_info, &window_state);

		ZUC_ASSERT_EQ(WINDOW_ORDER_FIELD_SHOW, fields);
		ZUC_ASSERT_EQ(cases[i].expected, window_state.showState);
	}
}

ZUC_TEST(rdprail_window_state_test, dispatches_window_update_order)
{
	struct dispatch_state state = {};
	rdpWindowUpdate window_update = {
		.WindowUpdate = record_window_update,
	};
	rdpUpdate update = {
		.context = (rdpContext *)&state,
		.BeginPaint = record_begin_paint,
		.EndPaint = record_end_paint,
		.window = &window_update,
	};
	WINDOW_ORDER_INFO order_info = {
		.windowId = 10,
		.fieldFlags = WINDOW_ORDER_TYPE_WINDOW |
			      WINDOW_ORDER_FIELD_OWNER |
			      WINDOW_ORDER_FIELD_SHOW,
	};
	WINDOW_STATE_ORDER window_state = {
		.ownerWindowId = 20,
		.showState = WINDOW_SHOW,
	};

	rdp_rail_send_window_state_order(&update, &order_info, &window_state);

	ZUC_ASSERT_EQ(3, state.count);
	ZUC_ASSERT_EQ(1, state.calls[0]);
	ZUC_ASSERT_EQ(2, state.calls[1]);
	ZUC_ASSERT_EQ(3, state.calls[2]);
	ZUC_ASSERT_EQ(order_info.windowId, state.order_info.windowId);
	ZUC_ASSERT_EQ(order_info.fieldFlags, state.order_info.fieldFlags);
	ZUC_ASSERT_EQ(window_state.ownerWindowId,
		      state.window_state.ownerWindowId);
	ZUC_ASSERT_EQ(window_state.showState, state.window_state.showState);
}
