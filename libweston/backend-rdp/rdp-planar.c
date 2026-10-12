/* SPDX-License-Identifier: MIT */
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <freerdp/codec/planar.h>

#include "rdp-planar.h"

bool
rdp_planar_compress(const uint8_t *pixels, uint32_t width, uint32_t height,
		    uint32_t stride, uint8_t **encoded, uint32_t *encoded_size)
{
	BITMAP_PLANAR_CONTEXT *context;
	uint8_t *bottom_up, *result;
	uint32_t row, row_size, result_size = 0;
	size_t raw_size;

	*encoded = NULL;
	*encoded_size = 0;
	if (!pixels || !width || !height || width > UINT32_MAX / 4)
		return false;
	row_size = width * 4;
	if (stride < row_size || height > UINT32_MAX / row_size ||
	    height > SIZE_MAX / stride)
		return false;
	raw_size = (size_t)row_size * height;

	/* Retain alpha and all RGB bits: no colour loss or subsampling. */
	context = freerdp_bitmap_planar_context_new(PLANAR_FORMAT_HEADER_RLE,
						  width, height);
	if (!context)
		return false;
	bottom_up = malloc(raw_size);
	if (!bottom_up) {
		freerdp_bitmap_planar_context_free(context);
		return false;
	}

	/* FreeRDP's encoder reads bottom-up. RDPGFX decodes top-down. */
	for (row = 0; row < height; row++)
		memcpy(bottom_up + (size_t)row * row_size,
		       pixels + (size_t)(height - row - 1) * stride, row_size);
	result = freerdp_bitmap_compress_planar(context, bottom_up,
					      PIXEL_FORMAT_BGRA32, width, height,
					      row_size, NULL, &result_size);
	free(bottom_up);
	freerdp_bitmap_planar_context_free(context);
	if (!result || !result_size || result_size >= raw_size) {
		free(result);
		return false;
	}

	*encoded = result;
	*encoded_size = result_size;
	return true;
}
