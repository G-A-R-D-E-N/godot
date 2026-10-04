#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D prepared_input_color;
layout(set = 0, binding = 1) uniform sampler2D dilated_motion_vectors;
layout(set = 0, binding = 2) uniform sampler2D dilated_reactive_masks;
layout(rgba8, set = 0, binding = 3) uniform restrict writeonly image2D rejected_buffer;

layout(push_constant, std430) uniform Params {
	ivec2 size;
	uint reset;
	uint pad;
}
params;

void main() {
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.size))) {
		return;
	}

	vec4 prepared = texelFetch(prepared_input_color, pos, 0);
	vec2 reprojected = (vec2(pos) + 0.5) / vec2(params.size) + texelFetch(dilated_motion_vectors, pos, 0).xy;
	float depth_clip = clamp(prepared.w, 0.0, 1.0);
	float reactive = texelFetch(dilated_reactive_masks, pos, 0).x;
	float luma = max(prepared.x, 0.0);

	vec3 color = vec3(0.25 * luma / (1.0 + luma)) + vec3(depth_clip, reactive * (1.0 - depth_clip), 0.0);
	if (params.reset != 0u) {
		color = vec3(1.0);
	} else if (any(lessThan(reprojected, vec2(0.0))) || any(greaterThan(reprojected, vec2(1.0)))) {
		color = vec3(0.0, 0.0, 1.0);
	}
	imageStore(rejected_buffer, pos, vec4(min(color, vec3(1.0)), 1.0));
}
