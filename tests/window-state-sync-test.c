/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include "shared/window-state-sync.h"

struct recorder {
	int schedules, delay, sends, updates;
	bool success;
};

static void
schedule(void *data, int delay)
{
	struct recorder *r = data;

	r->schedules++;
	r->delay = delay;
}

static bool
send(void *data)
{
	struct recorder *r = data;

	assert(r->updates == 0);
	r->sends++;
	return r->success;
}

int
main(void)
{
	struct weston_window_state_sync sync = { 0 };
	struct recorder r = { 0 };

	/* A quiet scene still schedules a send; repeated events coalesce. */
	weston_window_state_mark(&sync, schedule, &r);
	weston_window_state_mark(&sync, schedule, &r);
	assert(r.schedules == 1 && r.delay == 1);
	assert(!weston_window_state_flush(&sync, false, send, schedule, &r));
	assert(sync.dirty && r.sends == 0 && r.delay == 100);
	assert(!weston_window_state_flush(&sync, true, send, schedule, &r));
	assert(sync.dirty && r.sends == 1);
	r.success = true;
	assert(weston_window_state_flush(&sync, true, send, schedule, &r));
	assert(!sync.dirty && r.sends == 2);
	r.updates++; /* Window updates are allowed only after successful flush. */
	assert(weston_window_state_flush(&sync, true, send, schedule, &r));
	assert(r.sends == 2);
	weston_window_state_mark(&sync, schedule, &r);
	weston_window_state_stop(&sync);
	weston_window_state_stop(&sync);
	assert(!weston_window_state_flush(&sync, true, send, schedule, &r));
	weston_window_state_mark(&sync, schedule, &r);
	assert(!sync.dirty && r.sends == 2);
	return 0;
}
