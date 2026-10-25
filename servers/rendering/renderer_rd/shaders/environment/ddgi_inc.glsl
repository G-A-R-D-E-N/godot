#define DDGI_MAX_LEVELS 4
#define DDGI_IRRADIANCE_SIZE 6
#define DDGI_DISTANCE_SIZE 14

#define DDGI_STATE_NONE 0u
#define DDGI_STATE_ACTIVE 1u
#define DDGI_STATE_INSIDE 2u
#define DDGI_STATE_IDLE 3u

#define DDGI_LIST_HEADER 48
#define DDGI_NO_VIEW_LIMIT 1e6
#define DDGI_RADIANCE_SHARPEN 0.25

#include "../oct_inc.glsl"

struct DDGILevel {
	vec3 origin;
	float spacing;
	ivec3 g_min;
	ivec3 slot_offset;
};

layout(set = DDGI_SET, binding = DDGI_UBO_BINDING, std140) uniform DDGIData {
	DDGILevel levels[DDGI_MAX_LEVELS];

	ivec3 dims;
	uint level_count;

	vec2 irradiance_texel_size;
	vec2 distance_texel_size;

	float normal_bias;
	float view_bias;
	float energy;
	float hysteresis;

	mat4 ray_rotation;

	uint rays_per_probe;
	uint frame;
	float max_distance;
	uint probe_capacity;

	float change_threshold;
}
ddgi;

vec3 ddgi_texel_direction(ivec2 p_texel, int p_size) {
	return oct_to_vec3((vec2(p_texel) + 0.5) / float(p_size) * 2.0 - 1.0);
}

vec3 ddgi_ray_direction(uint p_index) {
	const float golden_ratio = 1.6180339887;
	float n = float(ddgi.rays_per_probe);
	float z = 1.0 - (2.0 * float(p_index) + 1.0) / n;
	float r = sqrt(max(0.0, 1.0 - z * z));
	float phi = 6.2831853 * fract(float(p_index) / golden_ratio);
	return mat3(ddgi.ray_rotation) * vec3(r * cos(phi), r * sin(phi), z);
}

ivec3 ddgi_local_from_index(uint p_index) {
	uint plane = uint(ddgi.dims.x * ddgi.dims.y);
	return ivec3(int(p_index % uint(ddgi.dims.x)), int((p_index % plane) / uint(ddgi.dims.x)), int(p_index / plane));
}

ivec3 ddgi_slot(uint p_level, ivec3 p_local) {
	return (p_local + ddgi.levels[p_level].slot_offset) % ddgi.dims;
}

ivec3 ddgi_probe_texel(uint p_level, ivec3 p_slot) {
	return ivec3(p_slot.x + p_slot.y * ddgi.dims.x, p_slot.z, int(p_level));
}

ivec2 ddgi_tile_origin(ivec3 p_slot, int p_size) {
	return ivec2(p_slot.x + p_slot.y * ddgi.dims.x, p_slot.z) * (p_size + 2);
}

ivec3 ddgi_stats_texel(uint p_level, ivec3 p_slot) {
	return ddgi_probe_texel(p_level + ddgi.level_count, p_slot);
}

uint ddgi_tag(uint p_level, ivec3 p_local) {
	uvec3 g = uvec3(ddgi.levels[p_level].g_min + p_local) & uvec3(0x3FFu);
	return g.x | (g.y << 10) | (g.z << 20);
}

uint ddgi_state(uvec4 p_data, uint p_level, ivec3 p_local) {
	if ((p_data.w & 0x3FFFFFFFu) != ddgi_tag(p_level, p_local)) {
		return DDGI_STATE_NONE;
	}
	return p_data.w >> 30;
}

vec3 ddgi_offset(uvec4 p_data) {
	return uintBitsToFloat(p_data.xyz);
}

vec3 ddgi_probe_position(uint p_level, ivec3 p_local, vec3 p_offset) {
	return ddgi.levels[p_level].origin + (vec3(p_local) + p_offset) * ddgi.levels[p_level].spacing;
}

#ifdef DDGI_SAMPLING

