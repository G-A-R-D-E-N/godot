#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "../../../servers/rendering/renderer_rd/shaders/environment/di_sampler_inc.glsl"
#include "../../../servers/rendering/renderer_rd/shaders/light_data_inc.glsl"
#ifdef DI_EMITTERS
#define RAY_SCENE_SET 1
#include "../../../servers/rendering/renderer_rd/shaders/environment/ray_scene_inc.glsl"
#endif

#define MAX_ENTRIES 8
#define MAX_NEIGHBORS 4

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
	mat4 prev_inv_projection;
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
	uint reservoirs;
	uint c_full;
	uint m_temporal_cap;
	uint dynamic_cap;
	uint m_stored_cap;
	uint spatial_neighbors;
	float spatial_radius;
	uint counter;
	uint head_rank;
	float head_swap_margin;
	uint head_swap_frames;
	uint temporal_reuse;
	uint head_window;
	float emitter_clamp;
	uint emitter_clamp_pad0;
	uint emitter_clamp_pad1;
	uint emitter_clamp_pad2;
	float emitter_pick_radius;
	uint emitter_pick_pad0;
	uint emitter_pick_pad1;
	uint emitter_pick_pad2;
	uint emitter_room_pick;
	uint emitter_room_pad0;
	uint emitter_room_pad1;
	uint emitter_room_pad2;
	uint emitter_half;
	uint pad_emitter_a;
	uint pad_emitter_b;
	uint pad_emitter_c;
	uint emitter_visibility;
	uint pad_visibility_a;
	uint pad_visibility_b;
	uint pad_visibility_c;
}
params;

layout(set = 0, binding = 5) uniform sampler2D depth_buffer;
layout(set = 0, binding = 6) uniform sampler2D normal_roughness_buffer;
layout(r16, set = 0, binding = 7) uniform restrict readonly image2DArray history_slots;
layout(r16, set = 0, binding = 8) uniform restrict readonly image2DArray history_factors;
layout(rg32ui, set = 0, binding = 9) uniform restrict readonly uimage2D history_guide;
#ifdef MODE_SPATIAL
layout(r32ui, set = 0, binding = 10) uniform restrict uimage2DArray entry_image;
#else
layout(r32ui, set = 0, binding = 10) uniform restrict writeonly uimage2DArray entry_image;
#endif
layout(rg32ui, set = 0, binding = 11) uniform restrict writeonly uimage2DArray record_image;

#ifndef MODE_SPATIAL
layout(set = 0, binding = 12, std430) restrict buffer DebugCounts {
	uint tail_pixels;
	uint header_pad[3];
	uint selections[];
}
debug_counts;
#endif

layout(r32ui, set = 0, binding = 13) uniform restrict uimage2D tile_flags;
layout(r32ui, set = 0, binding = 14) uniform restrict uimage2D history_header;
layout(rg32ui, set = 0, binding = 15) uniform restrict uimage2D history_values;
layout(r32ui, set = 0, binding = 16) uniform restrict uimage2D current_header;
layout(rg32ui, set = 0, binding = 17) uniform restrict uimage2D current_values;

#ifndef MODE_SPATIAL
layout(r32ui, set = 0, binding = 18) uniform restrict readonly uimage2D head_state_previous;
layout(r32ui, set = 0, binding = 19) uniform restrict writeonly uimage2D head_state;
#endif

#ifdef LAB_COUNTS
layout(set = 0, binding = 20, std430) restrict buffer LabCounts {
	uint words[8];
	uint finals[];
}
lab_counts;

#ifndef MODE_SPATIAL
layout(set = 0, binding = 21, std430) restrict buffer LabMask {
	uint bits[];
}
lab_mask;
#endif

#define LAB_PATH 0
#define LAB_PRESENT 1
#define LAB_DROPPED 2
#define LAB_DROPPED_MASS 3
#define LAB_TOTAL_MASS 4
#define LAB_DYNAMIC_CAPPED 5
#endif

#ifdef DI_EMITTERS
#define DI_EMITTER_CHUNK 4096u
#define DI_EMITTER_ITEMS 64u
#define DI_EMITTER_SUMS 0u
#define DI_EMITTER_PREFIX 256u
#define DI_EMITTER_TOTAL 512u
#define DI_EMITTER_MARK 0xFFFFFFFFu
#define DI_EMITTER_RAY_BIAS 0.02

layout(set = 0, binding = 22, std430) buffer EmitterCdf {
	float data[];
}
emitter_cdf;

layout(set = 0, binding = 23, std430) buffer EmitterScan {
	float data[];
}
emitter_scan;

