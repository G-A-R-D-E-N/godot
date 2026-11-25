#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../light_data_inc.glsl"
#include "direct_shadows_params_inc.glsl"

#define MAX_ENTRIES 8
#define REFERENCE_RAYS 16u
#define PI 3.14159265359

layout(set = 0, binding = 0, std430) restrict readonly buffer ClusterBuffer {
	uint data[];
}
cluster_buffer;

layout(set = 0, binding = 1, std430) restrict readonly buffer OmniLights {
	LightData data[];
}
omni_lights;

layout(set = 0, binding = 2, std430) restrict readonly buffer SpotLights {
	LightData data[];
}
spot_lights;

layout(set = 0, binding = 3, std430) restrict readonly buffer DirectLamps {
	DirectLamp data[];
}
direct_lamps;

layout(set = 0, binding = 5) uniform sampler2D depth_buffer;
layout(set = 0, binding = 6) uniform sampler2D normal_roughness_buffer;
layout(r16, set = 0, binding = 7) uniform restrict writeonly image2DArray slot_image;
layout(r16, set = 0, binding = 8) uniform restrict writeonly image2DArray factor_image;
#ifdef PRECISE_HISTORY
layout(rg32f, set = 0, binding = 9) uniform restrict writeonly image2DArray accum_image;
layout(rgba32f, set = 0, binding = 10) uniform restrict writeonly image2D rest_image;
layout(rg32f, set = 0, binding = 14) uniform restrict readonly image2DArray history_accum;
layout(rgba32f, set = 0, binding = 15) uniform restrict readonly image2D history_rest;
#else
layout(rg16f, set = 0, binding = 9) uniform restrict writeonly image2DArray accum_image;
layout(rgba16f, set = 0, binding = 10) uniform restrict writeonly image2D rest_image;
layout(rg16f, set = 0, binding = 14) uniform restrict readonly image2DArray history_accum;
layout(rgba16f, set = 0, binding = 15) uniform restrict readonly image2D history_rest;
#endif
layout(rgba16f, set = 0, binding = 11) uniform restrict writeonly image2D debug_image;
layout(r16, set = 0, binding = 13) uniform restrict readonly image2DArray history_slots;
layout(rg32ui, set = 0, binding = 16) uniform restrict readonly uimage2D history_guide;
layout(rg32ui, set = 0, binding = 17) uniform restrict writeonly uimage2D guide_image;
layout(r32ui, set = 0, binding = 18) uniform restrict writeonly uimage2D ray_image;

#define EMITTER_HIT_INSTANCES 4096u

layout(set = 0, binding = 27, std430) restrict buffer EmitterHitHist {
	uint hits[];
}
emitter_hit_hist;

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
	uint emitter_pixels;
	uint emitter_rays;
	uint emitter_blocked;
	uint emitter_backface;
	uint emitter_blocked_front;
	uint emitter_hist_005;
	uint emitter_hist_030;
	uint emitter_hist_200;
	uint emitter_hist_far;
	uint emitter_unblocked_front;
	uint emitter_end_005;
	uint emitter_end_030;
	uint emitter_end_200;
	uint emitter_end_far;
	uint len_u_1;
	uint len_b_1;
	uint len_u_3;
	uint len_b_3;
	uint len_u_10;
	uint len_b_10;
	uint len_u_far;
	uint len_b_far;
	uint blocked_alpha_surface;
	uint blocked_solid_surface;
	uint recv_near_005;
	uint recv_near_030;
	uint recv_near_200;
	uint recv_near_far;
	uint sample_min_x;
	uint sample_min_y;
	uint sample_min_z;
	uint sample_max_x;
	uint sample_max_y;
	uint sample_max_z;
	uint sample_count;
	uint min_ray_len;
	uint blocked_split_unavailable;
}
counters;

layout(set = 0, binding = 19, std430) restrict readonly buffer DirectExcludes {
	uvec2 data[];
}
direct_excludes;

#ifdef EXTERNAL_ENTRIES
layout(r32ui, set = 0, binding = 20) uniform restrict readonly uimage2DArray entry_image;
layout(set = 0, binding = 21, std430) restrict readonly buffer DirectEmitterSamples {
	vec4 data[];
}
direct_emitter_samples;
#endif

#define RAY_SCENE_EXCLUDE
uint exclude_first = 0u;
uint exclude_count = 0u;

bool ray_scene_excluded(uint p_material) {
	for (uint i = 0u; i < exclude_count; i++) {
		uvec2 entry = direct_excludes.data[exclude_first + i];
		if (p_material - entry.x < entry.y) {
			return true;
		}
	}
	return false;
}

#define RAY_SCENE_SET 1
#include "ray_scene_inc.glsl"

struct Entry {
	uint slot;
	float weight;
	vec3 position;
	vec3 target;
};

Entry own[MAX_ENTRIES];
uint own_count = 0u;
uint own_limit = 0u;
uint rest_layer = 0u;

#ifdef EXTERNAL_ENTRIES
uint head_slots[MAX_ENTRIES];
uint tail_count = 0u;
uint tail_slot[MAX_ENTRIES];
float tail_share[MAX_ENTRIES];
vec3 tail_scale[MAX_ENTRIES];
vec3 tail_position[MAX_ENTRIES];
bool tail_seen[MAX_ENTRIES];
float tail_hits[MAX_ENTRIES];
float tail_rays[MAX_ENTRIES];
uint emitter_tail_factor = 0u;
bool emitter_tail = false;
vec4 emitter_geometry = vec4(0.0);
vec4 emitter_color = vec4(0.0);
#endif

float rest_total = 0.0;
vec3 rest_target = vec3(0.0);
uint rest_key = 0u;
uint rest_count = 0u;
bool rest_reset = false;
bool rest_dynamic = false;
uint rest_slot[REFERENCE_RAYS];
float rest_weight[REFERENCE_RAYS];
vec3 rest_position[REFERENCE_RAYS];
vec3 rest_pick_target[REFERENCE_RAYS];
uint rest_seen = 0u;
uint rest_picks = 1u;

uint rng_state = 0u;
uint walked = 0u;

float random_value() {
	rng_state = ray_scene_hash(rng_state);
	return float(rng_state >> 8u) / 16777216.0;
}

