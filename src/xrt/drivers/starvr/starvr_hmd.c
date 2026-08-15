// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  StarVR HMD device.
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup drv_starvr
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"

#include "math/m_api.h"
#include "math/m_mathinclude.h"
#include "math/m_space.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_json.h"
#include "util/u_misc.h"
#include "util/u_var.h"
#include "util/u_visibility_mask.h"

#include "starvr_interface.h"
#include "starvr_hid.h"
#include "starvr_protocol.h"
#include "starvr_mesh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STARVR_MESH_LEFT "resources/meshes/StarVR_L_v2.mesh"
#define STARVR_MESH_RIGHT "resources/meshes/StarVR_R_v2.mesh"

DEBUG_GET_ONCE_LOG_OPTION(starvr_log, "STARVR_LOG", U_LOGGING_INFO)
DEBUG_GET_ONCE_OPTION(starvr_resource_path, "STARVR_RESOURCE_PATH", NULL)
DEBUG_GET_ONCE_FLOAT_OPTION(starvr_ipd_mm, "STARVR_IPD_MM", STARVR_DEFAULT_IPD_MM)
DEBUG_GET_ONCE_FLOAT_OPTION(starvr_analog_gain, "STARVR_ANALOG_GAIN", STARVR_MAX_ANALOG_GAIN)

/*!
 * The mesh asks for a 5800x4306 eye buffer, which nothing can drive at 90 Hz.
 * The distortion is normalised, so a smaller one is still correct, just softer.
 */
DEBUG_GET_ONCE_FLOAT_OPTION(starvr_render_scale, "STARVR_RENDER_SCALE", 0.5f)

#define SVR_TRACE(hmd, ...) U_LOG_XDEV_IFL_T(&hmd->base, hmd->log_level, __VA_ARGS__)
#define SVR_DEBUG(hmd, ...) U_LOG_XDEV_IFL_D(&hmd->base, hmd->log_level, __VA_ARGS__)
#define SVR_INFO(hmd, ...) U_LOG_XDEV_IFL_I(&hmd->base, hmd->log_level, __VA_ARGS__)
#define SVR_WARN(hmd, ...) U_LOG_XDEV_IFL_W(&hmd->base, hmd->log_level, __VA_ARGS__)
#define SVR_ERROR(hmd, ...) U_LOG_XDEV_IFL_E(&hmd->base, hmd->log_level, __VA_ARGS__)

struct starvr_hmd
{
	struct xrt_device base;

	enum u_logging_level log_level;

	struct starvr_hid *hid;

	struct starvr_mesh_data mesh;

	struct
	{
		struct xrt_device *xdev;
		//! Where the head sits in the tracker's frame, the XT mount needs this.
		struct xrt_pose tracker_to_head;
	} tracker;

	struct xrt_pose eye_poses[2];

	float ipd_m;
	float refresh_rate_hz;
	float vsync_to_photons_s;

	uint8_t brightness;

	char serial[64];
	char fw_version[33];
	char tracker_serial[64];
	char left_panel_serial[65];
	char right_panel_serial[65];
	char resource_path[512];
};

static inline struct starvr_hmd *
starvr_hmd(struct xrt_device *xdev)
{
	return (struct starvr_hmd *)xdev;
}

static float
rate_from_setting(uint8_t setting)
{
	for (size_t i = 0; i < ARRAY_SIZE(starvr_panel_rates); i++) {
		if (starvr_panel_rates[i].setting == setting) {
			return starvr_panel_rates[i].hz;
		}
	}

	return 90.0f;
}

static uint8_t
brightness_from_analog_gain(float gain)
{
	if (gain < STARVR_MIN_ANALOG_GAIN) {
		gain = STARVR_MIN_ANALOG_GAIN;
	} else if (gain > STARVR_MAX_ANALOG_GAIN) {
		gain = STARVR_MAX_ANALOG_GAIN;
	}

	return (uint8_t)(powf(gain, 1.0f / STARVR_PANEL_GAMMA) * 255.0f + 0.5f);
}

