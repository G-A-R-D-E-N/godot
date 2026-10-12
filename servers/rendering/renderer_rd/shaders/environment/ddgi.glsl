#[compute]

#version 450

#VERSION_DEFINES

#if defined(MODE_BLEND_DISTANCE)
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
#elif defined(MODE_BLEND_IRRADIANCE)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#elif defined(MODE_SCHEDULE_PLAN) || defined(MODE_SCHEDULE_FINALIZE)
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
#else
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#endif

#define DDGI_SET 0
#define DDGI_UBO_BINDING 0
#include "ddgi_inc.glsl"

#define DDGI_MAX_RAYS 256
#define DDGI_DISTANCE_SHARPNESS 50.0
#define DDGI_BACKFACE_FRACTION 0.25
#define DDGI_CHANGE_THRESHOLD 0.25
#define DDGI_MAX_UPDATES 255.0
#define DDGI_SCHEDULE_SLOTS 16u

layout(set = 0, binding = 1, std430) restrict buffer ProbeList {
	uint count;
	uint state_counts[DDGI_SCHEDULE_SLOTS];
	uint strides[DDGI_SCHEDULE_SLOTS];
	uint pad[15];
	uint entries[];
}
probe_list;

layout(rgba16f, set = 0, binding = 2) uniform restrict image2D ray_image;
layout(rgba16f, set = 0, binding = 3) uniform restrict image2DArray irradiance_image;
layout(rg16f, set = 0, binding = 4) uniform restrict image2DArray distance_image;
layout(rgba32ui, set = 0, binding = 5) uniform restrict uimage2DArray probe_image;

struct ProbeRef {
	uint level;
	ivec3 local;
	ivec3 slot;
	uint state;
	uvec4 data;
};

ProbeRef load_probe(uint p_list_index) {
	ProbeRef probe;
	uint entry = probe_list.entries[p_list_index];
	probe.level = entry >> 28;
	probe.local = ddgi_local_from_index(entry & 0x0FFFFFFFu);
	probe.slot = ddgi_slot(probe.level, probe.local);
	probe.data = imageLoad(probe_image, ddgi_probe_texel(probe.level, probe.slot));
	probe.state = ddgi_state(probe.data, probe.level, probe.local);
	return probe;
}

float probe_hysteresis(ProbeRef p_probe) {
	return (p_probe.state == DDGI_STATE_ACTIVE || p_probe.state == DDGI_STATE_IDLE) ? ddgi.hysteresis : 0.0;
}

ivec2 border_source(ivec2 p_texel, int p_size) {
	bool left = p_texel.x == 0;
	bool right = p_texel.x == p_size + 1;
	bool top = p_texel.y == 0;
	bool bottom = p_texel.y == p_size + 1;
	if ((left || right) && (top || bottom)) {
		return ivec2(left ? p_size : 1, top ? p_size : 1);
	}
	if (top || bottom) {
		return ivec2(p_size + 1 - p_texel.x, top ? 1 : p_size);
	}
	if (left || right) {
		return ivec2(left ? 1 : p_size, p_size + 1 - p_texel.y);
	}
	return p_texel;
}

float max3(vec3 p_value) {
	return max(p_value.x, max(p_value.y, p_value.z));
}

#if defined(MODE_BLEND_IRRADIANCE) || defined(MODE_BLEND_DISTANCE)

shared vec4 ray_values[DDGI_MAX_RAYS];
shared vec3 ray_directions[DDGI_MAX_RAYS];

void load_rays(uint p_list_index) {
	uint threads = gl_WorkGroupSize.x * gl_WorkGroupSize.y;
	for (uint r = gl_LocalInvocationIndex; r < ddgi.rays_per_probe; r += threads) {
		ray_values[r] = imageLoad(ray_image, ivec2(r, p_list_index));
		ray_directions[r] = ddgi_ray_direction(r);
	}
	barrier();
}

#endif

#ifdef MODE_BLEND_IRRADIANCE