vec3 to_world(vec3 p_view) {
	return vec3(dot(params.camera_basis[0].xyz, p_view), dot(params.camera_basis[1].xyz, p_view), dot(params.camera_basis[2].xyz, p_view));
}

float luminance(vec3 p_color) {
	return dot(p_color, luminance_weights);
}

float lamp_attenuation(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(p_distance, 0.0001), -p_decay);
}

float surface_response(vec3 p_to_light, vec3 p_normal, vec3 p_view, float p_roughness, float p_specular) {
	float n_dot_l = dot(p_normal, p_to_light);
	if (n_dot_l <= 0.0) {
		return 0.0;
	}
	vec3 h = normalize(p_to_light + p_view);
	float n_dot_h = max(dot(p_normal, h), 0.0);
	float n_dot_v = max(dot(p_normal, p_view), 1e-3);
	float v_dot_h = clamp(dot(p_view, h), 0.0, 1.0);
	float alpha = max(p_roughness * p_roughness, 1e-3);
	float alpha2 = alpha * alpha;
	float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
	d = alpha2 / (PI * d * d);
	float k = alpha * 0.5;
	float g = 0.25 / ((n_dot_l * (1.0 - k) + k) * (n_dot_v * (1.0 - k) + k));
	float f = 0.04 + 0.96 * pow(1.0 - v_dot_h, 5.0);
	return n_dot_l * (1.0 / PI + d * g * f * p_specular);
}

vec3 rest_sample(uint p_pick, float p_visible) {
	return p_visible * (rest_pick_target[p_pick] / max(rest_target, vec3(1e-30))) * (rest_total / rest_weight[p_pick]);
}

void count_rest(uint p_slot, float p_weight, vec3 p_target) {
	rest_total += p_weight;
	rest_target += p_target;
	rest_key += ray_scene_hash(p_slot);
	rest_count++;
	rest_reset = rest_reset || direct_lamps.data[p_slot].reset_frame > params.last_frame;
	rest_dynamic = rest_dynamic || direct_lamps.data[p_slot].dynamic != 0u;
}

void add_rest(uint p_slot, float p_weight, vec3 p_position, vec3 p_target) {
	count_rest(p_slot, p_weight, p_target);
	for (uint i = 0u; i < rest_picks; i++) {
		if (random_value() * rest_total <= p_weight) {
			rest_slot[i] = p_slot;
			rest_weight[i] = p_weight;
			rest_position[i] = p_position;
			rest_pick_target[i] = p_target;
		}
	}
}

void add_lamp(uint p_slot, float p_weight, vec3 p_position, vec3 p_target) {
	uint at = own_count;
	while (at > 0u && own[at - 1u].weight < p_weight) {
		at--;
	}
	if (at >= own_limit) {
		add_rest(p_slot, p_weight, p_position, p_target);
		return;
	}
	if (own_count == own_limit) {
		Entry out_entry = own[own_count - 1u];
		add_rest(out_entry.slot, out_entry.weight, out_entry.position, out_entry.target);
		own_count--;
	}
	for (uint i = own_count; i > at; i--) {
		own[i] = own[i - 1u];
	}
	own[at].slot = p_slot;
	own[at].weight = p_weight;
	own[at].position = p_position;
	own[at].target = p_target;
	own_count++;
}

#ifdef EXTERNAL_ENTRIES
void add_external(uint p_slot, float p_weight, vec3 p_position, vec3 p_target) {
	for (uint k = 0u; k < tail_count; k++) {
		if (tail_slot[k] == p_slot) {
			tail_position[k] = p_position;
			tail_seen[k] = true;
		}
	}
	if (p_weight <= 0.0) {
		return;
	}
	for (uint i = 0u; i < own_limit; i++) {
		if (head_slots[i] == p_slot) {
			add_lamp(p_slot, p_weight, p_position, p_target);
			return;
		}
	}
	count_rest(p_slot, p_weight, p_target);
}

void read_entries(ivec2 p_pixel) {
	for (uint i = 0u; i < own_limit; i++) {
		head_slots[i] = imageLoad(entry_image, ivec3(p_pixel, int(i))).r & 0xFFFFu;
	}
	tail_count = rest_layer - own_limit;
	emitter_tail = params.pad0 != 0u && tail_count > 0u;
	if (emitter_tail) {
		atomicAdd(counters.emitter_pixels, 1u);
		tail_count--;
		emitter_tail_factor = params.entries + 2u;
		uint sample_index = uint(p_pixel.y) * uint(params.full_size[0]) + uint(p_pixel.x);
		emitter_geometry = direct_emitter_samples.data[sample_index * 2u];
		emitter_color = direct_emitter_samples.data[sample_index * 2u + 1u];
	}
	for (uint k = 0u; k < tail_count; k++) {
		uint base = own_limit + 3u * k;
		uint packed = imageLoad(entry_image, ivec3(p_pixel, int(base))).r;
		tail_slot[k] = packed & 0xFFFFu;
		tail_share[k] = uintBitsToFloat(imageLoad(entry_image, ivec3(p_pixel, int(base + 1u))).r);
		tail_scale[k] = vec3(unpackHalf2x16(imageLoad(entry_image, ivec3(p_pixel, int(base + 2u))).r), unpackHalf2x16(packed >> 16u).x);
		tail_position[k] = vec3(0.0);
		tail_seen[k] = false;
		tail_hits[k] = 0.0;
		tail_rays[k] = 0.0;
	}
}
#endif

void consider(LightData p_light, bool p_spot, vec3 p_vertex, vec3 p_normal, vec3 p_view, float p_roughness) {
	if ((p_light.flags & 1u) == 0u) {
		return;
	}
	vec3 rel = p_light.position - p_vertex;
	if (dot(rel, rel) * p_light.inv_radius * p_light.inv_radius >= 1.001 && p_light.attenuation >= 0.0 && p_light.attenuation <= 9.0) {
		return;
	}
	float distance = length(rel);
	float attenuation = lamp_attenuation(distance, p_light.inv_radius, p_light.attenuation);
	if (attenuation <= 0.0) {
		return;
	}
	vec3 to_light = rel / max(distance, 1e-5);
	if (p_spot) {
		float scos = max(dot(-to_light, p_light.direction), p_light.cone_angle);
		float rim = max(1e-4, (1.0 - scos) / (1.0 - p_light.cone_angle));
		attenuation *= 1.0 - pow(rim, p_light.cone_attenuation);
	}
	float response = surface_response(to_light, p_normal, p_view, p_roughness, p_light.specular_amount);
	vec3 target = p_light.color * attenuation * response;
	float weight = luminance(target);
#ifdef EXTERNAL_ENTRIES
	add_external((p_light.flags >> 1) & 0xFFFFu, max(weight, 0.0), p_light.position, target);
#else
	if (weight <= 0.0) {
		return;
	}
	add_lamp((p_light.flags >> 1) & 0xFFFFu, weight, p_light.position, target);
#endif
}

