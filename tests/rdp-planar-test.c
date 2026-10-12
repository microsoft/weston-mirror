/* SPDX-License-Identifier: MIT */
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <freerdp/codec/planar.h>

#include "zunitc/zunitc.h"
#include "../libweston/backend-rdp/rdp-planar.h"

static void
check_roundtrip(uint32_t width, uint32_t height, uint32_t padding,
		bool varying)
{
	uint32_t stride = width * 4 + padding, size = 0, x, y;
	uint8_t *pixels = calloc(stride, height);
	uint8_t *decoded = calloc(width * 4, height);
	uint8_t *encoded = NULL;
	BITMAP_PLANAR_CONTEXT *decoder;

	ZUC_ASSERT_TRUE(pixels != NULL);
	ZUC_ASSERT_TRUE(decoded != NULL);
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			uint8_t *p = pixels + y * stride + x * 4;

			p[0] = varying ? (x * 7 + y * 3) % 256 : 32;
			p[1] = varying ? (x + y * 11) % 256 : 64;
			p[2] = varying ? (y * 19) % 256 : 128;
			p[3] = varying ? (x * 5 + y * 17) % 256 : 255;
		}
	}
	ZUC_ASSERT_TRUE(rdp_planar_compress(pixels, width, height, stride,
					  &encoded, &size));
	ZUC_ASSERT_TRUE(size < width * height * 4);
	decoder = freerdp_bitmap_planar_context_new(0, width, height);
	ZUC_ASSERT_TRUE(decoder != NULL);
	/* Match the RDPGFX decoder's destination offset and flip convention. */
	ZUC_ASSERT_TRUE(planar_decompress(decoder, encoded, size, width, height,
					 decoded, PIXEL_FORMAT_BGRA32, width * 4,
					 0, 0, width, height, FALSE));
	for (y = 0; y < height; y++)
		ZUC_ASSERT_EQ(0, memcmp(pixels + y * stride,
				       decoded + y * width * 4, width * 4));
	freerdp_bitmap_planar_context_free(decoder);
	free(encoded);
	free(decoded);
	free(pixels);
}

ZUC_TEST(rdp_planar, opaque_window)
{
	check_roundtrip(1600, 900, 0, false);
}

ZUC_TEST(rdp_planar, colours_alpha_and_padded_stride)
{
	check_roundtrip(640, 480, 16, true);
}

ZUC_TEST(rdp_planar, odd_rectangle)
{
	check_roundtrip(17, 9, 12, true);
}

ZUC_TEST(rdp_planar, raw_fallback)
{
	uint8_t pixels[] = { 32, 64, 128, 255 };
	uint8_t *encoded = pixels;
	uint32_t size = 123;

	/* A planar header makes a single pixel larger than the raw bitmap. */
	ZUC_ASSERT_FALSE(rdp_planar_compress(pixels, 1, 1, 4, &encoded, &size));
	ZUC_ASSERT_TRUE(encoded == NULL);
	ZUC_ASSERT_EQ(0, size);
	ZUC_ASSERT_FALSE(rdp_planar_compress(pixels, 0, 1, 4, &encoded, &size));
	ZUC_ASSERT_FALSE(rdp_planar_compress(pixels, 1, 1, 3, &encoded, &size));
	ZUC_ASSERT_FALSE(rdp_planar_compress(pixels, UINT32_MAX, 1, 4,
					   &encoded, &size));
}
