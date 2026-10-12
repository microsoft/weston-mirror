/* SPDX-License-Identifier: MIT */
#ifndef WESTON_RDP_PLANAR_H
#define WESTON_RDP_PLANAR_H

#include <stdbool.h>
#include <stdint.h>

/* On success, the caller owns *encoded and must free it. On failure, use
 * the original bitmap: *encoded is NULL and *encoded_size is zero. */
bool
rdp_planar_compress(const uint8_t *pixels, uint32_t width, uint32_t height,
		    uint32_t stride, uint8_t **encoded, uint32_t *encoded_size);

#endif
