// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Per-device tracking state for the space overseer.
 *
 * @ingroup base
 */

#pragma once

#include "xrt/xrt_space.h"


#ifdef __cplusplus
extern "C" {
#endif


struct b_tracked_device;


/*!
 * Create a tracked device with a reference to @p space.
 *
 * @public @memberof b_tracked_device
 */
struct b_tracked_device *
b_tracked_device_create(struct xrt_space *space);

/*!
 * @public @memberof b_tracked_device
 */
void
b_tracked_device_reference(struct b_tracked_device **dst, struct b_tracked_device *src);

/*!
 * Returns the device space without taking a reference.
 *
 * @public @memberof b_tracked_device
 */
struct xrt_space *
b_tracked_device_get_space(struct b_tracked_device *btd);


#ifdef __cplusplus
}
#endif
