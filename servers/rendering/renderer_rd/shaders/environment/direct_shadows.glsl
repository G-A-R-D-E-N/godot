#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../light_data_inc.glsl"

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

struct DirectLamp {
	uint caster_mask;
	float size;
	uint reset_frame;
	uint dynamic;
};

layout(set = 0, binding = 3, std430) restrict readonly buffer DirectLamps {
	DirectLamp data[];
}
direct_lamps;

layout(set = 0, binding = 4, std140) uniform Params {
	mat4 inv_projection;
	vec4 camera_basis[3];
	vec3 region_half;
	float bias;
	ivec2 size;
	vec2 pixel_size;
	uint cluster_shift;
	uint cluster_width;
	uint cluster_type_size;
	uint max_cluster_element_count_div_32;
	float z_far;
	uint frame;
	uint last_frame;
	uint entries;
	uint rays;
	uint reference;
	uint debug_view;
	uint pad;
}
params;

layout(set = 0, binding = 5) uniform sampler2D depth_buffer;
layout(set = 0, binding = 6) uniform sampler2D normal_roughness_buffer;
layout(r16, set = 0, binding = 7) uniform restrict image2DArray slot_image;
layout(r16, set = 0, binding = 8) uniform restrict writeonly image2DArray factor_image;
layout(rg32f, set = 0, binding = 9) uniform restrict image2DArray accum_image;
layout(rgba32f, set = 0, binding = 10) uniform restrict image2D rest_image;
layout(rgba16f, set = 0, binding = 11) uniform restrict writeonly image2D debug_image;

layout(set = 0, binding = 12, std430) restrict buffer Counters {
	uint lit_pixels;
	uint rest_pixels;
	uint rays;
	uint past_region_pixels;
	uint most_lamps;
	uint pad[3];
}
counters;

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

float rest_total = 0.0;
vec3 rest_target = vec3(0.0);
uint rest_key = 0u;
uint rest_count = 0u;
bool rest_reset = false;
uint rest_slot[REFERENCE_RAYS];
float rest_weight[REFERENCE_RAYS];
vec3 rest_position[REFERENCE_RAYS];
vec3 rest_pick_target[REFERENCE_RAYS];
uint rest_seen = 0u;
uint rest_picks = 1u;

uint rng_state = 0u;

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