#if defined(MODE_EMITTER_SPATIAL)
layout(set = 0, binding = 24, std430) restrict buffer EmitterSamples {
#else
layout(set = 0, binding = 24, std430) restrict writeonly buffer EmitterSamples {
#endif
	vec4 data[];
}
emitter_samples;

layout(set = 0, binding = 26, std430) restrict buffer EmitterReuseCounters {
	uint temporal_accepted;
	uint age_sum;
	uint plain_writes;
	uint instance_mismatch;
	uint depth_reject;
	uint normal_reject;
	uint held_selected;
	uint mass_sum;
	uint held_only;
	uint held_blocked;
	uint new_blocked;
	uint damp_sum;
}
emitter_reuse_counters;

#ifdef DI_EMITTER_TEMPORAL
layout(rgba32ui, set = 0, binding = 25) uniform restrict uimage2DArray emitter_history;

bool di_emitter_pending = false;
vec3 di_emitter_point = vec3(0.0);
vec3 di_emitter_color = vec3(0.0);
float di_emitter_geometric = 0.0;
uint di_emitter_instance = 0u;
uint di_emitter_triangle = 0u;
uint di_emitter_index = 0u;
#endif

float di_emitter_clamped_luminance(vec3 p_color, float p_total) {
	float unit = p_total * params.emitter_clamp;
	if (!(unit > 0.0)) {
		return 0.0;
	}
	float luminance = dot(max(p_color, vec3(0.0)), luminance_weights);
	return luminance > unit ? unit / luminance : 0.0;
}

float di_emitter_selection_luminance(uint p_material) {
	float luminance = dot(max(ray_scene_triangle_emitter_emission(p_material), vec3(0.0)), luminance_weights);
	return luminance > 0.0 ? luminance : ray_scene_triangle_emitter_mean_luminance(p_material);
}

float di_emitter_weight(uint p_index) {
	if (ray_scene_triangle_emitter_proxied(p_index) && ray_scene.proxy_emitters_traced == 0u) {
		return 0.0;
	}
	RaySceneTriangleEmitter emitter = ray_scene_triangle_emitter(p_index);
	vec3 e1 = emitter.v1 - emitter.v0;
	vec3 e2 = emitter.v2 - emitter.v0;
	return 0.5 * length(cross(e1, e2)) * di_emitter_selection_luminance(emitter.material);
}

vec3 di_emitter_to_view(vec3 p_ray) {
	vec3 relative = p_ray - ray_scene.camera_to_anchor;
	return vec3(
			dot(vec3(params.camera_basis[0].x, params.camera_basis[1].x, params.camera_basis[2].x), relative),
			dot(vec3(params.camera_basis[0].y, params.camera_basis[1].y, params.camera_basis[2].y), relative),
			dot(vec3(params.camera_basis[0].z, params.camera_basis[1].z, params.camera_basis[2].z), relative));
}

float di_emitter_hash(uint p_seed, uint p_salt) {
	return float(di_hash(p_seed ^ di_hash(p_salt)) >> 8u) / 16777216.0;
}

void di_emitter_write(ivec2 p_pixel, vec3 p_point, vec3 p_color, float p_geometric) {
	uint index = uint(p_pixel.y) * uint(params.size.x) + uint(p_pixel.x);
	emitter_samples.data[index * 2u] = vec4(p_point, p_geometric);
	emitter_samples.data[index * 2u + 1u] = vec4(p_color, 0.0);
}

void di_emitter_clear(ivec2 p_pixel, uint p_reason) {
	uint index = uint(p_pixel.y) * uint(params.size.x) + uint(p_pixel.x);
	emitter_samples.data[index * 2u] = vec4(0.0);
	emitter_samples.data[index * 2u + 1u] = vec4(0.0, 0.0, 0.0, float(p_reason));
	imageStore(record_image, ivec3(p_pixel, int(params.tail_samples + 1u)), uvec4(0u, 0u, 0u, 0u));
}

void di_emitter_clear_block(ivec2 p_pixel, uint p_reason) {
	if (params.emitter_half == 0u) {
		di_emitter_clear(p_pixel, p_reason);
		return;
	}
	for (int dy = 0; dy < 2; dy++) {
		for (int dx = 0; dx < 2; dx++) {
			di_emitter_clear(min(p_pixel + ivec2(dx, dy), params.size - ivec2(1)), p_reason);
		}
	}
}

void di_emitter_write_block(ivec2 p_pixel, vec3 p_point, vec3 p_color, float p_geometric) {
	uint layer = params.tail_samples + 1u;
	if (params.emitter_half == 0u) {
		di_emitter_write(p_pixel, p_point, p_color, p_geometric);
		imageStore(record_image, ivec3(p_pixel, int(layer)), uvec4(DI_EMITTER_MARK, floatBitsToUint(float(layer)), 0u, 0u));
		return;
	}
	for (int dy = 0; dy < 2; dy++) {
		for (int dx = 0; dx < 2; dx++) {
			ivec2 q = min(p_pixel + ivec2(dx, dy), params.size - ivec2(1));
			di_emitter_write(q, p_point, p_color, p_geometric);
			imageStore(record_image, ivec3(q, int(layer)), uvec4(DI_EMITTER_MARK, floatBitsToUint(float(layer)), 0u, 0u));
		}
	}
}

#ifdef DI_EMITTER_TEMPORAL
void di_emitter_commit(ivec2 p_pixel, vec3 p_point, vec3 p_color, float p_geometric, uint p_instance, uint p_triangle, uint p_index) {
	di_emitter_pending = true;
	di_emitter_point = p_point;
	di_emitter_color = p_color;
	di_emitter_geometric = p_geometric;
	di_emitter_instance = p_instance;
	di_emitter_triangle = p_triangle;
	di_emitter_index = p_index;
}
#else
void di_emitter_commit(ivec2 p_pixel, vec3 p_point, vec3 p_color, float p_geometric, uint p_instance, uint p_triangle, uint p_index) {
	atomicAdd(emitter_reuse_counters.plain_writes, 1u);
	di_emitter_write_block(p_pixel, p_point, p_color, p_geometric);
}
#endif

vec3 di_emitter_to_world(vec3 p_view) {
	vec3 offset = params.camera_basis[0].xyz * p_view.x + params.camera_basis[1].xyz * p_view.y + params.camera_basis[2].xyz * p_view.z;
	return ray_scene.camera_to_anchor + offset;
}

bool di_emitter_visible(vec3 p_vertex, vec3 p_normal, vec3 p_point, uint p_instance, float p_distance_sq, uint p_seed) {
	float distance = sqrt(p_distance_sq);
	if (!(distance > 2.0 * DI_EMITTER_RAY_BIAS)) {
		return true;
	}
	vec3 vertex = di_emitter_to_world(p_vertex);
	vec3 point = di_emitter_to_world(p_point);
	vec3 normal = di_emitter_to_world(p_normal) - ray_scene.camera_to_anchor;
	vec3 from = vertex + normal * DI_EMITTER_RAY_BIAS;
	vec3 to = point - normalize(point - vertex) * DI_EMITTER_RAY_BIAS;
	uint blocker = 0xFFFFFFFFu;
	float blocker_distance = 0.0;
	uint blocker_triangle = 0u;
	return ray_scene_sampled_visibility_blocker(from, to, RAY_SCENE_ALL_CASTERS, p_seed + 0x51ed270bu, int(p_instance), blocker, blocker_distance, blocker_triangle);
}

#ifdef DI_EMITTER_ROOM_PICK
layout(set = RAY_SCENE_SET, binding = 16) uniform sampler2D ray_room_grid;

vec3 di_emitter_ray_space(vec3 p_view) {
	mat3 to_ray = mat3(params.camera_basis[0].xyz, params.camera_basis[1].xyz, params.camera_basis[2].xyz);
	return ray_scene.camera_to_anchor + to_ray * p_view;
}

uint di_emitter_room_of(vec3 p_view) {
	vec3 ray_position = di_emitter_ray_space(p_view);
	ivec2 extent = textureSize(ray_room_grid, 0);
	float cell = max(ray_scene.room_grid_cell, 1e-3);
	vec2 texel = (ray_position.xz - ray_scene.room_grid_origin) / cell;
	if (any(lessThan(texel, vec2(0.0))) || any(greaterThanEqual(texel, vec2(extent)))) {
		return 0u;
	}
	return uint(texelFetch(ray_room_grid, ivec2(texel), 0).r * 255.0 + 0.5);
}

bool di_emitter_room_window(uint p_room, out uint r_first, out uint r_last) {
	r_first = 0u;
	r_last = 0u;
	uint room_count = ray_scene.room_count;
	if (room_count == 0u || p_room > room_count) {
		return false;
	}
	r_first = ray_scene.room_start[p_room];
	r_last = ray_scene.room_start[p_room + 1u];
	return r_last > r_first;
}
#endif

uint di_emitter_pick_index(float p_target, uint p_count) {
	uint low = 0u;
	uint high = p_count - 1u;
	while (low < high) {
		uint mid = (low + high) >> 1u;
		if (emitter_cdf.data[mid] < p_target) {
			low = mid + 1u;
		} else {
			high = mid;
		}
	}
	return low;
}

float di_emitter_distance(vec3 p_vertex, uint p_index) {
	RaySceneTriangleEmitter emitter = ray_scene_triangle_emitter(p_index);
	vec3 centre = di_emitter_to_view((emitter.v0 + emitter.v1 + emitter.v2) * (1.0 / 3.0));
	return length(centre - p_vertex);
}

void di_emitter_pick(ivec2 p_pixel, vec3 p_vertex, vec3 p_normal, uint p_seed) {
	uint count = ray_scene_triangle_emitter_count();
	float total = count == 0u ? 0.0 : emitter_scan.data[DI_EMITTER_TOTAL];
	if (!(total > 0.0)) {
		di_emitter_clear_block(p_pixel, 1u);
		return;
	}
	uint low = di_emitter_pick_index(di_emitter_hash(p_seed, 0x27d4eb2fu) * total, count);
	uint room_taken = 0u;
#ifdef DI_EMITTER_ROOM_PICK
	if (params.emitter_room_pick != 0u) {
		uint room_first = 0u;
		uint room_last = 0u;
		const uint room_id = di_emitter_room_of(p_vertex);
		if (di_emitter_room_window(room_id, room_first, room_last) && room_last <= count) {
			room_taken = room_id + 1u;
			float room_base = room_first > 0u ? emitter_cdf.data[room_first - 1u] : 0.0;
			float room_span = emitter_cdf.data[room_last - 1u] - room_base;
			if (!(room_span > 0.0)) {
				room_first = 0u;
				room_last = 0u;
				room_taken = 0u;
			}
		}
		if (room_last > room_first) {
			float room_base = room_first > 0u ? emitter_cdf.data[room_first - 1u] : 0.0;
			float room_span = emitter_cdf.data[room_last - 1u] - room_base;
			float room_target = room_base + di_emitter_hash(p_seed, 0x27d4eb2fu) * room_span;
			low = room_first;
			uint high = room_last - 1u;
			while (low < high) {
				uint mid = (low + high) >> 1u;
				if (emitter_cdf.data[mid] < room_target) {
					low = mid + 1u;
				} else {
					high = mid;
				}
			}
		}
	}
#endif
	if (params.emitter_pick_radius > 0.0) {
		uint nearest = low;
		float nearest_distance = di_emitter_distance(p_vertex, low);
		for (uint attempt = 1u; attempt < 4u; attempt++) {
			if (nearest_distance <= params.emitter_pick_radius) {
				break;
			}
			uint candidate = di_emitter_pick_index(di_emitter_hash(p_seed ^ (0x9e3779b9u * attempt), 0x27d4eb2fu) * total, count);
			float candidate_distance = di_emitter_distance(p_vertex, candidate);
			if (candidate_distance < nearest_distance) {
				nearest = candidate;
				nearest_distance = candidate_distance;
			}
		}
		low = nearest;
	}
	RaySceneTriangleEmitter emitter = ray_scene_triangle_emitter(low);
	vec3 v0 = di_emitter_to_view(emitter.v0);
	vec3 v1 = di_emitter_to_view(emitter.v1);
	vec3 v2 = di_emitter_to_view(emitter.v2);
	vec3 e1 = v1 - v0;
	vec3 e2 = v2 - v0;
	vec3 cross_e1e2 = cross(e2, e1);
	float normal_length = length(cross_e1e2);
	float area = 0.5 * normal_length;
	float luminance = di_emitter_selection_luminance(emitter.material);
	if (!(area > 0.0) || !(luminance > 0.0)) {
		di_emitter_clear_block(p_pixel, 1u);
		return;
	}
	float r1 = di_emitter_hash(p_seed, 0x68e31da4u);
	float r2 = di_emitter_hash(p_seed, 0x1b56c4e9u);
	float root = sqrt(r1);
	float b = root * (1.0 - r2);
	float c = root * r2;
	vec3 sample_point = v0 * (1.0 - root) + v1 * b + v2 * c;
	vec2 emitter_uv = ray_scene_triangle_emitter_uv(emitter.uv_index, vec3(b, c, 0.0));
	vec3 texel_emission = ray_scene_triangle_emitter_emission_texel(emitter.material, emitter_uv);
	vec3 rel = sample_point - p_vertex;
	float distance = length(rel);
	if (!(distance > 1e-5)) {
		di_emitter_clear_block(p_pixel, 2u);
		return;
	}
	vec3 to_light = rel / distance;
	vec3 light_normal = cross_e1e2 / normal_length;
	float cos_light = dot(light_normal, -to_light);
	if ((ray_scene_materials.data[emitter.material].flags & RAY_SCENE_MATERIAL_TWO_SIDED) != 0u) {
		cos_light = abs(cos_light);
	}
	if (!(cos_light > 0.0)) {
		di_emitter_clear_block(p_pixel, room_taken != 0u ? (100u + room_taken) : 3u);
		return;
	}
	float geometric = min(cos_light / max(distance * distance, 1e-8), 60000.0);
	vec3 color = texel_emission * (total / luminance);
	float clamp_scale = di_emitter_clamped_luminance(color, total);
	if (clamp_scale > 0.0) {
		color *= clamp_scale;
	}
	if (params.emitter_visibility != 0u && !di_emitter_visible(p_vertex, p_normal, sample_point, emitter.instance, distance * distance, p_seed)) {
		di_emitter_clear_block(p_pixel, 2u);
		return;
	}
	if (any(isinf(color)) || any(isnan(color))) {
		di_emitter_clear_block(p_pixel, 4u);
		return;
	}
	di_emitter_commit(p_pixel, sample_point, color, geometric, emitter.instance, emitter.uv_index, low);
	imageStore(record_image, ivec3(p_pixel, int(params.tail_samples + 1u)), uvec4(DI_EMITTER_MARK, floatBitsToUint(float(params.tail_samples + 1u)), 0u, 0u));
}

#ifdef DI_EMITTER_TABLE
shared float di_emitter_lane_sums[64];
shared float di_emitter_lane_prefix[64];

float di_emitter_lane_sum(uint p_base, uint p_count) {
	float sum = 0.0;
	for (uint i = 0u; i < DI_EMITTER_ITEMS; i++) {
		uint index = p_base + i;
		if (index >= p_count) {
			break;
		}
		sum += di_emitter_weight(index);
	}
	return sum;
}
#endif
#endif

#define DEBUG_STALE_PRIOR 1u
#define DEBUG_COUNT 2u
#define DEBUG_NO_STAMP 4u
#define RECORD_EXACT_CHECK 0x80000000u
#define RESERVOIRS_OFF 0u
#define RESERVOIRS_U1 2u
#define STREAM_STRATA 1u
#define STREAM_SELECT 2u
#define STREAM_TEMPORAL 3u
#define STREAM_SPATIAL 4u
#define STREAM_OFFSET 5u
#define STREAM_SALT 0xb5297a4du
#define STREAM_STEP 0x9e3779b9u

vec3 vertex = vec3(0.0);
vec3 normal = vec3(0.0, 0.0, 1.0);
vec3 view = vec3(0.0, 0.0, 1.0);
float roughness = 0.0;

uint head_count = 0u;
uint head_slot[MAX_ENTRIES];
float head_rank[MAX_ENTRIES];

bool has_history = false;
ivec2 history_pixel = ivec2(0);
float history_depth = 0.0;
vec3 history_normal = vec3(0.0, 0.0, 1.0);
uint history_count = 0u;
uint history_slot[MAX_ENTRIES];
float history_factor[MAX_ENTRIES];
vec3 history_ratio = vec3(1.0);

uint tail_lamps = 0u;
uint tail_candidates = 0u;
float sum_weight = 0.0;
vec3 sum_target = vec3(0.0);
bool plain_tail = false;
uint plain_candidates = 0u;
float plain_weight = 0.0;
vec3 plain_sum = vec3(0.0);
uint exact_slot[MAX_ENTRIES];
float exact_weight[MAX_ENTRIES];
vec3 exact_target[MAX_ENTRIES];

DiInversion inversions[MAX_ENTRIES];
vec3 pick_target[MAX_ENTRIES];
vec3 last_target = vec3(0.0);

uint kept_count = 0u;
uint kept_slot[MAX_ENTRIES];
float kept_rank[MAX_ENTRIES];

uint stream_seed = 0u;
DiList strata_list;
uint strata_index = 0u;
uint strata_seen = 0u;
uint strata_next = 0u;

uint own_slot = DI_SAMPLER_NO_PICK;
float own_weight = 0.0;
float own_mass = 0.0;
float own_p = 0.0;
vec3 own_target = vec3(0.0);
LightData own_light;
bool own_spot = false;

bool other_present[MAX_NEIGHBORS];
uint other_slot[MAX_NEIGHBORS];
float other_weight[MAX_NEIGHBORS];
float other_mass[MAX_NEIGHBORS];
float other_stored[MAX_NEIGHBORS];
float other_here[MAX_NEIGHBORS];
float other_there[MAX_NEIGHBORS];
vec3 other_target[MAX_NEIGHBORS];

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

float tail_prior(uint p_slot, vec3 p_mean) {
	return plain_tail ? 1.0 : prior(p_slot, p_mean);
}

float stream_uniform(uint p_stream, uint p_index) {
	return float(di_hash(stream_seed ^ di_hash(p_index + STREAM_SALT + p_stream * STREAM_STEP)) >> 8u) / 16777216.0;
}

uint reservoir_arm() {
	return params.reservoirs == RESERVOIRS_U1 ? 1u : 0u;
}

uint reservoir_header(uint p_slot, uint p_mass) {
	return p_slot | (p_mass << 16u) | (reservoir_arm() << 24u) | ((params.counter & 127u) << 25u);
}

bool reservoir_reads(uint p_header, uint p_counter) {
	return (p_header >> 25u) == (p_counter & 127u) && ((p_header >> 24u) & 1u) == reservoir_arm() && ((p_header >> 16u) & 0xFFu) != 0u;
}

bool was_head(uint p_slot) {
	if (!has_history) {
		return false;
	}
	uint count = min(history_count, params.head_entries);
	for (uint i = 0u; i < count; i++) {
		if (history_slot[i] == p_slot) {
			return true;
		}
	}
	return false;
}

bool is_head(uint p_slot) {
	for (uint i = 0u; i < head_count; i++) {
		if (head_slot[i] == p_slot) {
			return true;
		}
	}
	return false;
}

void rank_head(uint p_slot, float p_rank, uint p_limit) {
	uint at = head_count;
	while (at > 0u && (head_rank[at - 1u] < p_rank || (head_rank[at - 1u] == p_rank && head_slot[at - 1u] > p_slot))) {
		at--;
	}
	if (at >= p_limit) {
		return;
	}
	if (head_count < p_limit) {
		head_count++;
	}
	for (uint i = head_count - 1u; i > at; i--) {
		head_slot[i] = head_slot[i - 1u];
		head_rank[i] = head_rank[i - 1u];
	}
	head_slot[at] = p_slot;
	head_rank[at] = p_rank;
}

uint strata_begin(uint p_stratum) {
	return (p_stratum * tail_candidates) / params.c_full;
}

uint strata_position(uint p_stratum) {
	uint begin = strata_begin(p_stratum);
	uint size = strata_begin(p_stratum + 1u) - begin;
	return begin + min(size - 1u, uint(stream_uniform(STREAM_STRATA, p_stratum) * float(size)));
}

void visit_reservoir(LightData p_light, bool p_spot, uint p_pass, uint p_slot, vec3 p_target, float p_weight, bool p_candidate) {
	if (p_pass == 3u) {
		if (!p_candidate) {
			return;
		}
		if (strata_seen == strata_next) {
			uint size = strata_begin(strata_index + 1u) - strata_begin(strata_index);
			di_list_step(strata_list, p_weight * float(size), strata_index, p_slot);
			if (strata_list.pick == p_slot) {
				own_p = p_weight;
				own_target = p_target;
				own_light = p_light;
				own_spot = p_spot;
			}
			strata_index++;
			strata_next = strata_index < params.c_full ? strata_position(strata_index) : DI_SAMPLER_NO_PICK;
		}
		strata_seen++;
		return;
	}
	if (p_candidate) {
		tail_candidates++;
		sum_weight += p_weight;
		sum_target += p_target;
	}
	if (p_slot == own_slot) {
		own_target = p_target;
		own_light = p_light;
		own_spot = p_spot;
	}
	for (uint i = 0u; i < params.spatial_neighbors; i++) {
		if (other_present[i] && other_slot[i] == p_slot) {
			other_here[i] = p_candidate ? p_weight : 0.0;
			other_target[i] = p_target;
		}
	}
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
		vec3 anchor = p_light.position + (params.camera_basis[0].xyz * data.anchor_offset_x + params.camera_basis[1].xyz * data.anchor_offset_y + params.camera_basis[2].xyz * data.anchor_offset_z);
		DiGeometry anchored = di_geometry(anchor, p_light.inv_radius, p_light.attenuation, p_spot, p_light.direction, p_light.cone_angle, p_light.cone_attenuation, vertex);
		float rank = di_luminance(di_screen_target(mean, anchored, normal, view, roughness, p_light.specular_amount));
		if (params.head_rank == 0u) {
			rank_head(slot, rank, params.head_entries);
			return;
		}
		rank *= max(prior(slot, mean), params.floor_value);
		if (was_head(slot)) {
			kept_slot[kept_count] = slot;
			kept_rank[kept_count] = rank;
			kept_count++;
			return;
		}
		rank_head(slot, rank, params.head_entries + 1u);
		return;
	}
	if (is_head(slot)) {
		return;
	}
	vec3 target = di_screen_target(mean, geometry, normal, view, roughness, p_light.specular_amount);
	float weight = di_resample_weight(target, tail_prior(slot, mean), params.floor_value);
	bool candidate = di_candidate(weight);
	if (p_pass >= 3u) {
		visit_reservoir(p_light, p_spot, p_pass, slot, target, weight, candidate);
		return;
	}
	if (p_pass == 1u) {
		if (params.reservoirs == RESERVOIRS_U1) {
			float plain = di_resample_weight(target, 1.0, params.floor_value);
			bool plain_candidate = di_candidate(plain);
			if (plain_candidate) {
				plain_candidates++;
				plain_weight += plain;
				plain_sum += target;
			}
			if (slot == other_slot[0]) {
				other_here[0] = plain_candidate ? plain : 0.0;
				other_target[0] = target;
			}
		} else if (slot == other_slot[0]) {
			other_here[0] = candidate ? weight : 0.0;
			other_target[0] = target;
		}
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
	history_normal = di_oct_decode(unpackSnorm2x16(guide.y));
	if (dot(mat3(params.prev_view_from_view) * normal, history_normal) < 0.9) {
		return;
	}
	history_pixel = pixel;
	history_depth = uintBitsToFloat(guide.x);
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

float plain_target(vec3 p_lamp, vec3 p_direction, vec3 p_vertex, vec3 p_normal, vec3 p_view, float p_roughness) {
	DiGeometry geometry = di_geometry(p_lamp, own_light.inv_radius, own_light.attenuation, own_spot, p_direction, own_light.cone_angle, own_light.cone_attenuation, p_vertex);
	if (!di_in_set(geometry, p_normal)) {
		return 0.0;
	}
	DiSlot data = di_slots.data[own_slot];
	float weight = di_luminance(di_screen_target(vec3(data.mean_r, data.mean_g, data.mean_b), geometry, p_normal, p_view, p_roughness, own_light.specular_amount));
	return di_candidate(weight) ? weight : 0.0;
}

uint combine(uint p_count, uint p_stream, out float r_weight, out uint r_mass, out float r_target) {
	float weights[MAX_NEIGHBORS + 1];
	uint present = 0u;
	float mass = own_mass;
	for (uint i = 0u; i < p_count; i++) {
		weights[i + 1u] = 0.0;
		if (other_present[i]) {
			present++;
			mass += other_mass[i];
		}
	}
#ifdef LAB_COUNTS
	uint dropped = 0u;
	uint dropped_mass = 0u;
	for (uint i = 0u; i < p_count; i++) {
		if (other_present[i] && other_here[i] == 0.0) {
			dropped++;
			dropped_mass += uint(other_mass[i]);
		}
	}
	atomicAdd(lab_counts.words[LAB_PRESENT], present);
	atomicAdd(lab_counts.words[LAB_DROPPED], dropped);
	atomicAdd(lab_counts.words[LAB_DROPPED_MASS], dropped_mass);
	atomicAdd(lab_counts.words[LAB_TOTAL_MASS], uint(mass));
#endif
	weights[0] = 0.0;
	if (params.reservoirs == RESERVOIRS_U1) {
		float share = float(max(present, 1u));
		float own = own_mass * own_p;
		float pairs = 0.0;
		for (uint i = 0u; i < p_count; i++) {
			if (!other_present[i]) {
				continue;
			}
			float there = other_mass[i] * other_there[i];
			pairs += there == 0.0 ? 1.0 : own / (own + there);
			if (other_here[i] == 0.0) {
				continue;
			}
			float a = other_mass[i] * other_stored[i];
			float b = own_mass * other_here[i];
			weights[i + 1u] = a / ((a + b) * share) * other_here[i] * other_weight[i];
		}
		if (own_mass > 0.0) {
			weights[0] = (present == 0u ? 1.0 : pairs / share) * own_p * own_weight;
		}
	} else if (mass > 0.0) {
		if (own_mass > 0.0) {
			weights[0] = own_mass / mass * own_p * own_weight;
		}
		for (uint i = 0u; i < p_count; i++) {
			if (other_present[i] && other_here[i] != 0.0) {
				weights[i + 1u] = other_mass[i] / mass * other_here[i] * other_weight[i];
			}
		}
	}
	float sum = 0.0;
	for (uint i = 0u; i <= p_count; i++) {
		sum += weights[i];
	}
	DiInversion inversion = di_inversion_begin(sum, stream_uniform(p_stream, 0u));
	for (uint i = 0u; i <= p_count; i++) {
		di_inversion_step(inversion, weights[i], i);
	}
	uint pick = di_inversion_end(inversion);
	r_weight = 0.0;
	r_mass = 0u;
	r_target = 0.0;
	if (pick == DI_SAMPLER_NO_PICK) {
		return pick;
	}
	r_target = pick == 0u ? own_p : other_here[pick - 1u];
	r_weight = sum / r_target;
	r_mass = min(uint(mass), params.m_stored_cap);
	return pick;
}

#ifndef MODE_SPATIAL
bool head_fresh(uint p_slot) {
	uint reset = di_slots.data[p_slot].reset_frame;
	return params.head_window != 0u ? params.frame - reset < params.head_swap_frames : reset == params.frame;
}

void settle_head(ivec2 p_pixel) {
	uint fill_count = head_count;
	uint fill_slot[MAX_ENTRIES] = head_slot;
	float fill_rank[MAX_ENTRIES] = head_rank;
	head_count = 0u;
	for (uint i = 0u; i < kept_count && head_count < params.head_entries; i++) {
		head_slot[head_count] = kept_slot[i];
		head_rank[head_count] = kept_rank[i];
		head_count++;
	}
	uint used = 0u;
	while (head_count < params.head_entries && used < fill_count) {
		head_slot[head_count] = fill_slot[used];
		head_rank[head_count] = fill_rank[used];
		head_count++;
		used++;
	}
	uint state = 0u;
	if (has_history && params.head_entries > 0u && head_count == params.head_entries && used < fill_count) {
		uint weakest = 0u;
		for (uint i = 1u; i < head_count; i++) {
			if (head_rank[i] < head_rank[weakest] || (head_rank[i] == head_rank[weakest] && head_slot[i] > head_slot[weakest])) {
				weakest = i;
			}
		}
		uint slot = fill_slot[used];
		float rank = fill_rank[used];
		bool above = rank > head_rank[weakest] || (rank == head_rank[weakest] && slot < head_slot[weakest]);
		if (above && rank >= (1.0 + params.head_swap_margin) * head_rank[weakest]) {
			uint old = imageLoad(head_state_previous, history_pixel).r;
			uint count = ((old & 0xFFFFu) == slot ? (old >> 16u) & 7u : 0u) + 1u;
			if (count >= params.head_swap_frames || head_fresh(slot) || head_fresh(head_slot[weakest])) {
				head_slot[weakest] = slot;
				head_rank[weakest] = rank;
			} else {
				state = slot | (min(count, 7u) << 16u);
			}
		}
	}
	imageStore(head_state, p_pixel, uvec4(state, 0u, 0u, 0u));
}

float history_target() {
	if (was_head(own_slot)) {
		return 0.0;
	}
	vec2 uv = (vec2(history_pixel) + 0.5) * params.pixel_size;
	vec4 ray_h = params.prev_inv_projection * vec4(uv * 2.0 - 1.0, 0.5, 1.0);
	vec3 ray = ray_h.xyz / ray_h.w;
	vec3 point = ray * (history_depth / max(-ray.z, 1e-6));
	vec3 lamp = (params.prev_view_from_view * vec4(own_light.position, 1.0)).xyz;
	vec3 direction = mat3(params.prev_view_from_view) * own_light.direction;
	return plain_target(lamp, direction, point, history_normal, -normalize(point), roughness);
}

uint result_slot = 0u;
float result_weight = 0.0;
vec3 result_target = vec3(0.0);

void resample_reservoir(ivec2 p_pixel, uint p_cluster_offset, uint p_cluster_z) {
	strata_list = di_list_begin(stream_seed ^ di_hash(STREAM_SALT + STREAM_SELECT * STREAM_STEP));
	strata_index = 0u;
	strata_seen = 0u;
	strata_next = strata_position(0u);
	walk(p_cluster_offset, p_cluster_z, false, 3u);
	walk(p_cluster_offset + params.cluster_type_size, p_cluster_z, true, 3u);
	own_slot = strata_list.pick;
	if (own_slot != DI_SAMPLER_NO_PICK) {
		own_weight = strata_list.total / own_p;
		own_mass = float(params.c_full);
	}
	if (other_present[0]) {
		uint cap = params.m_temporal_cap;
		if (di_slots.data[other_slot[0]].dynamic != 0u) {
			cap = min(cap, params.dynamic_cap);
#ifdef LAB_COUNTS
			atomicAdd(lab_counts.words[LAB_DYNAMIC_CAPPED], 1u);
#endif
		}
		other_mass[0] = min(other_mass[0], float(cap));
		if (params.reservoirs == RESERVOIRS_U1 && own_slot != DI_SAMPLER_NO_PICK) {
			other_there[0] = history_target();
		}
	}
	float weight;
	uint mass;
	float target;
	uint pick = combine(1u, STREAM_TEMPORAL, weight, mass, target);
	if (pick != DI_SAMPLER_NO_PICK) {
		result_slot = pick == 0u ? own_slot : other_slot[0];
		result_weight = weight;
		result_target = pick == 0u ? own_target : other_target[0];
	}
	imageStore(current_header, p_pixel, uvec4(reservoir_header(result_slot, mass), 0u, 0u, 0u));
	imageStore(current_values, p_pixel, uvec4(floatBitsToUint(weight), floatBitsToUint(target), 0u, 0u));
	imageStore(tile_flags, p_pixel >> 3, uvec4(params.counter, 0u, 0u, 0u));
}
#endif

#ifdef DI_EMITTER_TEMPORAL
vec4 di_emitter_history_load(ivec2 p_pixel, int p_layer) {
	uvec4 value = imageLoad(emitter_history, ivec3(p_pixel, p_layer));
	return vec4(uintBitsToFloat(value.x), uintBitsToFloat(value.y), uintBitsToFloat(value.z), uintBitsToFloat(value.w));
}

bool di_emitter_history_valid(ivec2 p_pixel, vec3 p_vertex, vec3 p_normal, inout uvec4 r_header, inout uvec4 r_surface) {
	if (params.history_valid == 0u) {
		return false;
	}
	r_header = imageLoad(emitter_history, ivec3(p_pixel, 0));
	if ((r_header.y & 0x100u) == 0u) {
		return false;
	}
	r_surface = imageLoad(emitter_history, ivec3(p_pixel, 3));
	float expected = -(params.prev_view_from_view * vec4(p_vertex, 1.0)).z;
	float tolerance = expected * (0.01 + 0.02 / max(dot(p_normal, view), 0.05));
	float stored_depth = uintBitsToFloat(r_surface.y);
	vec3 stored_normal = di_oct_decode(unpackSnorm2x16(r_surface.x));
	if (!(expected > 0.0 && abs(stored_depth - expected) < tolerance)) {
		atomicAdd(emitter_reuse_counters.depth_reject, 1u);
		return false;
	}
	if (dot(mat3(params.prev_view_from_view) * p_normal, stored_normal) <= 0.9) {
		atomicAdd(emitter_reuse_counters.normal_reject, 1u);
		return false;
	}
	return true;
}

void di_emitter_history_store(ivec2 p_pixel, uvec4 p_header, vec4 p_point, vec4 p_color, uvec4 p_surface) {
	imageStore(emitter_history, ivec3(p_pixel, 0), p_header);
	imageStore(emitter_history, ivec3(p_pixel, 1), uvec4(floatBitsToUint(p_point.x), floatBitsToUint(p_point.y), floatBitsToUint(p_point.z), floatBitsToUint(p_point.w)));
	imageStore(emitter_history, ivec3(p_pixel, 2), uvec4(floatBitsToUint(p_color.x), floatBitsToUint(p_color.y), floatBitsToUint(p_color.z), 0u));
	imageStore(emitter_history, ivec3(p_pixel, 3), p_surface);
}

vec2 di_emitter_uv_at(RaySceneTriangleEmitter p_emitter, vec3 p_point) {
	vec3 point = di_emitter_to_view(p_point);
	vec3 v0 = di_emitter_to_view(p_emitter.v0);
	vec3 v1 = di_emitter_to_view(p_emitter.v1);
	vec3 v2 = di_emitter_to_view(p_emitter.v2);
	vec3 e1 = v1 - v0;
	vec3 e2 = v2 - v0;
	vec3 ep = point - v0;
	float d11 = dot(e1, e1);
	float d12 = dot(e1, e2);
	float d22 = dot(e2, e2);
	float dp1 = dot(ep, e1);
	float dp2 = dot(ep, e2);
	float denom = d11 * d22 - d12 * d12;
	if (abs(denom) < 1e-20) {
		return vec2(-1.0);
	}
	float v = (d22 * dp1 - d12 * dp2) / denom;
	float w = (d11 * dp2 - d12 * dp1) / denom;
	return ray_scene_triangle_emitter_uv(p_emitter.uv_index, vec3(v, w, 0.0));
}

vec3 di_emitter_color_at(uint p_index, vec3 p_point, out float r_luminance_weight) {
	RaySceneTriangleEmitter emitter = ray_scene_triangle_emitter(p_index);
	vec2 uv = di_emitter_uv_at(emitter, p_point);
	vec3 emission = uv == vec2(-1.0) ? ray_scene_triangle_emitter_emission(emitter.material) : ray_scene_triangle_emitter_emission_texel(emitter.material, uv);
	float luminance = dot(max(emission, vec3(0.0)), luminance_weights);
	float selection = di_emitter_selection_luminance(emitter.material);
	r_luminance_weight = luminance;
	if (!(luminance > 0.0 && selection > 0.0)) {
		return vec3(0.0);
	}
	float total = emitter_scan.data[DI_EMITTER_TOTAL];
	vec3 color = emission * (total / selection);
	float clamp_scale = di_emitter_clamped_luminance(color, total);
	return clamp_scale > 0.0 ? color * clamp_scale : color;
}

float di_emitter_target(vec3 p_color, float p_geometric) {
	return dot(max(p_color * p_geometric, vec3(0.0)), luminance_weights);
}



void di_emitter_accumulate(ivec2 p_pixel, vec3 p_vertex, vec3 p_normal) {
	vec3 new_point = di_emitter_point;
	vec3 new_color = di_emitter_color;
	float new_geometric = di_emitter_geometric;
	float new_target = di_emitter_target(new_color, new_geometric);
	float held_target = 0.0;
	float held_origin = 0.0;
	float held_m = 0.0;
	uint held_age = 0u;
	uint held_index = 0u;
	uint held_instance = 0u;
	vec3 held_point = vec3(0.0);
	vec3 held_color = vec3(0.0);
	float held_geometric = 0.0;
	bool merge = false;
	if (params.history_valid != 0u) {
		uvec4 header = uvec4(0u);
		uvec4 surface = uvec4(0u);
		if (di_emitter_history_valid(p_pixel, p_vertex, p_normal, header, surface)) {
			held_index = header.x & 0xFFFFu;
			held_instance = header.x >> 16u;
			held_age = header.y & 0xFFu;
			held_origin = uintBitsToFloat(header.w);
			RaySceneTriangleEmitter emitter = ray_scene_triangle_emitter(held_index);
			if (emitter.instance != held_instance) {
				atomicAdd(emitter_reuse_counters.instance_mismatch, 1u);
			} else {
				if (held_instance != (di_emitter_instance & 0xFFFFu)) {
					atomicAdd(emitter_reuse_counters.instance_mismatch, 1u);
				}
				vec4 held_vec = di_emitter_history_load(p_pixel, 1);
				held_point = (inverse(params.prev_view_from_view) * vec4(held_vec.xyz, 1.0)).xyz;
				vec3 v0 = di_emitter_to_view(emitter.v0);
				vec3 v1 = di_emitter_to_view(emitter.v1);
				vec3 v2 = di_emitter_to_view(emitter.v2);
				vec3 light_normal = cross(v2 - v0, v1 - v0);
				float normal_length = length(light_normal);
				light_normal = normal_length > 0.0 ? light_normal / normal_length : vec3(0.0, 0.0, 1.0);
				vec3 rel = held_point - p_vertex;
				float distance_sq = dot(rel, rel);
				vec3 to_light = distance_sq > 1e-12 ? rel * inversesqrt(distance_sq) : vec3(0.0);
				float cos_light = dot(light_normal, -to_light);
				if ((ray_scene_materials.data[emitter.material].flags & RAY_SCENE_MATERIAL_TWO_SIDED) != 0u) {
					cos_light = abs(cos_light);
				}
				held_geometric = cos_light > 0.0 ? min(cos_light / max(distance_sq, 1e-8), 60000.0) : 0.0;
				float held_luminance = 0.0;
				held_color = di_emitter_color_at(held_index, held_point, held_luminance);
				held_target = di_emitter_target(held_color, held_geometric);
				held_m = min(uintBitsToFloat(surface.z), 20.0);				merge = held_m > 0.0 && held_geometric > 0.0;
			}
		}
	}

	vec3 point = new_point;
	vec3 color = new_color;
	float geometric = new_geometric;
	float target = new_target;
	uint age = 0u;
	uint index = di_emitter_index;
	uint instance = di_emitter_instance;
	uint count = 1u;
	if (merge) {
		float total = 1.0 + held_m;
		color = (held_color * held_m + new_color) / total;
		geometric = (held_geometric * held_m + new_geometric) / total;
		target = di_emitter_target(color, geometric);
		count = uint(min(total, 21.0));
		age = min(held_age + 1u, 0xFFu);
		atomicAdd(emitter_reuse_counters.temporal_accepted, 1u);
		atomicAdd(emitter_reuse_counters.age_sum, age);
		atomicAdd(emitter_reuse_counters.mass_sum, count);
	}
	float damp = 1.0;
	atomicAdd(emitter_reuse_counters.damp_sum, uint(damp * 65536.0));

	uvec4 header = uvec4((index & 0xFFFFu) | ((instance & 0xFFFFu) << 16u), age | 0x100u, 0u, floatBitsToUint(target));
	uvec4 surface = uvec4(packSnorm2x16(di_oct_encode(p_normal)), floatBitsToUint(-p_vertex.z), floatBitsToUint(float(count)), 0u);
	di_emitter_history_store(p_pixel, header, vec4(point, geometric), vec4(color, damp), surface);
	di_emitter_write(p_pixel, point, color, geometric);
	imageStore(record_image, ivec3(p_pixel, int(params.tail_samples + 1u)), uvec4(DI_EMITTER_MARK, floatBitsToUint(float(params.tail_samples + 1u) * damp), 0u, 0u));
}

bool di_emitter_hold(ivec2 p_pixel, vec3 p_vertex, vec3 p_normal) {
	uvec4 header = uvec4(0u);
	uvec4 surface = uvec4(0u);
	if (!di_emitter_history_valid(p_pixel, p_vertex, p_normal, header, surface)) {
		return false;
	}
	uint index = header.x & 0xFFFFu;
	uint instance = header.x >> 16u;
	RaySceneTriangleEmitter emitter = ray_scene_triangle_emitter(index);
	if (emitter.instance != instance) {
		return false;
	}
	vec4 held_vec = di_emitter_history_load(p_pixel, 1);
	vec4 held_tail = di_emitter_history_load(p_pixel, 2);
	vec3 point = (inverse(params.prev_view_from_view) * vec4(held_vec.xyz, 1.0)).xyz;
	float geometric = held_vec.w;
	vec3 color = held_tail.rgb;
	float target = di_emitter_target(color, geometric);	uint age = min((header.y & 0xFFu) + 1u, 0xFFu);
	float mass = uintBitsToFloat(surface.z);
	float damp = held_tail.w;
	uvec4 next_header = uvec4(header.x, age | 0x100u, header.z, floatBitsToUint(target));
	uvec4 next_surface = uvec4(surface.x, surface.y, surface.z, 0u);
	di_emitter_history_store(p_pixel, next_header, vec4(point, geometric), vec4(color, damp), next_surface);
	di_emitter_write(p_pixel, point, color, geometric);
	imageStore(record_image, ivec3(p_pixel, int(params.tail_samples + 1u)), uvec4(DI_EMITTER_MARK, floatBitsToUint(float(params.tail_samples + 1u) * damp), 0u, 0u));
	atomicAdd(emitter_reuse_counters.held_only, 1u);
	atomicAdd(emitter_reuse_counters.age_sum, age);
	atomicAdd(emitter_reuse_counters.mass_sum, uint(mass));
	return true;
}
#endif

void write_empty(ivec2 p_pixel) {
#ifdef DI_EMITTERS
	di_emitter_clear_block(p_pixel, 0u);
#ifdef DI_EMITTER_TEMPORAL
	di_emitter_history_store(p_pixel, uvec4(0u), vec4(0.0), vec4(0.0), uvec4(0u));
#endif
#endif
#ifndef MODE_SPATIAL
	if (params.head_rank != 0u) {
		imageStore(head_state, p_pixel, uvec4(0u));
	}
#endif
	imageStore(record_image, ivec3(p_pixel, 0), uvec4(0u));
	for (uint i = 0u; i < params.head_entries; i++) {
		imageStore(entry_image, ivec3(p_pixel, int(i)), uvec4(0u));
	}
	for (uint k = 0u; k < params.tail_samples; k++) {
		write_sample(p_pixel, k, 0u, 0.0, 0.0, vec3(0.0));
	}
}

uint surface_packed_normal = 0u;
float surface_depth = 0.0;

bool surface_at(ivec2 p_pixel, out vec3 r_vertex, out vec3 r_normal, out float r_roughness, out uint r_packed_normal, out float r_depth) {
	r_depth = texelFetch(depth_buffer, p_pixel, 0).r;
	if (r_depth <= 0.0) {
		return false;
	}
	vec2 uv = (vec2(p_pixel) + 0.5) * params.pixel_size;
	vec4 view_h = params.inv_projection * vec4(uv * 2.0 - 1.0, r_depth, 1.0);
	r_vertex = view_h.xyz / view_h.w;
	vec4 normal_roughness = texelFetch(normal_roughness_buffer, p_pixel, 0);
	r_packed_normal = packSnorm2x16(di_oct_encode(normalize(normal_roughness.xyz * 2.0 - 1.0)));
	r_normal = di_oct_decode(unpackSnorm2x16(r_packed_normal));
	r_roughness = normal_roughness.w;
	if (r_roughness > 0.5) {
		r_roughness = 1.0 - r_roughness;
	}
	r_roughness /= 127.0 / 255.0;
	return true;
}

#ifdef DI_EMITTER_TABLE
#if defined(MODE_EMITTER_SUM)
void main() {
	uint chunk = gl_WorkGroupID.x;
	uint lane = gl_LocalInvocationIndex;
	uint count = ray_scene_triangle_emitter_count();
	di_emitter_lane_sums[lane] = di_emitter_lane_sum(chunk * DI_EMITTER_CHUNK + lane * DI_EMITTER_ITEMS, count);
	barrier();
	if (lane == 0u) {
		float total = 0.0;
		for (uint i = 0u; i < 64u; i++) {
			total += di_emitter_lane_sums[i];
		}
		emitter_scan.data[DI_EMITTER_SUMS + chunk] = total;
	}
}
#elif defined(MODE_EMITTER_SCAN)
void main() {
	if (gl_LocalInvocationIndex == 0u) {
		uint count = ray_scene_triangle_emitter_count();
		uint chunks = (count + DI_EMITTER_CHUNK - 1u) / DI_EMITTER_CHUNK;
		float running = 0.0;
		for (uint chunk = 0u; chunk < 256u; chunk++) {
			if (chunk >= chunks) {
				break;
			}
			emitter_scan.data[DI_EMITTER_PREFIX + chunk] = running;
			running += emitter_scan.data[DI_EMITTER_SUMS + chunk];
		}
		emitter_scan.data[DI_EMITTER_TOTAL] = running;
	}
}
#else
void main() {
	uint chunk = gl_WorkGroupID.x;
	uint lane = gl_LocalInvocationIndex;
	uint count = ray_scene_triangle_emitter_count();
	uint base = chunk * DI_EMITTER_CHUNK + lane * DI_EMITTER_ITEMS;
	di_emitter_lane_sums[lane] = di_emitter_lane_sum(base, count);
	barrier();
	if (lane == 0u) {
		float running = 0.0;
		for (uint i = 0u; i < 64u; i++) {
			di_emitter_lane_prefix[i] = running;
			running += di_emitter_lane_sums[i];
		}
	}
	barrier();
	float running = emitter_scan.data[DI_EMITTER_PREFIX + chunk] + di_emitter_lane_prefix[lane];
	for (uint i = 0u; i < DI_EMITTER_ITEMS; i++) {
		uint index = base + i;
		if (index >= count) {
			break;
		}
		running += di_emitter_weight(index);
		emitter_cdf.data[index] = running;
	}
}
#endif
#else
#if defined(MODE_EMITTER_SPATIAL)
float di_emitter_sample_mass(vec4 p_color, vec4 p_point) {
	return dot(max(p_color.rgb, vec3(0.0)), vec3(0.2126, 0.7152, 0.0722)) * max(p_point.w, 0.0);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) {
		return;
	}
	if (imageLoad(tile_flags, pixel >> 3).r != params.counter) {
		return;
	}
	vec3 vertex = vec3(0.0);
	vec3 normal = vec3(0.0, 0.0, 1.0);
	float roughness = 0.0;
	uint packed_normal = 0u;
	float depth = 0.0;
	if (!surface_at(pixel, vertex, normal, roughness, packed_normal, depth)) {
		return;
	}
	uint index = uint(pixel.y) * uint(params.size.x) + uint(pixel.x);
	vec4 selected_point = emitter_samples.data[index * 2u];
	vec4 selected_color = emitter_samples.data[index * 2u + 1u];
	float total_mass = di_emitter_sample_mass(selected_color, selected_point);
	vec3 total_weighted = max(selected_color.rgb, vec3(0.0)) * total_mass;
	float view_depth = max(-vertex.z, 0.001);
	float tolerance = view_depth * 0.05;
	uint seed = di_hash(uint(pixel.y) * 65537u + uint(pixel.x)) ^ di_hash(params.frame ^ 0x51ed270bu);
	uint neighbours = uint(params.spatial_neighbors);
	for (uint i = 0u; i < neighbours; i++) {
		float angle = (float(i) + di_emitter_hash(seed, i * 2u + 1u)) * 6.2831853 / float(max(neighbours, 1u));
		float radius = sqrt(di_emitter_hash(seed, i * 2u + 2u)) * params.spatial_radius;
		ivec2 other = pixel + ivec2(round(cos(angle) * radius), round(sin(angle) * radius));
		if (any(lessThan(other, ivec2(0))) || any(greaterThanEqual(other, params.size))) {
			continue;
		}
		vec3 other_vertex = vec3(0.0);
		vec3 other_normal = vec3(0.0, 0.0, 1.0);
		float other_roughness = 0.0;
		uint other_packed = 0u;
		float other_depth = 0.0;
		if (!surface_at(other, other_vertex, other_normal, other_roughness, other_packed, other_depth)) {
			continue;
		}
		if (abs(other_depth - depth) > tolerance) {
			continue;
		}
		if (dot(normal, other_normal) < 0.9) {
			continue;
		}
		uint other_index = uint(other.y) * uint(params.size.x) + uint(other.x);
		vec4 other_point = emitter_samples.data[other_index * 2u];
		vec4 other_color = emitter_samples.data[other_index * 2u + 1u];
		float other_mass = di_emitter_sample_mass(other_color, other_point);
		if (!(other_mass > 0.0)) {
			continue;
		}
		total_mass += other_mass;
		total_weighted += max(other_color.rgb, vec3(0.0)) * other_mass;
		if (di_emitter_hash(seed, i * 2u + 3u) * total_mass < other_mass) {
			selected_point = other_point;
			selected_color = other_color;
		}
	}
	if (!(total_mass > 0.0)) {
		return;
	}
	emitter_samples.data[index * 2u] = selected_point;
	emitter_samples.data[index * 2u + 1u] = vec4(total_weighted / total_mass, 0.0);
}
#elif !defined(MODE_SPATIAL)
void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) {
		return;
	}
	if (!surface_at(pixel, vertex, normal, roughness, surface_packed_normal, surface_depth) || any(greaterThan(abs(vec3(dot(params.camera_basis[0].xyz, vertex), dot(params.camera_basis[1].xyz, vertex), dot(params.camera_basis[2].xyz, vertex))), params.region_half))) {
		write_empty(pixel);
		return;
	}
	view = -normalize(vertex);

	load_history();

	for (uint i = 0u; i < MAX_NEIGHBORS; i++) {
		other_present[i] = false;
		other_slot[i] = DI_SAMPLER_NO_PICK;
		other_here[i] = 0.0;
		other_there[i] = 0.0;
		other_target[i] = vec3(0.0);
	}
	stream_seed = di_hash(uint(pixel.y) * 65537u + uint(pixel.x)) ^ di_hash(params.frame ^ 0x68e31da4u);