shared vec4 tile[DDGI_IRRADIANCE_SIZE][DDGI_IRRADIANCE_SIZE];

void main() {
	uint list_index = gl_WorkGroupID.x;
	ProbeRef probe = load_probe(list_index);
	load_rays(list_index);

	ivec2 tile_origin = ddgi_tile_origin(probe.slot, DDGI_IRRADIANCE_SIZE);
	ivec2 texel = ivec2(gl_LocalInvocationID.xy) - 1;
	if (all(greaterThanEqual(texel, ivec2(0))) && all(lessThan(texel, ivec2(DDGI_IRRADIANCE_SIZE)))) {
		vec3 direction = ddgi_texel_direction(texel, DDGI_IRRADIANCE_SIZE);
		vec3 sum = vec3(0.0);
		float weight_sum = 0.0;
		for (uint r = 0; r < ddgi.rays_per_probe; r++) {
			vec4 ray = ray_values[r];
			if (ray.a < 0.0) {
				continue;
			}
			float weight = max(0.0, dot(direction, ray_directions[r]));
			sum += weight * ray.rgb;
			weight_sum += weight;
		}
		vec3 result = weight_sum > 0.0 ? sum / weight_sum : vec3(0.0);
		vec4 previous = imageLoad(irradiance_image, ivec3(tile_origin + texel + 1, int(probe.level)));
		float hysteresis = probe_hysteresis(probe);
		float updates = hysteresis > 0.0 ? previous.a : 0.0;
		hysteresis = min(hysteresis, updates / (updates + 1.0));
		if (max3(abs(result - previous.rgb)) > DDGI_CHANGE_THRESHOLD * max(max3(previous.rgb), max3(result))) {
			hysteresis *= 0.5;
		}
		tile[texel.y][texel.x] = vec4(mix(result, previous.rgb, hysteresis), min(updates + 1.0, DDGI_MAX_UPDATES));
	}
	barrier();

	ivec2 source = border_source(ivec2(gl_LocalInvocationID.xy), DDGI_IRRADIANCE_SIZE) - 1;
	imageStore(irradiance_image, ivec3(tile_origin + ivec2(gl_LocalInvocationID.xy), int(probe.level)), tile[source.y][source.x]);
}

#endif

#ifdef MODE_BLEND_DISTANCE

shared vec2 tile[DDGI_DISTANCE_SIZE][DDGI_DISTANCE_SIZE];

void main() {
	uint list_index = gl_WorkGroupID.x;
	ProbeRef probe = load_probe(list_index);
	load_rays(list_index);

	float spacing = ddgi.levels[probe.level].spacing;
	float max_distance = min(spacing * 1.7320508 * 1.5, 255.0);
	ivec2 tile_origin = ddgi_tile_origin(probe.slot, DDGI_DISTANCE_SIZE);
	ivec2 texel = ivec2(gl_LocalInvocationID.xy) - 1;
	if (all(greaterThanEqual(texel, ivec2(0))) && all(lessThan(texel, ivec2(DDGI_DISTANCE_SIZE)))) {
		vec3 direction = ddgi_texel_direction(texel, DDGI_DISTANCE_SIZE);
		vec2 sum = vec2(0.0);
		float weight_sum = 0.0;
		for (uint r = 0; r < ddgi.rays_per_probe; r++) {
			float hit = ray_values[r].a;
			float distance = min(hit < 0.0 ? -hit * 0.2 : hit, max_distance);
			float weight = pow(max(0.0, dot(direction, ray_directions[r])), DDGI_DISTANCE_SHARPNESS);
			sum += weight * vec2(distance, distance * distance);
			weight_sum += weight;
		}
		vec2 result = weight_sum > 0.0 ? sum / weight_sum : vec2(max_distance, max_distance * max_distance);
		vec2 previous = imageLoad(distance_image, ivec3(tile_origin + texel + 1, int(probe.level))).xy;
		float hysteresis = probe_hysteresis(probe);
		if (abs(result.x - previous.x) > DDGI_CHANGE_THRESHOLD * spacing) {
			hysteresis *= 0.5;
		}
		tile[texel.y][texel.x] = hysteresis > 0.0 ? mix(result, previous, hysteresis) : result;
	}
	barrier();

	ivec2 source = border_source(ivec2(gl_LocalInvocationID.xy), DDGI_DISTANCE_SIZE) - 1;
	imageStore(distance_image, ivec3(tile_origin + ivec2(gl_LocalInvocationID.xy), int(probe.level)), vec4(tile[source.y][source.x], 0.0, 0.0));
}

