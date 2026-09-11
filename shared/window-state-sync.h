/* SPDX-License-Identifier: MIT */
#ifndef WESTON_WINDOW_STATE_SYNC_H
#define WESTON_WINDOW_STATE_SYNC_H

#include <stdbool.h>

struct weston_window_state_sync {
	bool dirty;
	bool stopped;
};

typedef void (*weston_state_schedule_func)(void *data, int delay_ms);
typedef bool (*weston_state_send_func)(void *data);

static inline void
weston_window_state_mark(struct weston_window_state_sync *sync,
			 weston_state_schedule_func schedule, void *data)
{
	if (sync->stopped || sync->dirty)
		return;
	sync->dirty = true;
	schedule(data, 1);
}

static inline bool
weston_window_state_flush(struct weston_window_state_sync *sync, bool ready,
			  weston_state_send_func send,
			  weston_state_schedule_func schedule, void *data)
{
	if (sync->stopped)
		return false;
	if (!sync->dirty)
		return true;
	if (ready && send(data)) {
		sync->dirty = false;
		return true;
	}
	schedule(data, 100);
	return false;
}

static inline void
weston_window_state_stop(struct weston_window_state_sync *sync)
{
	sync->stopped = true;
	sync->dirty = false;
}

#endif
