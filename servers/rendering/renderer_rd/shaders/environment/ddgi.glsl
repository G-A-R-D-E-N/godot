#[compute]

#version 450

#VERSION_DEFINES

#if defined(MODE_BLEND_DISTANCE)
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
#elif defined(MODE_BLEND_IRRADIANCE)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#elif defined(MODE_SCHEDULE_PLAN) || defined(MODE_SCHEDULE_FINALIZE)
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
#elif defined(MODE_SCHEDULE_SCAN)
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
#elif defined(MODE_RESET_COUNTS)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
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
#define DDGI_SCAN_SIZE 256u

layout(set = 0, binding = 1, std430) restrict buffer ProbeList {
	uint count;
	uint state_counts[DDGI_SCHEDULE_SLOTS];
	uint strides[DDGI_SCHEDULE_SLOTS];
	uint pad[15];
	uint entries[];
}
probe_list;

#if defined(MODE_SCHEDULE_SELECT) || defined(MODE_SCHEDULE_SCAN) || defined(MODE_SCHEDULE_APPEND)
// Per 64-probe group: (carried offset, carried mask, requested offset), (requested mask, left-out mask).
layout(set = 0, binding = 7, std430) restrict buffer ScheduleGroups {
	uint requested_cursor;
	uint carried_cursor;
	uint carried_total;
	uint requested_total;
	uint carried_start;
	uint requested_start;
	uint pad[2];
	uvec4 groups[];
}
schedule_groups;
#endif

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
	uvec4 stats;
};

ProbeRef load_probe(uint p_list_index) {
	ProbeRef probe;
	uint entry = probe_list.entries[p_list_index];
	probe.level = entry >> 28;
	probe.local = ddgi_local_from_index(entry & 0x0FFFFFFFu);
	probe.slot = ddgi_slot(probe.level, probe.local);
	probe.data = imageLoad(probe_image, ddgi_probe_texel(probe.level, probe.slot));
	probe.stats = imageLoad(probe_image, ddgi_stats_texel(probe.level, probe.slot));
	probe.state = ddgi_state(probe.data, probe.level, probe.local);
	return probe;
}

float probe_hysteresis(ProbeRef p_probe) {
	return (p_probe.state == DDGI_STATE_ACTIVE || p_probe.state == DDGI_STATE_IDLE) ? ddgi.hysteresis : 0.0;
}

#define DDGI_SHIFT_DECAY 0.85
#define DDGI_SHIFT_START 6.0
#define DDGI_SHIFT_SCALE 2.0
#define DDGI_SHIFT_NOISE_FLOOR 0.03
#define DDGI_SHIFT_WARMUP 32u
#define DDGI_LOG_EPSILON 1e-4

