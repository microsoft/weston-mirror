/* SPDX-License-Identifier: MIT */
#ifndef WESTON_POPUP_CONSTRAINT_H
#define WESTON_POPUP_CONSTRAINT_H

#include <stdbool.h>
#include <stdint.h>

/* Work in 64 bits: positioner coordinates are client supplied int32_t. */
static inline bool
popup_axis_fits(int64_t pos, int64_t size, int64_t lo, int64_t hi)
{
	return pos >= lo && pos + size <= hi;
}

static inline void
popup_constrain_axis(int64_t *pos, int64_t *size, int64_t flipped,
		     int64_t lo, int64_t hi, bool flip, bool slide, bool resize)
{
	int64_t end;

	if (hi <= lo || popup_axis_fits(*pos, *size, lo, hi))
		return;
	if (flip && popup_axis_fits(flipped, *size, lo, hi))
		*pos = flipped;
	if (slide) {
		if (*pos + *size > hi)
			*pos = hi - *size;
		if (*pos < lo)
			*pos = lo;
	}
	if (resize && !popup_axis_fits(*pos, *size, lo, hi)) {
		end = *pos + *size < hi ? *pos + *size : hi;
		if (*pos < lo)
			*pos = lo;
		/* A disjoint rectangle cannot be resized into the work area. */
		if (end > *pos)
			*size = end - *pos;
	}
}

#endif