static bool
is_valid_panel_serial(const char *serial)
{
	if (strlen(serial) != 22 || strncmp(serial, "KL0470500", 9) != 0) {
		return false;
	}

	for (size_t i = 0; i < 22; i++) {
		const char c = serial[i];
		const bool upper = c >= 'A' && c <= 'Z';
		const bool digit = c >= '0' && c <= '9';
		if (!upper && !digit) {
			return false;
		}
	}

	return true;
}

static void
read_eeprom_json(struct starvr_hmd *hmd)
{
	uint8_t length_be[2] = {0};
	if (!starvr_hid_eeprom_read(hmd->hid, STARVR_EEPROM_ADDR_CONFIG_LENGTH, 2, length_be)) {
		SVR_WARN(hmd, "Could not read the configuration blob length");
		return;
	}

	const uint16_t length = (uint16_t)((uint16_t)length_be[0] << 8 | length_be[1]);
	if (length == 0 || length > 0x4000) {
		SVR_WARN(hmd, "Configuration blob length %u is not plausible", length);
		return;
	}

	char *text = U_TYPED_ARRAY_CALLOC(char, (size_t)length + 1);
	if (text == NULL) {
		return;
	}

	if (!starvr_hid_eeprom_read(hmd->hid, STARVR_EEPROM_ADDR_CONFIG, length, text)) {
		SVR_WARN(hmd, "Could not read the configuration blob");
		free(text);
		return;
	}

	cJSON *json = cJSON_Parse(text);
	free(text);

	if (json == NULL) {
		SVR_WARN(hmd, "Could not parse the configuration blob");
		return;
	}

	const cJSON *serial = u_json_get(u_json_get(u_json_get(json, "Tracking"), "Lighthouse"), "SerialNumber");
	if (serial != NULL) {
		u_json_get_string_into_array(serial, hmd->tracker_serial, sizeof(hmd->tracker_serial));

		if (strcmp(hmd->tracker_serial, STARVR_UNPAIRED_TRACKER) == 0) {
			hmd->tracker_serial[0] = '\0';
		}
	}

	float ms = 0.0f;
	if (u_json_get_float(u_json_get(json, "msFromVsyncToPhotons"), &ms)) {
		hmd->vsync_to_photons_s = ms / 1000.0f;
	}

	cJSON_Delete(json);
}

static void
apply_analog_gain(struct starvr_hmd *hmd, float gain)
{
	const uint8_t value = brightness_from_analog_gain(gain);

	bool ok = true;
	for (uint8_t channel = 0; channel < 2; channel++) {
		ok = starvr_hid_brightness_write(hmd->hid, channel, value) && ok;
	}

	if (!ok) {
		SVR_WARN(hmd, "Could not set the panel brightness");
		return;
	}

	hmd->brightness = value;
	SVR_INFO(hmd, "Panel brightness set to %u (analog gain %.3f)", value, (double)gain);
}

static void
read_firmware_info(struct starvr_hmd *hmd)
{
	uint8_t fps_setting = 0;
	if (starvr_hid_fps_setting_read(hmd->hid, &fps_setting)) {
		hmd->refresh_rate_hz = rate_from_setting(fps_setting);
	} else {
		SVR_WARN(hmd, "Could not read the panel refresh rate, assuming %.2f Hz", (double)hmd->refresh_rate_hz);
	}

	char unique_id[33] = {0};
	if (starvr_hid_eeprom_read(hmd->hid, STARVR_EEPROM_ADDR_UNIQUE_ID, 32, unique_id)) {
		snprintf(hmd->serial, sizeof(hmd->serial), "%s", unique_id);
	} else {
		SVR_WARN(hmd, "Could not read the headset serial number");
	}

	char left[65] = {0};
	char right[65] = {0};
	if (starvr_hid_eeprom_read(hmd->hid, STARVR_EEPROM_ADDR_PANEL_SERIAL_LEFT, 64, left) &&
	    starvr_hid_eeprom_read(hmd->hid, STARVR_EEPROM_ADDR_PANEL_SERIAL_RIGHT, 64, right)) {
		if (is_valid_panel_serial(left) && is_valid_panel_serial(right)) {
			snprintf(hmd->left_panel_serial, sizeof(hmd->left_panel_serial), "%s", left);
			snprintf(hmd->right_panel_serial, sizeof(hmd->right_panel_serial), "%s", right);
		} else {
			SVR_WARN(hmd, "Panel serials look wrong");
		}
	} else {
		SVR_WARN(hmd, "Could not read the panel serial numbers");
	}

	if (!starvr_hid_fw_version_read(hmd->hid, hmd->fw_version)) {
		SVR_WARN(hmd, "Could not read the firmware version");
	}

	read_eeprom_json(hmd);
}

