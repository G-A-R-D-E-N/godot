#define SURFEL_FLAG_ALIVE 1u
#define SURFEL_FLAG_KILL 2u
#define SURFEL_MAX_PER_LOOKUP 128u
#define SURFEL_FULL_WEIGHT 0.3
#define SURFEL_CONFIDENT_SAMPLES 4.0
#define SURFEL_SPAWN_COVERAGE 0.75
#define SURFEL_REACH 1.5
#define SURFEL_CELL_RADII 4.0
#define SURFEL_INVALID 0xFFFFFFFFu
#define SURFEL_NO_CLIP 0xFFFFFFFFu

const vec2 surfel_clip_directions[8] = vec2[](vec2(1.0, 0.0), vec2(0.70710678, 0.70710678), vec2(0.0, 1.0), vec2(-0.70710678, 0.70710678), vec2(-1.0, 0.0), vec2(-0.70710678, -0.70710678), vec2(0.0, -1.0), vec2(0.70710678, -0.70710678));

struct Surfel {
	vec3 position;
	float radius;
	vec3 normal;
	uint samples;
	vec4 sh_r;
	vec4 sh_g;
	vec4 sh_b;
	vec3 short_mean;
	float variance;
	uint last_seen;
	uint flags;
	uint clip;
	uint inside_count;
};

struct SurfelCounters {
	uint free_count;
	uint alive_count;
	uint update_count;
	uint previous_alive;
};

layout(set = SURFEL_SET, binding = SURFEL_UBO_BINDING, std140) uniform SurfelData {
	vec3 camera_to_anchor;
	float base_cell;

	vec3 anchor_shift;
	float radius_scale;

	uint capacity;
	uint bucket_count;
	uint cell_capacity;
	uint frame;

	uint max_level;
	uint updates_per_frame;
	float ray_distance;
	float spawn_chance;

	float max_distance;
	float min_radius;
	float max_radius;
	uint debug_view;

	uint unseen_frames;
	uint max_samples;
	float ambient_occlusion_strength;
	float pad0;

	mat4 world_to_clip;
	mat4 clip_to_view;
}
surfel_data;

uint surfel_hash(uint p_value) {
	p_value ^= p_value >> 16u;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15u;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16u;
	return p_value;
}

float surfel_random(uint p_seed) {
	return float(surfel_hash(p_seed) >> 8u) / 16777216.0;
}

float surfel_cell_size(uint p_level) {
	return surfel_data.base_cell * float(1u << p_level);
}

uint surfel_level(float p_distance) {
	float size = clamp(p_distance * surfel_data.radius_scale, surfel_data.min_radius, surfel_data.max_radius) * SURFEL_CELL_RADII;
	return min(surfel_data.max_level, uint(max(0.0, ceil(log2(size / surfel_data.base_cell) - 1e-4))));
}

float surfel_spawn_radius(float p_distance) {
	return min(surfel_cell_size(surfel_level(p_distance)) / SURFEL_CELL_RADII, surfel_data.max_radius);
}

uint surfel_bucket(uint p_level, ivec3 p_cell) {
	uvec3 c = uvec3(p_cell);
	return surfel_hash((c.x * 73856093u) ^ (c.y * 19349663u) ^ (c.z * 83492791u) ^ (p_level * 2654435761u)) % surfel_data.bucket_count;
}

uint surfel_buckets(vec3 p_position, float p_radius, out uint r_buckets[16]) {
	float distance = length(p_position - surfel_data.camera_to_anchor);
	uint near_level = surfel_level(max(0.0, distance - p_radius));
	uint far_level = surfel_level(distance + p_radius);
	uint count = 0u;
	for (uint level = near_level; level <= far_level && level <= near_level + 1u; level++) {
		float size = surfel_cell_size(level);
		ivec3 low = ivec3(floor((p_position - p_radius) / size));
		ivec3 high = min(ivec3(floor((p_position + p_radius) / size)), low + 1);
		for (int z = low.z; z <= high.z; z++) {
			for (int y = low.y; y <= high.y; y++) {
				for (int x = low.x; x <= high.x; x++) {
					r_buckets[count] = surfel_bucket(level, ivec3(x, y, z));
					count++;
				}
			}
		}
	}
	return count;
}

void surfel_tangent_frame(vec3 p_normal, out vec3 r_tangent, out vec3 r_bitangent) {
	r_tangent = normalize(cross(abs(p_normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), p_normal));
	r_bitangent = cross(p_normal, r_tangent);
}

bool surfel_clipped(vec3 p_delta, vec3 p_normal, float p_radius, uint p_clip) {
	vec3 tangent;
	vec3 bitangent;
	surfel_tangent_frame(p_normal, tangent, bitangent);
	vec2 local = vec2(dot(p_delta, tangent), dot(p_delta, bitangent)) / p_radius;
	for (uint k = 0u; k < 8u; k++) {
		uint bits = (p_clip >> (k * 4u)) & 15u;
		if (bits < 15u && dot(local, surfel_clip_directions[k]) > float(bits) / 14.0) {
			return true;
		}
	}
	return false;
}

vec3 surfel_eval_irradiance(vec4 p_r, vec4 p_g, vec4 p_b, vec3 p_normal) {
	vec4 basis = vec4(1.0, p_normal * (2.0 / 3.0));
	return max(vec3(0.0), vec3(dot(p_r, basis), dot(p_g, basis), dot(p_b, basis)));
}
