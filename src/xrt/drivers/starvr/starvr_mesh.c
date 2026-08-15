// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  StarVR One distortion mesh files.
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup drv_starvr
 */

#include "starvr_mesh.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STARVR_FILE_VERTEX_SIZE (44)
#define STARVR_MIN_MESH_VERSION (4)

#define STARVR_MAX_VERTICES (1u << 22)
#define STARVR_MAX_INDICES (1u << 24)

struct file_vertex
{
	float position[2];
	float tangent[3];
	float binormal[3];
	float normal[3];
};

struct mesh_file
{
	FILE *file;
	const char *path;
};

static bool
read_bytes(struct mesh_file *mf, void *dst, size_t size)
{
	return fread(dst, 1, size, mf->file) == size;
}

static bool
read_u32(struct mesh_file *mf, uint32_t *out)
{
	return read_bytes(mf, out, sizeof(*out));
}

static bool
read_u16(struct mesh_file *mf, uint16_t *out)
{
	return read_bytes(mf, out, sizeof(*out));
}

static bool
read_f32(struct mesh_file *mf, float *out)
{
	return read_bytes(mf, out, sizeof(*out));
}

static bool
read_f32_array(struct mesh_file *mf, float *out, size_t count)
{
	return read_bytes(mf, out, sizeof(float) * count);
}

static bool
seek_abs(struct mesh_file *mf, uint32_t offset)
{
	return fseek(mf->file, (long)offset, SEEK_SET) == 0;
}

static bool
seek_rel(struct mesh_file *mf, long offset)
{
	return fseek(mf->file, offset, SEEK_CUR) == 0;
}

struct raw_eye
{
	struct starvr_eye_params params;

	struct file_vertex *vertices;
	uint32_t vertex_count;

	uint32_t *indices;
	uint32_t index_count;

	float *hidden_area;
	uint32_t hidden_area_triangle_count;
};

static void
raw_eye_free(struct raw_eye *raw)
{
	free(raw->vertices);
	free(raw->indices);
	free(raw->hidden_area);
	U_ZERO(raw);
}