static bool
directory_has_meshes(const char *path)
{
	char probe[1024];

	snprintf(probe, sizeof(probe), "%s/%s", path, STARVR_MESH_LEFT);

	return access(probe, R_OK) == 0;
}

static void
resolve_resource_path(struct starvr_hmd *hmd)
{
	static const char *system_dirs[] = {
	    "/usr/local/share/starvr",
	    "/usr/share/starvr",
	    "/opt/starvr",
	};

	const char *from_env = debug_get_option_starvr_resource_path();
	if (from_env != NULL && from_env[0] != '\0') {
		snprintf(hmd->resource_path, sizeof(hmd->resource_path), "%s", from_env);
		return;
	}

	char candidate[512] = {0};

	const char *data_home = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");

	if (data_home != NULL && data_home[0] != '\0') {
		snprintf(candidate, sizeof(candidate), "%s/starvr", data_home);
	} else if (home != NULL && home[0] != '\0') {
		snprintf(candidate, sizeof(candidate), "%s/.local/share/starvr", home);
	}

	if (candidate[0] != '\0' && directory_has_meshes(candidate)) {
		snprintf(hmd->resource_path, sizeof(hmd->resource_path), "%s", candidate);
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(system_dirs); i++) {
		if (directory_has_meshes(system_dirs[i])) {
			snprintf(hmd->resource_path, sizeof(hmd->resource_path), "%s", system_dirs[i]);
			return;
		}
	}

	snprintf(hmd->resource_path, sizeof(hmd->resource_path), "%s", system_dirs[1]);
}

static bool
load_meshes(struct starvr_hmd *hmd)
{
	char left[1024];
	char right[1024];

	snprintf(left, sizeof(left), "%s/%s", hmd->resource_path, STARVR_MESH_LEFT);
	snprintf(right, sizeof(right), "%s/%s", hmd->resource_path, STARVR_MESH_RIGHT);

	if (!starvr_mesh_load(left, right, hmd->ipd_m * 1000.0f, true, true, &hmd->mesh)) {
		SVR_ERROR(hmd,
		          "Could not load the distortion meshes. Expected them at:\n\t%s\n\t%s\n"
		          "Set STARVR_RESOURCE_PATH if they live elsewhere.",
		          left, right);
		return false;
	}

	hmd->ipd_m = hmd->mesh.ipd_mm / 1000.0f;

	return true;
}

static void
starvr_hmd_destroy(struct xrt_device *xdev)
{
	struct starvr_hmd *hmd = starvr_hmd(xdev);

	u_var_remove_root(hmd);

	starvr_mesh_free(&hmd->mesh);

	if (hmd->hid != NULL) {
		starvr_hid_close(hmd->hid);
		hmd->hid = NULL;
	}

	// starvr_mesh_data owns these, u_device_free must not free them again.
	hmd->base.hmd->distortion.mesh.vertices = NULL;
	hmd->base.hmd->distortion.mesh.indices = NULL;

	u_device_free(&hmd->base);
}

static xrt_result_t
starvr_hmd_update_inputs(struct xrt_device *xdev)
{
	return XRT_SUCCESS;
}

