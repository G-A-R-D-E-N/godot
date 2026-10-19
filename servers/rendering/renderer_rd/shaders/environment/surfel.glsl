#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : enable

#if defined(MODE_SCAN_LOCAL) || defined(MODE_SCAN_BLOCKS) || defined(MODE_SCAN_APPLY)
#define MODE_SCAN
layout(local_size_x = 1024, local_size_y = 1, local_size_z = 1) in;
#elif defined(MODE_RESET)
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
#else
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#endif

#define SURFEL_SET 0
#define SURFEL_UBO_BINDING 0
#include "surfel_inc.glsl"

layout(set = 0, binding = 1, std430) restrict buffer Surfels {
	Surfel data[];
}
surfels;

layout(set = 0, binding = 2, std430) restrict buffer Counters {
	SurfelCounters data;
}
counters;

layout(set = 0, binding = 3, std430) restrict buffer FreeList {
	uint data[];
}
free_list;

layout(set = 0, binding = 4, std430) restrict buffer AliveList {
	uint data[];
}
alive_list;

layout(set = 0, binding = 5, std430) restrict buffer UpdateList {
	uint data[];
}
update_list;

layout(set = 0, binding = 6, std430) restrict buffer BucketCounts {
	uint data[];
}
bucket_counts;

layout(set = 0, binding = 7, std430) restrict buffer BucketOffsets {
	uint data[];
}
surfel_offsets;

layout(set = 0, binding = 8, std430) restrict buffer BucketCursor {
	uint data[];
}
bucket_cursor;

layout(set = 0, binding = 9, std430) restrict buffer Cells {
	uint data[];
}
surfel_cells;

#ifdef MODE_SCAN_BLOCKS
layout(set = 0, binding = 10, std430) restrict writeonly buffer Dispatch {
	uint alive_args[3];
	uint trace_args[3];
}
dispatch;
#endif

#ifdef MODE_SCAN
layout(set = 0, binding = 12, std430) restrict buffer BucketBlocks {
	uint data[];
}
bucket_blocks;
#endif

#ifdef MODE_VALIDATE
layout(set = 0, binding = 11) uniform texture2D depth_texture;

#include "surfel_gather_inc.glsl"
#endif

#ifdef MODE_RESET

void main() {
	counters.data.previous_alive = counters.data.alive_count;
	counters.data.alive_count = 0u;
	counters.data.update_count = 0u;
	counters.data.previous_saturated_kills = counters.data.saturated_kills;
	counters.data.saturated_kills = 0u;
}

#endif

#ifdef MODE_MAINTAIN

void main() {
	uint index = gl_GlobalInvocationID.x;
	if (index >= surfel_data.capacity || (surfels.data[index].flags & SURFEL_FLAG_ALIVE) == 0u) {
		return;
	}

	vec3 position = surfels.data[index].position + surfel_data.anchor_shift;
	surfels.data[index].position = position;
	float radius = surfels.data[index].radius;
	float distance = length(position - surfel_data.camera_to_anchor);

	float pressure = float(counters.data.previous_alive) / float(surfel_data.capacity);
	uint unseen_limit = pressure > 0.9 ? surfel_data.unseen_frames / 8u : (pressure > 0.75 ? surfel_data.unseen_frames / 2u : surfel_data.unseen_frames);
	bool kill = (surfels.data[index].flags & SURFEL_FLAG_KILL) != 0u;
	kill = kill || distance > surfel_data.max_distance;
	kill = kill || surfel_data.frame - surfels.data[index].last_seen > unseen_limit;
	kill = kill || radius > surfel_spawn_radius(distance) * 2.0;
	if (kill) {
		surfels.data[index].flags = 0u;
		free_list.data[atomicAdd(counters.data.free_count, 1u)] = index;
		return;
	}

	alive_list.data[atomicAdd(counters.data.alive_count, 1u)] = index;

	uint buckets[16];
	uint bucket_total = surfel_buckets(position, radius * SURFEL_REACH, buckets);
	for (uint i = 0u; i < bucket_total; i++) {
		atomicAdd(bucket_counts.data[buckets[i]], 1u);
	}

	uint stride = max(1u, (counters.data.previous_alive + surfel_data.updates_per_frame - 1u) / surfel_data.updates_per_frame);
	bool young = float(surfels.data[index].samples) < SURFEL_CONFIDENT_SAMPLES * 2.0;
	if (young || (surfel_hash(index) + surfel_data.frame) % stride == 0u) {
		uint slot = atomicAdd(counters.data.update_count, 1u);
		if (slot < surfel_data.updates_per_frame) {
			update_list.data[slot] = index;
		}
	}
}

#endif

#ifdef MODE_SCAN

shared uint partial[1024];

uint scan_exclusive(uint p_value) {
	uint thread = gl_LocalInvocationIndex;
	partial[thread] = p_value;
	barrier();
	for (uint offset = 1u; offset < 1024u; offset <<= 1u) {
		uint value = thread >= offset ? partial[thread - offset] : 0u;
		barrier();
		partial[thread] += value;
		barrier();
	}
	return partial[thread] - p_value;
}

