#[compute]

#version 450

#VERSION_DEFINES

#include "gi_compositor_inc.glsl"

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform restrict image2D ambient_buffer;
layout(rgba16f, set = 0, binding = 1) uniform restrict readonly image2D radiance_buffer;
layout(r16f, set = 0, binding = 2) uniform restrict readonly image2D coverage_buffer;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, imageSize(ambient_buffer)))) {
		return;
	}
	float coverage = imageLoad(coverage_buffer, pos).r;
	if (!(coverage > 0.0)) {
		return;
	}
	imageStore(ambient_buffer, pos, gi_compositor_blend(imageLoad(ambient_buffer, pos), imageLoad(radiance_buffer, pos).rgb, coverage));
}
