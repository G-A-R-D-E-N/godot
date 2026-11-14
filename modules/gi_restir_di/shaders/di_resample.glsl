#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../../../servers/rendering/renderer_rd/shaders/light_data_inc.glsl"
#include "../../../servers/rendering/renderer_rd/shaders/environment/di_sampler_inc.glsl"

#define MAX_ENTRIES 8

struct DiSlot {
	uint caster_mask;
	float size;
	uint reset_frame;
	uint dynamic;
	float clearance;
	uint exclude;
	float anchor_offset_x;
	float anchor_offset_y;
	float anchor_offset_z;
	float mean_r;
	float mean_g;
	float mean_b;
};

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

layout(set = 0, binding = 3, std430) restrict readonly buffer DiSlots {
	DiSlot data[];
}
di_slots;

layout(set = 0, binding = 4, std140) uniform Params {
	mat4 inv_projection;
	mat4 prev_view_from_view;
	mat4 prev_projection;
	vec4 camera_basis[3];
	vec3 region_half;
	float floor_value;
	ivec2 size;
	vec2 pixel_size;
	uint cluster_shift;
	uint cluster_width;
	uint cluster_type_size;
	uint max_cluster_element_count_div_32;
	float z_far;
	uint frame;
	uint head_entries;
	uint tail_samples;
	uint history_valid;
	uint live_energy;
	uint debug_flags;
	uint pad1;
}
params;

layout(set = 0, binding = 5) uniform sampler2D depth_buffer;
layout(set = 0, binding = 6) uniform sampler2D normal_roughness_buffer;
layout(r16, set = 0, binding = 7) uniform restrict readonly image2DArray history_slots;
layout(r16, set = 0, binding = 8) uniform restrict readonly image2DArray history_factors;
layout(rg32ui, set = 0, binding = 9) uniform restrict readonly uimage2D history_guide;
layout(r32ui, set = 0, binding = 10) uniform restrict writeonly uimage2DArray entry_image;
layout(rg32ui, set = 0, binding = 11) uniform restrict writeonly uimage2DArray record_image;

layout(set = 0, binding = 12, std430) restrict buffer DebugCounts {
	uint tail_pixels;
	uint header_pad[3];
	uint selections[];
}
debug_counts;

#define DEBUG_STALE_PRIOR 1u
#define DEBUG_COUNT 2u
#define RECORD_EXACT_CHECK 0x80000000u

vec3 vertex = vec3(0.0);
vec3 normal = vec3(0.0, 0.0, 1.0);
vec3 view = vec3(0.0, 0.0, 1.0);
float roughness = 0.0;

uint head_count = 0u;
uint head_slot[MAX_ENTRIES];
float head_rank[MAX_ENTRIES];

bool has_history = false;
uint history_count = 0u;
uint history_slot[MAX_ENTRIES];
float history_factor[MAX_ENTRIES];
vec3 history_ratio = vec3(1.0);

uint tail_lamps = 0u;
uint tail_candidates = 0u;
float sum_weight = 0.0;
vec3 sum_target = vec3(0.0);
uint exact_slot[MAX_ENTRIES];
float exact_weight[MAX_ENTRIES];
vec3 exact_target[MAX_ENTRIES];

DiInversion inversions[MAX_ENTRIES];
vec3 pick_target[MAX_ENTRIES];
vec3 last_target = vec3(0.0);

vec3 to_world(vec3 p_view) {
	return vec3(dot(params.camera_basis[0].xyz, p_view), dot(params.camera_basis[1].xyz, p_view), dot(params.camera_basis[2].xyz, p_view));
}

vec3 to_view(vec3 p_world) {
	return params.camera_basis[0].xyz * p_world.x + params.camera_basis[1].xyz * p_world.y + params.camera_basis[2].xyz * p_world.z;
}

