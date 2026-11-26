#[compute]

#version 450

#VERSION_DEFINES

#define QUARTER

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "direct_shadows_params_inc.glsl"

#define MAX_ENTRIES 8
#define HALO 2
#define TILE (8 + 2 * HALO)
#define TILE_SLOT_WORDS (MAX_ENTRIES / 2)
#define TILE_FACTOR_WORDS ((MAX_ENTRIES + 2) / 2)
#define MAX_USED 16
#define MAX_MERGED 32
#define TIER_NONE 0u
#define TIER_NEAR 1u
#define TIER_WIDE 2u
#define TIER_EMPTY 3u

layout(set = 0, binding = 5) uniform sampler2D depth_buffer;
layout(set = 0, binding = 6) uniform sampler2D normal_roughness_buffer;
layout(r16, set = 0, binding = 7) uniform restrict readonly image2DArray block_slots;
layout(r16, set = 0, binding = 8) uniform restrict writeonly image2DArray factor_image;
layout(r16, set = 0, binding = 13) uniform restrict writeonly image2DArray slot_image;
layout(rg32ui, set = 0, binding = 17) uniform restrict readonly uimage2D block_guide;
layout(r16, set = 0, binding = 19) uniform restrict readonly image2DArray block_factors;
layout(r8ui, set = 0, binding = 20) uniform restrict writeonly uimage2D tier_image;
layout(r32f, set = 0, binding = 21) uniform restrict writeonly image2D depth_image;
layout(r32ui, set = 0, binding = 22) uniform restrict writeonly uimage2D normal_image;
layout(rg32ui, set = 0, binding = 23) uniform restrict writeonly uimage2D resolved_guide;

layout(set = 0, binding = 12, std430) restrict buffer Counters {
	uint lit_pixels;
	uint rest_pixels;
	uint rays;
	uint past_region_pixels;
	uint most_lamps;
	uint walked_lights;
	uint most_walked;
	uint passed_lights;
	uint history_accepted;
	uint resolve_tier1;
	uint resolve_tier2;
	uint resolve_tier3;
	uint resolve_cut;
}
counters;

shared uvec2 tile_guide[TILE * TILE];
shared uint tile_slots[TILE * TILE * TILE_SLOT_WORDS];
shared uint tile_factors[TILE * TILE * TILE_FACTOR_WORDS];
shared uint tile_any;

uint own_limit = 0u;

uint tile_slot(uint p_texel, uint p_layer) {
	return (tile_slots[p_texel * TILE_SLOT_WORDS + (p_layer >> 1u)] >> ((p_layer & 1u) * 16u)) & 0xFFFFu;
}

float tile_factor(uint p_texel, uint p_layer) {
	return float((tile_factors[p_texel * TILE_FACTOR_WORDS + (p_layer >> 1u)] >> ((p_layer & 1u) * 16u)) & 0xFFFFu) / 65535.0;
}

uint pack_layer(float p_value) {
	return uint(round(clamp(p_value, 0.0, 1.0) * 65535.0));
}

void load_tile(ivec2 p_origin) {
	if (gl_LocalInvocationIndex == 0u) {
		tile_any = 0u;
	}
	barrier();
	uint slot_layers = own_limit + 1u;
	uint factor_layers = own_limit + 3u;
	for (uint k = gl_LocalInvocationIndex; k < uint(TILE * TILE); k += 64u) {
		ivec2 at = p_origin + ivec2(int(k % uint(TILE)), int(k / uint(TILE)));
		tile_guide[k] = uvec2(0u);
		if (any(lessThan(at, ivec2(0))) || any(greaterThanEqual(at, params.size))) {
			continue;
		}
		uvec2 raw = imageLoad(block_guide, at).xy;
		tile_guide[k] = raw;
		if ((raw.x & ~3u) == 0u) {
			continue;
		}
		atomicOr(tile_any, 1u);
		for (uint j = 0u; j < slot_layers; j += 2u) {
			uint low = pack_layer(imageLoad(block_slots, ivec3(at, int(j))).r);
			uint high = j + 1u < slot_layers ? pack_layer(imageLoad(block_slots, ivec3(at, int(j + 1u))).r) : 0u;
			tile_slots[k * TILE_SLOT_WORDS + (j >> 1u)] = low | (high << 16u);
		}
		for (uint j = 0u; j < factor_layers; j += 2u) {
			uint low = pack_layer(imageLoad(block_factors, ivec3(at, int(j))).r);
			uint high = j + 1u < factor_layers ? pack_layer(imageLoad(block_factors, ivec3(at, int(j + 1u))).r) : 0u;
			tile_factors[k * TILE_FACTOR_WORDS + (j >> 1u)] = low | (high << 16u);
		}
	}
	memoryBarrierShared();
	barrier();
}

