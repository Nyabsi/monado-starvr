// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  @ref drv_starvr driver builder.
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup xrt_iface
 */

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"

#include "math/m_api.h"
#include "math/m_space.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_system_helpers.h"
#include "util/u_trace_marker.h"
#include "util/u_builder_search.h"

#include "target_builder_helpers.h"

#include "starvr/starvr_interface.h"
#include "steamvr_lh/steamvr_lh_interface.h"

#include <assert.h>
#include <string.h>

DEBUG_GET_ONCE_LOG_OPTION(starvr_log, "STARVR_LOG", U_LOGGING_INFO)
DEBUG_GET_ONCE_BOOL_OPTION(starvr_lighthouse_tracking_enabled, "STARVR_LIGHTHOUSE_TRACKING_ENABLED", true)
DEBUG_GET_ONCE_OPTION(starvr_tracker_serial, "STARVR_TRACKER_SERIAL", NULL)

/*
 *
 * There is two variants of the StarVR: XT and Lighthouse
 * We could determine this from EEPROM but this is easier
 *
 */
DEBUG_GET_ONCE_OPTION(starvr_device_variant, "STARVR_DEVICE_VARIANT", "xt")

#define SVR_INFO(b, ...) U_LOG_IFL_I(b->log_level, __VA_ARGS__)
#define SVR_WARN(b, ...) U_LOG_IFL_W(b->log_level, __VA_ARGS__)
#define SVR_ERROR(b, ...) U_LOG_IFL_E(b->log_level, __VA_ARGS__)
#define SVR_DEBUG(b, ...) U_LOG_IFL_D(b->log_level, __VA_ARGS__)

static const char *driver_list[] = {
    "starvr",
    "steamvr_lh",
};

struct starvr_system
{
	struct t_builder base;

	enum u_logging_level log_level;
};

static struct xrt_device *
find_tracker_by_serial(struct starvr_system *ss, struct xrt_system_devices *xsysd, const char *serial)
{
	if (serial == NULL || serial[0] == '\0') {
		SVR_WARN(ss,
		         "No lighthouse tracker serial, set STARVR_TRACKER_SERIAL or pair the headset"
		         ", the headset will not have tracking.");
		return NULL;
	}

	for (size_t i = 0; i < xsysd->static_xdev_count; i++) {
		struct xrt_device *xdev = xsysd->static_xdevs[i];

		if (xdev == NULL || xdev->device_type == XRT_DEVICE_TYPE_HMD) {
			continue;
		}

		if (strcmp(xdev->serial, serial) == 0) {
			SVR_INFO(ss, "Using lighthouse tracker '%s' with serial: '%s'", xdev->str, xdev->serial);
			return xdev;
		}
	}

	SVR_WARN(ss,
	         "Could not find suitable lighthouse tracker with serial: '%s'"
	         ", the headset will not have tracking.",
	         serial);

	return NULL;
}

static void
apply_offset_to_pose(struct xrt_pose *out)
{
	struct xrt_vec3 vec_axis = {1.0f, 0.0f, 0.0f};
	math_quat_from_angle_vector(-M_PI_2, &vec_axis, &out->orientation);

	/*
	 * Static offset extracted from device EEPROM.
	 */
	struct xrt_vec3 tracker_offset = {0.0f, -0.0789995f, 0.0529276f};
	math_quat_rotate_vec3(&out->orientation, &tracker_offset, &out->position);
}

static void
set_hmd_tracking_device(struct starvr_system *ss, struct xrt_device *hmd_xdev, struct xrt_device *tracker_xdev)
{
	struct xrt_pose tracker_to_head = XRT_POSE_IDENTITY;

	if (strcmp(debug_get_option_starvr_device_variant(), "xt") == 0) {
		apply_offset_to_pose(&tracker_to_head);
	}

	starvr_hmd_set_tracking_device(hmd_xdev, tracker_xdev, &tracker_to_head);

	/*
	 * Copy the tracking origin from the tracker to the hmd
	 * otherwise other devices would be in the wrong space
	 */
	hmd_xdev->tracking_origin = tracker_xdev->tracking_origin;
}

static void
add_lighthouse_devices(struct starvr_system *ss, struct xrt_system_devices *xsysd, struct xrt_prober *xp)
{
	struct xrt_system_devices *lh_xsysd = NULL;

	xrt_result_t xret = steamvr_lh_create_devices(xp, &lh_xsysd);
	if (xret != XRT_SUCCESS || lh_xsysd == NULL) {
		SVR_WARN(ss, "drv_steamvr_lh failed to create devices, the headset will not have tracking.");
		return;
	}

	for (size_t i = 0; i < lh_xsysd->static_xdev_count; i++) {
		struct xrt_device *xdev = lh_xsysd->static_xdevs[i];
		if (xdev == NULL) {
			continue;
		}

		if (xdev->device_type == XRT_DEVICE_TYPE_HMD) {
			SVR_WARN(ss, "lighthouse HMD found, disconnect it from your computer.");
			continue;
		}

		if (xsysd->static_xdev_count >= ARRAY_SIZE(xsysd->static_xdevs)) {
			break;
		}

		SVR_DEBUG(ss, "Found Lighthouse device '%s' (%s)", xdev->str, xdev->serial);

		xsysd->static_xdevs[xsysd->static_xdev_count++] = xdev;
	}
}

static void
assign_controller_roles(struct starvr_system *ss,
                        struct xrt_system_devices *xsysd,
                        struct xrt_device *hmd_xdev,
                        struct xrt_device *tracker_xdev,
                        struct t_builder_roles_helper *tbrh)
{
	int head_idx = -1;
	int eyes_idx = -1;
	int face_idx = -1;
	int left_idx = -1;
	int right_idx = -1;
	int gamepad_idx = -1;

