#[compute]

#version 450

#VERSION_DEFINES

#define BINS 256
#define MIN_LOG2 -32.0
#define BIN_WIDTH 0.25
#define TENT_STEPS 32u
#define WEIGHT_BASE 8.0
#define CENTER_Y 0.6
#define CENTER_SIGMA 0.3

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) restrict buffer Histogram {
	uint bins[BINS];
}
histogram;

#ifdef ACCUMULATE
layout(set = 1, binding = 0) uniform sampler2D source_texture;
#endif

#ifdef RESOLVE
layout(set = 1, binding = 0) uniform sampler2D prev_luminance;
layout(r32f, set = 2, binding = 0) uniform restrict writeonly image2D dest_luminance;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	int stride;
	float center_weight;
	float clip_low;
	float clip_high;
	float min_luminance;
	float max_luminance;
	float exposure_adjust;
	uint set_immediate;
	float adaptation;
	float adaptation_pivot;
}
params;

#include "luminance_adaptation_inc.glsl"

shared uint local_bins[BINS];

#ifdef RESOLVE
shared float local_sum[BINS];
shared float local_kept[BINS];
#endif

void main() {
	uint t = gl_LocalInvocationIndex;

#ifdef ACCUMULATE
	local_bins[t] = 0u;
	groupMemoryBarrier();
	barrier();

	ivec2 pos = ivec2(gl_GlobalInvocationID.xy) * params.stride;
	if (all(lessThan(pos, params.source_size))) {
		vec3 rgb = texelFetch(source_texture, pos, 0).rgb;
		float y = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
		if (!isnan(y)) {
			float w = float(params.source_size.x);
			float h = float(params.source_size.y);
			vec2 d = (vec2(pos) + 0.5 - vec2(0.5 * w, CENTER_Y * h)) / h;
			float g = exp(-dot(d, d) / (2.0 * CENTER_SIGMA * CENTER_SIGMA));
			uint weight = uint(floor(WEIGHT_BASE * (1.0 + 3.0 * clamp(params.center_weight, 0.0, 1.0) * g) + 0.5));
			float p = 0.0;
			if (isinf(y) && y > 0.0) {
				p = float(BINS - 1);
			} else if (y > 0.0) {
				p = (log2(y) - MIN_LOG2) / BIN_WIDTH - 0.5;
			}
			p = clamp(p, 0.0, float(BINS - 1));
			uint bin = min(uint(floor(p)), uint(BINS - 1));
			uint upper = uint(floor((p - float(bin)) * float(TENT_STEPS) + 0.5));
			atomicAdd(local_bins[bin], weight * (TENT_STEPS - upper));
			if (upper > 0u) {
				atomicAdd(local_bins[min(bin + 1u, uint(BINS - 1))], weight * upper);
			}
		}
	}

	groupMemoryBarrier();
	barrier();

	if (local_bins[t] > 0u) {
		atomicAdd(histogram.bins[t], local_bins[t]);
	}
#endif

#ifdef RESOLVE
	uint count = histogram.bins[t];
	local_bins[t] = count;
	groupMemoryBarrier();
	barrier();

	for (uint offset = 1u; offset < uint(BINS); offset <<= 1u) {
		uint add = t >= offset ? local_bins[t - offset] : 0u;
		groupMemoryBarrier();
		barrier();
		local_bins[t] += add;
		groupMemoryBarrier();
		barrier();
	}

	uint total = local_bins[BINS - 1];
	float lo = clamp(params.clip_low, 0.0, 0.45) * float(total);
	float hi = (1.0 - clamp(params.clip_high, 0.0, 0.45)) * float(total);
	float above = float(local_bins[t]);
	float below = float(local_bins[t] - count);
	float part = max(min(above, hi) - max(below, lo), 0.0);
	local_sum[t] = part * (MIN_LOG2 + (float(t) + 0.5) * BIN_WIDTH);
	local_kept[t] = part;
	groupMemoryBarrier();
	barrier();

	for (uint size = uint(BINS) >> 1u; size > 0u; size >>= 1u) {
		if (t < size) {
			local_sum[t] += local_sum[t + size];
			local_kept[t] += local_kept[t + size];
		}
		groupMemoryBarrier();
		barrier();
	}

	if (t == 0u) {
		float prev = texelFetch(prev_luminance, ivec2(0, 0), 0).r;
		float result = clamp(prev > 0.0 ? prev : params.min_luminance, params.min_luminance, params.max_luminance);
		if (local_kept[0] > 0.0) {
			float target = luminance_adapt(exp2(local_sum[0] / local_kept[0]), params.adaptation, params.adaptation_pivot);
			if (params.set_immediate != 0u || !(prev > 0.0)) {
				result = clamp(target, params.min_luminance, params.max_luminance);
			} else {
				result = clamp(prev + (target - prev) * params.exposure_adjust, params.min_luminance, params.max_luminance);
			}
		}
		imageStore(dest_luminance, ivec2(0, 0), vec4(result));
	}
#endif
}
