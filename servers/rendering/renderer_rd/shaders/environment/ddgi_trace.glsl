#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : enable

#if defined(MODE_DEBUG_RAYS) || defined(MODE_RTGI)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
#else
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;
#endif

#define DDGI_SET 0
#define DDGI_UBO_BINDING 0
#define DDGI_SAMPLING

layout(set = 0, binding = 3) uniform utexture2DArray ddgi_probe_texture;
layout(set = 0, binding = 4) uniform texture2DArray ddgi_irradiance_texture;
layout(set = 0, binding = 5) uniform texture2DArray ddgi_distance_texture;
layout(set = 0, binding = 6) uniform sampler ddgi_linear_sampler;

#include "ddgi_inc.glsl"

layout(set = 0, binding = 1, std430) restrict readonly buffer ProbeList {
	uint count;
	uint header[DDGI_LIST_HEADER - 1];
	uint entries[];
}
probe_list;

layout(rgba16f, set = 0, binding = 2) uniform restrict writeonly image2D ray_image;

#define RAY_SCENE_SET 1
#include "ray_scene_inc.glsl"

#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 2, binding = 0) uniform texture2DArray sky_radiance;
#else
layout(set = 2, binding = 0) uniform texture2D sky_radiance;
#endif
layout(set = 2, binding = 1) uniform sampler sky_sampler;

#define SKY_FLAGS_MODE_COLOR 0x01
#define SKY_FLAGS_MODE_SKY 0x02
#define SKY_FLAGS_ORIENTATION_SIGN 0x04

#define DDGI_MISS_DISTANCE 65504.0
#define DDGI_MAX_RADIANCE 60000.0

layout(push_constant, std430) uniform Params {
	vec3 sky_color_or_orientation;
	uint sky_flags;

	vec2 sky_border_size;
	float sky_energy;
	float bounce_energy;

	float emission_exposure;
	uint debug_mode;
	vec2 screen_size;

	mat4 debug_inv_view_projection;
}
params;

vec3 sky_radiance_in(vec3 p_direction) {
	if (bool(params.sky_flags & SKY_FLAGS_MODE_SKY)) {
		float sky_sign = bool(params.sky_flags & SKY_FLAGS_ORIENTATION_SIGN) ? 1.0 : -1.0;
		vec4 sky_quat = vec4(params.sky_color_or_orientation, sky_sign * sqrt(max(0.0, 1.0 - dot(params.sky_color_or_orientation, params.sky_color_or_orientation))));
		vec3 sky_dir = cross(sky_quat.xyz, p_direction);
		sky_dir = p_direction + ((sky_dir * sky_quat.w) + cross(sky_quat.xyz, sky_dir)) * 2.0;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
		return textureLod(sampler2DArray(sky_radiance, sky_sampler), vec3(vec3_to_oct_with_border(sky_dir, params.sky_border_size), 0.0), 2.0).rgb * params.sky_energy;
#else
		return textureLod(sampler2D(sky_radiance, sky_sampler), vec3_to_oct_with_border(sky_dir, params.sky_border_size), 2.0).rgb * params.sky_energy;
#endif
	}
	if (bool(params.sky_flags & SKY_FLAGS_MODE_COLOR)) {
		return params.sky_color_or_orientation * params.sky_energy;
	}
	return vec3(0.0);
}

#ifdef MODE_SURFEL_TRACE
#define SURFEL_SET 3
#define SURFEL_UBO_BINDING 0
#include "surfel_inc.glsl"

#define SURFEL_RAYS 8u
#define SURFEL_FOLLOW_RATIO 2.0
#define SURFEL_FOLLOW_MEMORY 0.7
#define SURFEL_FOLLOW_FLOOR 1e-5

layout(set = 3, binding = 1, std430) restrict buffer Surfels {
	Surfel data[];
}
surfels;

layout(set = 3, binding = 2, std430) restrict readonly buffer UpdateList {
	uint data[];
}
update_list;

layout(set = 3, binding = 3, std430) restrict readonly buffer Counters {
	SurfelCounters data;
}
counters;

layout(set = 3, binding = 4, std430) restrict readonly buffer BucketOffsets {
	uint data[];
}
surfel_offsets;