float prior(uint p_slot, vec3 p_mean) {
	if ((params.debug_flags & DEBUG_STALE_PRIOR) != 0u && (p_slot & 1u) == 0u) {
		return 0.0;
	}
	if (!has_history) {
		return 1.0;
	}
	for (uint i = 0u; i < history_count; i++) {
		if (history_slot[i] == p_slot) {
			return history_factor[i];
		}
	}
	float total = p_mean.r + p_mean.g + p_mean.b;
	return total > 0.0 ? dot(history_ratio, p_mean) / total : 1.0;
}

bool is_head(uint p_slot) {
	for (uint i = 0u; i < head_count; i++) {
		if (head_slot[i] == p_slot) {
			return true;
		}
	}
	return false;
}

void rank_head(uint p_slot, float p_rank) {
	uint at = head_count;
	while (at > 0u && (head_rank[at - 1u] < p_rank || (head_rank[at - 1u] == p_rank && head_slot[at - 1u] > p_slot))) {
		at--;
	}
	if (at >= params.head_entries) {
		return;
	}
	if (head_count < params.head_entries) {
		head_count++;
	}
	for (uint i = head_count - 1u; i > at; i--) {
		head_slot[i] = head_slot[i - 1u];
		head_rank[i] = head_rank[i - 1u];
	}
	head_slot[at] = p_slot;
	head_rank[at] = p_rank;
}

void visit(LightData p_light, bool p_spot, uint p_pass) {
	if ((p_light.flags & 1u) == 0u) {
		return;
	}
	DiGeometry geometry = di_geometry(p_light.position, p_light.inv_radius, p_light.attenuation, p_spot, p_light.direction, p_light.cone_angle, p_light.cone_attenuation, vertex);
	if (!di_in_set(geometry, normal)) {
		return;
	}
	uint slot = (p_light.flags >> 1) & 0xFFFFu;
	DiSlot data = di_slots.data[slot];
	vec3 mean = vec3(data.mean_r, data.mean_g, data.mean_b);
	if (p_pass == 0u) {
		vec3 anchor = p_light.position + to_view(vec3(data.anchor_offset_x, data.anchor_offset_y, data.anchor_offset_z));
		DiGeometry anchored = di_geometry(anchor, p_light.inv_radius, p_light.attenuation, p_spot, p_light.direction, p_light.cone_angle, p_light.cone_attenuation, vertex);
		rank_head(slot, di_luminance(di_screen_target(mean, anchored, normal, view, roughness, p_light.specular_amount)));
		return;
	}
	if (is_head(slot)) {
		return;
	}
	vec3 target = di_screen_target(mean, geometry, normal, view, roughness, p_light.specular_amount);
	float weight = di_resample_weight(target, prior(slot, mean), params.floor_value);
	bool candidate = di_candidate(weight);
	if (p_pass == 1u) {
		if (tail_lamps < params.tail_samples) {
			exact_slot[tail_lamps] = slot;
			exact_weight[tail_lamps] = candidate ? weight : 0.0;
			exact_target[tail_lamps] = target;
		}
		tail_lamps++;
		if (candidate) {
			tail_candidates++;
			sum_weight += weight;
			sum_target += target;
		}
		return;
	}
	if (!candidate) {
		return;
	}
	for (uint k = 0u; k < params.tail_samples; k++) {
		bool open = inversions[k].pick == DI_SAMPLER_NO_PICK;
		di_inversion_step(inversions[k], weight, slot);
		if (open && inversions[k].pick != DI_SAMPLER_NO_PICK) {
			pick_target[k] = target;
		}
	}
	last_target = target;
}

void walk(uint p_cluster_offset, uint p_cluster_z, bool p_spot, uint p_pass) {
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
			visit(p_spot ? spot_lights.data[index] : omni_lights.data[index], p_spot, p_pass);
		}
	}
}