static bool
read_entry(struct mesh_file *mf, bool flip_y, bool narrow_panel, struct raw_eye *out_raw)
{
	U_ZERO(out_raw);

	uint32_t narrow_offset = 0;
	if (!read_u32(mf, &narrow_offset)) {
		return false;
	}

	if (narrow_panel) {
		// The narrow variant repeats the entry body, minus the word just consumed.
		if (!seek_abs(mf, narrow_offset + 4)) {
			return false;
		}
	}

	uint32_t index_count = 0;
	uint32_t vertex_count = 0;
	if (!read_u32(mf, &index_count) || !read_u32(mf, &vertex_count)) {
		return false;
	}

	if (index_count == 0 || index_count > STARVR_MAX_INDICES ||
	    vertex_count == 0 || vertex_count > STARVR_MAX_VERTICES ||
	    index_count % 3 != 0) {
		U_LOG_E("StarVR: implausible mesh entry in %s, %u indices %u vertices", mf->path, index_count,
		        vertex_count);
		return false;
	}

	out_raw->indices = U_TYPED_ARRAY_CALLOC(uint32_t, index_count);
	out_raw->vertices = U_TYPED_ARRAY_CALLOC(struct file_vertex, vertex_count);
	if (out_raw->indices == NULL || out_raw->vertices == NULL) {
		goto err;
	}
	out_raw->index_count = index_count;
	out_raw->vertex_count = vertex_count;

	if (!read_bytes(mf, out_raw->indices, sizeof(uint32_t) * index_count)) {
		goto err;
	}
	if (!read_bytes(mf, out_raw->vertices, (size_t)STARVR_FILE_VERTEX_SIZE * vertex_count)) {
		goto err;
	}

	for (uint32_t i = 0; i < index_count; i++) {
		if (out_raw->indices[i] >= vertex_count) {
			U_LOG_E("StarVR: out of range index in %s", mf->path);
			goto err;
		}
	}

	if (flip_y) {
		// Mirroring reverses the winding, put it back.
		for (uint32_t i = 0; i + 2 < index_count; i += 3) {
			const uint32_t tmp = out_raw->indices[i + 1];
			out_raw->indices[i + 1] = out_raw->indices[i + 2];
			out_raw->indices[i + 2] = tmp;
		}
		for (uint32_t i = 0; i < vertex_count; i++) {
			out_raw->vertices[i].position[1] = -out_raw->vertices[i].position[1];
		}
	}

	uint32_t discard = 0;
	if (!read_u32(mf, &discard) || !read_u32(mf, &discard)) {
		goto err;
	}

	struct starvr_eye_params *p = &out_raw->params;
	if (!read_f32(mf, &p->yaw_rad) ||
	    !read_f32(mf, &p->pitch_rad) ||
	    !read_f32(mf, &p->roll_rad) ||
	    !read_f32(mf, &p->fov_h_rad) ||
	    !read_f32(mf, &p->fov_v_rad) ||
	    !read_u32(mf, &p->render_width) || !read_u32(mf, &p->render_height)) {
		goto err;
	}

	if (!read_f32_array(mf, p->plane, 4) ||
	    !read_f32_array(mf, p->origin, 4) ||
	    !read_f32_array(mf, p->u_axis, 4) ||
	    !read_f32_array(mf, p->v_axis, 4)) {
		goto err;
	}

	// An unused mesh sits in front of the hidden area mesh, step over it.
	uint32_t skip_indices = 0;
	uint32_t skip_vertices = 0;
	if (!read_u32(mf, &skip_indices) || !read_u32(mf, &skip_vertices)) {
		goto err;
	}
	if (!seek_rel(mf, (long)(4 * ((int64_t)skip_indices + 2 * (int64_t)skip_vertices)))) {
		goto err;
	}

	uint32_t ha_index_count = 0;
	uint32_t ha_vertex_count = 0;
	if (!read_u32(mf, &ha_index_count) || !read_u32(mf, &ha_vertex_count)) {
		goto err;
	}

	if (ha_index_count > STARVR_MAX_INDICES || ha_vertex_count > STARVR_MAX_VERTICES || ha_index_count % 3 != 0) {
		U_LOG_W("StarVR: implausible hidden area mesh in %s, ignoring it", mf->path);
		return true;
	}

	if (ha_index_count == 0 || ha_vertex_count == 0) {
		return true;
	}

	uint32_t *ha_indices = U_TYPED_ARRAY_CALLOC(uint32_t, ha_index_count);
	float *ha_vertices = U_TYPED_ARRAY_CALLOC(float, 2 * (size_t)ha_vertex_count);
	bool ok = ha_indices != NULL && ha_vertices != NULL &&
	          read_bytes(mf, ha_indices, sizeof(uint32_t) * ha_index_count) &&
	          read_bytes(mf, ha_vertices, sizeof(float) * 2 * (size_t)ha_vertex_count);

	if (ok) {
		out_raw->hidden_area = U_TYPED_ARRAY_CALLOC(float, 2 * (size_t)ha_index_count);
		if (out_raw->hidden_area != NULL) {
			for (uint32_t i = 0; i < ha_index_count; i++) {
				const uint32_t index = ha_indices[i];
				if (index >= ha_vertex_count) {
					free(out_raw->hidden_area);
					out_raw->hidden_area = NULL;
					break;
				}
				out_raw->hidden_area[i * 2 + 0] = ha_vertices[index * 2 + 0];
				out_raw->hidden_area[i * 2 + 1] = -ha_vertices[index * 2 + 1];
			}
		}
		if (out_raw->hidden_area != NULL) {
			out_raw->hidden_area_triangle_count = ha_index_count / 3;
		}
	} else {
		U_LOG_W("StarVR: could not read the hidden area mesh from %s", mf->path);
	}

	free(ha_indices);
	free(ha_vertices);

	return true;

err:
	raw_eye_free(out_raw);
	return false;
}

/*!
 * One ray's intersection with the eye's projection plane as (p, q, w), the eye
 * buffer coordinate being (p/w, q/w). All three are linear in the ray, and the
 * sign is normalised so a w of zero or less is exactly the case where the ray
 * never reaches the plane.
 */