layout(set = 3, binding = 5, std430) restrict readonly buffer Cells {
	uint data[];
}
surfel_cells;

#include "surfel_gather_inc.glsl"

vec3 origin_surfel_position = vec3(0.0);
vec3 origin_surfel_normal = vec3(0.0, 1.0, 0.0);
#endif

vec4 shade_hit(vec3 p_origin, vec3 p_direction, RaySceneHit hit, uint p_seed) {
	RaySceneMaterial material = ray_scene_hit_material(hit);
	bool two_sided = hit.blend || bool(material.flags & RAY_SCENE_MATERIAL_TWO_SIDED);
	if (hit.backface && !two_sided) {
		return vec4(0.0, 0.0, 0.0, -hit.t);
	}

	vec3 normal = ray_scene_hit_normal(hit);
	if (hit.backface) {
		normal = -normal;
	}
	vec3 position = p_origin + p_direction * hit.t;
	float bias = 0.01 + hit.t * 0.001;
	vec3 irradiance = gi_hit_direct_light(position, normal, bias, ddgi.max_distance, p_seed);
	if (params.bounce_energy > 0.0) {
		vec3 bounce = vec3(0.0);
		float near_origin = 0.0;
#ifdef MODE_SURFEL_TRACE
		float spacing = ddgi.levels[0].spacing;
		near_origin = (1.0 - smoothstep(spacing, spacing * 2.0, hit.t)) * (1.0 - smoothstep(spacing * 0.4, spacing, max(dot(position - origin_surfel_position, origin_surfel_normal), 0.0)));
		if (near_origin > 0.0) {
			bounce = near_origin * ddgi_sample_direction(origin_surfel_position - ray_scene.camera_to_anchor, origin_surfel_normal, origin_surfel_normal, normal, 0.0, DDGI_NO_VIEW_LIMIT).rgb;
		}
#endif
		if (near_origin < 1.0) {
			bounce += (1.0 - near_origin) * ddgi_sample_direction(position - ray_scene.camera_to_anchor, normal, -p_direction, normal, 0.0, hit.t * 0.5).rgb;
		}
#ifdef MODE_SURFEL_TRACE
		SurfelLookup lookup = surfel_lookup(position, normal, SURFEL_INVALID);
		bounce = mix(bounce, surfel_eval_irradiance(lookup.sh_r, lookup.sh_g, lookup.sh_b, normal), surfel_blend(lookup));
#endif
		irradiance += bounce * params.bounce_energy;
	}
	vec3 radiance = material.emission * params.emission_exposure + material.albedo * irradiance;
	return vec4(min(radiance, vec3(DDGI_MAX_RADIANCE)), hit.t);
}

vec4 shade_ray(vec3 p_origin, vec3 p_direction, float p_max_distance, uint p_seed) {
	RaySceneHit hit;
	if (!ray_scene_trace(p_origin, p_direction, p_max_distance, false, p_seed, hit)) {
		return vec4(min(sky_radiance_in(p_direction), vec3(DDGI_MAX_RADIANCE)), DDGI_MISS_DISTANCE);
	}
	return shade_hit(p_origin, p_direction, hit, p_seed);
}

#ifdef MODE_PROBE_TRACE