#ifdef DI_EMITTERS
#ifndef DI_EMITTER_TEMPORAL
	bool di_emitter_origin = params.emitter_half == 0u || (((pixel.x & 1) == 0) && ((pixel.y & 1) == 0));
	if (di_emitter_origin) {
		di_emitter_pick(pixel, vertex, normal, stream_seed ^ 0x2545f491u);
	}
#else
	di_emitter_pick(pixel, vertex, normal, stream_seed ^ 0x2545f491u);
#endif
#ifdef DI_EMITTER_TEMPORAL
	if (di_emitter_pending) {
		di_emitter_accumulate(pixel, vertex, normal);
	} else if (!di_emitter_hold(pixel, vertex, normal)) {
		di_emitter_clear(pixel, 1u);
		di_emitter_history_store(pixel, uvec4(0u), vec4(0.0), vec4(0.0), uvec4(0u));
	}
#endif
#endif
	if (params.reservoirs != RESERVOIRS_OFF && params.temporal_reuse != 0u && has_history) {
		uint held = imageLoad(history_header, history_pixel).r;
#ifdef LAB_COUNTS
		if ((params.debug_flags & DEBUG_NO_STAMP) != 0u ? (((held >> 24u) & 1u) == reservoir_arm() && ((held >> 16u) & 0xFFu) != 0u) : reservoir_reads(held, params.counter - 1u)) {
#else
		if (reservoir_reads(held, params.counter - 1u)) {
#endif
			uvec2 values = imageLoad(history_values, history_pixel).xy;
			other_present[0] = true;
			other_slot[0] = held & 0xFFFFu;
			other_mass[0] = float((held >> 16u) & 0xFFu);
			other_weight[0] = uintBitsToFloat(values.x);
			other_stored[0] = uintBitsToFloat(values.y);
		}
	}

	uvec2 cluster_pos = uvec2(pixel) >> params.cluster_shift;
	uint cluster_offset = (params.cluster_width * cluster_pos.y + cluster_pos.x) * (params.max_cluster_element_count_div_32 + 32u);
	uint cluster_z = uint(clamp((-vertex.z / params.z_far) * 32.0, 0.0, 31.0));
	for (uint walk_pass = 0u; walk_pass < 2u; walk_pass++) {
		walk(cluster_offset, cluster_z, false, walk_pass);
		walk(cluster_offset + params.cluster_type_size, cluster_z, true, walk_pass);
		if (walk_pass == 0u && params.head_rank != 0u) {
			settle_head(pixel);
		}
	}

	uint tails = params.tail_samples;
	bool unbiased = params.reservoirs == RESERVOIRS_U1;
	bool reservoir = params.reservoirs != RESERVOIRS_OFF && (unbiased ? plain_candidates : tail_candidates) > params.c_full;
	if (reservoir && unbiased) {
		plain_tail = true;
		tail_candidates = plain_candidates;
		sum_weight = plain_weight;
		sum_target = plain_sum;
	}
	if (reservoir) {
#ifdef LAB_COUNTS
		atomicAdd(lab_counts.words[LAB_PATH], 1u);
		uint lab_index = uint(pixel.y) * uint(params.size.x) + uint(pixel.x);
		if ((lab_index >> 5u) < uint(lab_mask.bits.length())) {
			atomicOr(lab_mask.bits[lab_index >> 5u], 1u << (lab_index & 31u));
		}
#endif
		resample_reservoir(pixel, cluster_offset, cluster_z);
	}
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
	if (!exact && !reservoir) {
		for (uint k = 0u; k < tails; k++) {
			inversions[k] = di_inversion_begin(sum_weight, stream_uniform(0u, k));
			pick_target[k] = vec3(0.0);
		}
		walk(cluster_offset, cluster_z, false, 2u);
		walk(cluster_offset + params.cluster_type_size, cluster_z, true, 2u);
	}

	imageStore(record_image, ivec3(pixel, 0), uvec4(surface_packed_normal, floatBitsToUint(surface_depth) | record_flag, 0u, 0u));
	for (uint i = 0u; i < params.head_entries; i++) {
		imageStore(entry_image, ivec3(pixel, int(i)), uvec4(i < head_count ? head_slot[i] : 0u, 0u, 0u, 0u));
	}
	if (reservoir) {
		if (params.spatial_neighbors == 0u) {
			if (result_slot == 0u) {
				write_sample(pixel, 0u, 0u, 0.0, 0.0, vec3(0.0));
			} else {
				write_sample(pixel, 0u, result_slot, result_weight, sum_weight, rest_scale(result_target, result_weight));
#ifdef LAB_COUNTS
				atomicAdd(lab_counts.finals[result_slot], 1u);
#endif
			}
		}
		return;
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
#else
float neighbour_target(ivec2 p_pixel, vec3 p_vertex, vec3 p_normal, float p_roughness) {
	for (uint i = 0u; i < params.head_entries; i++) {
		if ((imageLoad(entry_image, ivec3(p_pixel, int(i))).r & 0xFFFFu) == own_slot) {
			return 0.0;
		}
	}
	return plain_target(own_light.position, own_light.direction, p_vertex, p_normal, -normalize(p_vertex), p_roughness);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.size))) {
		return;
	}
	if (imageLoad(tile_flags, pixel >> 3).r != params.counter) {
		return;
	}
	uint header = imageLoad(current_header, pixel).r;
	if ((header >> 25u) != (params.counter & 127u) || ((header >> 24u) & 1u) != reservoir_arm()) {
		return;
	}
	if (!surface_at(pixel, vertex, normal, roughness, surface_packed_normal, surface_depth)) {
		return;
	}
	view = -normalize(vertex);
	if (params.reservoirs != RESERVOIRS_U1) {
		load_history();
	} else {
		plain_tail = true;
	}
	head_count = params.head_entries;
	for (uint i = 0u; i < params.head_entries; i++) {
		head_slot[i] = imageLoad(entry_image, ivec3(pixel, int(i))).r & 0xFFFFu;
	}

	uvec2 values = imageLoad(current_values, pixel).xy;
	own_mass = float((header >> 16u) & 0xFFu);
	own_slot = own_mass > 0.0 ? header & 0xFFFFu : DI_SAMPLER_NO_PICK;
	own_weight = uintBitsToFloat(values.x);
	own_p = uintBitsToFloat(values.y);
	stream_seed = di_hash(uint(pixel.y) * 65537u + uint(pixel.x)) ^ di_hash(params.frame ^ 0x68e31da4u);

	ivec2 other_pixel[MAX_NEIGHBORS];
	vec3 other_vertex[MAX_NEIGHBORS];
	vec3 other_normal[MAX_NEIGHBORS];
	float other_roughness[MAX_NEIGHBORS];
	float expected = -vertex.z;
	float tolerance = expected * (0.01 + 0.02 / max(dot(normal, view), 0.05));
	for (uint i = 0u; i < MAX_NEIGHBORS; i++) {
		other_present[i] = false;
		other_slot[i] = DI_SAMPLER_NO_PICK;
		other_here[i] = 0.0;
		other_there[i] = 0.0;
		other_target[i] = vec3(0.0);
		if (i >= params.spatial_neighbors) {
			continue;
		}
		float reach = params.spatial_radius * sqrt(stream_uniform(STREAM_OFFSET, 2u * i));
		float angle = 6.28318530718 * stream_uniform(STREAM_OFFSET, 2u * i + 1u);
		ivec2 offset = ivec2(round(reach * vec2(cos(angle), sin(angle))));
		ivec2 at = pixel + offset;
		if (offset == ivec2(0) || any(lessThan(at, ivec2(0))) || any(greaterThanEqual(at, params.size))) {
			continue;
		}
		uint other = imageLoad(current_header, at).r;
		if (!reservoir_reads(other, params.counter)) {
			continue;
		}
		if (!surface_at(at, other_vertex[i], other_normal[i], other_roughness[i], surface_packed_normal, surface_depth)) {
			continue;
		}
		if (abs(-other_vertex[i].z - expected) > tolerance || dot(normal, other_normal[i]) < 0.9) {
			continue;
		}
		uvec2 held = imageLoad(current_values, at).xy;
		uint mass = (other >> 16u) & 0xFFu;
		other_present[i] = true;
		other_pixel[i] = at;
		other_slot[i] = other & 0xFFFFu;
		if (di_slots.data[other_slot[i]].dynamic != 0u) {
			mass = min(mass, params.dynamic_cap);
#ifdef LAB_COUNTS
			atomicAdd(lab_counts.words[LAB_DYNAMIC_CAPPED], 1u);
#endif
		}
		other_mass[i] = float(mass);
		other_weight[i] = uintBitsToFloat(held.x);
		other_stored[i] = uintBitsToFloat(held.y);
	}

	uvec2 cluster_pos = uvec2(pixel) >> params.cluster_shift;
	uint cluster_offset = (params.cluster_width * cluster_pos.y + cluster_pos.x) * (params.max_cluster_element_count_div_32 + 32u);
	uint cluster_z = uint(clamp((-vertex.z / params.z_far) * 32.0, 0.0, 31.0));
	walk(cluster_offset, cluster_z, false, 4u);
	walk(cluster_offset + params.cluster_type_size, cluster_z, true, 4u);

	if (params.reservoirs == RESERVOIRS_U1 && own_slot != DI_SAMPLER_NO_PICK) {
		for (uint i = 0u; i < params.spatial_neighbors; i++) {
			if (other_present[i]) {
				other_there[i] = neighbour_target(other_pixel[i], other_vertex[i], other_normal[i], other_roughness[i]);
			}
		}
	}

	float weight;
	uint mass;
	float target;
	uint pick = combine(params.spatial_neighbors, STREAM_SPATIAL, weight, mass, target);
	uint slot = 0u;
	if (pick == DI_SAMPLER_NO_PICK) {
		write_sample(pixel, 0u, 0u, 0.0, 0.0, vec3(0.0));
	} else {
		slot = pick == 0u ? own_slot : other_slot[pick - 1u];
		write_sample(pixel, 0u, slot, weight, sum_weight, rest_scale(pick == 0u ? own_target : other_target[pick - 1u], weight));
#ifdef LAB_COUNTS
		atomicAdd(lab_counts.finals[slot], 1u);
#endif
	}
	if (params.reservoirs == RESERVOIRS_U1) {
		imageStore(history_header, pixel, uvec4(reservoir_header(slot, mass), 0u, 0u, 0u));
		imageStore(history_values, pixel, uvec4(floatBitsToUint(weight), floatBitsToUint(target), 0u, 0u));
	}
}
#endif
#endif