vec3 to_world(vec3 p_view) {
	return vec3(dot(params.camera_basis[0].xyz, p_view), dot(params.camera_basis[1].xyz, p_view), dot(params.camera_basis[2].xyz, p_view));
}

bool block_coherent(ivec2 p_block) {
	ivec2 first = direct_block_pixel(p_block, 0u);
	float first_raw = texelFetch(depth_buffer, first, 0).r;
	bool first_sky = first_raw <= 0.0;
	vec3 first_position = vec3(0.0);
	vec3 first_normal = vec3(0.0);
	float first_depth = 0.0;
	bool first_in_region = false;
	if (!first_sky) {
		vec4 view_h = params.inv_projection * vec4(((vec2(first) + 0.5) * params.pixel_size) * 2.0 - 1.0, first_raw, 1.0);
		first_position = view_h.xyz / view_h.w;
		first_depth = -first_position.z;
		first_normal = normalize(texelFetch(normal_roughness_buffer, first, 0).xyz * 2.0 - 1.0);
		first_in_region = all(lessThanEqual(abs(to_world(first_position)), params.region_half));
	}
	for (uint q = 1u; q < 4u; q++) {
		ivec2 pixel = direct_block_pixel(p_block, q);
		if (any(greaterThanEqual(pixel, params.full_size))) {
			continue;
		}
		float raw = texelFetch(depth_buffer, pixel, 0).r;
		if ((raw <= 0.0) != first_sky) {
			return false;
		}
		if (!first_sky) {
			vec4 view_h = params.inv_projection * vec4(((vec2(pixel) + 0.5) * params.pixel_size) * 2.0 - 1.0, raw, 1.0);
			vec3 position = view_h.xyz / view_h.w;
			vec3 normal = normalize(texelFetch(normal_roughness_buffer, pixel, 0).xyz * 2.0 - 1.0);
			bool in_region = all(lessThanEqual(abs(to_world(position)), params.region_half));
			if (in_region != first_in_region || !(direct_surface_weight(first_position, first_normal, first_depth, position, normal) >= 0.95)) {
				return false;
			}
		}
	}
	return true;
}

bool broadcast_block = false;

void store_slot(ivec2 p_pixel, int p_layer, float p_value) {
	for (uint q = 0u; q < (broadcast_block ? 4u : 1u); q++) {
		ivec2 pixel = broadcast_block ? direct_block_pixel(p_pixel >> 1, q) : p_pixel;
		if (all(lessThan(pixel, params.full_size))) {
			imageStore(slot_image, ivec3(pixel, p_layer), vec4(p_value));
		}
	}
}

void store_factor(ivec2 p_pixel, int p_layer, float p_value) {
	for (uint q = 0u; q < (broadcast_block ? 4u : 1u); q++) {
		ivec2 pixel = broadcast_block ? direct_block_pixel(p_pixel >> 1, q) : p_pixel;
		if (all(lessThan(pixel, params.full_size))) {
			imageStore(factor_image, ivec3(pixel, p_layer), vec4(p_value));
		}
	}
}

void store_guide(ivec2 p_pixel, bool p_valid, float p_depth, vec3 p_normal) {
	for (uint q = 0u; q < (broadcast_block ? 4u : 1u); q++) {
		ivec2 pixel = broadcast_block ? direct_block_pixel(p_pixel >> 1, q) : p_pixel;
		if (any(greaterThanEqual(pixel, params.full_size))) {
			continue;
		}
		uvec4 guide = uvec4(0u);
		if (p_valid) {
			float depth = p_depth;
			vec3 normal = p_normal;
			if (q != 0u) {
				float raw = texelFetch(depth_buffer, pixel, 0).r;
				vec4 view_h = params.inv_projection * vec4(((vec2(pixel) + 0.5) * params.pixel_size) * 2.0 - 1.0, raw, 1.0);
				depth = -view_h.z / view_h.w;
				normal = normalize(texelFetch(normal_roughness_buffer, pixel, 0).xyz * 2.0 - 1.0);
			}
			guide = uvec4(floatBitsToUint(depth), packSnorm2x16(direct_oct_encode(normal)), 0u, 0u);
		}
		imageStore(resolved_guide, pixel, guide);
	}
}