#endif

#ifdef MODE_PROBE_STATE

void main() {
	uint list_index = gl_GlobalInvocationID.x;
	if (list_index >= probe_list.count) {
		return;
	}
	ProbeRef probe = load_probe(list_index);

	float spacing = ddgi.levels[probe.level].spacing;
	vec3 offset = probe.state == DDGI_STATE_NONE ? vec3(0.0) : ddgi_offset(probe.data);

	uint backfaces = 0;
	float closest_front = 1e20;
	float closest_back = 1e20;
	float farthest_front = 0.0;
	vec3 closest_front_direction = vec3(0.0);
	vec3 closest_back_direction = vec3(0.0);
	vec3 farthest_front_direction = vec3(0.0);
	for (uint r = 0; r < ddgi.rays_per_probe; r++) {
		float hit = imageLoad(ray_image, ivec2(r, list_index)).a;
		vec3 direction = ddgi_ray_direction(r);
		if (hit < 0.0) {
			backfaces++;
			if (-hit < closest_back) {
				closest_back = -hit;
				closest_back_direction = direction;
			}
		} else {
			if (hit < closest_front) {
				closest_front = hit;
				closest_front_direction = direction;
			}
			if (hit > farthest_front) {
				farthest_front = hit;
				farthest_front_direction = direction;
			}
		}
	}

	float min_front = 0.25 * spacing;
	bool inside = float(backfaces) > DDGI_BACKFACE_FRACTION * float(ddgi.rays_per_probe);
	vec3 move = vec3(0.0);
	if (inside) {
		move = closest_back_direction * (closest_back + min_front * 0.5);
	} else if (closest_front < min_front && dot(closest_front_direction, farthest_front_direction) <= 0.0) {
		move = farthest_front_direction * min(farthest_front, min_front);
	}
	vec3 moved = offset + move / spacing;
	if (all(lessThanEqual(abs(moved), vec3(0.45)))) {
		offset = moved;
	}

	uint state = DDGI_STATE_ACTIVE;
	if (inside) {
		state = DDGI_STATE_INSIDE;
	} else if (closest_front > spacing * 1.7320508) {
		state = DDGI_STATE_IDLE;
	}
	imageStore(probe_image, ddgi_probe_texel(probe.level, probe.slot), uvec4(floatBitsToUint(offset), (state << 30) | ddgi_tag(probe.level, probe.local)));
}

#endif

#if defined(MODE_SCHEDULE_COUNT) || defined(MODE_SCHEDULE_APPEND)

shared uint group_values[DDGI_SCHEDULE_SLOTS];
shared uint group_base;

uint schedule_hash(uint p_value) {
	p_value ^= p_value >> 16u;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15u;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16u;
	return p_value;
}

bool schedule_probe(out uint r_level, out uint r_index, out uint r_state, out uint r_tag) {
	uint per_level = uint(ddgi.dims.x * ddgi.dims.y * ddgi.dims.z);
	uint id = gl_GlobalInvocationID.x;
	if (id >= per_level * ddgi.level_count) {
		return false;
	}
	r_level = id / per_level;
	r_index = id % per_level;
	ivec3 local = ddgi_local_from_index(r_index);
	uvec4 data = imageLoad(probe_image, ddgi_probe_texel(r_level, ddgi_slot(r_level, local)));
	r_state = ddgi_state(data, r_level, local);
	r_tag = ddgi_tag(r_level, local);
	return true;
}

#endif

#ifdef MODE_SCHEDULE_COUNT