#endif

#ifdef MODE_SCAN_LOCAL

void main() {
	uint b = gl_GlobalInvocationID.x;
	uint count = bucket_counts.data[b];
	bucket_counts.data[b] = 0u;
	uint offset = scan_exclusive(count);
	surfel_offsets.data[b] = offset;
	if (gl_LocalInvocationIndex == 1023u) {
		bucket_blocks.data[gl_WorkGroupID.x] = offset + count;
	}
}

#endif

#ifdef MODE_SCAN_BLOCKS

void main() {
	uint thread = gl_LocalInvocationIndex;
	uint block_count = surfel_data.bucket_count / 1024u;
	uint carry = 0u;
	for (uint base = 0u; base < block_count; base += 1024u) {
		uint block = base + thread;
		uint sum = block < block_count ? bucket_blocks.data[block] : 0u;
		uint offset = scan_exclusive(sum);
		if (block < block_count) {
			bucket_blocks.data[block] = carry + offset;
		}
		carry += partial[1023];
		barrier();
	}
	if (thread == 0u) {
		surfel_offsets.data[surfel_data.bucket_count] = carry;
		uint alive = counters.data.alive_count;
		uint updates = min(counters.data.update_count, surfel_data.updates_per_frame);
		counters.data.update_count = updates;
		dispatch.alive_args[0] = (alive + 63u) / 64u;
		dispatch.alive_args[1] = 1u;
		dispatch.alive_args[2] = 1u;
		dispatch.trace_args[0] = (updates + 7u) / 8u;
		dispatch.trace_args[1] = 1u;
		dispatch.trace_args[2] = 1u;
	}
}

#endif

#ifdef MODE_SCAN_APPLY

void main() {
	uint b = gl_GlobalInvocationID.x;
	uint offset = surfel_offsets.data[b] + bucket_blocks.data[gl_WorkGroupID.x];
	surfel_offsets.data[b] = offset;
	bucket_cursor.data[b] = offset;
}

#endif

#ifdef MODE_INSERT

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= counters.data.alive_count) {
		return;
	}
	uint index = alive_list.data[i];
	uint buckets[16];
	uint bucket_total = surfel_buckets(surfels.data[index].position, surfels.data[index].radius * SURFEL_REACH, buckets);
	for (uint b = 0u; b < bucket_total; b++) {
		uint slot = atomicAdd(bucket_cursor.data[buckets[b]], 1u);
		if (slot < surfel_data.cell_capacity) {
			surfel_cells.data[slot] = index;
		}
	}
}

#endif

#ifdef MODE_VALIDATE

float scene_distance(vec2 p_ndc, ivec2 p_size) {
	ivec2 pixel = clamp(ivec2((p_ndc * 0.5 + 0.5) * vec2(p_size)), ivec2(0), p_size - 1);
	float depth = texelFetch(depth_texture, pixel, 0).r;
	vec4 view = surfel_data.clip_to_view * vec4(p_ndc, depth, 1.0);
	return -view.z / view.w;
}

void main() {
	uint i = gl_GlobalInvocationID.x;
	if (i >= counters.data.alive_count) {
		return;
	}
	uint index = alive_list.data[i];
	vec3 position = surfels.data[index].position;
	float radius = surfels.data[index].radius;
	vec3 normal = surfels.data[index].normal;

	vec4 clip = surfel_data.world_to_clip * vec4(position - surfel_data.camera_to_anchor, 1.0);
	if (clip.w > 0.0) {
		vec2 ndc = clip.xy / clip.w;
		if (all(lessThan(abs(ndc), vec2(1.0)))) {
			ivec2 size = textureSize(depth_texture, 0);
			float tolerance = max(radius, 0.02 * clip.w);
			vec2 step = 4.0 / vec2(size);
			float farthest = scene_distance(ndc, size);
			float nearest = farthest;
			for (int k = 0; k < 4; k++) {
				vec2 offset = vec2(k == 0 ? step.x : (k == 1 ? -step.x : 0.0), k == 2 ? step.y : (k == 3 ? -step.y : 0.0));
				float sample_distance = scene_distance(clamp(ndc + offset, vec2(-1.0), vec2(1.0)), size);
				farthest = max(farthest, sample_distance);
				nearest = min(nearest, sample_distance);
			}
			if (clip.w < nearest - tolerance * 2.0) {
				surfels.data[index].flags |= SURFEL_FLAG_KILL;
				return;
			}
			if (clip.w < farthest + tolerance) {
				surfels.data[index].last_seen = surfel_data.frame;
			}
		}
	}

	if (((gl_WorkGroupID.x + surfel_data.frame) & 3u) != 0u) {
		return;
	}
	SurfelLookup lookup = surfel_lookup(position, normal, index);
	if (lookup.coverage > 3.0 || lookup.saturated) {
		surfels.data[index].flags |= SURFEL_FLAG_KILL;
		if (lookup.coverage <= 3.0) {
			atomicAdd(counters.data.saturated_kills, 1u);
		}
	}
}

#endif