static void
ray_to_projective(const struct starvr_eye_params *p, const float dir[3], bool flip_y, float out_h[3])
{
	// The mesh stores directions in a left handed frame.
	const float d[3] = {dir[0], dir[1], -dir[2]};

	const float w = d[0] * p->plane[0] + d[1] * p->plane[1] + d[2] * p->plane[2];

	const float reach = -p->plane[3];

	const float d_u = d[0] * p->u_axis[0] + d[1] * p->u_axis[1] + d[2] * p->u_axis[2];
	const float d_v = d[0] * p->v_axis[0] + d[1] * p->v_axis[1] + d[2] * p->v_axis[2];

	const float o_u = p->origin[0] * p->u_axis[0] + p->origin[1] * p->u_axis[1] + p->origin[2] * p->u_axis[2];
	const float o_v = p->origin[0] * p->v_axis[0] + p->origin[1] * p->v_axis[1] + p->origin[2] * p->v_axis[2];

	out_h[0] = (reach * d_u - o_u * w) / p->u_axis[3];
	out_h[1] = (reach * d_v - o_v * w) / p->v_axis[3];
	out_h[2] = w;

	if (reach < 0.0f) {
		out_h[0] = -out_h[0];
		out_h[1] = -out_h[1];
		out_h[2] = -out_h[2];
	}

	if (flip_y) {
		out_h[1] = out_h[2] - out_h[1];
	}
}

static bool
build_mesh(struct raw_eye raw[2], bool flip_y, struct starvr_mesh_data *out_data)
{
	const uint32_t vertex_count = raw[0].vertex_count + raw[1].vertex_count;

	float *vertices = U_TYPED_ARRAY_CALLOC(float, STARVR_MESH_VERTEX_FLOATS * (size_t)vertex_count);
	uint32_t *indices = U_TYPED_ARRAY_CALLOC(uint32_t, raw[0].index_count + raw[1].index_count);
	if (vertices == NULL || indices == NULL) {
		free(vertices);
		free(indices);
		return false;
	}

	uint32_t vertex_base = 0;
	uint32_t index_used = 0;

	for (uint32_t view = 0; view < 2; view++) {
		const struct starvr_eye_params *p = &raw[view].params;

		if (p->u_axis[3] == 0.0f || p->v_axis[3] == 0.0f) {
			U_LOG_E("StarVR: view %u has a degenerate projection frame", view);
			free(vertices);
			free(indices);
			return false;
		}

		for (uint32_t i = 0; i < raw[view].vertex_count; i++) {
			const struct file_vertex *src = &raw[view].vertices[i];
			float *dst = &vertices[(vertex_base + i) * STARVR_MESH_VERTEX_FLOATS];

			/*
			 * The file's x runs down the panel and its y across it,
			 * and Vulkan's clip space has y pointing the other way
			 * from the original pipeline's, so both axes negate.
			 */
			dst[0] = -src->position[1];
			dst[1] = -src->position[0];

			const float *dirs[3] = {src->tangent, src->binormal, src->normal};

			for (uint32_t channel = 0; channel < 3; channel++) {
				ray_to_projective(p, dirs[channel], flip_y, &dst[2 + channel * 3]);
			}
		}

		out_data->index_offsets[view] = index_used;

		for (uint32_t i = 0; i + 2 < raw[view].index_count; i += 3) {
			indices[index_used++] = raw[view].indices[i + 0] + vertex_base;
			indices[index_used++] = raw[view].indices[i + 1] + vertex_base;
			indices[index_used++] = raw[view].indices[i + 2] + vertex_base;
		}

		out_data->index_counts[view] = index_used - out_data->index_offsets[view];
		vertex_base += raw[view].vertex_count;
	}

	if (out_data->index_counts[0] == 0 || out_data->index_counts[1] == 0) {
		U_LOG_E("StarVR: distortion mesh came out empty");
		free(vertices);
		free(indices);
		return false;
	}

	out_data->vertices = vertices;
	out_data->vertex_count = vertex_count;
	out_data->indices = indices;
	out_data->index_count_total = index_used;

	return true;
}