void walk_cluster(uint p_cluster_offset, uint p_cluster_z, bool p_spot, vec3 p_vertex, vec3 p_normal, vec3 p_view, float p_roughness) {
	uint header = cluster_buffer.data[p_cluster_offset + params.max_cluster_element_count_div_32 + p_cluster_z];
	uint item_min = header & 0xFFFFu;
	uint item_max = header >> 16;
	uint item_from = item_min >> 5;
	uint item_to = item_max == 0u ? 0u : ((item_max - 1u) >> 5) + 1u;
	for (uint i = item_from; i < item_to; i++) {
		uint mask = cluster_buffer.data[p_cluster_offset + i];
		uint begin = max(item_min, i * 32u) - i * 32u;
		uint end = min(item_max, (i + 1u) * 32u) - i * 32u;
		mask &= (end >= 32u ? 0xFFFFFFFFu : ((1u << end) - 1u)) & ~((1u << begin) - 1u);
		while (mask != 0u) {
			uint bit = findLSB(mask);
			mask &= ~(1u << bit);
			uint index = 32u * i + bit;
			walked++;
			consider(p_spot ? spot_lights.data[index] : omni_lights.data[index], p_spot, p_vertex, p_normal, p_view, p_roughness);
		}
	}
}

bool lamp_ray(vec3 p_vertex, vec3 p_anchored, vec3 p_normal, vec3 p_lamp, uint p_slot, float p_u, float p_v, uint p_seed) {
	DirectLamp lamp = direct_lamps.data[p_slot];
	vec3 rel = p_lamp - p_vertex;
	float distance = length(rel);
	vec3 axis = rel / max(distance, 1e-5);
	float radius = max(lamp.size, 0.0);
	float clear = max(lamp.clearance, radius);
	if (clear > 0.0 && distance <= clear) {
		return true;
	}
	vec3 direction = axis;
	float reach = distance - clear;
	if (radius > 0.0) {
		float sin_max = radius / distance;
		float cos_max = sqrt(max(1.0 - sin_max * sin_max, 0.0));
		float cos_theta = mix(cos_max, 1.0, p_u);
		float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
		float phi = 2.0 * PI * p_v;
		vec3 side = abs(axis.y) < 0.99 ? normalize(cross(axis, vec3(0.0, 1.0, 0.0))) : normalize(cross(axis, vec3(1.0, 0.0, 0.0)));
		vec3 up = cross(axis, side);
		direction = normalize(side * cos(phi) * sin_theta + up * sin(phi) * sin_theta + axis * cos_theta);
		reach = distance * cos_theta - sqrt(max(clear * clear - distance * distance * sin_theta * sin_theta, 0.0));
	}
	exclude_first = lamp.exclude >> 3u;
	exclude_count = lamp.exclude & 7u;
	return ray_scene_unshadowed(p_anchored, to_world(p_normal), params.bias, to_world(direction), reach, lamp.caster_mask, p_seed);
}

#ifdef EXTERNAL_ENTRIES
float tail_visible(ivec2 p_pixel, uint p_k, vec3 p_vertex, vec3 p_anchored, vec3 p_normal) {
	float u = random_value();
	float v = random_value();
	if (params.tail_visibility == 1u) {
		return 1.0;
	}
	if (params.tail_visibility == 2u) {
		uint slot = tail_slot[p_k];
		float rho = 0.2 + 0.7 * float((slot * 40503u) & 0xFFFFu) / 65536.0;
		uint h = ray_scene_hash(ray_scene_hash(uint(p_pixel.y) * 65537u + uint(p_pixel.x)) ^ ray_scene_hash(params.frame ^ 0x5bd1e995u) ^ ray_scene_hash(slot + 0x27d4eb2fu));
		return float(h >> 8u) * (1.0 / 16777216.0) < rho ? 1.0 : 0.0;
	}
	return lamp_ray(p_vertex, p_anchored, p_normal, tail_position[p_k], tail_slot[p_k], u, v, rng_state) ? 1.0 : 0.0;
}
#endif

vec3 slot_color(uint p_slot) {
	uint h = ray_scene_hash(p_slot + 1u);
	return p_slot == 0u ? vec3(0.0) : vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
}

bool find_history(ivec2 p_pixel, vec3 p_vertex, vec3 p_normal, vec3 p_view, out ivec2 r_pixel) {
	r_pixel = p_pixel;
	if (params.history_valid == 0u) {
		return false;
	}
	if (params.reference != 0u) {
		return true;
	}
	vec4 prev_view = params.prev_view_from_view * vec4(p_vertex, 1.0);
	vec4 prev_clip = params.prev_projection * prev_view;
	if (prev_clip.w <= 0.0) {
		return false;
	}
	r_pixel = ivec2(floor((prev_clip.xy / prev_clip.w * 0.5 + 0.5) * vec2(params.size)));
	if (any(lessThan(r_pixel, ivec2(0))) || any(greaterThanEqual(r_pixel, params.size))) {
		return false;
	}
	uvec2 guide = imageLoad(history_guide, r_pixel).xy;
	if (guide.x == 0u) {
		return false;
	}
	float expected = -prev_view.z;
	float tolerance = expected * (0.01 + 0.02 / max(dot(p_normal, p_view), 0.05));
	if (abs(uintBitsToFloat(guide.x) - expected) > tolerance) {
		return false;
	}
	return dot(mat3(params.prev_view_from_view) * p_normal, direct_oct_decode(unpackSnorm2x16(guide.y))) >= 0.9;
}

