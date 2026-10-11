#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2DArray source_shadows;
layout(r8, set = 0, binding = 1) uniform restrict writeonly image2D dest_shadow;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	uint layer_count;
	uint layer_stride;
}
params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.screen_size))) {
		return;
	}

	float shadow = 1.0;
	for (uint i = 0u; i < params.layer_count; i++) {
		shadow = min(shadow, texelFetch(source_shadows, ivec3(pixel, int(i * params.layer_stride)), 0).r);
	}
	imageStore(dest_shadow, pixel, vec4(shadow));
}