static xrt_result_t
starvr_hmd_get_tracked_pose(struct xrt_device *xdev,
                            enum xrt_input_name name,
                            int64_t at_timestamp_ns,
                            struct xrt_space_relation *out_relation)
{
	struct starvr_hmd *hmd = starvr_hmd(xdev);

	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	/*
	 * Done here rather than with a tracking override because a wrapper hides
	 * that the head is a StarVR One from anything that has to recognise it.
	 */
	if (hmd->tracker.xdev != NULL) {
		struct xrt_space_relation tracker_relation = XRT_SPACE_RELATION_ZERO;

		xrt_result_t xret = xrt_device_get_tracked_pose(hmd->tracker.xdev, XRT_INPUT_GENERIC_TRACKER_POSE,
		                                                at_timestamp_ns, &tracker_relation);
		if (xret != XRT_SUCCESS) {
			return xret;
		}

		struct xrt_relation_chain xrc = {0};
		m_relation_chain_push_pose_if_not_identity(&xrc, &hmd->tracker.tracker_to_head);
		m_relation_chain_push_relation(&xrc, &tracker_relation);
		m_relation_chain_resolve(&xrc, out_relation);

		return XRT_SUCCESS;
	}

	struct xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
	relation.pose = (struct xrt_pose)XRT_POSE_IDENTITY;
	relation.relation_flags = (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                          XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                                                          XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                                                          XRT_SPACE_RELATION_POSITION_TRACKED_BIT);

	*out_relation = relation;

	return XRT_SUCCESS;
}

static xrt_result_t
starvr_hmd_get_view_poses(struct xrt_device *xdev,
                          const struct xrt_vec3 *default_eye_relation,
                          int64_t at_timestamp_ns,
                          enum xrt_view_type view_type,
                          uint32_t view_count,
                          struct xrt_space_relation *out_head_relation,
                          struct xrt_fov *out_fovs,
                          struct xrt_pose *out_poses)
{
	struct starvr_hmd *hmd = starvr_hmd(xdev);

	xrt_result_t xret =
	    xrt_device_get_tracked_pose(xdev, XRT_INPUT_GENERIC_HEAD_POSE, at_timestamp_ns, out_head_relation);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	// The panels are canted, so the generic helper cannot be used.
	for (uint32_t i = 0; i < view_count && i < ARRAY_SIZE(hmd->eye_poses); i++) {
		out_fovs[i] = hmd->base.hmd->distortion.fov[i];
		out_poses[i] = hmd->eye_poses[i];
	}

	return XRT_SUCCESS;
}

static xrt_result_t
starvr_hmd_get_visibility_mask(struct xrt_device *xdev,
                               enum xrt_visibility_mask_type type,
                               uint32_t view_index,
                               struct xrt_visibility_mask **out_mask)
{
	struct starvr_hmd *hmd = starvr_hmd(xdev);

	if (view_index >= 2 || type != XRT_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH ||
	    hmd->mesh.hidden_area[view_index] == NULL || hmd->mesh.hidden_area_triangle_count[view_index] == 0) {
		u_visibility_mask_get_noop(type, out_mask);
		return XRT_SUCCESS;
	}

	const uint32_t vertex_count = hmd->mesh.hidden_area_triangle_count[view_index] * 3;
	const size_t size = sizeof(struct xrt_visibility_mask) + sizeof(uint32_t) * vertex_count +
	                    sizeof(struct xrt_vec2) * vertex_count;

	struct xrt_visibility_mask *mask = U_CALLOC_WITH_CAST(struct xrt_visibility_mask, size);
	if (mask == NULL) {
		u_visibility_mask_get_noop(type, out_mask);
		return XRT_SUCCESS;
	}

	mask->type = type;
	mask->index_count = vertex_count;
	mask->vertex_count = vertex_count;

	uint32_t *indices = xrt_visibility_mask_get_indices(mask);
	struct xrt_vec2 *vertices = xrt_visibility_mask_get_vertices(mask);

