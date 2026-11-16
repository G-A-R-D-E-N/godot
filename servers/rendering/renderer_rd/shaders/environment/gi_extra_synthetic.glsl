#[compute]

#version 450

#VERSION_DEFINES

#include "gi_compositor_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform restrict writeonly image2D radiance_buffer;
layout(r16f, set = 0, binding = 1) uniform restrict writeonly image2D coverage_buffer;

layout(push_constant, std430) uniform Params {
	uint pattern;
	float coverage;
	float radiance;
	uint pad;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(radiance_buffer);
	if (any(greaterThanEqual(pos, size))) {
		return;
	}
	float value = gi_compositor_pattern_coverage(params.pattern, params.coverage, uvec2(pos), uvec2(size));
	imageStore(radiance_buffer, pos, vec4(params.radiance, params.radiance, params.radiance, 1.0));
	imageStore(coverage_buffer, pos, vec4(value, 0.0, 0.0, 0.0));
}
