// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
// Author: Nyabsi <nyabsi@sovellus.cc>

#version 450

layout(constant_id = 0) const bool do_timewarp = false;

layout (binding = 1, std140) uniform Config
{
	vec4 vertex_rot;
	vec4 post_transform;
	vec4 pre_transform;
	mat4 transform_scanout_begin;
	mat4 transform_scanout_end;
} ubo;

layout (location = 0)  in vec2 in_position;
layout (location = 1)  in vec3 in_r;
layout (location = 2)  in vec3 in_g;
layout (location = 3)  in vec3 in_b;

layout (location = 0) out vec3 out_r;
layout (location = 1) out vec3 out_g;
layout (location = 2) out vec3 out_b;

out gl_PerVertex
{
	vec4 gl_Position;
};


// Affine in the sample position, so it survives being applied before the divide.
vec3 transform_subimage(vec3 h)
{
	return vec3(fma(h.xy, ubo.post_transform.zw, ubo.post_transform.xy * h.z), h.z);
}

// Not affine, so this one has to divide here and hand the result on with weight one.
vec3 transform_timewarp(vec3 h, float scanout_fraction)
{
	vec4 values = vec4(h.xy / h.z, -1, 1);

	values.xy = fma(values.xy, ubo.pre_transform.zw, ubo.pre_transform.xy);
	values.y = -values.y;

	values = ubo.transform_scanout_begin * values * (1.0 - scanout_fraction) +
	         ubo.transform_scanout_end * values * scanout_fraction;
	values.xy = values.xy * (1.0 / max(values.w, 0.00001));

	values.xy = values.xy * 0.5 + 0.5;
	values.xy = fma(values.xy, ubo.post_transform.zw, ubo.post_transform.xy);

	return vec3(values.xy, 1.0);
}

vec3 transform(vec3 h, float scanout_fraction)
{
	if (!do_timewarp) {
		return transform_subimage(h);
	}

	// Nothing to divide by, the fragment stage still sees the sign and blacks it out.
	if (h.z <= 0.0) {
		return h;
	}

	return transform_timewarp(h, scanout_fraction);
}


void main()
{
	mat2x2 rot = {
		ubo.vertex_rot.xy,
		ubo.vertex_rot.zw,
	};

	vec2 pos = rot * in_position;
	gl_Position = vec4(pos, 0.0f, 1.0f);

	float scanout_fraction = pos.y * 0.5 + 0.5;

	out_r = transform(in_r, scanout_fraction);
	out_g = transform(in_g, scanout_fraction);
	out_b = transform(in_b, scanout_fraction);
}