void main() {
	uint ray_index = gl_GlobalInvocationID.x;
	uint list_index = gl_WorkGroupID.y;
	if (ray_index >= ddgi.rays_per_probe || list_index >= probe_list.count) {
		return;
	}

	uint entry = probe_list.entries[list_index];
	uint level = entry >> 28;
	ivec3 local = ddgi_local_from_index(entry & 0x0FFFFFFFu);
	ivec3 slot = ddgi_slot(level, local);
	uvec4 data = texelFetch(ddgi_probe_texture, ddgi_probe_texel(level, slot), 0);
	uint state = ddgi_state(data, level, local);
	vec3 offset = state == DDGI_STATE_NONE ? vec3(0.0) : ddgi_offset(data);
	vec3 origin = ddgi_probe_position(level, local, offset) + ray_scene.camera_to_anchor;
	vec3 direction = ddgi_ray_direction(ray_index);
	uint seed = ray_scene_hash(entry * 9781u + ddgi.frame * 6271u) + ray_index * 0x2c1b3c6du;
	RaySceneHit hit;
	vec4 ray;
	float geometry = DDGI_MISS_DISTANCE;
	if (!ray_scene_trace(origin, direction, ddgi.max_distance, false, seed, hit)) {
		ray = vec4(min(sky_radiance_in(direction), vec3(DDGI_MAX_RADIANCE)), DDGI_MISS_DISTANCE);
	} else {
		ray = shade_hit(origin, direction, hit, seed);
		geometry = ray.a;
		if (hit.blend) {
			RaySceneHit opaque;
			geometry = DDGI_MISS_DISTANCE;
			if (ray_scene_trace_opaque(origin, direction, ddgi.max_distance, seed, opaque)) {
				bool back = opaque.backface && !bool(ray_scene_hit_material(opaque).flags & RAY_SCENE_MATERIAL_TWO_SIDED);
				geometry = back ? -opaque.t : opaque.t;
			}
		}
	}
	imageStore(ray_image, ivec2(ray_index, list_index), ray);
	imageStore(ray_image, ivec2(ddgi.rays_per_probe + ray_index, list_index), vec4(0.0, 0.0, 0.0, geometry));
}

#endif

#ifdef MODE_SURFEL_TRACE

shared vec4 ray_values[64];
shared vec3 ray_directions[64];
shared uint clip_bits[64];

