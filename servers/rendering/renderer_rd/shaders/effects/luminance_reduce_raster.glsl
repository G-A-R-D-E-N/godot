/* clang-format off */
#[vertex]

#version 450

#VERSION_DEFINES

#include "luminance_reduce_raster_inc.glsl"

layout(location = 0) out vec2 uv_interp;
/* clang-format on */

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
	uv_interp = clamp(gl_Position.xy, vec2(0.0, 0.0), vec2(1.0, 1.0)) * 2.0; // saturate(x) * 2.0
}

/* clang-format off */
#[fragment]

#version 450

#VERSION_DEFINES

#include "luminance_reduce_raster_inc.glsl"

layout(location = 0) in vec2 uv_interp;
/* clang-format on */

layout(set = 0, binding = 0) uniform sampler2D source_exposure;

#if defined(FINAL_PASS) || defined(FIRST_PASS)
layout(set = 1, binding = 0) uniform sampler2D prev_luminance;
#endif

layout(location = 0) out highp float luminance;

void main() {
	ivec2 dest_pos = ivec2(gl_FragCoord.xy);
	ivec2 src_pos = dest_pos * 8;
	ivec2 src_end = min(src_pos + ivec2(8), settings.source_size);

#ifdef FIRST_PASS
	highp float value_limit = 3.0e38;
	if (settings.highlight_limit > 0.0) {
		highp float prev_avg = texelFetch(prev_luminance, ivec2(0, 0), 0).r;
		if (prev_avg > 0.0) {
			value_limit = prev_avg * settings.highlight_limit;
		}
	}
#endif

	highp float block_sum = 0.0;
	for (int j = src_pos.y; j < src_end.y; j++) {
		for (int i = src_pos.x; i < src_end.x; i++) {
			highp vec3 source_color = texelFetch(source_exposure, ivec2(i, j), 0).rgb;
#ifdef FIRST_PASS
			block_sum += min(max(source_color.r, max(source_color.g, source_color.b)), value_limit);
#else
			block_sum += source_color.r;
#endif
		}
	}

	luminance = block_sum * settings.block_scale;

#ifdef FINAL_PASS
	luminance = clamp(luminance, settings.min_luminance, settings.max_luminance);

	highp float prev_lum = texelFetch(prev_luminance, ivec2(0, 0), 0).r;
	luminance = prev_lum + (luminance - prev_lum) * clamp(settings.exposure_adjust, 0.0, 1.0);
#endif
}
