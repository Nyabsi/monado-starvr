// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  StarVR One distortion mesh files.
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup drv_starvr
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! position.xy plus one projective sample position per colour channel.
#define STARVR_MESH_VERTEX_FLOATS (2 + 3 * 3)

/*!
 * The per eye frame the mesh's rays are projected through. Intersecting a ray
 * with @p plane and expressing the hit in the @p u_axis / @p v_axis basis gives
 * the eye buffer coordinate to sample.
 *
 * @ingroup drv_starvr
 */
struct starvr_eye_params
{
	float yaw_rad, pitch_rad, roll_rad;

	float fov_h_rad, fov_v_rad;

	uint32_t render_width, render_height;

	float plane[4];
	float origin[4];
	float u_axis[4];
	float v_axis[4];
};

/*!
 * One IPD entry of a mesh file pair, in the form the compositor consumes.
 *
 * @ingroup drv_starvr
 */
struct starvr_mesh_data
{
	float ipd_mm;

	struct starvr_eye_params eye[2];

	/*!
	 * Both views, packed as position.xy followed by one sample position per
	 * channel. Left undivided so the rasteriser interpolates the components
	 * and the fragment shader divides, which is what reproduces the
	 * projection per pixel; interpolating the quotient instead is off by
	 * tens of eye buffer pixels across this field of view.
	 */
	float *vertices;
	uint32_t vertex_count;

	uint32_t *indices;
	uint32_t index_counts[2];
	uint32_t index_offsets[2];
	uint32_t index_count_total;

	//! Triangle list of 2D points in normalised device coordinates.
	float *hidden_area[2];
	uint32_t hidden_area_triangle_count[2];
};

bool
starvr_mesh_load(const char *left_path,
                 const char *right_path,
                 float target_ipd_mm,
                 bool flip_y,
                 bool narrow_panel,
                 struct starvr_mesh_data *out_data);

void
starvr_mesh_free(struct starvr_mesh_data *data);

#ifdef __cplusplus
}
#endif