void main() {
	uint local = gl_LocalInvocationIndex;
	uint slot = gl_WorkGroupID.x * (64u / SURFEL_RAYS) + local / SURFEL_RAYS;
	uint ray = local % SURFEL_RAYS;
	bool valid = slot < counters.data.update_count;
	uint index = valid ? update_list.data[slot] : 0u;

	vec3 radiance = vec3(0.0);
	vec3 direction = vec3(0.0, 0.0, 1.0);
	float backface = 0.0;
	uint clip = 15u;
	bool refresh_clip = false;
	if (valid) {
		vec3 position = surfels.data[index].position;
		vec3 normal = surfels.data[index].normal;
		uint seed = surfel_hash(index * 9781u + surfel_data.frame * 6271u) + ray * 2u;
		float cos_theta = (float(ray & 1u) + surfel_random(seed)) * 0.5;
		float phi = 6.2831853 * (float(ray >> 1u) + surfel_random(seed + 1u)) * 0.25;
		float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));
		vec3 tangent;
		vec3 bitangent;
		surfel_tangent_frame(normal, tangent, bitangent);
		direction = tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) + normal * cos_theta;

		float surfel_radius = surfels.data[index].radius;
		float clip_length = surfel_radius * SURFEL_REACH;
		vec3 origin = position + normal * (0.02 + surfel_radius * 0.1);
		origin_surfel_position = position;
		origin_surfel_normal = normal;
		RaySceneHit hit;
		refresh_clip = surfels.data[index].samples == 0u || ((surfel_hash(index) + surfel_data.frame) & 63u) == 0u;
		if (refresh_clip) {
			vec2 clip_direction = surfel_clip_directions[ray];
			if (ray_scene_trace(origin, tangent * clip_direction.x + bitangent * clip_direction.y, clip_length, false, seed ^ 0x5bd1e995u, hit)) {
				clip = min(14u, uint(ceil(hit.t / clip_length * 14.0)));
			}
		}
		if (ray_scene_trace(origin, direction, surfel_data.ray_distance, false, seed, hit)) {
			vec4 shaded = shade_hit(origin, direction, hit, seed * 0x2c1b3c6du + 0x297a2d39u);
			radiance = shaded.rgb;
			backface = shaded.a < 0.0 ? 1.0 : 0.0;
		} else if (surfel_data.ray_distance < ddgi.max_distance) {
			vec4 probe_light = ddgi_sample_radiance(origin + direction * surfel_data.ray_distance - ray_scene.camera_to_anchor, -direction, -direction, direction, DDGI_NO_VIEW_LIMIT);
			radiance = probe_light.a > 0.0 ? max(probe_light.rgb, vec3(0.0)) : sky_radiance_in(direction);
		} else {
			radiance = sky_radiance_in(direction);
		}
	}
	ray_values[local] = vec4(radiance, backface);
	ray_directions[local] = direction;
	clip_bits[local] = clip << (ray * 4u);
	barrier();

	if (!valid || ray != 0u) {
		return;
	}

	vec3 normal = surfels.data[index].normal;
	vec3 mean = vec3(0.0);
	vec3 moment_r = vec3(0.0);
	vec3 moment_g = vec3(0.0);
	vec3 moment_b = vec3(0.0);
	float backfaces = 0.0;
	for (uint k = 0u; k < SURFEL_RAYS; k++) {
		vec4 value = ray_values[local + k];
		vec3 ray_direction = ray_directions[local + k];
		mean += value.rgb * dot(ray_direction, normal);
		moment_r += value.r * ray_direction;
		moment_g += value.g * ray_direction;
		moment_b += value.b * ray_direction;
		backfaces += value.a;
	}

	if (refresh_clip) {
		uint packed = 0u;
		for (uint k = 0u; k < SURFEL_RAYS; k++) {
			packed |= clip_bits[local + k];
		}
		surfels.data[index].clip = packed;
	}

	uint inside = backfaces > float(SURFEL_RAYS) * 0.5 ? surfels.data[index].inside_count + 1u : 0u;
	surfels.data[index].inside_count = inside;
	if (inside > 2u) {
		surfels.data[index].flags |= SURFEL_FLAG_KILL;
		return;
	}

	float scale = 1.0 / float(SURFEL_RAYS);
	vec4 estimate_r = vec4(mean.r, moment_r * 1.5) * scale;
	vec4 estimate_g = vec4(mean.g, moment_g * 1.5) * scale;
	vec4 estimate_b = vec4(mean.b, moment_b * 1.5) * scale;

	vec4 sh_r = surfels.data[index].sh_r;
	vec4 sh_g = surfels.data[index].sh_g;
	vec4 sh_b = surfels.data[index].sh_b;
	uint samples = surfels.data[index].samples;
	vec3 stored_state = surfels.data[index].short_mean;

	vec4 probe_light = ddgi_sample(surfels.data[index].position - ray_scene.camera_to_anchor, normal, normal);
	float probe_luma = dot(probe_light.rgb, luminance_weights);
	vec3 sample_light = surfel_eval_irradiance(estimate_r, estimate_g, estimate_b, normal);
	float sample_luma = dot(sample_light, luminance_weights);
	float current = dot(surfel_eval_irradiance(sh_r, sh_g, sh_b, normal), luminance_weights);
	float probe_reference = samples == 0u ? probe_luma : stored_state.y;
	float smoothed_luma = mix(stored_state.x, sample_luma, 0.25);
	if (samples != 0u && probe_light.a > 0.5) {
		float probe_ratio = (probe_luma + SURFEL_FOLLOW_FLOOR) / (probe_reference + SURFEL_FOLLOW_FLOOR);
		float sample_ratio = (smoothed_luma + SURFEL_FOLLOW_FLOOR) / (current + SURFEL_FOLLOW_FLOOR);
		bool dropped = probe_ratio < 1.0 / SURFEL_FOLLOW_RATIO && sample_ratio < 1.0 / SURFEL_FOLLOW_RATIO;
		bool risen = probe_ratio > SURFEL_FOLLOW_RATIO && sample_ratio > SURFEL_FOLLOW_RATIO;
		probe_reference = mix(probe_luma, probe_reference, SURFEL_FOLLOW_MEMORY);
		if (dropped || risen) {
			samples = 0u;
			probe_reference = probe_luma;
		}
	}

	float short_luma = samples == 0u ? sample_luma : smoothed_luma;
	float delta = sample_luma - short_luma;
	float variance = samples == 0u ? 0.0 : mix(surfels.data[index].variance, delta * delta, 0.25);
	if (samples > 4u && abs(short_luma - current) > sqrt(variance) + 0.1 * current) {
		samples = 2u;
	}

	float blend = 1.0 / float(min(samples, surfel_data.max_samples) + 1u);
	surfels.data[index].sh_r = mix(sh_r, estimate_r, blend);
	surfels.data[index].sh_g = mix(sh_g, estimate_g, blend);
	surfels.data[index].sh_b = mix(sh_b, estimate_b, blend);
	surfels.data[index].short_mean = vec3(short_luma, probe_reference, 0.0);
	surfels.data[index].variance = variance;
	surfels.data[index].samples = min(samples + 1u, surfel_data.max_samples);
}