void main() {
	if (gl_LocalInvocationIndex < DDGI_SCHEDULE_SLOTS) {
		group_values[gl_LocalInvocationIndex] = 0u;
	}
	barrier();
	uint level, index, state, tag;
	if (schedule_probe(level, index, state, tag)) {
		atomicAdd(group_values[level * 4u + state], 1u);
	}
	barrier();
	if (gl_LocalInvocationIndex < DDGI_SCHEDULE_SLOTS && group_values[gl_LocalInvocationIndex] > 0u) {
		atomicAdd(probe_list.state_counts[gl_LocalInvocationIndex], group_values[gl_LocalInvocationIndex]);
	}
}

#endif

#ifdef MODE_SCHEDULE_PLAN

void main() {
	const float state_weights[4] = float[](64.0, 1.0, 1.0 / 32.0, 1.0 / 8.0);
	float counts[DDGI_SCHEDULE_SLOTS];
	float weights[DDGI_SCHEDULE_SLOTS];
	for (uint i = 0; i < DDGI_SCHEDULE_SLOTS; i++) {
		counts[i] = float(probe_list.state_counts[i]);
		weights[i] = state_weights[i & 3u] / float(1u << (i >> 2u));
	}

	float capacity = float(ddgi.probe_capacity);
	float low = -40.0;
	float high = 40.0;
	for (int step = 0; step < 40; step++) {
		float middle = (low + high) * 0.5;
		float scale = exp2(middle);
		float total = 0.0;
		for (uint i = 0; i < DDGI_SCHEDULE_SLOTS; i++) {
			total += counts[i] * min(1.0, scale * weights[i]);
		}
		if (total > capacity) {
			high = middle;
		} else {
			low = middle;
		}
	}

	float scale = exp2(low);
	for (uint i = 0; i < DDGI_SCHEDULE_SLOTS; i++) {
		float rate = max(min(1.0, scale * weights[i]), 1e-6);
		probe_list.strides[i] = uint(ceil(1.0 / rate - 1e-3));
	}
	probe_list.count = 0u;
}

#endif

#ifdef MODE_SCHEDULE_APPEND

void main() {
	if (gl_LocalInvocationIndex == 0u) {
		group_values[0] = 0u;
	}
	barrier();
	uint level, index, state, tag;
	bool selected = false;
	uint local_slot = 0u;
	if (schedule_probe(level, index, state, tag)) {
		uint stride = probe_list.strides[level * 4u + state];
		selected = (schedule_hash(tag ^ (level * 0x9E3779B9u)) + ddgi.frame) % stride == 0u;
		if (selected) {
			local_slot = atomicAdd(group_values[0], 1u);
		}
	}
	barrier();
	if (gl_LocalInvocationIndex == 0u) {
		group_base = atomicAdd(probe_list.count, group_values[0]);
	}
	barrier();
	uint list_index = group_base + local_slot;
	if (selected && list_index < ddgi.probe_capacity) {
		probe_list.entries[list_index] = (level << 28) | index;
	}
}

#endif

#ifdef MODE_SCHEDULE_FINALIZE

layout(set = 0, binding = 6, std430) restrict writeonly buffer Dispatch {
	uint trace_args[3];
	uint blend_args[3];
	uint state_args[3];
}
dispatch;

void main() {
	uint count = min(probe_list.count, ddgi.probe_capacity);
	probe_list.count = count;
	dispatch.trace_args[0] = (ddgi.rays_per_probe + 63u) / 64u;
	dispatch.trace_args[1] = count;
	dispatch.trace_args[2] = 1u;
	dispatch.blend_args[0] = count;
	dispatch.blend_args[1] = 1u;
	dispatch.blend_args[2] = 1u;
	dispatch.state_args[0] = (count + 63u) / 64u;
	dispatch.state_args[1] = 1u;
	dispatch.state_args[2] = 1u;
	for (uint i = 0; i < DDGI_SCHEDULE_SLOTS; i++) {
		probe_list.state_counts[i] = 0u;
	}
}

#endif