bool
starvr_mesh_load(const char *left_path,
                 const char *right_path,
                 float target_ipd_mm,
                 bool flip_y,
                 bool narrow_panel,
                 struct starvr_mesh_data *out_data)
{
	struct mesh_file files[2] = {
	    {NULL, left_path},
	    {NULL, right_path},
	};
	struct raw_eye raw[2] = {0};
	bool success = false;

	U_ZERO(out_data);

	for (uint32_t i = 0; i < 2; i++) {
		files[i].file = fopen(files[i].path, "rb");
		if (files[i].file == NULL) {
			U_LOG_E("StarVR: could not open distortion mesh '%s'", files[i].path);
			goto out;
		}
	}

	float start_mm = 0.0f;
	float step_mm = 0.0f;
	uint16_t entry_count = 0;

	for (uint32_t i = 0; i < 2; i++) {
		uint32_t version = 0;
		float this_start = 0.0f;
		float this_step = 0.0f;
		uint16_t this_count = 0;

		if (!read_u32(&files[i], &version) ||
		    !read_f32(&files[i], &this_start) ||
		    !read_f32(&files[i], &this_step) ||
		    !read_u16(&files[i], &this_count)) {
			U_LOG_E("StarVR: truncated header in '%s'", files[i].path);
			goto out;
		}

		if (version < STARVR_MIN_MESH_VERSION) {
			U_LOG_E("StarVR: '%s' is version %u, need %u or newer", files[i].path, version,
			        STARVR_MIN_MESH_VERSION);
			goto out;
		}

		if (this_count == 0 || this_step <= 0.0f) {
			U_LOG_E("StarVR: '%s' has a nonsensical IPD table", files[i].path);
			goto out;
		}

		if (i == 0) {
			start_mm = this_start;
			step_mm = this_step;
			entry_count = this_count;
		} else if (this_start != start_mm || this_step != step_mm || this_count != entry_count) {
			U_LOG_E("StarVR: the left and right meshes disagree about the IPD table");
			goto out;
		}
	}

	int32_t wanted = (int32_t)lroundf((target_ipd_mm - start_mm) / step_mm);
	if (wanted < 0) {
		wanted = 0;
	}
	if (wanted > (int32_t)entry_count - 1) {
		wanted = (int32_t)entry_count - 1;
	}

	out_data->ipd_mm = start_mm + step_mm * (float)wanted;

	for (uint32_t i = 0; i < 2; i++) {
		for (int32_t entry = 0;; entry++) {
			uint32_t next_offset = 0;
			if (!read_u32(&files[i], &next_offset)) {
				U_LOG_E("StarVR: truncated entry chain in '%s'", files[i].path);
				goto out;
			}

			if (entry == wanted) {
				if (!read_entry(&files[i], flip_y, narrow_panel, &raw[i])) {
					U_LOG_E("StarVR: could not read entry %d of '%s'", entry, files[i].path);
					goto out;
				}
				break;
			}

			if (!seek_abs(&files[i], next_offset)) {
				U_LOG_E("StarVR: bad entry offset in '%s'", files[i].path);
				goto out;
			}
		}
	}

	out_data->eye[0] = raw[0].params;
	out_data->eye[1] = raw[1].params;

	for (uint32_t i = 0; i < 2; i++) {
		out_data->hidden_area[i] = raw[i].hidden_area;
		out_data->hidden_area_triangle_count[i] = raw[i].hidden_area_triangle_count;
		raw[i].hidden_area = NULL;
	}

	if (!build_mesh(raw, flip_y, out_data)) {
		goto out;
	}

	U_LOG_I("StarVR: distortion mesh for %.1f mm IPD, %u vertices, %u triangles", (double)out_data->ipd_mm,
	        out_data->vertex_count, out_data->index_count_total / 3);

	success = true;

out:
	for (uint32_t i = 0; i < 2; i++) {
		raw_eye_free(&raw[i]);
		if (files[i].file != NULL) {
			fclose(files[i].file);
		}
	}

	if (!success) {
		starvr_mesh_free(out_data);
	}

	return success;
}

void
starvr_mesh_free(struct starvr_mesh_data *data)
{
	if (data == NULL) {
		return;
	}

	free(data->vertices);
	free(data->indices);
	free(data->hidden_area[0]);
	free(data->hidden_area[1]);

	U_ZERO(data);
}
