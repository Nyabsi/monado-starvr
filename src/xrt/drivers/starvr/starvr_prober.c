// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  StarVR HMD device prober
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup drv_starvr
 */

#include "xrt/xrt_prober.h"

#include "starvr_interface.h"

int
starvr_found(struct xrt_prober *xp,
             struct xrt_prober_device **devices,
             size_t device_count,
             size_t index,
             cJSON *attached_data,
             struct xrt_device **out_xdevs)
{
	struct xrt_device *hmd = starvr_hmd_create(devices[index]);
	if (hmd == NULL) {
		return -1;
	}

	*out_xdevs = hmd;
	return 1;
}