bool cold_lamp(uint p_slot) {
	return params.cold_frames != 0u && params.frame - direct_lamps.data[p_slot].reset_frame < params.cold_frames;
}

#ifdef TRACE_QUARTER
#define QUARTER_CANDIDATES 9

ivec2 candidate_block[QUARTER_CANDIDATES];
float candidate_weight[QUARTER_CANDIDATES];
uint candidate_count = 0u;
float candidate_total = 0.0;

bool pick_representative(ivec2 p_block, out ivec2 r_pixel, out uint r_phase) {
	float depth_of[4];
	vec3 normal_of[4];
	vec3 position_of[4];
	int nearest = -1;
	for (uint k = 0u; k < 4u; k++) {
		depth_of[k] = 0.0;
		normal_of[k] = vec3(0.0);
		position_of[k] = vec3(0.0);
		ivec2 at = direct_block_pixel(p_block, k);
		if (any(greaterThanEqual(at, params.full_size))) {
			continue;
		}
		float raw = texelFetch(depth_buffer, at, 0).r;
		if (raw <= 0.0) {
			continue;
		}
		vec4 view_h = params.inv_projection * vec4(((vec2(at) + 0.5) * params.pixel_size) * 2.0 - 1.0, raw, 1.0);
		float linear_depth = -(view_h.z / view_h.w);
		if (linear_depth <= 0.0) {
			continue;
		}
		depth_of[k] = linear_depth;
		normal_of[k] = normalize(texelFetch(normal_roughness_buffer, at, 0).xyz * 2.0 - 1.0);
		position_of[k] = direct_rebuild_position(at, linear_depth);
		if (nearest < 0 || linear_depth < depth_of[nearest]) {
			nearest = int(k);
		}
	}
	if (nearest < 0) {
		return false;
	}
	bool on_surface[4];
	bool smooth_block = true;
	for (uint k = 0u; k < 4u; k++) {
		on_surface[k] = depth_of[k] > 0.0 && direct_surface_weight(position_of[nearest], normal_of[nearest], depth_of[nearest], position_of[k], normal_of[k]) >= 0.25;
		smooth_block = smooth_block && (depth_of[k] <= 0.0 || on_surface[k]);
	}
	bool want_surface = ((p_block.x + p_block.y + (p_block.y >> 1)) & 1) == 0;
	r_phase = 4u;
	for (uint k = 0u; k < 4u && r_phase == 4u; k++) {
		if (depth_of[k] <= 0.0) {
			continue;
		}
		if (smooth_block || on_surface[k] == want_surface) {
			r_phase = k;
		}
	}
	if (r_phase == 4u) {
		r_phase = uint(nearest);
	}
	r_pixel = direct_block_pixel(p_block, r_phase);
	return true;
}

bool candidate_passes(uvec2 p_guide, float p_expected, float p_tolerance, vec3 p_prev_normal) {
	if ((p_guide.x & ~3u) == 0u) {
		return false;
	}
	if (abs(direct_guide_depth(p_guide.x) - p_expected) > p_tolerance) {
		return false;
	}
	return dot(p_prev_normal, direct_oct_decode(unpackSnorm2x16(p_guide.y))) >= 0.9;
}

void gather_history(ivec2 p_block, ivec2 p_pixel, vec3 p_vertex, vec3 p_normal, vec3 p_view) {
	candidate_count = 0u;
	candidate_total = 0.0;
	if (params.history_valid == 0u) {
		return;
	}
	vec4 prev_view = params.prev_view_from_view * vec4(p_vertex, 1.0);
	vec4 prev_clip = params.prev_projection * prev_view;
	if (prev_clip.w <= 0.0) {
		return;
	}
	vec2 x = (prev_clip.xy / prev_clip.w * 0.5 + 0.5) * vec2(params.full_size) - params.jitter_delta;
	if (!all(greaterThanEqual(x, vec2(-4.0))) || !all(lessThanEqual(x, vec2(params.full_size) + 4.0))) {
		return;
	}
	float expected = -prev_view.z;
	float tolerance = expected * (0.01 + 0.02 / max(dot(p_normal, p_view), 0.05));
	vec3 prev_normal = mat3(params.prev_view_from_view) * p_normal;
	if (all(lessThanEqual(abs(x - (vec2(p_pixel) + 0.5)), vec2(0.01))) && candidate_passes(imageLoad(history_guide, p_block).xy, expected, tolerance, prev_normal)) {
		candidate_block[0] = p_block;
		candidate_weight[0] = 1.0;
		candidate_count = 1u;
		candidate_total = 1.0;
		return;
	}
	ivec2 k_min = ivec2(floor((x - 3.5) * 0.5)) + 1;
	ivec2 k_max = ivec2(ceil((x + 1.5) * 0.5)) - 1;
	for (int ky = k_min.y; ky <= k_max.y; ky++) {
		for (int kx = k_min.x; kx <= k_max.x; kx++) {
			ivec2 k = ivec2(kx, ky);
			if (candidate_count >= uint(QUARTER_CANDIDATES) || any(lessThan(k, ivec2(0))) || any(greaterThanEqual(k, params.size))) {
				continue;
			}
			uvec2 guide = imageLoad(history_guide, k).xy;
			if ((guide.x & ~3u) == 0u) {
				continue;
			}
			vec2 gap = abs(vec2(direct_block_pixel(k, guide.x & 3u)) + 0.5 - x);
			float weight = max(0.0, 1.0 - 0.5 * gap.x) * max(0.0, 1.0 - 0.5 * gap.y);
			if (weight <= 0.0 || !candidate_passes(guide, expected, tolerance, prev_normal)) {
				continue;
			}
			candidate_block[candidate_count] = k;
			candidate_weight[candidate_count] = weight;
			candidate_total += weight;
			candidate_count++;
		}
	}
}
#endif

float history_limit(bool p_dynamic) {
	if (params.history_cap <= 0.0 || params.reference != 0u) {
		return 1e30;
	}
	return p_dynamic ? min(params.history_cap, params.dynamic_cap) : params.history_cap;
}