#endif

#ifdef MODE_DEBUG_RAYS

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(vec2(pixel), params.screen_size))) {
		return;
	}

	vec2 uv = (vec2(pixel) + 0.5) / params.screen_size;
	vec4 world = params.debug_inv_view_projection * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	vec3 direction = normalize(world.xyz / world.w);

	vec3 color;
	if (params.debug_mode == 1u) {
		RaySceneHit hit;
		if (ray_scene_trace(ray_scene.camera_to_anchor, direction, ddgi.max_distance, false, uint(pixel.y) * 65537u + uint(pixel.x), hit)) {
			vec3 normal = ray_scene_hit_normal(hit);
			vec4 irradiance = ddgi_sample(direction * hit.t, hit.backface ? -normal : normal, -direction);
			color = irradiance.a > 0.0 ? irradiance.rgb : vec3(1.0, 0.0, 1.0);
		} else {
			color = sky_radiance_in(direction);
		}
	} else if (params.debug_mode == 2u) {
		color = shade_ray(ray_scene.camera_to_anchor, direction, ddgi.max_distance, uint(pixel.y) * 65537u + uint(pixel.x)).rgb;
	} else if (params.debug_mode == 4u || params.debug_mode == 5u) {
		RaySceneHit hit;
		color = vec3(0.0);
		if (ray_scene_trace(ray_scene.camera_to_anchor, direction, ddgi.max_distance, false, uint(pixel.y) * 65537u + uint(pixel.x), hit)) {
			vec3 position = direction * hit.t;
			vec3 normal = ray_scene_hit_normal(hit);
			normal = hit.backface ? -normal : normal;
			color = vec3(0.3);
			for (uint level = 0; level < ddgi.level_count; level++) {
				if (ddgi_level_coverage(level, position) <= 0.0) {
					continue;
				}
				if (params.debug_mode == 5u) {
					float weight_sum;
					ddgi_sample_level(level, position, normal, -direction, normal, 0.0, DDGI_NO_VIEW_LIMIT, weight_sum);
					color = weight_sum > 0.0 ? mix(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), clamp(weight_sum / 0.5, 0.0, 1.0)) : vec3(1.0, 0.0, 1.0);
					break;
				}
				vec3 grid = (position - ddgi.levels[level].origin) / ddgi.levels[level].spacing;
				ivec3 local = clamp(ivec3(round(grid)), ivec3(0), ddgi.dims - 1);
				uvec4 data = texelFetch(ddgi_probe_texture, ddgi_probe_texel(level, ddgi_slot(level, local)), 0);
				uint state = ddgi_state(data, level, local);
				color = state == DDGI_STATE_ACTIVE ? vec3(0.1, 0.8, 0.1) : (state == DDGI_STATE_IDLE ? vec3(0.1, 0.2, 0.9) : (state == DDGI_STATE_INSIDE ? vec3(0.9, 0.1, 0.1) : vec3(0.0)));
				vec3 edge = mix(abs(fract(grid) - 0.5), vec3(1.0), greaterThan(abs(normal), vec3(0.7)));
				if (min(edge.x, min(edge.y, edge.z)) < 0.03) {
					color *= 0.25;
				}
				break;
			}
			color *= 0.6 + 0.4 * max(0.0, dot(normal, -direction));
		}
		color = color / max(vec3(1.0) - color, vec3(1e-4));
	} else {
		RaySceneHit hit;
		if (ray_scene_trace(ray_scene.camera_to_anchor, direction, ddgi.max_distance, false, uint(pixel.y) * 65537u + uint(pixel.x), hit)) {
			color = hit.backface ? vec3(1.0, 0.0, 0.0) : ray_scene_hit_normal(hit) * 0.5 + 0.5;
		} else {
			color = vec3(0.0);
		}
	}
	imageStore(ray_image, pixel, vec4(color / (1.0 + color), 1.0));
}

#endif

#ifdef MODE_RTGI

#include "rtgi_inc.glsl"

