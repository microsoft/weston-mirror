/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stddef.h>
#include "shared/popup-constraint.h"

struct axis_case {
	int64_t pos, size, flipped, lo, hi;
	bool flip, slide, resize;
	int64_t expected_pos, expected_size;
};

int
main(void)
{
	const struct axis_case cases[] = {
		/* Right and bottom edges: flip before sliding over the parent. */
		{ 950, 200, 550, 0, 1000, true, true, true, 550, 200 },
		{ 750, 200, 350, 0, 800, true, true, true, 350, 200 },
		/* Left and top panels, including negative output coordinates. */
		{ 0, 100, 200, 40, 1000, true, true, false, 200, 100 },
		{ -30, 100, 100, 40, 800, true, true, false, 100, 100 },
		{ -1100, 200, -900, -1000, 0, true, true, false, -900, 200 },
		/* No permission means no adjustment. */
		{ 950, 200, 550, 0, 1000, false, false, false, 950, 200 },
		/* Failed flip must fall back to the original, then slide. */
		{ 950, 200, -300, 0, 1000, true, true, false, 800, 200 },
		{ 950, 200, -300, 0, 1000, true, false, false, 950, 200 },
		/* Oversized menu, resize alone, and disjoint geometry. */
		{ 500, 1200, -800, 0, 1000, true, true, true, 0, 1000 },
		{ 900, 200, 500, 0, 1000, false, false, true, 900, 100 },
		{ 1200, 200, 1100, 0, 1000, true, false, true, 1200, 200 },
		/* Logical coordinates after a 2x output conversion. */
		{ 475, 100, 275, 0, 500, true, true, false, 275, 100 },
		{ 100, 100, 0, 0, 800, true, true, true, 100, 100 },
		{ INT32_MAX, 200, INT32_MAX - 400, 0, INT32_MAX,
		  true, true, false, INT32_MAX - 400, 200 },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		const struct axis_case *c = &cases[i];
		int64_t pos = c->pos, size = c->size;

		popup_constrain_axis(&pos, &size, c->flipped, c->lo, c->hi,
				     c->flip, c->slide, c->resize);
		assert(pos == c->expected_pos && size == c->expected_size);
		/* Re-evaluating unchanged inputs must never toggle direction. */
		for (int frame = 0; frame < 100; frame++) {
			pos = c->pos;
			size = c->size;
			popup_constrain_axis(&pos, &size, c->flipped, c->lo, c->hi,
					     c->flip, c->slide, c->resize);
			assert(pos == c->expected_pos && size == c->expected_size);
		}
	}
	return 0;
}