void write_empty(ivec2 p_pixel, uint p_factor_layers) {
	for (uint i = 0u; i < params.entries; i++) {
		imageStore(slot_image, ivec3(p_pixel, int(i)), vec4(0.0));
	}
	for (int i = 0; i < imageSize(accum_image).z; i++) {
		imageStore(accum_image, ivec3(p_pixel, i), vec4(0.0));
	}
	for (uint i = 0u; i < p_factor_layers; i++) {
		imageStore(factor_image, ivec3(p_pixel, int(i)), vec4(1.0));
	}
	imageStore(rest_image, p_pixel, vec4(0.0));
	imageStore(guide_image, p_pixel, uvec4(0u));
	imageStore(ray_image, p_pixel, uvec4(0u));
	if (params.debug_view != 0u) {
		imageStore(debug_image, p_pixel, vec4(0.0, 0.0, 0.0, 1.0));
	}
}

void main() {
	ivec2 at = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(at, params.size))) {
		return;
	}
	own_limit = params.head_entries;
	rest_layer = params.entries - 1u;
	uint factor_layers = rest_layer + 4u;

#ifdef TRACE_QUARTER
	ivec2 pixel;
	uint phase;
	if (!pick_representative(at, pixel, phase)) {
		write_empty(at, factor_layers);
		return;
	}
#else
	ivec2 pixel = at;
#endif
	float depth = texelFetch(depth_buffer, pixel, 0).r;
	if (depth <= 0.0) {
		write_empty(at, factor_layers);
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) * params.pixel_size;
	vec4 view_h = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 vertex = view_h.xyz / view_h.w;
	vec3 relative = to_world(vertex);
	if (any(greaterThan(abs(relative), params.region_half))) {
		write_empty(at, factor_layers);
		if (params.count != 0u) {
			atomicAdd(counters.past_region_pixels, 1u);
		}
		return;
	}

	vec4 normal_roughness = texelFetch(normal_roughness_buffer, pixel, 0);
	vec3 normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
	float roughness = normal_roughness.w;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	roughness /= 127.0 / 255.0;
	vec3 view = -normalize(vertex);

	rng_state = ray_scene_hash(uint(pixel.y) * 65537u + uint(pixel.x)) ^ ray_scene_hash(params.frame);
	rest_picks = params.reference != 0u ? REFERENCE_RAYS : 1u;
#ifdef EXTERNAL_ENTRIES
	read_entries(pixel);
#endif

	uvec2 cluster_pos = uvec2(pixel) >> params.cluster_shift;
	uint cluster_offset = (params.cluster_width * cluster_pos.y + cluster_pos.x) * (params.max_cluster_element_count_div_32 + 32u);
	uint cluster_z = uint(clamp((-vertex.z / params.z_far) * 32.0, 0.0, 31.0));
	walk_cluster(cluster_offset, cluster_z, false, vertex, normal, view, roughness);
	walk_cluster(cluster_offset + params.cluster_type_size, cluster_z, true, vertex, normal, view, roughness);
	if (params.count != 0u) {
		atomicAdd(counters.walked_lights, walked);
		atomicMax(counters.most_walked, walked);
	}