layout(set = 3, binding = 0, std140) uniform RtgiData {
	mat4 inv_projection;
	mat4 cam_transform;
	ivec2 screen_size;
	ivec2 trace_size;
	uint frame;
	uint seed;
	int scale;
	uint flags;
	float origin_scale;
	float ray_fraction;
	float origin_floor;
	uint mirror_below_surface;
}
rtgi;

layout(set = 3, binding = 1) uniform texture2D rtgi_depth;
layout(set = 3, binding = 2) uniform texture2D rtgi_normal_roughness;
layout(set = 3, binding = 3) uniform utexture2D rtgi_tiles;
layout(rgba32ui, set = 3, binding = 4) uniform restrict writeonly uimage2D rtgi_candidate_first;
layout(rgba32ui, set = 3, binding = 5) uniform restrict writeonly uimage2D rtgi_candidate_second;

#define RTGI_ORIGIN_VIEW_ULPS 4.0
#define RTGI_ORIGIN_ANCHOR_ULPS 4.0
#define RTGI_ORIGIN_RASTER 0.00390625
#define RTGI_FLOAT_EPSILON 1.1920929e-7

vec3 rtgi_view_position_at(ivec2 p_pixel, float p_depth) {
	vec2 uv = (vec2(p_pixel) + 0.5) / vec2(rtgi.screen_size);
	vec4 clip = rtgi.inv_projection * vec4(uv * 2.0 - 1.0, p_depth, 1.0);
	return clip.xyz / clip.w;
}

vec3 rtgi_geometric_normal(ivec2 p_pixel, vec3 p_view_position, vec3 p_fallback, out float r_footprint) {
	r_footprint = length(rtgi_view_position_at(p_pixel + ivec2(1, 0), texelFetch(rtgi_depth, p_pixel, 0).x) - p_view_position);
	ivec2 limit = rtgi.screen_size - 1;
	vec3 best[2];
	float best_gap[2];
	for (int axis = 0; axis < 2; axis++) {
		best[axis] = vec3(0.0);
		best_gap[axis] = 1e30;
		for (int side = -1; side <= 1; side += 2) {
			ivec2 tap = clamp(p_pixel + (axis == 0 ? ivec2(side, 0) : ivec2(0, side)), ivec2(0), limit);
			float tap_depth = texelFetch(rtgi_depth, tap, 0).x;
			if (tap_depth == 0.0 || tap == p_pixel) {
				continue;
			}
			vec3 delta = (rtgi_view_position_at(tap, tap_depth) - p_view_position) * float(side);
			float gap = abs(dot(delta, normalize(p_view_position)));
			if (gap < best_gap[axis]) {
				best_gap[axis] = gap;
				best[axis] = delta;
			}
		}
	}
	if (best_gap[0] > 1e29 || best_gap[1] > 1e29) {
		return p_fallback;
	}
	r_footprint = max(length(best[0]), length(best[1]));
	vec3 n = cross(best[0], best[1]);
	float n_length = length(n);
	if (n_length <= 0.0) {
		return p_fallback;
	}
	n /= n_length;
	return dot(n, p_view_position) > 0.0 ? -n : n;
}

float rtgi_origin_offset(float p_view_distance, vec3 p_anchor_position, float p_footprint) {
	float anchor_extent = max(abs(p_anchor_position.x), max(abs(p_anchor_position.y), abs(p_anchor_position.z)));
	float error = RTGI_FLOAT_EPSILON * (RTGI_ORIGIN_VIEW_ULPS * p_view_distance + RTGI_ORIGIN_ANCHOR_ULPS * anchor_extent) + p_footprint * RTGI_ORIGIN_RASTER;
	return max(rtgi.origin_scale * error, rtgi.origin_floor * p_view_distance);
}

