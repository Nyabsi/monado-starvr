// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
// Author: Nyabsi <nyabsi@sovellus.cc>

#version 450

layout (binding = 0) uniform sampler2D tex_sampler;

layout (location = 0)  in vec3 in_r;
layout (location = 1)  in vec3 in_g;
layout (location = 2)  in vec3 in_b;
layout (location = 0) out vec4 out_color;


void main()
{
	// A weight of zero or less is a ray that never reaches the eye buffer.
	if (in_r.z <= 0.0 || in_g.z <= 0.0 || in_b.z <= 0.0) {
		out_color = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}

	vec2 r_uv = in_r.xy / in_r.z;
	vec2 g_uv = in_g.xy / in_g.z;
	vec2 b_uv = in_b.xy / in_b.z;

	float r = texture(tex_sampler, r_uv).x;
	float g = texture(tex_sampler, g_uv).y;
	float b = texture(tex_sampler, b_uv).z;

	out_color = vec4(r, g, b, 1.0);
}