#ifdef EXTERNAL_ENTRIES
	bool tail_lit = false;
	for (uint k = 0u; k < tail_count; k++) {
		tail_lit = tail_lit || tail_seen[k];
	}
	bool emitter_lit = emitter_tail && emitter_geometry.w > 0.0;
	if (own_count + rest_count == 0u && !tail_lit && !emitter_lit) {
#else
	if (own_count + rest_count == 0u) {
#endif
		write_empty(at, factor_layers);
		return;
	}
	if (params.count != 0u) {
		atomicAdd(counters.lit_pixels, 1u);
		atomicMax(counters.most_lamps, own_count + rest_count);
		atomicAdd(counters.passed_lights, own_count + rest_count);
	}

	vec2 accum[MAX_ENTRIES];
	vec4 rest_accum = vec4(0.0);
	bool has_rest = rest_count > 0u;
	uint rest_key_value = max(rest_key & 0xFFFFu, 1u);
#ifdef TRACE_QUARTER
	gather_history(at, pixel, vertex, normal, view);
	bool has_history = candidate_count > 0u;
	imageStore(guide_image, at, uvec4((floatBitsToUint(-vertex.z) & ~3u) | phase, packSnorm2x16(direct_oct_encode(normal)), 0u, 0u));
	for (uint i = 0u; i < own_count; i++) {
		accum[i] = vec2(0.0);
		if (direct_lamps.data[own[i].slot].reset_frame > params.last_frame || cold_lamp(own[i].slot)) {
			continue;
		}
		for (uint c = 0u; c < candidate_count; c++) {
			for (uint j = 0u; j < own_limit; j++) {
				if (uint(round(imageLoad(history_slots, ivec3(candidate_block[c], int(j))).r * 65535.0)) == own[i].slot) {
					accum[i] += candidate_weight[c] * imageLoad(history_accum, ivec3(candidate_block[c], int(j))).rg;
					break;
				}
			}
		}
		if (has_history) {
			accum[i] /= candidate_total;
		}
	}
	if (has_rest) {
		if (params.count != 0u) {
			atomicAdd(counters.rest_pixels, 1u);
		}
		if (has_history && !rest_reset) {
			for (uint c = 0u; c < candidate_count; c++) {
				if (uint(round(imageLoad(history_slots, ivec3(candidate_block[c], int(rest_layer))).r * 65535.0)) == rest_key_value) {
					rest_accum += candidate_weight[c] * imageLoad(history_rest, candidate_block[c]);
				}
			}
			rest_accum /= candidate_total;
		}
	}
#else
	ivec2 history_pixel;
	bool has_history = find_history(pixel, vertex, normal, view, history_pixel);
	imageStore(guide_image, pixel, uvec4(floatBitsToUint(-vertex.z), packSnorm2x16(direct_oct_encode(normal)), 0u, 0u));
	uint old_slots[MAX_ENTRIES];
	vec2 old_accum[MAX_ENTRIES];
	for (uint i = 0u; i < own_limit; i++) {
		old_slots[i] = has_history ? uint(round(imageLoad(history_slots, ivec3(history_pixel, int(i))).r * 65535.0)) : 0u;
		old_accum[i] = has_history ? imageLoad(history_accum, ivec3(history_pixel, int(i))).rg : vec2(0.0);
	}
	for (uint i = 0u; i < own_count; i++) {
		accum[i] = vec2(0.0);
		if (direct_lamps.data[own[i].slot].reset_frame > params.last_frame || cold_lamp(own[i].slot)) {
			continue;
		}
		for (uint j = 0u; j < own_limit; j++) {
			if (old_slots[j] == own[i].slot) {
				accum[i] = old_accum[j];
				break;
			}
		}
	}

	if (has_rest) {
		if (params.count != 0u) {
			atomicAdd(counters.rest_pixels, 1u);
		}
		if (has_history && !rest_reset && uint(round(imageLoad(history_slots, ivec3(history_pixel, int(rest_layer))).r * 65535.0)) == rest_key_value) {
			rest_accum = imageLoad(history_rest, history_pixel);
		}
	}
#endif
	if (params.count != 0u && has_history) {
		atomicAdd(counters.history_accepted, 1u);
	}

	vec3 anchored = relative + ray_scene.camera_to_anchor;
	{
		const vec3 omni_anchor = vec3(-3.720, -0.857, -1.148) - params.anchor.xyz;
		float receiver_omni_distance = length(anchored - omni_anchor);
		if (receiver_omni_distance < 1.0) {
			atomicAdd(counters.recv_near_005, 1u);
		} else if (receiver_omni_distance < 3.0) {
			atomicAdd(counters.recv_near_030, 1u);
		} else if (receiver_omni_distance < 10.0) {
			atomicAdd(counters.recv_near_200, 1u);
		} else {
			atomicAdd(counters.recv_near_far, 1u);
		}
	}
	uint traced_rays = 0u;
	uint ray_record = 0u;

#ifdef EXTERNAL_ENTRIES
	float emitter_visible = 0.0;
	uint emitter_traces = 0u;
	if (emitter_tail && emitter_geometry.w > 0.0) {
		vec3 emitter_point = to_world(emitter_geometry.xyz) + ray_scene.camera_to_anchor;
		vec3 emitter_from = anchored + to_world(normal) * params.bias;
		{
			uint sx = uint(clamp((emitter_point.x + 4096.0) * 16.0, 0.0, 131071.0));
			uint sy = uint(clamp((emitter_point.y + 4096.0) * 16.0, 0.0, 131071.0));
			uint sz = uint(clamp((emitter_point.z + 4096.0) * 16.0, 0.0, 131071.0));
			atomicMin(counters.sample_min_x, sx);
			atomicMin(counters.sample_min_y, sy);
			atomicMin(counters.sample_min_z, sz);
			atomicMax(counters.sample_max_x, sx);
			atomicMax(counters.sample_max_y, sy);
			atomicMax(counters.sample_max_z, sz);
			atomicAdd(counters.sample_count, 1u);
		}
		vec3 emitter_offset = emitter_point - emitter_from;
		float emitter_reach = length(emitter_offset);
		float emitter_ndotl = dot(to_world(normal), normalize(emitter_point - anchored));
		if (emitter_ndotl <= 0.0) {
			emitter_visible = 1.0;
			emitter_traces = 0u;
			atomicAdd(counters.emitter_backface, 1u);
		} else if (emitter_reach <= params.bias) {
			emitter_visible = 1.0;
			emitter_traces = 1u;
		} else {
			vec3 emitter_to = emitter_from + emitter_offset * (1.0 - params.bias / emitter_reach);
			uint emitter_rays = params.reference != 0u ? REFERENCE_RAYS : 1u;
			float emitter_ray_length = length(emitter_to - emitter_from);
			atomicMin(counters.min_ray_len, uint(clamp(emitter_ray_length * 4.0, 0.0, 4095.0)));
			for (uint r = 0u; r < emitter_rays; r++) {
				int skip_instance = params.pad1 != 0u ? int(params.pad1) - 1 : -1;
				uint blocker = 0xFFFFFFFFu;
				uint blocker_triangle = 0u;
				float blocker_distance = 0.0;
				bool emitter_unblocked = ray_scene_sampled_visibility_blocker(emitter_from, emitter_to, RAY_SCENE_ALL_CASTERS, rng_state + r, skip_instance, blocker, blocker_distance, blocker_triangle);
								emitter_visible += emitter_unblocked ? 1.0 : 0.0;
				if (emitter_ray_length < 1.0) {
					if (emitter_unblocked) { atomicAdd(counters.len_u_1, 1u); } else { atomicAdd(counters.len_b_1, 1u); }
				} else if (emitter_ray_length < 3.0) {
					if (emitter_unblocked) { atomicAdd(counters.len_u_3, 1u); } else { atomicAdd(counters.len_b_3, 1u); }
				} else if (emitter_ray_length < 10.0) {
					if (emitter_unblocked) { atomicAdd(counters.len_u_10, 1u); } else { atomicAdd(counters.len_b_10, 1u); }
				} else {
					if (emitter_unblocked) { atomicAdd(counters.len_u_far, 1u); } else { atomicAdd(counters.len_b_far, 1u); }
				}
				if (emitter_unblocked) {
					atomicAdd(counters.emitter_unblocked_front, 1u);
				}
				if (!emitter_unblocked) {
					uint hist_id = blocker < EMITTER_HIT_INSTANCES ? blocker : EMITTER_HIT_INSTANCES;
					uint hist_base = hist_id * 16u;
					uint hist_distance = uint(clamp(blocker_distance, 0.0, 250.0) * 4.0);
					atomicAdd(emitter_hit_hist.hits[hist_base], 1u);
					atomicAdd(emitter_hit_hist.hits[hist_base + 1u], hist_distance);
					atomicMin(emitter_hit_hist.hits[hist_base + 2u], hist_distance);
					atomicMax(emitter_hit_hist.hits[hist_base + 3u], hist_distance);
					#ifdef RAY_SCENE_ALPHA
					RaySceneHit blocker_hit;
					blocker_hit.instance = blocker;
					blocker_hit.triangle = blocker_triangle;
					const RaySceneMaterial blocker_material = ray_scene_hit_material(blocker_hit);
					if ((blocker_material.flags >> RAY_SCENE_ALPHA_SHIFT) != 0u) {
						atomicAdd(counters.blocked_alpha_surface, 1u);
					} else {
						atomicAdd(counters.blocked_solid_surface, 1u);
					}
					#else
						atomicAdd(counters.blocked_split_unavailable, 1u);
					#endif
					atomicAdd(counters.emitter_blocked_front, 1u);
					if (blocker_distance < 0.05) {
						atomicAdd(counters.emitter_hist_005, 1u);
					} else if (blocker_distance < 0.3) {
						atomicAdd(counters.emitter_hist_030, 1u);
					} else if (blocker_distance < 2.0) {
						atomicAdd(counters.emitter_hist_200, 1u);
					} else {
						atomicAdd(counters.emitter_hist_far, 1u);
					}
					float emitter_end_distance = max(emitter_ray_length - blocker_distance, 0.0);
					uint hist_end_distance = uint(clamp(emitter_end_distance, 0.0, 250.0) * 4.0);
					atomicAdd(emitter_hit_hist.hits[hist_base + 4u], 1u);
					atomicAdd(emitter_hit_hist.hits[hist_base + 5u], hist_end_distance);
					atomicMin(emitter_hit_hist.hits[hist_base + 6u], hist_end_distance);
					atomicMax(emitter_hit_hist.hits[hist_base + 7u], hist_end_distance);
					uint hist_rb = blocker_distance < 0.05 ? 8u : blocker_distance < 0.3 ? 9u : blocker_distance < 2.0 ? 10u : 11u;
					uint hist_eb = emitter_end_distance < 0.05 ? 12u : emitter_end_distance < 0.3 ? 13u : emitter_end_distance < 2.0 ? 14u : 15u;
					atomicAdd(emitter_hit_hist.hits[hist_base + hist_rb], 1u);
					atomicAdd(emitter_hit_hist.hits[hist_base + hist_eb], 1u);
					if (emitter_end_distance < 0.05) {
						atomicAdd(counters.emitter_end_005, 1u);
					} else if (emitter_end_distance < 0.3) {
						atomicAdd(counters.emitter_end_030, 1u);
					} else if (emitter_end_distance < 2.0) {
						atomicAdd(counters.emitter_end_200, 1u);
					} else {
						atomicAdd(counters.emitter_end_far, 1u);
					}
				}
				emitter_traces++;
			}
		}
		traced_rays += emitter_traces;
		atomicAdd(counters.emitter_rays, emitter_traces);
		if (emitter_traces > 0u) {
			atomicAdd(counters.emitter_blocked, emitter_traces - uint(emitter_visible + 0.5));
		}
	}
#endif

	if (params.reference != 0u) {
		for (uint i = 0u; i < own_count; i++) {
			for (uint r = 0u; r < REFERENCE_RAYS; r++) {
				float u = (float(r & 3u) + random_value()) * 0.25;
				float v = (float(r >> 2u) + random_value()) * 0.25;
				if (lamp_ray(vertex, anchored, normal, own[i].position, own[i].slot, u, v, rng_state)) {
					accum[i].x += 1.0;
				}
				accum[i].y += 1.0;
				traced_rays++;
			}
		}
#ifdef EXTERNAL_ENTRIES
		for (uint k = 0u; k < tail_count; k++) {
			if (!tail_seen[k]) {
				continue;
			}
			for (uint r = 0u; r < REFERENCE_RAYS; r++) {
				float visible = tail_visible(pixel, k, vertex, anchored, normal);
				tail_hits[k] += visible;
				tail_rays[k] += 1.0;
				rest_accum.rgb += visible * tail_scale[k];
				rest_accum.a += 1.0;
				traced_rays += params.tail_visibility == 0u ? 1u : 0u;
			}
		}
#else
		if (has_rest) {
			for (uint r = 0u; r < REFERENCE_RAYS; r++) {
				float visible = lamp_ray(vertex, anchored, normal, rest_position[r], rest_slot[r], random_value(), random_value(), rng_state) ? 1.0 : 0.0;
				rest_accum.rgb += rest_sample(r, visible);
				rest_accum.a += 1.0;
				traced_rays++;
			}
		}
#endif
	} else {
		float pick[MAX_ENTRIES];
#ifdef EXTERNAL_ENTRIES
		uint tail_of_pick[MAX_ENTRIES];
		uint tail_picks = 0u;
		bool tail_dynamic = false;
		for (uint k = 0u; k < tail_count; k++) {
			if (tail_seen[k] && tail_share[k] > 0.0) {
				tail_of_pick[tail_picks] = k;
				tail_picks++;
				tail_dynamic = tail_dynamic || direct_lamps.data[tail_slot[k]].dynamic != 0u;
			}
		}
		uint pick_count = own_count + tail_picks;
#else
		uint pick_count = own_count + (has_rest ? 1u : 0u);
#endif
		float floor_share = 1.0 / (4.0 * float(pick_count));
		float total = 0.0;
		for (uint i = 0u; i < own_count; i++) {
			float count = accum[i].y;
			float mean = count > 0.0 ? accum[i].x / count : 0.5;
			float doubt = count < 8.0 ? 1.0 : 4.0 * mean * (1.0 - mean) + 0.05;
			pick[i] = own[i].weight * doubt;
			total += pick[i];
		}
#ifdef EXTERNAL_ENTRIES
		for (uint t = 0u; t < tail_picks; t++) {
			pick[own_count + t] = tail_share[tail_of_pick[t]] * (rest_accum.a < 8.0 ? 1.0 : 0.5);
			total += pick[own_count + t];
		}
#else
		if (has_rest) {
			pick[own_count] = rest_total * (rest_accum.a < 8.0 ? 1.0 : 0.5);
			total += pick[own_count];
		}
#endif
		float share_total = 0.0;
		for (uint i = 0u; i < pick_count; i++) {
#ifdef EXTERNAL_ENTRIES
			bool dynamic = i < own_count ? direct_lamps.data[own[i].slot].dynamic != 0u : tail_dynamic;
#else
			bool dynamic = i < own_count ? direct_lamps.data[own[i].slot].dynamic != 0u : rest_dynamic;
#endif
			pick[i] = max(pick[i] / max(total, 1e-20), dynamic ? 1.0 / float(pick_count) : floor_share);
			share_total += pick[i];
		}
		for (uint i = 0u; i < pick_count; i++) {
			pick[i] /= share_total;
		}
		for (uint r = 0u; r < params.rays && pick_count > 0u; r++) {
			float choice = random_value();
			uint chosen = pick_count - 1u;
			for (uint i = 0u; i < pick_count; i++) {
				choice -= pick[i];
				if (choice <= 0.0) {
					chosen = i;
					break;
				}
			}
			if (chosen < own_count && cold_lamp(own[chosen].slot)) {
				continue;
			}
			if (chosen < own_count) {
				bool hit = lamp_ray(vertex, anchored, normal, own[chosen].position, own[chosen].slot, random_value(), random_value(), rng_state);
				if (hit) {
					accum[chosen].x += 1.0;
				}
				accum[chosen].y += 1.0;
				ray_record = (own[chosen].slot << 16u) | (hit ? 1u : 0u);
			} else {
#ifdef EXTERNAL_ENTRIES
				uint k = tail_of_pick[chosen - own_count];
				float visible = tail_visible(pixel, k, vertex, anchored, normal);
				tail_hits[k] += visible;
				tail_rays[k] += 1.0;
				rest_accum.rgb += visible * tail_scale[k];
#else
				float visible = lamp_ray(vertex, anchored, normal, rest_position[0], rest_slot[0], random_value(), random_value(), rng_state) ? 1.0 : 0.0;
				rest_accum.rgb += rest_sample(0u, visible);
#endif
				rest_accum.a += 1.0;
			}
			traced_rays += chosen < own_count || params.tail_visibility == 0u ? 1u : 0u;
		}
		for (uint i = 0u; i < pick_count; i++) {
			bool dynamic = i < own_count ? direct_lamps.data[own[i].slot].dynamic != 0u : rest_dynamic;
			float limit = history_limit(dynamic);
			if (i < own_count) {
				if (accum[i].y > limit) {
					accum[i] *= limit / accum[i].y;
				}
			} else if (rest_accum.a > limit) {
				rest_accum *= limit / rest_accum.a;
			}
		}
#ifndef PRECISE_HISTORY
		rest_accum.rgb = min(rest_accum.rgb, vec3(4.0 * rest_accum.a));
#endif
	}
	imageStore(ray_image, at, uvec4(ray_record));
	if (params.count != 0u) {
		atomicAdd(counters.rays, traced_rays);
	}

	vec3 shown = vec3(0.0);
	float shown_weight = 0.0;
	for (uint i = 0u; i < own_limit; i++) {
		bool used = i < own_count;
		float mean = used && accum[i].y > 0.0 ? accum[i].x / accum[i].y : 1.0;
		imageStore(slot_image, ivec3(at, int(i)), vec4(used ? float(own[i].slot) / 65535.0 : 0.0));
		imageStore(accum_image, ivec3(at, int(i)), vec4(used ? accum[i] : vec2(0.0), 0.0, 0.0));
		if (params.filter_on == 0u) {
			imageStore(factor_image, ivec3(at, int(i)), vec4(params.force_one != 0u ? 1.0 : mean));
		}
		if (used) {
			shown += own[i].target * mean;
			shown_weight += own[i].weight;
		}
	}
	vec3 rest_mean = rest_accum.a > 0.0 ? clamp(rest_accum.rgb / rest_accum.a, vec3(0.0), vec3(1.0)) : vec3(1.0);
#ifdef EXTERNAL_ENTRIES
	for (uint k = 0u; k < tail_count; k++) {
		float visible = 1.0;
		if (tail_rays[k] > 0.0) {
			visible = tail_hits[k] / tail_rays[k];
		} else if (tail_seen[k]) {
			DirectLamp lamp = direct_lamps.data[tail_slot[k]];
			vec3 mean_color = vec3(lamp.mean_r, lamp.mean_g, lamp.mean_b);
			visible = dot(rest_mean, mean_color) / max(mean_color.r + mean_color.g + mean_color.b, 1e-6);
		}
		imageStore(slot_image, ivec3(at, int(own_limit + k)), vec4(0.0));
		imageStore(accum_image, ivec3(at, int(own_limit + k)), vec4(visible, 1.0, 0.0, 0.0));
		if (params.filter_on == 0u) {
			imageStore(factor_image, ivec3(at, int(own_limit + k)), vec4(params.force_one != 0u && params.tail_visibility != 2u ? 1.0 : visible));
		}
	}
	if (emitter_tail) {
		float visible = emitter_traces > 0u ? emitter_visible / float(emitter_traces) : 1.0;
		imageStore(factor_image, ivec3(at, int(emitter_tail_factor)), vec4(params.force_one != 0u ? 1.0 : visible));
	} else if (params.pad0 != 0u) {
		imageStore(factor_image, ivec3(at, int(params.entries + 2u)), vec4(1.0));
	}
#endif
	imageStore(slot_image, ivec3(at, int(rest_layer)), vec4(has_rest ? float(rest_key_value) / 65535.0 : 0.0));
#ifdef PRECISE_HISTORY
	imageStore(accum_image, ivec3(at, int(rest_layer)), vec4(0.0, float(rest_count), 0.0, 0.0));
#endif
	imageStore(rest_image, at, has_rest ? rest_accum : vec4(0.0));
	if (params.filter_on == 0u) {
		for (uint c = 0u; c < 3u; c++) {
			imageStore(factor_image, ivec3(at, int(rest_layer + c)), vec4(params.force_one != 0u ? 1.0 : rest_mean[c]));
		}
	}

	if (params.debug_view != 0u) {
		vec3 color = vec3(0.0);
		if (params.debug_view == 1u) {
			color = vec3(accum[0].y > 0.0 ? accum[0].x / accum[0].y : 1.0);
		} else if (params.debug_view == 2u) {
			vec3 total_target = rest_target;
			for (uint i = 0u; i < own_count; i++) {
				total_target += own[i].target;
			}
			color = clamp((shown + rest_target * rest_mean) / max(total_target, vec3(1e-30)), vec3(0.0), vec3(1.0));
		} else if (params.debug_view == 3u) {
			color = slot_color(own[0].slot);
		} else if (params.debug_view == 4u) {
			color = vec3(float(own_count + rest_count) / 16.0, has_rest ? 1.0 : 0.0, 0.0);
		} else if (params.debug_view == 5u) {
			color = vec3(min(accum[0].y / 256.0, 1.0));
		}
		imageStore(debug_image, at, vec4(color, 1.0));
	}
}