void main() {
	ivec2 trace_pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(trace_pixel, rtgi.trace_size))) {
		return;
	}

	uint tag = uint(trace_pixel.y * rtgi.trace_size.x + trace_pixel.x) ^ rtgi.frame;
	uint tile = texelFetch(rtgi_tiles, trace_pixel / RTGI_TILE, 0).r;
	ivec2 scale = ivec2(max(rtgi.scale, 1));
	ivec2 pixel = min(trace_pixel * scale + scale / 2, rtgi.screen_size - 1);
	float depth = texelFetch(rtgi_depth, pixel, 0).x;

	bool candidate = (tile & RTGI_TILE_TRACE) != 0u && depth != 0.0;
	bool traced = candidate && rtgi_thin_keeps(uvec2(trace_pixel), rtgi.frame, rtgi.seed, rtgi.ray_fraction);
	if (!traced) {
		uint skipped = RTGI_FLAG_NO_CANDIDATE | RTGI_FLAG_NOT_TRACED | (candidate ? RTGI_FLAG_THINNED : 0u);
		imageStore(rtgi_candidate_first, trace_pixel, uvec4(0u));
		imageStore(rtgi_candidate_second, trace_pixel, uvec4(0u, skipped << 16u, 0u, tag));
		imageStore(ray_image, trace_pixel, vec4(0.0, 0.0, 0.0, -1.0));
		return;
	}

	vec4 normal_roughness = texelFetch(rtgi_normal_roughness, pixel, 0);
	vec2 uv = (vec2(pixel) + 0.5) / vec2(rtgi.screen_size);
	vec4 clip = rtgi.inv_projection * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec3 view_position = clip.xyz / clip.w;
	mat3 to_world = mat3(rtgi.cam_transform);
	vec3 normal = normalize(to_world * normalize(normal_roughness.xyz * 2.0 - 1.0));
	float view_distance = length(view_position);
	vec3 shading_normal_view = normalize(normal_roughness.xyz * 2.0 - 1.0);
	float footprint;
	vec3 geometric_view = rtgi_geometric_normal(pixel, view_position, shading_normal_view, footprint);
	vec3 offset_normal = dot(geometric_view, shading_normal_view) > 0.7 ? normalize(to_world * geometric_view) : normal;
	vec3 anchor_position = to_world * view_position + ray_scene.camera_to_anchor;
	vec3 origin = anchor_position + offset_normal * rtgi_origin_offset(view_distance, anchor_position, footprint);

	vec2 u = rtgi_rand2(uvec2(trace_pixel), rtgi.frame, RTGI_RNG_DIRECTION, rtgi.seed);
	vec3 direction = rtgi_cosine_direction(normal, u);
	if (rtgi.mirror_below_surface != 0u && dot(offset_normal, normal) > 0.999) {
		direction -= 2.0 * min(dot(direction, offset_normal), 0.0) * offset_normal;
	}
	float pdf = max(dot(direction, normal), 1e-4) * 0.31830988;

	RaySceneHit hit;
	vec3 radiance;
	vec3 point;
	uint normal_packed = 0u;
	uint flags = 0u;
	float hit_distance = DDGI_MISS_DISTANCE;
	uint seed = ray_scene_hash(uint(pixel.y) * 65537u + uint(pixel.x) + rtgi.frame * 6271u);
	if (ray_scene_trace(origin, direction, ddgi.max_distance, false, seed, hit)) {
		vec4 shaded = shade_hit(origin, direction, hit, seed);
		radiance = shaded.rgb;
		hit_distance = hit.t;
		point = origin + direction * hit.t;
		vec3 hit_normal = ray_scene_hit_normal(hit);
		normal_packed = rtgi_pack_normal(hit.backface ? -hit_normal : hit_normal);
		if (shaded.a < 0.0) {
			flags |= RTGI_FLAG_BACKFACE;
		}
		if (dot(ray_scene_hit_material(hit).emission, vec3(1.0)) > 0.0) {
			flags |= RTGI_FLAG_EMISSIVE;
		}
	} else {
		radiance = min(sky_radiance_in(direction), vec3(DDGI_MAX_RADIANCE));
		point = direction;
		flags |= RTGI_FLAG_MISS_SKY;
	}

	uvec4 first;
	uvec4 second;
	rtgi_pack_candidate(point, normal_packed, radiance, flags, pdf, tag, first, second);
	imageStore(rtgi_candidate_first, trace_pixel, first);
	imageStore(rtgi_candidate_second, trace_pixel, second);
	imageStore(ray_image, trace_pixel, vec4(radiance, hit_distance));
}

#endif