	struct xrt_device *cdevs[XRT_SYSTEM_MAX_DEVICES] = {0};
	size_t candidate_idx = 0;

	for (size_t i = 0; i < xsysd->static_xdev_count; i++) {
		struct xrt_device *xdev = xsysd->static_xdevs[i];

		if (xdev == NULL || xdev == hmd_xdev || xdev == tracker_xdev) {
			continue;
		}

		cdevs[candidate_idx++] = xdev;
	}

	u_device_assign_xdev_roles(cdevs, candidate_idx, &head_idx, &eyes_idx, &face_idx, &left_idx, &right_idx,
	                           &gamepad_idx);

	if (left_idx != -1) {
		tbrh->left = cdevs[left_idx];
		SVR_INFO(ss, "Left hand is '%s'", tbrh->left->str);
	}
	if (right_idx != -1) {
		tbrh->right = cdevs[right_idx];
		SVR_INFO(ss, "Right hand is '%s'", tbrh->right->str);
	}
	if (gamepad_idx != -1) {
		tbrh->gamepad = cdevs[gamepad_idx];
	}
}

/*
 *
 * Member functions.
 *
 */

static xrt_result_t
starvr_estimate_system(struct xrt_builder *xb,
                       cJSON *config,
                       struct xrt_prober *xp,
                       struct xrt_builder_estimate *estimate)
{
	struct xrt_prober_device **xpdevs = NULL;
	size_t xpdev_count = 0;

	U_ZERO(estimate);

	xrt_result_t xret = xrt_prober_lock_list(xp, &xpdevs, &xpdev_count);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	const bool found =
	    u_builder_find_prober_device(xpdevs, xpdev_count, STARVR_VID, STARVR_PID, XRT_BUS_TYPE_USB) != NULL;

	xret = xrt_prober_unlock_list(xp, &xpdevs);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	estimate->certain.head = found;
	estimate->maybe.dof6 = found && debug_get_bool_option_starvr_lighthouse_tracking_enabled();

	return XRT_SUCCESS;
}

static xrt_result_t
starvr_open_system_impl(struct xrt_builder *xb,
                        cJSON *config,
                        struct xrt_prober *xp,
                        struct xrt_tracking_origin *origin,
                        struct xrt_system_devices *xsysd,
                        struct xrt_frame_context *xfctx,
                        struct t_builder_roles_helper *tbrh)
{
	struct starvr_system *ss = (struct starvr_system *)xb;

	struct xrt_prober_device **xpdevs = NULL;
	size_t xpdev_count = 0;
	struct xrt_device *hmd_xdev = NULL;

	DRV_TRACE_MARKER();

	xrt_result_t xret = xrt_prober_lock_list(xp, &xpdevs, &xpdev_count);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	struct xrt_prober_device *head_xpdev =
	    u_builder_find_prober_device(xpdevs, xpdev_count, STARVR_VID, STARVR_PID, XRT_BUS_TYPE_USB);
	if (head_xpdev != NULL) {
		hmd_xdev = starvr_hmd_create(head_xpdev);
	}

	xret = xrt_prober_unlock_list(xp, &xpdevs);
	if (xret != XRT_SUCCESS) {
		goto fail;
	}

	if (hmd_xdev == NULL) {
		SVR_ERROR(ss, "StarVR HMD device creation failed");
		goto fail;
	}

	xsysd->static_xdevs[xsysd->static_xdev_count++] = hmd_xdev;

	tbrh->head = hmd_xdev;
	tbrh->eyes = hmd_xdev;
	tbrh->face = hmd_xdev;

	if (!debug_get_bool_option_starvr_lighthouse_tracking_enabled()) {
		return XRT_SUCCESS;
	}

	add_lighthouse_devices(ss, xsysd, xp);

	/*
	 * The headset knows which tracker it was paired with, the environment
	 * only has to say so when that pairing is wrong or missing.
	 */
	const char *tracker_serial = debug_get_option_starvr_tracker_serial();
	if (tracker_serial == NULL || tracker_serial[0] == '\0') {
		tracker_serial = starvr_hmd_get_tracker_serial(hmd_xdev);
	}

	struct xrt_device *tracker_xdev = find_tracker_by_serial(ss, xsysd, tracker_serial);
	if (tracker_xdev != NULL) {
		set_hmd_tracking_device(ss, hmd_xdev, tracker_xdev);
	}

	assign_controller_roles(ss, xsysd, hmd_xdev, tracker_xdev, tbrh);

	return XRT_SUCCESS;

fail:
	return XRT_ERROR_DEVICE_CREATION_FAILED;
}

static void
starvr_destroy(struct xrt_builder *xb)
{
	free(xb);
}

/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_builder *
t_builder_starvr_create(void)
{
	struct starvr_system *ss = U_TYPED_CALLOC(struct starvr_system);

	ss->log_level = debug_get_log_option_starvr_log();

	ss->base.base.estimate_system = starvr_estimate_system;
	ss->base.base.open_system = t_builder_open_system_static_roles;
	ss->base.base.destroy = starvr_destroy;
	ss->base.base.identifier = "starvr";
	ss->base.base.name = "StarVR One";
	ss->base.base.driver_identifiers = driver_list;
	ss->base.base.driver_identifier_count = ARRAY_SIZE(driver_list);

	ss->base.open_system_static_roles = starvr_open_system_impl;

	return &ss->base.base;
}