	const struct xrt_fov fov = hmd->base.hmd->distortion.fov[view_index];

	const float tan_left = tanf(fov.angle_left);
	const float tan_right = tanf(fov.angle_right);
	const float tan_down = tanf(fov.angle_down);
	const float tan_up = tanf(fov.angle_up);

	const float tan_width = tan_right - tan_left;
	const float tan_height = tan_up - tan_down;

	const float offset_x = ((tan_right + tan_left) - tan_width) / 2.0f;
	const float offset_y = (-(tan_up + tan_down) - tan_height) / 2.0f;

	const float *src = hmd->mesh.hidden_area[view_index];

	for (uint32_t i = 0; i < vertex_count; i++) {
		indices[i] = i;
		vertices[i].x = (src[i * 2 + 0] * 0.5f + 0.5f) * tan_width + offset_x;
		vertices[i].y = (src[i * 2 + 1] * 0.5f + 0.5f) * tan_height + offset_y;
	}

	*out_mask = mask;

	return XRT_SUCCESS;
}

static void
fill_in_eye_poses(struct starvr_hmd *hmd)
{
	for (uint32_t view = 0; view < 2; view++) {
		const struct starvr_eye_params *p = &hmd->mesh.eye[view];

		struct xrt_quat yaw = XRT_QUAT_IDENTITY;
		struct xrt_quat pitch = XRT_QUAT_IDENTITY;
		struct xrt_quat roll = XRT_QUAT_IDENTITY;

		const struct xrt_vec3 axis_y = {0.0f, 1.0f, 0.0f};
		const struct xrt_vec3 axis_x = {1.0f, 0.0f, 0.0f};
		const struct xrt_vec3 axis_z = {0.0f, 0.0f, 1.0f};

		/*
		 * The mesh's angles are in a left handed frame that this one is
		 * the Z mirror of. Mirroring one axis negates the rotations about
		 * the other two and leaves the one about it alone, hence yaw and
		 * pitch flip and roll does not. The angles are already signed per
		 * eye, so no per view sign goes on top of that.
		 */
		math_quat_from_angle_vector(-p->yaw_rad, &axis_y, &yaw);
		math_quat_from_angle_vector(-p->pitch_rad, &axis_x, &pitch);
		math_quat_from_angle_vector(p->roll_rad, &axis_z, &roll);

		struct xrt_quat orientation = XRT_QUAT_IDENTITY;
		math_quat_rotate(&yaw, &pitch, &orientation);
		math_quat_rotate(&orientation, &roll, &orientation);
		math_quat_normalize(&orientation);

		hmd->eye_poses[view].orientation = orientation;
		hmd->eye_poses[view].position.x = (view == 0 ? -0.5f : 0.5f) * hmd->ipd_m;
		hmd->eye_poses[view].position.y = 0.0f;
		hmd->eye_poses[view].position.z = 0.0f;
	}
}

