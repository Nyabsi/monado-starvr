// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Per-device tracking state for the space overseer.
 *
 * @ingroup base
 */

#include "util/u_misc.h"

#include "b_tracked_device.h"

#include <assert.h>


struct b_tracked_device
{
	struct xrt_reference reference;
	struct xrt_space *space;
};

static void
b_tracked_device_destroy(struct b_tracked_device *btd)
{
	struct xrt_space *space = btd->space;
	xrt_space_reference(&space, NULL);
	free(btd);
}

struct b_tracked_device *
b_tracked_device_create(struct xrt_space *space)
{
	assert(space != NULL);

	struct b_tracked_device *btd = U_TYPED_CALLOC(struct b_tracked_device);
	btd->reference.count = 1;

	xrt_space_reference(&btd->space, space);

	return btd;
}

void
b_tracked_device_reference(struct b_tracked_device **dst, struct b_tracked_device *src)
{
	struct b_tracked_device *old_dst = *dst;

	if (old_dst == src) {
		return;
	}

	if (src != NULL) {
		xrt_reference_inc(&src->reference);
	}

	*dst = src;

	if (old_dst != NULL) {
		if (xrt_reference_dec_and_is_zero(&old_dst->reference)) {
			b_tracked_device_destroy(old_dst);
		}
	}
}

struct xrt_space *
b_tracked_device_get_space(struct b_tracked_device *btd)
{
	assert(btd != NULL);
	return btd->space;
}