float probe_shift_keep(ProbeRef p_probe) {
	if ((p_probe.state != DDGI_STATE_ACTIVE && p_probe.state != DDGI_STATE_IDLE) || p_probe.stats.z < DDGI_SHIFT_WARMUP) {
		return 1.0;
	}
	float sigma = max(unpackHalf2x16(p_probe.stats.y).y, DDGI_SHIFT_NOISE_FLOOR) * sqrt((1.0 - DDGI_SHIFT_DECAY) / (1.0 + DDGI_SHIFT_DECAY));
	float excess = max(abs(unpackHalf2x16(p_probe.stats.y).x) / sigma - DDGI_SHIFT_START, 0.0) / DDGI_SHIFT_SCALE;
	return 1.0 / (1.0 + excess * excess);
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
	float keep = probe_shift_keep(probe);

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
		hysteresis = min(hysteresis * keep, updates / (updates + 1.0));
		if (max3(abs(result - previous.rgb)) > ddgi.change_threshold * max(max3(previous.rgb), max3(result))) {
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
	float keep = probe_shift_keep(probe);

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
		float hysteresis = probe_hysteresis(probe) * keep;
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
	float closest_surface = 1e20;
	float log_sum = 0.0;
	float log_square_sum = 0.0;
	for (uint r = 0; r < ddgi.rays_per_probe; r++) {
		vec4 ray = imageLoad(ray_image, ivec2(r, list_index));
		float hit = imageLoad(ray_image, ivec2(ddgi.rays_per_probe + r, list_index)).a;
		if (ray.a >= 0.0) {
			closest_surface = min(closest_surface, ray.a);
		}
		vec3 direction = ddgi_ray_direction(r);
		if (hit < 0.0) {
			backfaces++;
			if (-hit < closest_back) {
				closest_back = -hit;
				closest_back_direction = direction;
			}
		} else {
			float value = log(max(dot(ray.rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0) + DDGI_LOG_EPSILON);
			log_sum += value;
			log_square_sum += value * value;
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

	float front_count = max(float(ddgi.rays_per_probe - backfaces), 1.0);
	float log_mean = log_sum / front_count;
	float log_variance = max(log_square_sum / front_count - log_mean * log_mean, 0.0);
	float innovation = 0.0;
	uint updates = probe.state == DDGI_STATE_NONE ? 0u : probe.stats.z;
	if (probe.state == DDGI_STATE_ACTIVE || probe.state == DDGI_STATE_IDLE) {
		float previous_mean = uintBitsToFloat(probe.stats.x);
		if (updates >= DDGI_SHIFT_WARMUP) {
			innovation = mix(log_mean - previous_mean, unpackHalf2x16(probe.stats.y).x, DDGI_SHIFT_DECAY);
		}
		log_mean = mix(log_mean, previous_mean, min(ddgi.hysteresis * probe_shift_keep(probe), float(updates) / (float(updates) + 1.0)));
	}
	uint state = DDGI_STATE_ACTIVE;
	if (inside) {
		state = DDGI_STATE_INSIDE;
	} else if (closest_surface > spacing * 1.7320508) {
		state = DDGI_STATE_IDLE;
	}
	imageStore(probe_image, ddgi_probe_texel(probe.level, probe.slot), uvec4(floatBitsToUint(offset), (state << 30) | ddgi_tag(probe.level, probe.local)));
	imageStore(probe_image, ddgi_stats_texel(probe.level, probe.slot), uvec4(floatBitsToUint(log_mean), packHalf2x16(vec2(innovation, sqrt(log_variance / front_count))), min(updates + 1u, 255u), 0u));
}

#endif

#if defined(MODE_SCHEDULE_COUNT) || defined(MODE_SCHEDULE_SELECT) || defined(MODE_SCHEDULE_APPEND)

// Schedule ids must be storage slots, so the left-out masks stay with their cells when the volume scrolls.
ivec3 schedule_local(uint p_level, ivec3 p_slot) {
	return (p_slot - ddgi.levels[p_level].slot_offset + ddgi.dims) % ddgi.dims;
}

#endif

#if defined(MODE_SCHEDULE_COUNT) || defined(MODE_SCHEDULE_SELECT)

uint schedule_hash(uint p_value) {
	p_value ^= p_value >> 16u;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15u;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16u;
	return p_value;
}

bool schedule_probe(out uint r_level, out uint r_state, out uint r_tag) {
	uint per_level = uint(ddgi.dims.x * ddgi.dims.y * ddgi.dims.z);
	uint id = gl_GlobalInvocationID.x;
	if (id >= per_level * ddgi.level_count) {
		return false;
	}
	r_level = id / per_level;
	ivec3 slot = ddgi_local_from_index(id % per_level);
	ivec3 local = schedule_local(r_level, slot);
	uvec4 data = imageLoad(probe_image, ddgi_probe_texel(r_level, slot));
	r_state = ddgi_state(data, r_level, local);
	r_tag = ddgi_tag(r_level, local);
	return true;
}

#endif

#ifdef MODE_SCHEDULE_COUNT

shared uint group_values[DDGI_SCHEDULE_SLOTS];

void main() {
	if (gl_LocalInvocationIndex < DDGI_SCHEDULE_SLOTS) {
		group_values[gl_LocalInvocationIndex] = 0u;
	}
	barrier();
	uint level, state, tag;
	if (schedule_probe(level, state, tag)) {
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
}

#endif

#ifdef MODE_SCHEDULE_SELECT

shared uint group_masks[4];

void main() {
	uint lane = gl_LocalInvocationIndex;
	if (lane < 4u) {
		group_masks[lane] = 0u;
	}
	barrier();
	uint entry = gl_WorkGroupID.x * 2u;
	uint left_lo = schedule_groups.groups[entry + 1u].z;
	uint left_hi = schedule_groups.groups[entry + 1u].w;
	uint word = lane >> 5u;
	uint bit = 1u << (lane & 31u);
	uint level, state, tag;
	if (schedule_probe(level, state, tag)) {
		if (((word == 0u ? left_lo : left_hi) & bit) != 0u) {
			atomicOr(group_masks[word], bit);
		} else {
			uint stride = probe_list.strides[level * 4u + state];
			if ((schedule_hash(tag ^ (level * 0x9E3779B9u)) + ddgi.frame) % stride == 0u) {
				atomicOr(group_masks[2u + word], bit);
			}
		}
	}
	barrier();
	if (lane == 0u) {
		uint carried = uint(bitCount(group_masks[0]) + bitCount(group_masks[1]));
		uint requested = uint(bitCount(group_masks[2]) + bitCount(group_masks[3]));
		schedule_groups.groups[entry] = uvec4(carried, group_masks[0], group_masks[1], requested);
		schedule_groups.groups[entry + 1u].xy = uvec2(group_masks[2], group_masks[3]);
	}
}

#endif

#ifdef MODE_SCHEDULE_SCAN

shared uvec2 scan_values[DDGI_SCAN_SIZE];
shared uint cursor_rank;

void main() {
	uint lane = gl_LocalInvocationIndex;
	uint group_count = (uint(ddgi.dims.x * ddgi.dims.y * ddgi.dims.z) * ddgi.level_count + 63u) / 64u;
	uint chunk = (group_count + DDGI_SCAN_SIZE - 1u) / DDGI_SCAN_SIZE;
	uint first = min(lane * chunk, group_count);
	uint last = min(first + chunk, group_count);
	uvec2 sum = uvec2(0u);
	for (uint g = first; g < last; g++) {
		sum += schedule_groups.groups[g * 2u].xw;
	}
	scan_values[lane] = sum;
	if (lane == 0u) {
		cursor_rank = 0u;
	}
	barrier();
	for (uint step = 1u; step < DDGI_SCAN_SIZE; step <<= 1u) {
		uvec2 add = lane >= step ? scan_values[lane - step] : uvec2(0u);
		barrier();
		scan_values[lane] += add;
		barrier();
	}
	uint cursor = schedule_groups.carried_cursor;
	uvec2 offset = scan_values[lane] - sum;
	for (uint g = first; g < last; g++) {
		uvec4 group = schedule_groups.groups[g * 2u];
		if (g == cursor / 64u) {
			uint below = (1u << (cursor & 31u)) - 1u;
			cursor_rank = offset.x + uint((cursor & 32u) == 0u ? bitCount(group.y & below) : bitCount(group.y) + bitCount(group.z & below));
		}
		schedule_groups.groups[g * 2u].xw = offset;
		offset += group.xw;
	}
	barrier();
	if (lane == DDGI_SCAN_SIZE - 1u) {
		uint carried = scan_values[lane].x;
		uint requested = scan_values[lane].y;
		uint room = ddgi.probe_capacity - min(carried, ddgi.probe_capacity);
		uint start = 0u;
		if (requested > room && room > 0u) {
			start = schedule_groups.requested_cursor % requested;
			schedule_groups.requested_cursor = (start + room) % requested;
		}
		schedule_groups.carried_total = carried;
		schedule_groups.requested_total = requested;
		schedule_groups.carried_start = carried > ddgi.probe_capacity ? cursor_rank : 0u;
		schedule_groups.requested_start = start;
		probe_list.count = carried + requested;
	}
}

#endif

#ifdef MODE_SCHEDULE_APPEND

shared uint left_out[2];

void main() {
	uint lane = gl_LocalInvocationIndex;
	if (lane < 2u) {
		left_out[lane] = 0u;
	}
	barrier();
	uint entry = gl_WorkGroupID.x * 2u;
	uvec4 carried = schedule_groups.groups[entry];
	uint requested_lo = schedule_groups.groups[entry + 1u].x;
	uint requested_hi = schedule_groups.groups[entry + 1u].y;
	uint word = lane >> 5u;
	uint bit = 1u << (lane & 31u);
	uint below = bit - 1u;
	uint capacity = ddgi.probe_capacity;
	uint carried_total = schedule_groups.carried_total;
	uint served = min(carried_total, capacity);
	uint position = capacity;
	bool selected = false;
	if (((word == 0u ? carried.y : carried.z) & bit) != 0u) {
		// Ordering: carried probes take the first positions, rotated from the cursor when they overflow.
		selected = true;
		position = carried.x + uint(word == 0u ? bitCount(carried.y & below) : bitCount(carried.y) + bitCount(carried.z & below));
		if (carried_total > capacity) {
			position = (position + carried_total - schedule_groups.carried_start) % carried_total;
			if (position == capacity) {
				schedule_groups.carried_cursor = gl_GlobalInvocationID.x;
			}
		}
	} else if (((word == 0u ? requested_lo : requested_hi) & bit) != 0u) {
		selected = true;
		uint rank = carried.w + uint(word == 0u ? bitCount(requested_lo & below) : bitCount(requested_lo) + bitCount(requested_hi & below));
		uint requested_total = schedule_groups.requested_total;
		uint room = capacity - served;
		if (requested_total > room && room > 0u) {
			rank = (rank + requested_total - schedule_groups.requested_start) % requested_total;
		}
		position = rank < room ? served + rank : capacity;
	}
	if (selected) {
		if (position < capacity) {
			uint per_level = uint(ddgi.dims.x * ddgi.dims.y * ddgi.dims.z);
			uint id = gl_GlobalInvocationID.x;
			uint level = id / per_level;
			ivec3 local = schedule_local(level, ddgi_local_from_index(id % per_level));
			probe_list.entries[position] = (level << 28) | uint(local.x + (local.y + local.z * ddgi.dims.y) * ddgi.dims.x);
		} else {
			atomicOr(left_out[word], bit);
		}
	}
	barrier();
	if (lane == 0u) {
		schedule_groups.groups[entry + 1u].zw = uvec2(left_out[0], left_out[1]);
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

#ifdef MODE_RESET_COUNTS

#define DDGI_RESET_BOXES 32

layout(set = 0, binding = 8, std140) uniform ResetData {
	vec4 boxes[DDGI_RESET_BOXES];
	uint count;
	uint pad0;
	uint pad1;
	uint pad2;
}
reset;

void main() {
	uint level = gl_WorkGroupID.z;
	ivec3 slot = ivec3(int(gl_WorkGroupID.x) % ddgi.dims.x, int(gl_WorkGroupID.x) / ddgi.dims.x, int(gl_WorkGroupID.y));
	ivec3 local = (slot + ddgi.dims - ddgi.levels[level].slot_offset) % ddgi.dims;
	uvec4 data = imageLoad(probe_image, ddgi_probe_texel(level, slot));
	uint state = ddgi_state(data, level, local);
	if (state == DDGI_STATE_NONE) {
		return;
	}

	bool inside = false;
	vec3 offset = (state == DDGI_STATE_ACTIVE || state == DDGI_STATE_IDLE) ? ddgi_offset(data) : vec3(0.0);
	vec3 position = ddgi_probe_position(level, local, offset);
	for (uint i = 0u; i < reset.count && !inside; i++) {
		inside = all(lessThanEqual(abs(position - reset.boxes[i].xyz), vec3(reset.boxes[i].w)));
	}
	if (!inside) {
		return;
	}

	ivec3 texel = ivec3(ddgi_tile_origin(slot, DDGI_IRRADIANCE_SIZE) + ivec2(gl_LocalInvocationID.xy), int(level));
	imageStore(irradiance_image, texel, vec4(imageLoad(irradiance_image, texel).rgb, 0.0));
}

#endif