void write_none(ivec2 p_pixel, uint p_tier, float p_depth, uint p_normal) {
	for (uint i = 0u; i <= own_limit; i++) {
		store_slot(p_pixel, int(i), 0.0);
	}
	for (uint i = 0u; i < own_limit + 3u; i++) {
		store_factor(p_pixel, int(i), 1.0);
	}
	store_guide(p_pixel, false, 0.0, vec3(0.0));
	if (params.count != 0u) {
		imageStore(tier_image, p_pixel, uvec4(p_tier));
		imageStore(depth_image, p_pixel, vec4(p_depth));
		imageStore(normal_image, p_pixel, uvec4(p_normal));
	}
}

void main() {
	own_limit = params.entries - 1u;
	ivec2 block = ivec2(gl_GlobalInvocationID.xy);
	ivec2 tile_origin = ivec2(gl_WorkGroupID.xy) * 8 - HALO;
	load_tile(tile_origin);
	if (any(greaterThanEqual(block, params.size))) {
		return;
	}

	broadcast_block = params.block_resolve != 0u && block_coherent(block);
	for (uint q = 0u; q < 4u; q++) {
		if (broadcast_block && q != 0u) {
			break;
		}
		ivec2 pixel = direct_block_pixel(block, q);
		if (any(greaterThanEqual(pixel, params.full_size))) {
			continue;
		}
		if (tile_any == 0u && params.count == 0u) {
			write_none(pixel, TIER_EMPTY, 0.0, 0u);
			continue;
		}
		vec4 normal_roughness = texelFetch(normal_roughness_buffer, pixel, 0);
		uint packed_normal = packUnorm4x8(normal_roughness);
		float raw_depth = texelFetch(depth_buffer, pixel, 0).r;
		if (raw_depth <= 0.0) {
			write_none(pixel, TIER_NONE, 0.0, packed_normal);
			continue;
		}
		vec4 view_h = params.inv_projection * vec4(((vec2(pixel) + 0.5) * params.pixel_size) * 2.0 - 1.0, raw_depth, 1.0);
		vec3 vertex = view_h.xyz / view_h.w;
		float depth = -vertex.z;
		if (any(greaterThan(abs(to_world(vertex)), params.region_half))) {
			write_none(pixel, TIER_NONE, depth, packed_normal);
			continue;
		}
		vec3 normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
		vec3 position = direct_rebuild_position(pixel, depth);
		vec2 center = vec2(pixel) + 0.5;

		uint used_texel[MAX_USED];
		float used_weight[MAX_USED];
		uint used_count = 0u;
		uint tier = TIER_EMPTY;
		ivec2 bracket = (pixel - 1) >> 1;
		for (uint wide = 0u; wide < 2u && used_count == 0u; wide++) {
			int reach = wide == 0u ? 1 : 2;
			int from = wide == 0u ? 0 : -1;
			for (int dy = from; dy <= reach; dy++) {
				for (int dx = from; dx <= reach; dx++) {
					ivec2 k = bracket + ivec2(dx, dy);
					ivec2 tile_at = k - tile_origin;
					uint texel = uint(tile_at.y * TILE + tile_at.x);
					uvec2 raw = tile_guide[texel];
					if ((raw.x & ~3u) == 0u) {
						continue;
					}
					ivec2 represented = direct_block_pixel(k, raw.x & 3u);
					float other_depth = direct_guide_depth(raw.x);
					float g = direct_surface_weight(position, normal, depth, direct_rebuild_position(represented, other_depth), direct_oct_decode(unpackSnorm2x16(raw.y)));
					if (g < 0.25) {
						continue;
					}
					vec2 gap = vec2(represented) + 0.5 - center;
					float weight;
					if (wide == 0u) {
						weight = max(0.0, 1.0 - 0.5 * abs(gap.x)) * max(0.0, 1.0 - 0.5 * abs(gap.y)) * g;
					} else {
						float d2 = dot(gap, gap);
						weight = d2 <= 9.0 ? g / (1.0 + d2) : 0.0;
					}
					if (weight <= 0.0) {
						continue;
					}
					used_texel[used_count] = texel;
					used_weight[used_count] = weight;
					used_count++;
				}
			}
			if (used_count > 0u) {
				tier = wide == 0u ? TIER_NEAR : TIER_WIDE;
			}
		}

		if (used_count == 0u) {
			write_none(pixel, TIER_EMPTY, depth, packed_normal);
			if (params.count != 0u) {
				atomicAdd(counters.resolve_tier3, 1u);
			}
			continue;
		}

		uint merged_slot[MAX_MERGED];
		float merged_score[MAX_MERGED];
		uint merged_count = 0u;
		bool overflow = false;
		for (uint u = 0u; u < used_count; u++) {
			for (uint r = 0u; r < own_limit; r++) {
				uint slot = tile_slot(used_texel[u], r);
				if (slot == 0u) {
					break;
				}
				float score = used_weight[u] * float(own_limit - r);
				uint found = merged_count;
				for (uint m = 0u; m < merged_count; m++) {
					if (merged_slot[m] == slot) {
						found = m;
						break;
					}
				}
				if (found == merged_count) {
					if (merged_count == uint(MAX_MERGED)) {
						overflow = true;
						continue;
					}
					merged_slot[merged_count] = slot;
					merged_score[merged_count] = 0.0;
					merged_count++;
				}
				merged_score[found] += score;
			}
		}
		uint kept = min(merged_count, own_limit);
		for (uint i = 0u; i < kept; i++) {
			uint best = i;
			for (uint m = i + 1u; m < merged_count; m++) {
				if (merged_score[m] > merged_score[best] || (merged_score[m] == merged_score[best] && merged_slot[m] < merged_slot[best])) {
					best = m;
				}
			}
			uint slot = merged_slot[best];
			float score = merged_score[best];
			merged_slot[best] = merged_slot[i];
			merged_score[best] = merged_score[i];
			merged_slot[i] = slot;
			merged_score[i] = score;
		}

		for (uint i = 0u; i < own_limit; i++) {
			float factor = 1.0;
			uint slot = i < kept ? merged_slot[i] : 0u;
			if (slot != 0u) {
				float sum = 0.0;
				float total = 0.0;
				for (uint u = 0u; u < used_count; u++) {
					for (uint r = 0u; r < own_limit; r++) {
						uint other = tile_slot(used_texel[u], r);
						if (other == 0u) {
							break;
						}
						if (other == slot) {
							sum += used_weight[u] * tile_factor(used_texel[u], r);
							total += used_weight[u];
							break;
						}
					}
				}
				factor = total > 0.0 ? sum / total : 1.0;
			}
			store_slot(pixel, int(i), float(slot) / 65535.0);
			store_factor(pixel, int(i), factor);
		}

		uint rest_key = 0u;
		float rest_weight = 0.0;
		for (uint u = 0u; u < used_count; u++) {
			uint key = tile_slot(used_texel[u], own_limit);
			if (key == 0u) {
				continue;
			}
			float summed = 0.0;
			for (uint v = 0u; v < used_count; v++) {
				if (tile_slot(used_texel[v], own_limit) == key) {
					summed += used_weight[v];
				}
			}
			if (summed > rest_weight || (summed == rest_weight && key < rest_key)) {
				rest_key = key;
				rest_weight = summed;
			}
		}
		vec3 rest_sum = vec3(0.0);
		float rest_total = 0.0;
		for (uint u = 0u; rest_key != 0u && u < used_count; u++) {
			if (tile_slot(used_texel[u], own_limit) != rest_key) {
				continue;
			}
			for (uint c = 0u; c < 3u; c++) {
				rest_sum[c] += used_weight[u] * tile_factor(used_texel[u], own_limit + c);
			}
			rest_total += used_weight[u];
		}
		store_slot(pixel, int(own_limit), float(rest_key) / 65535.0);
		for (uint c = 0u; c < 3u; c++) {
			store_factor(pixel, int(own_limit + c), rest_total > 0.0 ? rest_sum[c] / rest_total : 1.0);
		}

		store_guide(pixel, kept != 0u || rest_key != 0u, depth, normal);
		if (params.count != 0u) {
			imageStore(tier_image, pixel, uvec4(tier));
			imageStore(depth_image, pixel, vec4(depth));
			imageStore(normal_image, pixel, uvec4(packed_normal));
			if (tier == TIER_NEAR) {
				atomicAdd(counters.resolve_tier1, 1u);
			} else {
				atomicAdd(counters.resolve_tier2, 1u);
			}
			if (merged_count > own_limit || overflow) {
				atomicAdd(counters.resolve_cut, 1u);
			}
		}
	}
}