static void
fill_in_views(struct starvr_hmd *hmd)
{
	struct xrt_hmd_parts *parts = hmd->base.hmd;

	parts->screens[0].w_pixels = STARVR_PANEL_NARROW_WIDTH;
	parts->screens[0].h_pixels = STARVR_PANEL_HEIGHT;
	parts->screens[0].nominal_frame_interval_ns = (uint64_t)(1000000000.0 / (double)hmd->refresh_rate_hz);

	parts->screens[0].strip_count = (uint32_t)ARRAY_SIZE(starvr_strips_narrow);
	memcpy(parts->screens[0].strips, starvr_strips_narrow, sizeof(starvr_strips_narrow));

	parts->view_count = 2;

	float scale = debug_get_float_option_starvr_render_scale();
	if (!(scale > 0.05f) || scale > 4.0f) {
		scale = 0.5f;
	}

	for (uint32_t view = 0; view < 2; view++) {
		const struct starvr_eye_params *p = &hmd->mesh.eye[view];

		uint32_t render_width = (uint32_t)((float)p->render_width * scale);
		uint32_t render_height = (uint32_t)((float)p->render_height * scale);

		render_width &= ~1u;
		render_height &= ~1u;

		parts->views[view].viewport.x_pixels = view == 0 ? 0 : STARVR_PANEL_NARROW_WIDTH / 2;
		parts->views[view].viewport.y_pixels = 0;
		parts->views[view].viewport.w_pixels = STARVR_PANEL_NARROW_WIDTH / 2;
		parts->views[view].viewport.h_pixels = STARVR_PANEL_HEIGHT;

		parts->views[view].display.w_pixels = render_width;
		parts->views[view].display.h_pixels = render_height;

		parts->views[view].rot = u_device_rotation_ident;

		const float half_h = p->fov_h_rad * 0.5f;
		const float half_v = p->fov_v_rad * 0.5f;

		parts->distortion.fov[view].angle_left = -half_h;
		parts->distortion.fov[view].angle_right = half_h;
		parts->distortion.fov[view].angle_up = half_v;
		parts->distortion.fov[view].angle_down = -half_v;
	}

	parts->blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	parts->blend_mode_count = 1;
}

static void
fill_in_distortion(struct starvr_hmd *hmd)
{
	struct xrt_hmd_parts *parts = hmd->base.hmd;

	parts->distortion.models = XRT_DISTORTION_MODEL_MESHUV;
	parts->distortion.preferred = XRT_DISTORTION_MODEL_MESHUV;

	parts->distortion.mesh.vertices = hmd->mesh.vertices;
	parts->distortion.mesh.vertex_count = hmd->mesh.vertex_count;
	parts->distortion.mesh.stride = STARVR_MESH_VERTEX_FLOATS * sizeof(float);
	parts->distortion.mesh.uv_channels_count = 3;

	parts->distortion.mesh.indices = (int *)hmd->mesh.indices;
	parts->distortion.mesh.index_count_total = hmd->mesh.index_count_total;

	for (uint32_t view = 0; view < 2; view++) {
		parts->distortion.mesh.index_counts[view] = hmd->mesh.index_counts[view];
		parts->distortion.mesh.index_offsets[view] = hmd->mesh.index_offsets[view];
	}

	parts->distortion.mesh.triangle_list = true;
	parts->distortion.mesh.kind = XRT_DISTORTION_MESH_KIND_PROJECTIVE;
}

const char *
starvr_hmd_get_tracker_serial(struct xrt_device *xdev)
{
	return starvr_hmd(xdev)->tracker_serial;
}

void
starvr_hmd_set_tracking_device(struct xrt_device *xdev_hmd,
                               struct xrt_device *xdev_tracker,
                               struct xrt_pose *tracker_to_head)
{
	struct starvr_hmd *hmd = starvr_hmd(xdev_hmd);

	hmd->tracker.xdev = xdev_tracker;
	hmd->tracker.tracker_to_head = (struct xrt_pose)XRT_POSE_IDENTITY;
	if (tracker_to_head != NULL) {
		hmd->tracker.tracker_to_head = *tracker_to_head;
	}

	hmd->base.supported.orientation_tracking = xdev_tracker != NULL;
	hmd->base.supported.position_tracking = xdev_tracker != NULL;
}

