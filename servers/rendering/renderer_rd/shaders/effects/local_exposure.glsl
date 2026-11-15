#[compute]

#version 450

#VERSION_DEFINES

#define GRID_DEPTH 64
#define LOG_MIN -20.0
#define LOG_MAX 16.0
#define LUMINANCE_FLOOR 9.5367431640625e-07
#define LUMINANCE_CEILING 65504.0
#define BLUR_RADIUS 4
#define MEDIAN_NONE 1.0e30

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef MODE_DOWNSAMPLE
layout(set = 0, binding = 0) uniform sampler2D source_color;
layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_log;
#endif

#ifdef MODE_BUILD
layout(set = 0, binding = 0) uniform sampler2D source_log;
layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image3D dest_grid;
#endif

#ifdef MODE_BLUR
layout(set = 0, binding = 0) uniform sampler3D source_grid;
layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image3D dest_grid;
#endif

#ifdef MODE_WIDE
layout(set = 0, binding = 0) uniform sampler3D source_grid;
layout(rgba16f, set = 1, binding = 0) uniform restrict writeonly image2D dest_wide;
#endif

#ifdef MODE_TEMPORAL
layout(set = 0, binding = 0) uniform sampler3D source_grid;
layout(set = 0, binding = 1) uniform sampler3D source_history;
layout(rgba32f, set = 1, binding = 0) uniform restrict writeonly image3D dest_grid;
#endif

#ifdef MODE_MEDIAN
layout(set = 0, binding = 0) uniform sampler3D source_grid;
layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_median;

shared float bin_weight[GRID_DEPTH];
shared float bin_sum[GRID_DEPTH];
#endif

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	ivec2 dest_size;
	ivec3 axis;
	float sigma;
}
params;

void main() {
#ifdef MODE_DOWNSAMPLE
	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pos, params.dest_size))) {
		return;
	}
	vec2 pixel = 1.0 / vec2(params.source_size);
	ivec2 origin = pos * 8;
	float sum = 0.0;
	float taps = 0.0;
	for (int j = 0; j < 4; j++) {
		for (int i = 0; i < 4; i++) {
			ivec2 corner = origin + ivec2(2 * i + 1, 2 * j + 1);
			if (any(greaterThan(corner, params.source_size))) {
				continue;
			}
			vec3 rgb = textureLod(source_color, vec2(corner) * pixel, 0.0).rgb;
			float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
			y = y > LUMINANCE_FLOOR ? min(y, LUMINANCE_CEILING) : LUMINANCE_FLOOR;
			sum += log2(y);
			taps += 1.0;
		}
	}
	imageStore(dest_log, pos, vec4(taps > 0.0 ? sum / taps : log2(LUMINANCE_FLOOR)));
#endif

#ifdef MODE_BUILD
	ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(cell, params.dest_size))) {
		return;
	}
	vec2 scale = vec2(params.source_size) / vec2(params.dest_size);
	vec2 center = (vec2(cell) + 0.5) * scale - 0.5;
	ivec2 from = max(ivec2(floor(center - scale)), ivec2(0));
	ivec2 to = min(ivec2(ceil(center + scale)), params.source_size - 1);
	float sum[GRID_DEPTH];
	float weight[GRID_DEPTH];
	for (int z = 0; z < GRID_DEPTH; z++) {
		sum[z] = 0.0;
		weight[z] = 0.0;
	}
	for (int y = from.y; y <= to.y; y++) {
		for (int x = from.x; x <= to.x; x++) {
			vec2 at = (vec2(x, y) + 0.5) / scale - 0.5;
			vec2 tent = max(vec2(0.0), 1.0 - abs(at - vec2(cell)));
			float w = tent.x * tent.y;
			if (w <= 0.0) {
				continue;
			}
			float l = texelFetch(source_log, ivec2(x, y), 0).r;
			float p = clamp((l - LOG_MIN) / (LOG_MAX - LOG_MIN) * float(GRID_DEPTH) - 0.5, 0.0, float(GRID_DEPTH - 1));
			int low = int(floor(p));
			int high = min(low + 1, GRID_DEPTH - 1);
			float f = p - float(low);
			sum[low] += w * (1.0 - f) * l;
			weight[low] += w * (1.0 - f);
			sum[high] += w * f * l;
			weight[high] += w * f;
		}
	}
	for (int z = 0; z < GRID_DEPTH; z++) {
		imageStore(dest_grid, ivec3(cell, z), vec4(sum[z], weight[z], 0.0, 0.0));
	}
#endif

#ifdef MODE_BLUR
	ivec3 cell = ivec3(gl_GlobalInvocationID);
	ivec3 size = ivec3(params.dest_size, GRID_DEPTH);
	if (any(greaterThanEqual(cell, size))) {
		return;
	}
	float sigma = max(params.sigma, 0.05);
	vec2 total = vec2(0.0);
	float norm = 0.0;
	for (int i = -BLUR_RADIUS; i <= BLUR_RADIUS; i++) {
		float w = exp(-float(i * i) / (2.0 * sigma * sigma));
		norm += w;
		ivec3 at = cell + params.axis * i;
		if (any(lessThan(at, ivec3(0))) || any(greaterThanEqual(at, size))) {
			continue;
		}
		total += w * texelFetch(source_grid, at, 0).rg;
	}
	imageStore(dest_grid, cell, vec4(total / norm, 0.0, 0.0));
#endif

#ifdef MODE_WIDE
	ivec2 cell = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(cell, params.dest_size))) {
		return;
	}
	vec2 total = vec2(0.0);
	for (int z = 0; z < GRID_DEPTH; z++) {
		total += texelFetch(source_grid, ivec3(cell, z), 0).rg;
	}
	imageStore(dest_wide, cell, vec4(total.g > 1e-5 ? total.r / total.g : log2(LUMINANCE_FLOOR)));
#endif

#ifdef MODE_TEMPORAL
	ivec3 cell = ivec3(gl_GlobalInvocationID);
	ivec3 size = ivec3(params.dest_size, GRID_DEPTH);
	if (any(greaterThanEqual(cell, size))) {
		return;
	}
	vec2 now = texelFetch(source_grid, cell, 0).rg;
	if (params.sigma < 1.0) {
		now = mix(texelFetch(source_history, cell, 0).rg, now, params.sigma);
	}
	imageStore(dest_grid, cell, vec4(now, 0.0, 0.0));
#endif

#ifdef MODE_MEDIAN
	int z = int(gl_LocalInvocationIndex);
	vec2 total = vec2(0.0);
	for (int y = 0; y < params.dest_size.y; y++) {
		for (int x = 0; x < params.dest_size.x; x++) {
			total += texelFetch(source_grid, ivec3(x, y, z), 0).rg;
		}
	}
	bin_sum[z] = total.r;
	bin_weight[z] = total.g;
	barrier();
	if (z == 0) {
		float all = 0.0;
		for (int i = 0; i < GRID_DEPTH; i++) {
			all += bin_weight[i];
		}
		float median = MEDIAN_NONE;
		if (all > 1e-5) {
			float running = 0.0;
			for (int i = 0; i < GRID_DEPTH; i++) {
				running += bin_weight[i];
				if (running >= 0.5 * all) {
					median = bin_sum[i] / bin_weight[i];
					break;
				}
			}
		}
		imageStore(dest_median, ivec2(0), vec4(median));
	}
#endif
}
