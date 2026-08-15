// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to the StarVR driver.
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup drv_starvr
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_defines.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_starvr StarVR driver
 * @ingroup drv
 *
 * @brief Driver for StarVR One and StarVR One XT.
 */

/*!
 * @dir drivers/starvr
 *
 * @brief @ref drv_starvr files.
 */

struct xrt_device;
struct xrt_prober;
struct xrt_prober_device;

#define STARVR_VID 0x048D
#define STARVR_PID 0x8595

#define STARVR_PANEL_NARROW_WIDTH 3840
#define STARVR_PANEL_HEIGHT 2240

#define STARVR_MIN_IPD_MM 53.0f
#define STARVR_MAX_IPD_MM 77.0f
#define STARVR_DEFAULT_IPD_MM 64.0f

#define STARVR_PANEL_GAMMA 2.2f
#define STARVR_MIN_ANALOG_GAIN 0.1f
#define STARVR_MAX_ANALOG_GAIN 1.0f

static const struct xrt_panel_strip starvr_strips_narrow[8] = {
    {60, 300}, {600, 298}, {1020, 300}, {1560, 297}, {1980, 300}, {2520, 298}, {2940, 300}, {3480, 297},
};

//! The firmware takes an index rather than a rate.
struct starvr_panel_rate
{
	uint8_t setting;
	float hz;
};

static const struct starvr_panel_rate starvr_panel_rates[] = {
    {0, 90.0f},
    {1, 72.0f},
    {2, 75.0f},
    {3, 89.98f},
};

const char *
starvr_hmd_get_tracker_serial(struct xrt_device *xdev);

void
starvr_hmd_set_tracking_device(struct xrt_device *xdev_hmd,
                               struct xrt_device *xdev_tracker,
                               struct xrt_pose *tracker_to_head);

struct xrt_device *
starvr_hmd_create(struct xrt_prober_device *xpdev);

//! @see xrt_prober_found_func_t
int
starvr_found(struct xrt_prober *xp,
             struct xrt_prober_device **devices,
             size_t device_count,
             size_t index,
             cJSON *attached_data,
             struct xrt_device **out_xdevs);

#ifdef __cplusplus
}
#endif