struct xrt_device *
starvr_hmd_create(struct xrt_prober_device *xpdev)
{
	const enum u_device_alloc_flags flags =
	    (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);

	struct starvr_hmd *hmd = U_DEVICE_ALLOCATE(struct starvr_hmd, flags, 1, 0);
	if (hmd == NULL) {
		return NULL;
	}

	hmd->log_level = debug_get_log_option_starvr_log();

	u_device_populate_function_pointers(&hmd->base, starvr_hmd_get_tracked_pose, starvr_hmd_destroy);

	hmd->base.name = XRT_DEVICE_STARVR_ONE;
	hmd->base.device_type = XRT_DEVICE_TYPE_HMD;
	hmd->base.update_inputs = starvr_hmd_update_inputs;
	hmd->base.get_view_poses = starvr_hmd_get_view_poses;
	hmd->base.get_visibility_mask = starvr_hmd_get_visibility_mask;

	hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

	hmd->base.supported.orientation_tracking = false;
	hmd->base.supported.position_tracking = false;

	hmd->refresh_rate_hz = 90.0f;
	hmd->vsync_to_photons_s = 0.0018f;

	hmd->hid = starvr_hid_open(xpdev);
	if (hmd->hid == NULL) {
		SVR_ERROR(hmd, "Could not talk to the headset");
		goto err;
	}

	read_firmware_info(hmd);
	SVR_INFO(hmd, "StarVR One '%s' firmware '%s'", hmd->serial, hmd->fw_version);

	resolve_resource_path(hmd);

	float ipd_mm = debug_get_float_option_starvr_ipd_mm();
	if (!(ipd_mm > 0.0f)) {
		ipd_mm = STARVR_DEFAULT_IPD_MM;
	}
	if (ipd_mm < STARVR_MIN_IPD_MM) {
		ipd_mm = STARVR_MIN_IPD_MM;
	}
	if (ipd_mm > STARVR_MAX_IPD_MM) {
		ipd_mm = STARVR_MAX_IPD_MM;
	}
	hmd->ipd_m = ipd_mm / 1000.0f;

	if (!load_meshes(hmd)) {
		goto err;
	}

	fill_in_views(hmd);
	fill_in_distortion(hmd);
	fill_in_eye_poses(hmd);

	const float gain = debug_get_float_option_starvr_analog_gain();
	if (gain > 0.0f) {
		apply_analog_gain(hmd, gain);
	} else {
		uint8_t current[2] = {0, 0};
		if (starvr_hid_brightness_read(hmd->hid, current)) {
			hmd->brightness = current[0];
			SVR_INFO(hmd, "Leaving the panel at brightness %u", hmd->brightness);
		}
	}

	snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "StarVR One");
	snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "%s", hmd->serial);

	SVR_INFO(hmd, "Panel %ux%u at %.2f Hz", STARVR_PANEL_NARROW_WIDTH, STARVR_PANEL_HEIGHT,
	         (double)hmd->refresh_rate_hz);
	SVR_INFO(hmd, "Field of view %.1f x %.1f degrees, IPD %.1f mm",
	         (double)(hmd->mesh.eye[0].fov_h_rad * 180.0f / (float)M_PI),
	         (double)(hmd->mesh.eye[0].fov_v_rad * 180.0f / (float)M_PI), (double)(hmd->ipd_m * 1000.0f));
	SVR_INFO(hmd, "Per eye render target %ux%u", hmd->base.hmd->views[0].display.w_pixels,
	         hmd->base.hmd->views[0].display.h_pixels);
	SVR_INFO(hmd, "Panels canted %+.1f and %+.1f degrees, %.1f degrees apart",
	         (double)(-hmd->mesh.eye[0].yaw_rad * 180.0f / (float)M_PI),
	         (double)(-hmd->mesh.eye[1].yaw_rad * 180.0f / (float)M_PI),
	         (double)(fabsf(hmd->mesh.eye[0].yaw_rad - hmd->mesh.eye[1].yaw_rad) * 180.0f / (float)M_PI));

	if (hmd->tracker_serial[0] != '\0') {
		SVR_INFO(hmd, "Paired with lighthouse tracker '%s'", hmd->tracker_serial);
	}

	u_var_add_root(hmd, "StarVR One", true);
	u_var_add_ro_text(hmd, hmd->serial, "Serial");
	u_var_add_ro_text(hmd, hmd->fw_version, "Firmware");
	u_var_add_ro_text(hmd, hmd->tracker_serial, "Paired tracker");
	u_var_add_ro_f32(hmd, &hmd->refresh_rate_hz, "Refresh rate (Hz)");

	return &hmd->base;

err:
	starvr_hmd_destroy(&hmd->base);
	return NULL;
}
