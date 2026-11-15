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

Guide guide_from(uvec2 raw, ivec2 p_pixel) {
	Guide guide;
	guide.depth = uintBitsToFloat(raw.x);
	guide.normal = direct_oct_decode(unpackSnorm2x16(raw.y));
	vec2 uv = (vec2(p_pixel) + 0.5) * params.pixel_size * 2.0 - 1.0;
	vec2 at_unit = vec2(params.inv_projection[0][0], params.inv_projection[1][1]) * uv;
	guide.position = params.inv_projection[2][3] == 0.0 ? vec3(at_unit + params.inv_projection[3].xy, -guide.depth) : vec3(at_unit * guide.depth, -guide.depth);
	return guide;
}

Guide read_guide(ivec2 p_pixel) {
	return guide_from(imageLoad(guide_image, p_pixel).xy, p_pixel);
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
	uint own_limit = params.entries - 1u;
	uint slots[MAX_ENTRIES];
	float rays[MAX_ENTRIES];
	float hits[MAX_ENTRIES];
	bool wanted = false;
	for (uint i = 0u; i < own_limit; i++) {
		slots[i] = read_slot(pixel, i);
		if (slots[i] != 0u && direct_lamps.data[slots[i]].dynamic == 0u) {
			slots[i] = 0u;
		}
		wanted = wanted || slots[i] != 0u;
		rays[i] = 0.0;
		hits[i] = 0.0;
	}
	if (!wanted) {
		return;
	}
	Guide center = read_guide(pixel);
	for (int dy = -CLAMP_RADIUS; dy <= CLAMP_RADIUS; dy++) {
		for (int dx = -CLAMP_RADIUS; dx <= CLAMP_RADIUS; dx++) {
			ivec2 at = pixel + ivec2(dx, dy);
			if (any(lessThan(at, ivec2(0))) || any(greaterThanEqual(at, params.size))) {
				continue;
			}
			uint ray = imageLoad(ray_image, at).x;
			uint slot = ray >> 16u;
			if (slot == 0u) {
				continue;
			}
			int match = -1;
			for (uint i = 0u; i < own_limit; i++) {
				if (slots[i] == slot) {
					match = int(i);
					break;
				}
			}
			if (match < 0) {
				continue;
			}
			if ((dx != 0 || dy != 0) && geometry_weight(center, read_guide(at)) < 0.5) {
				continue;
			}
			rays[match] += 1.0;
			hits[match] += float(ray & 1u);
		}
	}
	for (uint i = 0u; i < own_limit; i++) {
		if (slots[i] == 0u || rays[i] < params.clamp_min_rays) {
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

#ifdef MODE_FIRST
#define HALO 1
#else
#define HALO 2
#endif
#define TILE (8 + 2 * HALO)
#define TILE_SLOT_WORDS (MAX_ENTRIES / 2)

shared uvec2 tile_guide[TILE * TILE];
shared uint tile_slots[TILE * TILE * TILE_SLOT_WORDS];

uint tile_slot(uint p_texel, uint p_layer) {
	return (tile_slots[p_texel * TILE_SLOT_WORDS + (p_layer >> 1u)] >> ((p_layer & 1u) * 16u)) & 0xFFFFu;
}

void load_tile(ivec2 p_origin, uint p_layers) {
	for (uint k = gl_LocalInvocationIndex; k < uint(TILE * TILE); k += 64u) {
		ivec2 at = p_origin + ivec2(int(k % uint(TILE)), int(k / uint(TILE)));
		if (any(lessThan(at, ivec2(0))) || any(greaterThanEqual(at, params.size))) {
			continue;
		}
		uvec2 raw = imageLoad(guide_image, at).xy;
		tile_guide[k] = raw;
		if (uintBitsToFloat(raw.x) <= 0.0) {
			continue;
		}
		for (uint j = 0u; j < p_layers; j += 2u) {
			uint low = read_slot(at, j);
			uint high = j + 1u < p_layers ? read_slot(at, j + 1u) : 0u;
			tile_slots[k * TILE_SLOT_WORDS + (j >> 1u)] = low | (high << 16u);
		}
	}
	memoryBarrierShared();
	barrier();
}

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
	uint own_limit = params.entries - 1u;
	uint layers = own_limit + 3u;
	ivec2 tile_origin = ivec2(gl_WorkGroupID.xy) * 8 - HALO;
	load_tile(tile_origin, own_limit + 1u);
	if (any(greaterThanEqual(pixel, params.size))) {
		return;
	}
	ivec2 center_at = pixel - tile_origin;
	uint center_texel = uint(center_at.y * TILE + center_at.x);
	Guide center = guide_from(tile_guide[center_texel], pixel);
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
		slots[i] = tile_slot(center_texel, i);
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
	uint rest_key = tile_slot(center_texel, own_limit);
#ifdef MODE_FIRST
	const int step_size = 1;
#else
	const int step_size = 2;
#endif
	const float kernel[3] = float[3](0.25, 0.5, 0.25);
	float ring_weight[MAX_ENTRIES * 3];
	for (uint i = 0u; i < own_limit; i++) {
		if (slots[i] == 0u) {
			continue;
		}
		for (uint r = 0u; r < 3u; r++) {
			float dist2 = float(r) * float(step_size * step_size);
			ring_weight[i * 3u + r] = exp(-dist2 * inv_two_sigma[i]);
		}
	}
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 at = pixel + ivec2(dx, dy) * step_size;
			if (any(lessThan(at, ivec2(0))) || any(greaterThanEqual(at, params.size))) {
				continue;
			}
			ivec2 tile_at = at - tile_origin;
			uint texel = uint(tile_at.y * TILE + tile_at.x);
			float base = kernel[dx + 1] * kernel[dy + 1];
			if (dx != 0 || dy != 0) {
				base *= geometry_weight(center, guide_from(tile_guide[texel], at));
				if (base <= 0.0) {
					continue;
				}
			}
			uint ring = uint(dx * dx + dy * dy);
			for (uint j = 0u; j < own_limit; j++) {
				uint slot = tile_slot(texel, j);
				if (slot == 0u) {
					continue;
				}
				for (uint i = 0u; i < own_limit; i++) {
					if (slots[i] != slot) {
						continue;
					}
					float weight = base * ring_weight[i * 3u + ring];
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
			if (rest_key != 0u && tile_slot(texel, own_limit) == rest_key) {
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