void load_history() {
	if (params.history_valid == 0u) {
		return;
	}
	vec4 prev_view = params.prev_view_from_view * vec4(vertex, 1.0);
	vec4 prev_clip = params.prev_projection * prev_view;
	if (prev_clip.w <= 0.0) {
		return;
	}
	ivec2 pixel = ivec2(floor((prev_clip.xy / prev_clip.w * 0.5 + 0.5) * vec2(params.size)));
	if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, params.size))) {
		return;
	}
	uvec2 guide = imageLoad(history_guide, pixel).xy;
	if (guide.x == 0u) {
		return;
	}
	float expected = -prev_view.z;
	float tolerance = expected * (0.01 + 0.02 / max(dot(normal, view), 0.05));
	if (abs(uintBitsToFloat(guide.x) - expected) > tolerance) {
		return;
	}
	if (dot(mat3(params.prev_view_from_view) * normal, di_oct_decode(unpackSnorm2x16(guide.y))) < 0.9) {
		return;
	}
	int layers = imageSize(history_slots).z;
	history_count = uint(clamp(layers - 1, 0, MAX_ENTRIES - 1));
	for (uint i = 0u; i < history_count; i++) {
		history_slot[i] = uint(round(imageLoad(history_slots, ivec3(pixel, int(i))).r * 65535.0));
		history_factor[i] = imageLoad(history_factors, ivec3(pixel, int(i))).r;
	}
	int rest = layers - 1;
	history_ratio = vec3(imageLoad(history_factors, ivec3(pixel, rest)).r, imageLoad(history_factors, ivec3(pixel, rest + 1)).r, imageLoad(history_factors, ivec3(pixel, rest + 2)).r);
	has_history = true;
}

vec3 rest_scale(vec3 p_target, float p_weight) {
	vec3 share = vec3(sum_target.r > 0.0 ? p_target.r / sum_target.r : 0.0, sum_target.g > 0.0 ? p_target.g / sum_target.g : 0.0, sum_target.b > 0.0 ? p_target.b / sum_target.b : 0.0);
	return min(share * p_weight, vec3(65000.0));
}

void write_sample(ivec2 p_pixel, uint p_k, uint p_slot, float p_weight, float p_share, vec3 p_scale) {
	float sign = params.live_energy != 0u ? -1.0 : 1.0;
	imageStore(record_image, ivec3(p_pixel, int(p_k + 1u)), uvec4(p_slot, floatBitsToUint(sign * p_weight), 0u, 0u));
	uint base = params.head_entries + 3u * p_k;
	imageStore(entry_image, ivec3(p_pixel, int(base)), uvec4(p_slot | (packHalf2x16(vec2(p_scale.b, 0.0)) << 16u), 0u, 0u, 0u));
	imageStore(entry_image, ivec3(p_pixel, int(base + 1u)), uvec4(floatBitsToUint(p_share), 0u, 0u, 0u));
	imageStore(entry_image, ivec3(p_pixel, int(base + 2u)), uvec4(packHalf2x16(p_scale.rg), 0u, 0u, 0u));
}