void add_rest(uint p_slot, float p_weight, vec3 p_position, vec3 p_target) {
	rest_total += p_weight;
	rest_target += p_target;
	rest_key += ray_scene_hash(p_slot);
	rest_count++;
	rest_reset = rest_reset || direct_lamps.data[p_slot].reset_frame > params.last_frame;
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

void consider(LightData p_light, bool p_spot, vec3 p_vertex, vec3 p_normal, vec3 p_view, float p_roughness) {
	if ((p_light.flags & 1u) == 0u) {
		return;
	}
	vec3 rel = p_light.position - p_vertex;
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
	if (weight <= 0.0) {
		return;
	}
	add_lamp((p_light.flags >> 1) & 0xFFFFu, weight, p_light.position, target);
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
	vec3 direction = axis;
	float reach = distance;
	if (radius > 0.0) {
		if (distance <= radius) {
			return true;
		}
		float sin_max = radius / distance;
		float cos_max = sqrt(max(1.0 - sin_max * sin_max, 0.0));
		float cos_theta = mix(cos_max, 1.0, p_u);
		float sin_theta = sqrt(max(1.0 - cos_theta * cos_theta, 0.0));
		float phi = 2.0 * PI * p_v;
		vec3 side = abs(axis.y) < 0.99 ? normalize(cross(axis, vec3(0.0, 1.0, 0.0))) : normalize(cross(axis, vec3(1.0, 0.0, 0.0)));
		vec3 up = cross(axis, side);
		direction = normalize(side * cos(phi) * sin_theta + up * sin(phi) * sin_theta + axis * cos_theta);
		reach = distance * cos_theta - sqrt(max(radius * radius - distance * distance * sin_theta * sin_theta, 0.0));
	}
	return ray_scene_unshadowed(p_anchored, to_world(p_normal), params.bias, to_world(direction), reach, lamp.caster_mask, p_seed);
}

vec3 slot_color(uint p_slot) {
	uint h = ray_scene_hash(p_slot + 1u);
	return p_slot == 0u ? vec3(0.0) : vec3(float(h & 255u), float((h >> 8u) & 255u), float((h >> 16u) & 255u)) / 255.0;
}

void write_empty(ivec2 p_pixel, uint p_factor_layers) {
	for (uint i = 0u; i < params.entries; i++) {
		imageStore(slot_image, ivec3(p_pixel, int(i)), vec4(0.0));
		imageStore(accum_image, ivec3(p_pixel, int(i)), vec4(0.0));
	}
	for (uint i = 0u; i < p_factor_layers; i++) {
		imageStore(factor_image, ivec3(p_pixel, int(i)), vec4(1.0));
	}
	imageStore(rest_image, p_pixel, vec4(0.0));
	if (params.debug_view != 0u) {
		imageStore(debug_image, p_pixel, vec4(0.0, 0.0, 0.0, 1.0));
	}
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) {
		return;
	}
	own_limit = params.entries - 1u;
	uint factor_layers = own_limit + 3u;

	float depth = texelFetch(depth_buffer, pixel, 0).r;
	if (depth <= 0.0) {
		write_empty(pixel, factor_layers);
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) * params.pixel_size;
	vec4 view_h = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 vertex = view_h.xyz / view_h.w;
	vec3 relative = to_world(vertex);
	if (any(greaterThan(abs(relative), params.region_half))) {
		write_empty(pixel, factor_layers);
		atomicAdd(counters.past_region_pixels, 1u);
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

	uvec2 cluster_pos = uvec2(pixel) >> params.cluster_shift;
	uint cluster_offset = (params.cluster_width * cluster_pos.y + cluster_pos.x) * (params.max_cluster_element_count_div_32 + 32u);
	uint cluster_z = uint(clamp((-vertex.z / params.z_far) * 32.0, 0.0, 31.0));
	walk_cluster(cluster_offset, cluster_z, false, vertex, normal, view, roughness);
	walk_cluster(cluster_offset + params.cluster_type_size, cluster_z, true, vertex, normal, view, roughness);

	if (own_count + rest_count == 0u) {
		write_empty(pixel, factor_layers);
		return;
	}
	atomicAdd(counters.lit_pixels, 1u);
	atomicMax(counters.most_lamps, own_count + rest_count);

	uint old_slots[MAX_ENTRIES];
	vec2 old_accum[MAX_ENTRIES];
	for (uint i = 0u; i < own_limit; i++) {
		old_slots[i] = uint(round(imageLoad(slot_image, ivec3(pixel, int(i))).r * 65535.0));
		old_accum[i] = imageLoad(accum_image, ivec3(pixel, int(i))).rg;
	}
	vec2 accum[MAX_ENTRIES];
	for (uint i = 0u; i < own_count; i++) {
		accum[i] = vec2(0.0);
		if (direct_lamps.data[own[i].slot].reset_frame > params.last_frame) {
			continue;
		}
		for (uint j = 0u; j < own_limit; j++) {
			if (old_slots[j] == own[i].slot) {
				accum[i] = old_accum[j];
				break;
			}
		}
	}

	vec4 rest_accum = vec4(0.0);
	float rest_stored_key = imageLoad(accum_image, ivec3(pixel, int(own_limit))).r;
	bool has_rest = rest_count > 0u;
	float rest_key_value = float(rest_key & 0xFFFFFFu);
	if (has_rest) {
		atomicAdd(counters.rest_pixels, 1u);
		if (rest_stored_key == rest_key_value && !rest_reset) {
			rest_accum = imageLoad(rest_image, pixel);
		}
	}

	vec3 anchored = relative + ray_scene.camera_to_anchor;
	uint traced_rays = 0u;

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
		if (has_rest) {
			for (uint r = 0u; r < REFERENCE_RAYS; r++) {
				float visible = lamp_ray(vertex, anchored, normal, rest_position[r], rest_slot[r], random_value(), random_value(), rng_state) ? 1.0 : 0.0;
				rest_accum.rgb += visible * rest_pick_target[r] * rest_total / (rest_weight[r] * max(rest_target, vec3(1e-6)));
				rest_accum.a += 1.0;
				traced_rays++;
			}
		}
	} else {
		float pick[MAX_ENTRIES];
		uint pick_count = own_count + (has_rest ? 1u : 0u);
		float floor_share = 1.0 / (4.0 * float(pick_count));
		float total = 0.0;
		for (uint i = 0u; i < own_count; i++) {
			float count = accum[i].y;
			float mean = count > 0.0 ? accum[i].x / count : 0.5;
			float doubt = count < 8.0 ? 1.0 : 4.0 * mean * (1.0 - mean) + 0.05;
			pick[i] = own[i].weight * doubt;
			total += pick[i];
		}
		if (has_rest) {
			float count = rest_accum.a;
			pick[own_count] = rest_total * (count < 8.0 ? 1.0 : 0.5);
			total += pick[own_count];
		}
		float share_total = 0.0;
		for (uint i = 0u; i < pick_count; i++) {
			bool dynamic = i < own_count && direct_lamps.data[own[i].slot].dynamic != 0u;
			pick[i] = max(pick[i] / max(total, 1e-20), dynamic ? 1.0 / float(pick_count) : floor_share);
			share_total += pick[i];
		}
		for (uint r = 0u; r < params.rays; r++) {
			float choice = random_value() * share_total;
			uint chosen = pick_count - 1u;
			for (uint i = 0u; i < pick_count; i++) {
				choice -= pick[i];
				if (choice <= 0.0) {
					chosen = i;
					break;
				}
			}
			if (chosen < own_count) {
				if (lamp_ray(vertex, anchored, normal, own[chosen].position, own[chosen].slot, random_value(), random_value(), rng_state)) {
					accum[chosen].x += 1.0;
				}
				accum[chosen].y += 1.0;
			} else {
				float visible = lamp_ray(vertex, anchored, normal, rest_position[0], rest_slot[0], random_value(), random_value(), rng_state) ? 1.0 : 0.0;
				rest_accum.rgb += visible * rest_pick_target[0] * rest_total / (rest_weight[0] * max(rest_target, vec3(1e-6)));
				rest_accum.a += 1.0;
			}
			traced_rays++;
		}
	}
	atomicAdd(counters.rays, traced_rays);

	vec3 shown = vec3(0.0);
	float shown_weight = 0.0;
	for (uint i = 0u; i < own_limit; i++) {
		bool used = i < own_count;
		float mean = used && accum[i].y > 0.0 ? accum[i].x / accum[i].y : 1.0;
		imageStore(slot_image, ivec3(pixel, int(i)), vec4(used ? float(own[i].slot) / 65535.0 : 0.0));
		imageStore(accum_image, ivec3(pixel, int(i)), vec4(used ? accum[i] : vec2(0.0), 0.0, 0.0));
		imageStore(factor_image, ivec3(pixel, int(i)), vec4(mean));
		if (used) {
			shown += own[i].target * mean;
			shown_weight += own[i].weight;
		}
	}
	vec3 rest_mean = rest_accum.a > 0.0 ? clamp(rest_accum.rgb / rest_accum.a, vec3(0.0), vec3(1.0)) : vec3(1.0);
	imageStore(slot_image, ivec3(pixel, int(own_limit)), vec4(has_rest ? 1.0 : 0.0));
	imageStore(accum_image, ivec3(pixel, int(own_limit)), vec4(has_rest ? rest_key_value : 0.0, float(rest_count), 0.0, 0.0));
	imageStore(rest_image, pixel, has_rest ? rest_accum : vec4(0.0));
	for (uint c = 0u; c < 3u; c++) {
		imageStore(factor_image, ivec3(pixel, int(own_limit + c)), vec4(rest_mean[c]));
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
			color = clamp((shown + rest_target * rest_mean) / max(total_target, vec3(1e-6)), vec3(0.0), vec3(1.0));
		} else if (params.debug_view == 3u) {
			color = slot_color(own[0].slot);
		} else if (params.debug_view == 4u) {
			color = vec3(float(own_count + rest_count) / 16.0, has_rest ? 1.0 : 0.0, 0.0);
		} else if (params.debug_view == 5u) {
			color = vec3(min(accum[0].y / 256.0, 1.0));
		}
		imageStore(debug_image, pixel, vec4(color, 1.0));
	}
}
