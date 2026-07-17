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

#include <assert.h>
#include <stdbool.h>

#include <libweston/backend-rdp.h>

#include "rdprail-window-state.h"

uint32_t
rdp_rail_prepare_window_state_order(uint32_t previous_owner,
				    uint32_t requested_owner,
				    uint32_t previous_show_state,
				    uint32_t requested_show_state,
				    WINDOW_ORDER_INFO *order_info,
				    WINDOW_STATE_ORDER *window_state)
{
	uint32_t fields = 0;

	if (previous_owner != requested_owner) {
		fields |= WINDOW_ORDER_FIELD_OWNER;
		window_state->ownerWindowId = requested_owner;
	}

	if (previous_show_state != requested_show_state) {
		fields |= WINDOW_ORDER_FIELD_SHOW;
		switch (requested_show_state) {
		case RDP_WINDOW_HIDE:
			window_state->showState = WINDOW_HIDE;
			break;
		case RDP_WINDOW_SHOW:
			window_state->showState = WINDOW_SHOW;
			break;
		case RDP_WINDOW_SHOW_MINIMIZED:
			window_state->showState = WINDOW_SHOW_MINIMIZED;
			break;
		case RDP_WINDOW_SHOW_MAXIMIZED:
			window_state->showState = WINDOW_SHOW_MAXIMIZED;
			break;
		case RDP_WINDOW_SHOW_FULLSCREEN:
			window_state->showState = WINDOW_SHOW;
			break;
		default:
			assert(false);
		}
	}

	order_info->fieldFlags |= fields;
	return fields;
}

void
rdp_rail_send_window_state_order(rdpUpdate *update,
				 const WINDOW_ORDER_INFO *order_info,
				 const WINDOW_STATE_ORDER *window_state)
{
	update->BeginPaint(update->context);
	update->window->WindowUpdate(update->context, order_info, window_state);
	update->EndPaint(update->context);
}
