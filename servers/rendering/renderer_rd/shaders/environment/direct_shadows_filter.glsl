#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "direct_shadows_params_inc.glsl"

#define MAX_ENTRIES 8
#define CLAMP_RADIUS 2
#define NO_EVIDENCE_GAP 0.001

layout(set = 0, binding = 3, std430) restrict readonly buffer DirectLamps {
	DirectLamp data[];
}
direct_lamps;

layout(r16, set = 0, binding = 7) uniform restrict readonly image2DArray slot_image;
layout(rg32ui, set = 0, binding = 17) uniform restrict readonly uimage2D guide_image;

#ifdef MODE_CLAMP
layout(rg16f, set = 0, binding = 9) uniform restrict image2DArray accum_image;
layout(r32ui, set = 0, binding = 18) uniform restrict readonly uimage2D ray_image;
#endif

#ifdef MODE_FIRST
layout(rg16f, set = 0, binding = 9) uniform restrict readonly image2DArray accum_image;
layout(rgba16f, set = 0, binding = 10) uniform restrict readonly image2D rest_image;
layout(r16, set = 0, binding = 8) uniform restrict writeonly image2DArray out_image;
#endif

#ifdef MODE_SECOND
layout(rg16f, set = 0, binding = 9) uniform restrict readonly image2DArray accum_image;
layout(r16, set = 0, binding = 19) uniform restrict readonly image2DArray in_image;
layout(r16, set = 0, binding = 8) uniform restrict writeonly image2DArray out_image;
layout(rgba16f, set = 0, binding = 11) uniform restrict writeonly image2D debug_image;
#endif

uint read_slot(ivec2 p_pixel, uint p_layer) {
	return uint(round(imageLoad(slot_image, ivec3(p_pixel, int(p_layer))).r * 65535.0));
}

struct Guide {
	float depth;
	vec3 normal;
	vec3 position;
};

Guide read_guide(ivec2 p_pixel) {
	uvec2 raw = imageLoad(guide_image, p_pixel).xy;
	Guide guide;
	guide.depth = uintBitsToFloat(raw.x);
	guide.normal = direct_oct_decode(unpackSnorm2x16(raw.y));
	vec2 uv = (vec2(p_pixel) + 0.5) * params.pixel_size * 2.0 - 1.0;
	vec2 at_unit = vec2(params.inv_projection[0][0], params.inv_projection[1][1]) * uv;
	guide.position = params.inv_projection[2][3] == 0.0 ? vec3(at_unit + params.inv_projection[3].xy, -guide.depth) : vec3(at_unit * guide.depth, -guide.depth);
	return guide;
}

float geometry_weight(Guide p_center, Guide p_other) {
	if (p_other.depth <= 0.0) {
		return 0.0;
	}
	float plane = abs(dot(p_center.normal, p_other.position - p_center.position));
	float depth_weight = exp(-plane / max(0.01 * p_center.depth, 1e-4));
	float facing = max(dot(p_center.normal, p_other.normal), 0.0);
	facing *= facing;
	facing *= facing;
	facing *= facing;
	facing *= facing;
	facing *= facing;
	return depth_weight * facing;
}

#ifdef MODE_CLAMP
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size)) || imageLoad(guide_image, pixel).x == 0u) {
		return;
	}
	Guide center = read_guide(pixel);
	uint own_limit = params.entries - 1u;
	uint slots[MAX_ENTRIES];
	float rays[MAX_ENTRIES];
	float hits[MAX_ENTRIES];
	for (uint i = 0u; i < own_limit; i++) {
		slots[i] = read_slot(pixel, i);
		rays[i] = 0.0;
		hits[i] = 0.0;
	}
	for (int dy = -CLAMP_RADIUS; dy <= CLAMP_RADIUS; dy++) {
		for (int dx = -CLAMP_RADIUS; dx <= CLAMP_RADIUS; dx++) {
			ivec2 at = pixel + ivec2(dx, dy);
			if (any(lessThan(at, ivec2(0))) || any(greaterThanEqual(at, params.size))) {
				continue;
			}
			if ((dx != 0 || dy != 0) && geometry_weight(center, read_guide(at)) < 0.5) {
				continue;
			}
			uint ray = imageLoad(ray_image, at).x;
			uint slot = ray >> 16u;
			if (slot == 0u) {
				continue;
			}
			for (uint i = 0u; i < own_limit; i++) {
				if (slots[i] == slot) {
					rays[i] += 1.0;
					hits[i] += float(ray & 1u);
					break;
				}
			}
		}
	}
	for (uint i = 0u; i < own_limit; i++) {
		if (slots[i] == 0u || rays[i] < params.clamp_min_rays || direct_lamps.data[slots[i]].dynamic == 0u) {
			continue;
		}
		vec2 accum = imageLoad(accum_image, ivec3(pixel, int(i))).rg;
		if (accum.y <= 0.0) {
			continue;
		}
		float mean = hits[i] / rays[i];
		float spread = params.clamp_sigmas * sqrt(mean * (1.0 - mean) / rays[i] + 1.0 / (rays[i] * rays[i]));
		float current = accum.x / accum.y;
		float clamped = clamp(current, mean - spread, mean + spread);
		if (clamped != current) {
			imageStore(accum_image, ivec3(pixel, int(i)), vec4(clamped * accum.y, accum.y, 0.0, 0.0));
		}
	}
}
#else

float sampling_noise(vec2 p_accum) {
	float count = max(min(p_accum.y, params.history_cap), 1e-3);
	float mean = p_accum.y > 0.0 ? p_accum.x / p_accum.y : 0.5;
	return mean * (1.0 - mean) / count + 1.0 / (count * count);
}

