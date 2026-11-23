#[compute]

#version 450

#VERSION_DEFINES

#define BLOCK_SIZE 8

layout(local_size_x = BLOCK_SIZE, local_size_y = BLOCK_SIZE, local_size_z = 1) in;

shared float tmp_data[BLOCK_SIZE * BLOCK_SIZE];

#ifdef READ_TEXTURE

//use for main texture
layout(set = 0, binding = 0) uniform sampler2D source_texture;

#else

//use for intermediate textures
layout(r32f, set = 0, binding = 0) uniform restrict readonly image2D source_luminance;

#endif

layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_luminance;

#if defined(READ_TEXTURE) || defined(WRITE_LUMINANCE)
layout(set = 2, binding = 0) uniform sampler2D prev_luminance;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	float max_luminance;
	float min_luminance;
	float exposure_adjust;
	float block_scale;
	float highlight_limit;
	float adaptation;
	float adaptation_pivot;
	uint final_level;
	float light_limit;
	uint hold;
}
params;

#include "luminance_adaptation_inc.glsl"

void main() {
	uint t = gl_LocalInvocationID.y * BLOCK_SIZE + gl_LocalInvocationID.x;
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);

	if (all(lessThan(pos, params.source_size))) {
#ifdef READ_TEXTURE
		vec3 v = texelFetch(source_texture, pos, 0).rgb;
		float lum = max(v.r, max(v.g, v.b));
		if (params.highlight_limit > 0.0) {
			float prev_avg = texelFetch(prev_luminance, ivec2(0, 0), 0).r;
			if (prev_avg > 0.0) {
				lum = min(lum, luminance_unadapt(prev_avg, params.adaptation, params.adaptation_pivot) * params.highlight_limit);
			}
		}
		tmp_data[t] = lum;
#else
		tmp_data[t] = imageLoad(source_luminance, pos).r;
#endif
	} else {
		tmp_data[t] = 0.0;
	}

	groupMemoryBarrier();
	barrier();

	uint size = (BLOCK_SIZE * BLOCK_SIZE) >> 1;

	do {
		if (t < size) {
			tmp_data[t] += tmp_data[t + size];
		}
		groupMemoryBarrier();
		barrier();

		size >>= 1;
	} while (size >= 1);

	if (t == 0) {
		float avg = tmp_data[0] * params.block_scale;
		if (params.final_level != 0u) {
			avg = luminance_adapt(avg, params.adaptation, params.adaptation_pivot);
		}
		pos /= ivec2(BLOCK_SIZE);
#ifdef WRITE_LUMINANCE
		float prev_lum = texelFetch(prev_luminance, ivec2(0, 0), 0).r; //1 pixel previous exposure
		avg = clamp(prev_lum + (avg - prev_lum) * params.exposure_adjust, params.min_luminance, params.max_luminance);
		if (params.light_limit > 0.0 && prev_lum > 0.0) {
			avg = max(min(avg, prev_lum * params.light_limit), params.min_luminance);
		}
		if (params.hold != 0u && prev_lum > 0.0) {
			avg = prev_lum;
		}
#endif
		imageStore(dest_luminance, pos, vec4(avg));
	}
}