void write_empty(ivec2 p_pixel) {
	imageStore(record_image, ivec3(p_pixel, 0), uvec4(0u));
	for (uint i = 0u; i < params.head_entries; i++) {
		imageStore(entry_image, ivec3(p_pixel, int(i)), uvec4(0u));
	}
	for (uint k = 0u; k < params.tail_samples; k++) {
		write_sample(p_pixel, k, 0u, 0.0, 0.0, vec3(0.0));
	}
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) {
		return;
	}
	float depth = texelFetch(depth_buffer, pixel, 0).r;
	if (depth <= 0.0) {
		write_empty(pixel);
		return;
	}
	vec2 uv = (vec2(pixel) + 0.5) * params.pixel_size;
	vec4 view_h = params.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vertex = view_h.xyz / view_h.w;
	if (any(greaterThan(abs(to_world(vertex)), params.region_half))) {
		write_empty(pixel);
		return;
	}

	vec4 normal_roughness = texelFetch(normal_roughness_buffer, pixel, 0);
	uint packed_normal = packSnorm2x16(di_oct_encode(normalize(normal_roughness.xyz * 2.0 - 1.0)));
	normal = di_oct_decode(unpackSnorm2x16(packed_normal));
	roughness = normal_roughness.w;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	roughness /= 127.0 / 255.0;
	view = -normalize(vertex);

	load_history();

	uvec2 cluster_pos = uvec2(pixel) >> params.cluster_shift;
	uint cluster_offset = (params.cluster_width * cluster_pos.y + cluster_pos.x) * (params.max_cluster_element_count_div_32 + 32u);
	uint cluster_z = uint(clamp((-vertex.z / params.z_far) * 32.0, 0.0, 31.0));
	for (uint walk_pass = 0u; walk_pass < 2u; walk_pass++) {
		walk(cluster_offset, cluster_z, false, walk_pass);
		walk(cluster_offset + params.cluster_type_size, cluster_z, true, walk_pass);
	}

	uint tails = params.tail_samples;
	bool exact = tail_lamps <= tails;
	bool counting = (params.debug_flags & DEBUG_COUNT) != 0u;
	uint record_flag = counting && exact ? RECORD_EXACT_CHECK : 0u;
	if (counting) {
		if (tail_lamps > 0u) {
			atomicAdd(debug_counts.tail_pixels, 1u);
		}
		for (uint i = 0u; i < head_count; i++) {
			atomicAdd(debug_counts.selections[head_slot[i]], 1u);
		}
	}
	if (!exact) {
		uint seed = di_hash(uint(pixel.y) * 65537u + uint(pixel.x)) ^ di_hash(params.frame ^ 0x68e31da4u);
		for (uint k = 0u; k < tails; k++) {
			uint h = di_hash(seed ^ di_hash(k + 0xb5297a4du));
			inversions[k] = di_inversion_begin(sum_weight, float(h >> 8u) / 16777216.0);
			pick_target[k] = vec3(0.0);
		}
		walk(cluster_offset, cluster_z, false, 2u);
		walk(cluster_offset + params.cluster_type_size, cluster_z, true, 2u);
	}

	imageStore(record_image, ivec3(pixel, 0), uvec4(packed_normal, floatBitsToUint(depth) | record_flag, 0u, 0u));
	for (uint i = 0u; i < params.head_entries; i++) {
		imageStore(entry_image, ivec3(pixel, int(i)), uvec4(i < head_count ? head_slot[i] : 0u, 0u, 0u, 0u));
	}
	for (uint k = 0u; k < tails; k++) {
		if (exact) {
			if (k < tail_lamps) {
				bool picked = exact_weight[k] > 0.0;
				float share = picked ? sum_weight / float(tail_candidates) : 0.0;
				write_sample(pixel, k, exact_slot[k], float(tails), share, picked ? rest_scale(exact_target[k], float(tail_candidates)) : vec3(0.0));
				if (counting && picked) {
					atomicAdd(debug_counts.selections[exact_slot[k]], 1u);
				}
			} else {
				write_sample(pixel, k, 0u, 0.0, 0.0, vec3(0.0));
			}
			continue;
		}
		bool opened = inversions[k].pick != DI_SAMPLER_NO_PICK;
		uint pick = di_inversion_end(inversions[k]);
		if (pick == DI_SAMPLER_NO_PICK) {
			write_sample(pixel, k, 0u, 0.0, 0.0, vec3(0.0));
			continue;
		}
		vec3 target = opened ? pick_target[k] : last_target;
		float weight = di_weight(sum_weight, inversions[k].pick_target);
		write_sample(pixel, k, pick, weight, sum_weight / float(tails), rest_scale(target, weight));
		if (counting) {
			atomicAdd(debug_counts.selections[pick], 1u);
		}
	}
}