float range_weight(float p_own, float p_other, float p_noise) {
	float gap = p_own - p_other;
	return exp(-gap * gap * params.range_scale / p_noise);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) {
		return;
	}
	uint own_limit = params.entries - 1u;
	uint layers = own_limit + 3u;
	Guide center = read_guide(pixel);
	if (center.depth <= 0.0) {
		for (uint i = 0u; i < layers; i++) {
			imageStore(out_image, ivec3(pixel, int(i)), vec4(1.0));
		}
		return;
	}
	uint slots[MAX_ENTRIES];
	float inv_two_sigma[MAX_ENTRIES];
	float own_value[MAX_ENTRIES];
	float own_noise[MAX_ENTRIES];
	float sum[MAX_ENTRIES + 2];
	float total[MAX_ENTRIES + 2];
	for (uint i = 0u; i < own_limit; i++) {
		slots[i] = read_slot(pixel, i);
		float sigma = params.kernel_floor;
		if (slots[i] != 0u) {
			float reach = params.inv_projection[2][3] == 0.0 ? 1.0 : center.depth;
			sigma = max(sigma, params.kernel_scale * direct_lamps.data[slots[i]].size / max(reach * params.world_per_pixel, 1e-6));
		}
		inv_two_sigma[i] = 1.0 / (2.0 * sigma * sigma);
		vec2 own_accum = imageLoad(accum_image, ivec3(pixel, int(i))).rg;
		own_noise[i] = sampling_noise(own_accum);
		own_value[i] = own_accum.y > 0.0 ? own_accum.x / own_accum.y : 0.5;
#ifdef MODE_SECOND
		float own_seen = imageLoad(in_image, ivec3(pixel, int(i))).r;
		if (own_seen > 0.0) {
			own_value[i] = (own_seen - NO_EVIDENCE_GAP) / (1.0 - NO_EVIDENCE_GAP);
		}
#endif
	}
	for (uint i = 0u; i < layers; i++) {
		sum[i] = 0.0;
		total[i] = 0.0;
	}
	uint rest_key = read_slot(pixel, own_limit);
#ifdef MODE_FIRST
	const int step_size = 1;
#else
	const int step_size = 2;
#endif
	const float kernel[3] = float[3](0.25, 0.5, 0.25);
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 at = pixel + ivec2(dx, dy) * step_size;
			if (any(lessThan(at, ivec2(0))) || any(greaterThanEqual(at, params.size))) {
				continue;
			}
			float base = kernel[dx + 1] * kernel[dy + 1];
			if (dx != 0 || dy != 0) {
				base *= geometry_weight(center, read_guide(at));
				if (base <= 0.0) {
					continue;
				}
			}
			float dist2 = float(dx * dx + dy * dy) * float(step_size * step_size);
			for (uint j = 0u; j < own_limit; j++) {
				uint slot = read_slot(at, j);
				if (slot == 0u) {
					continue;
				}
				for (uint i = 0u; i < own_limit; i++) {
					if (slots[i] != slot) {
						continue;
					}
					float weight = base * exp(-dist2 * inv_two_sigma[i]);
#ifdef MODE_FIRST
					vec2 accum = imageLoad(accum_image, ivec3(at, int(j))).rg;
					if (accum.y > 0.0) {
						float value = accum.x / accum.y;
						float evidence = weight * min(accum.y, params.history_cap) * range_weight(own_value[i], value, own_noise[i] + sampling_noise(accum));
						sum[i] += evidence * value;
						total[i] += evidence;
					}
#else
					float seen = imageLoad(in_image, ivec3(at, int(j))).r;
					if (seen > 0.0) {
						float value = (seen - NO_EVIDENCE_GAP) / (1.0 - NO_EVIDENCE_GAP);
						float kept = weight * range_weight(own_value[i], value, own_noise[i] + sampling_noise(imageLoad(accum_image, ivec3(at, int(j))).rg));
						sum[i] += kept * value;
						total[i] += kept;
					}
#endif
					break;
				}
			}
			if (rest_key != 0u && read_slot(at, own_limit) == rest_key) {
#ifdef MODE_FIRST
				vec4 rest = imageLoad(rest_image, at);
				if (rest.a > 0.0) {
					float evidence = base * min(rest.a, params.history_cap);
					for (uint c = 0u; c < 3u; c++) {
						sum[own_limit + c] += evidence * clamp(rest[c] / rest.a, 0.0, 1.0);
						total[own_limit + c] += evidence;
					}
				}
#else
				for (uint c = 0u; c < 3u; c++) {
					float seen = imageLoad(in_image, ivec3(at, int(own_limit + c))).r;
					if (seen > 0.0) {
						sum[own_limit + c] += base * (seen - NO_EVIDENCE_GAP) / (1.0 - NO_EVIDENCE_GAP);
						total[own_limit + c] += base;
					}
				}
#endif
			}
		}
	}
	for (uint i = 0u; i < layers; i++) {
		float value = total[i] > 0.0 ? sum[i] / total[i] : 1.0;
#ifdef MODE_FIRST
		value = total[i] > 0.0 ? NO_EVIDENCE_GAP + (1.0 - NO_EVIDENCE_GAP) * value : 0.0;
#endif
#ifdef MODE_SECOND
		if (params.force_one != 0u) {
			value = 1.0;
		}
#endif
		imageStore(out_image, ivec3(pixel, int(i)), vec4(value));
	}
#ifdef MODE_SECOND
	if (params.debug_view == 6u) {
		imageStore(debug_image, pixel, vec4(vec3(total[0] > 0.0 ? sum[0] / total[0] : 1.0), 1.0));
	}
#endif
}
#endif