vec3 ddgi_sample_level(uint p_level, vec3 p_position, vec3 p_normal, vec3 p_view, vec3 p_direction, float p_sharpen, float p_view_limit, out float r_weight) {
	float spacing = ddgi.levels[p_level].spacing;
	vec3 biased = p_position + p_normal * ddgi.normal_bias * spacing + p_view * min(ddgi.view_bias * spacing, p_view_limit);
	vec3 grid = (biased - ddgi.levels[p_level].origin) / spacing;
	ivec3 base = ivec3(floor(grid));
	vec3 alpha = grid - vec3(base);

	vec3 irradiance = vec3(0.0);
	float weight_sum = 0.0;
	for (int i = 0; i < 8; i++) {
		ivec3 corner = ivec3(i, i >> 1, i >> 2) & ivec3(1);
		ivec3 local = clamp(base + corner, ivec3(0), ddgi.dims - 1);
		ivec3 slot = ddgi_slot(p_level, local);
		uvec4 data = texelFetch(ddgi_probe_texture, ddgi_probe_texel(p_level, slot), 0);
		uint state = ddgi_state(data, p_level, local);
		if (state == DDGI_STATE_NONE || state == DDGI_STATE_INSIDE) {
			continue;
		}

		vec3 probe_position = ddgi_probe_position(p_level, local, ddgi_offset(data));
		vec3 probe_delta = probe_position - p_position;
		vec3 to_probe = probe_delta / max(length(probe_delta), 1e-4);
		float wrap = (dot(to_probe, p_normal) + 1.0) * 0.5;
		float weight = wrap * wrap + 0.2;

		vec3 probe_to_biased = biased - probe_position;
		float distance_to_biased = length(probe_to_biased);
		vec2 distance_uv = (vec2(ddgi_tile_origin(slot, DDGI_DISTANCE_SIZE)) + 1.0 + vec3_to_oct(probe_to_biased / max(distance_to_biased, 1e-4)) * float(DDGI_DISTANCE_SIZE)) * ddgi.distance_texel_size;
		vec2 moments = textureLod(sampler2DArray(ddgi_distance_texture, ddgi_linear_sampler), vec3(distance_uv, float(p_level)), 0.0).xy;
		if (distance_to_biased > moments.x) {
			float variance = abs(moments.y - moments.x * moments.x);
			float delta = distance_to_biased - moments.x;
			float chebyshev = variance / (variance + delta * delta);
			weight *= chebyshev * chebyshev * chebyshev;
			weight *= weight < 0.5 ? pow(weight, 7.0) * 128.0 : 1.0;
		}

		weight = max(1e-6, weight);
		if (weight < 0.2) {
			weight *= weight * weight * 25.0;
		}

		vec3 trilinear = mix(1.0 - alpha, alpha, vec3(corner));
		weight *= trilinear.x * trilinear.y * trilinear.z;

		vec2 tile = vec2(ddgi_tile_origin(slot, DDGI_IRRADIANCE_SIZE)) + 1.0;
		vec2 irradiance_uv = (tile + vec3_to_oct(p_direction) * float(DDGI_IRRADIANCE_SIZE)) * ddgi.irradiance_texel_size;
		vec3 probe_irradiance = textureLod(sampler2DArray(ddgi_irradiance_texture, ddgi_linear_sampler), vec3(irradiance_uv, float(p_level)), 0.0).rgb;
		if (p_sharpen > 0.0) {
			vec2 opposite_uv = (tile + vec3_to_oct(-p_direction) * float(DDGI_IRRADIANCE_SIZE)) * ddgi.irradiance_texel_size;
			probe_irradiance = probe_irradiance * (1.0 + p_sharpen) - textureLod(sampler2DArray(ddgi_irradiance_texture, ddgi_linear_sampler), vec3(opposite_uv, float(p_level)), 0.0).rgb * p_sharpen;
		}
		irradiance += weight * probe_irradiance;
		weight_sum += weight;
	}

	r_weight = weight_sum;
	return weight_sum > 0.0 ? irradiance / weight_sum : vec3(0.0);
}

float ddgi_level_coverage(uint p_level, vec3 p_position) {
	vec3 grid = (p_position - ddgi.levels[p_level].origin) / ddgi.levels[p_level].spacing;
	vec3 edge = min(grid, vec3(ddgi.dims - 1) - grid);
	return clamp(min(edge.x, min(edge.y, edge.z)) * 0.5, 0.0, 1.0);
}

vec4 ddgi_sample_direction(vec3 p_position, vec3 p_normal, vec3 p_view, vec3 p_direction, float p_sharpen, float p_view_limit) {
	vec4 result = vec4(0.0);
	for (uint level = 0; level < ddgi.level_count && result.a < 0.999; level++) {
		float coverage = ddgi_level_coverage(level, p_position);
		if (coverage <= 0.0) {
			continue;
		}
		float weight_sum;
		vec3 irradiance = ddgi_sample_level(level, p_position, p_normal, p_view, p_direction, p_sharpen, p_view_limit, weight_sum);
		if (weight_sum <= 0.0) {
			continue;
		}
		float share = coverage * (1.0 - result.a);
		result.rgb += irradiance * share;
		result.a += share;
	}
	if (result.a > 0.0) {
		result.rgb /= result.a;
	}
	return result;
}

vec4 ddgi_sample(vec3 p_position, vec3 p_normal, vec3 p_view) {
	return ddgi_sample_direction(p_position, p_normal, p_view, p_normal, 0.0, DDGI_NO_VIEW_LIMIT);
}

vec4 ddgi_sample_radiance(vec3 p_position, vec3 p_normal, vec3 p_view, vec3 p_direction, float p_view_limit) {
	return ddgi_sample_direction(p_position, p_normal, p_view, p_direction, DDGI_RADIANCE_SHARPEN, p_view_limit);
}

#endif
