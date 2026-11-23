#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "rtgi_inc.glsl"

#if defined(RTGI_COUNTERS) && (defined(MODE_CLASSIFY) || defined(MODE_TEMPORAL))
#define RTGI_COUNTER_TILES_TOTAL 0
#define RTGI_COUNTER_TILES_TRACED 1
#define RTGI_COUNTER_TRACE_PIXELS 2
#define RTGI_COUNTER_RAYS 3
#define RTGI_COUNTER_HITS 4
#define RTGI_COUNTER_SKY_MISSES 5
#define RTGI_COUNTER_BACKFACE 6
#define RTGI_COUNTER_HISTORY_REJECTS 7
#define RTGI_COUNTER_RESTARTED 8
#define RTGI_COUNTER_TILES_WRONG_EVENTS 9
#define RTGI_COUNTER_TILES_WRONG_MEMORY 10
#define RTGI_COUNTER_NONFINITE_ORIGIN 11
#define RTGI_COUNTER_NONFINITE_NORMAL 12

layout(set = 0, binding = 15, std430) restrict buffer RtgiCounters {
	uint words[13];
}
rtgi_counters;

#define RTGI_COUNT(m_slot, m_value) atomicAdd(rtgi_counters.words[m_slot], m_value)
#else
#define RTGI_COUNT(m_slot, m_value)
#endif

layout(set = 0, binding = 0, std140) uniform RtgiFrame {
	mat4 inv_projection;
	mat4 view_to_prev_clip;
	mat4 cur_to_prev_view;
	mat4 cam_transform;

	ivec2 screen_size;
	ivec2 trace_size;
	ivec2 slot_size;
	ivec2 tile_count;

	uint frame;
	uint seed;
	int scale;
	uint flags;

	float target_samples;
	float history_cap;
	float normal_sigma;
	float depth_sigma;

	float variance_sigma;
	float ddgi_alpha_min;
	float ssgi_min_frames;
	uint check_period;

	uint force_trace;
	uint debug_mode;
	float energy;
	int atrous_step;

	int slot_scale;
	uint soft_cut_count;
	float luma_dead_zone;
	uint restart_count;

	vec4 restart_spheres[16];

	uint ddgi_wrong_frames;
	uint ddgi_wrong_bits;
	uint n_target;
	float ddgi_wrong_rho;

	float ddgi_wrong_tau;
	float ddgi_wrong_target;
	float ddgi_wrong_noise;
	float ddgi_wrong_floor;
}
rtgi;

const vec3 rtgi_luma = vec3(0.2126, 0.7152, 0.0722);

vec3 rtgi_decode_normal(vec4 p_encoded) {
	return normalize(p_encoded.xyz * 2.0 - 1.0);
}

vec3 rtgi_view_position(ivec2 p_pixel, float p_depth) {
	vec2 uv = (vec2(p_pixel) + 0.5) / vec2(rtgi.screen_size);
	vec4 clip = rtgi.inv_projection * vec4(uv * 2.0 - 1.0, p_depth, 1.0);
	return clip.xyz / clip.w;
}

#ifdef MODE_CLASSIFY

#define DDGI_SET 1
#define DDGI_UBO_BINDING 0
#define DDGI_SAMPLING
layout(set = 1, binding = 1) uniform utexture2DArray ddgi_probe_texture;
layout(set = 1, binding = 2) uniform texture2DArray ddgi_irradiance_texture;
layout(set = 1, binding = 3) uniform texture2DArray ddgi_distance_texture;
layout(set = 1, binding = 4) uniform sampler ddgi_linear_sampler;
#include "ddgi_inc.glsl"

layout(set = 0, binding = 1) uniform texture2D depth_texture;
layout(set = 0, binding = 2) uniform texture2D normal_roughness_texture;
layout(set = 0, binding = 3) uniform texture2D ssgi_length_texture;
layout(rgba32f, set = 0, binding = 4) uniform restrict writeonly image2D geo_position;
layout(rgba8, set = 0, binding = 5) uniform restrict writeonly image2D geo_normal;
layout(r32ui, set = 0, binding = 6) uniform restrict writeonly uimage2D tile_output;
layout(set = 0, binding = 7) uniform utexture2D tile_memory_previous;
layout(r16ui, set = 0, binding = 8) uniform restrict writeonly uimage2D tile_memory_current;
layout(set = 0, binding = 9) uniform texture2D geo_position_previous;
layout(set = 0, binding = 10) uniform texture2D history_previous;

shared uint shared_geometry;
shared uint shared_ssgi_valid;
shared vec4 shared_ddgi[4];
shared uint shared_ddgi_taps;
shared uint shared_events;
shared uint shared_memory;
shared vec3 shared_history[64];

bool rtgi_previous_trace_pixel(vec3 p_view_position, vec3 p_view_normal, out ivec2 r_pixel) {
	r_pixel = ivec2(0);
	vec4 previous_clip = rtgi.view_to_prev_clip * vec4(p_view_position, 1.0);
	if (previous_clip.w <= 0.0) {
		return false;
	}
	vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
	int scale = max(rtgi.scale, 1);
	vec2 tap_position = (previous_uv * vec2(rtgi.screen_size) - 0.5 - float(scale / 2)) / float(scale);
	ivec2 tap = ivec2(floor(tap_position + 0.5));
	if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, rtgi.trace_size))) {
		return false;
	}
	vec4 previous_geometry = texelFetch(geo_position_previous, tap, 0);
	if (previous_geometry.w < 0.5) {
		return false;
	}
	vec3 previous_position = (rtgi.cur_to_prev_view * vec4(p_view_position, 1.0)).xyz;
	vec3 previous_normal = normalize(mat3(rtgi.cur_to_prev_view) * p_view_normal);
	float tolerance = rtgi.depth_sigma * 0.01 * max(-previous_position.z, 0.1) + 0.005;
	if (abs(dot(previous_normal, previous_position - previous_geometry.xyz)) > tolerance) {
		return false;
	}
	r_pixel = tap;
	return true;
}

bool rtgi_tile_disagrees() {
	float count = 0.0;
	float sum = 0.0;
	float sum_squared = 0.0;
	for (uint i = 0u; i < 64u; i++) {
		sum += shared_history[i].x;
		sum_squared += shared_history[i].y;
		count += shared_history[i].z;
	}
	float ddgi_count = 0.0;
	float ddgi_sum = 0.0;
	for (uint i = 0u; i < 4u; i++) {
		if ((shared_ddgi_taps & (1u << i)) != 0u && shared_ddgi[i].a > 0.0) {
			ddgi_sum += dot(shared_ddgi[i].rgb, rtgi_luma);
			ddgi_count += 1.0;
		}
	}
	if (count < 2.0 || ddgi_count < 1.0) {
		return false;
	}
	float mean = sum / count;
	float ddgi_mean = ddgi_sum / ddgi_count;
	float variance = max(sum_squared - sum * mean, 0.0) / (count - 1.0);
	float effective_count = count / (1.0 + rtgi.ddgi_wrong_rho * (count - 1.0));
	float standard_error = sqrt(variance / effective_count);
	float tau = rtgi.ddgi_wrong_tau;
	return abs(mean - ddgi_mean) / max(ddgi_mean, rtgi.ddgi_wrong_floor) > tau && standard_error < tau * mean / 3.0;
}

uint rtgi_tile_memory(uint p_value) {
	uint left = shared_memory & RTGI_MEMORY_FRAMES;
	uint marks = shared_memory & (RTGI_MEMORY_EVENTS | RTGI_MEMORY_DISAGREE);
	left = left > 0u ? left - 1u : 0u;
	if (left == 0u) {
		marks = 0u;
	}
	uint frames = clamp(rtgi.ddgi_wrong_frames, 1u, RTGI_MEMORY_FRAMES);
	if ((rtgi.ddgi_wrong_bits & RTGI_DDGI_WRONG_EVENTS) != 0u && shared_events != 0u) {
		left = frames;
		marks |= RTGI_MEMORY_EVENTS;
	}
	bool traced = left > 0u || (p_value & (RTGI_TILE_REASON_DDGI | RTGI_TILE_REASON_SSGI | RTGI_TILE_REASON_CHECK | RTGI_TILE_REASON_FORCED)) != 0u;
	if ((rtgi.ddgi_wrong_bits & RTGI_DDGI_WRONG_MEMORY) != 0u && traced && rtgi_tile_disagrees()) {
		left = frames;
		marks |= RTGI_MEMORY_DISAGREE;
	}
	return left | marks;
}

void main() {
	ivec2 trace_pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 local = ivec2(gl_LocalInvocationID.xy);
	uint local_index = gl_LocalInvocationIndex;
	if (local_index == 0u) {
		shared_geometry = 0u;
		shared_ssgi_valid = 0u;
		shared_ddgi_taps = 0u;
		shared_events = 0u;
		shared_memory = 0u;
		for (int i = 0; i < 4; i++) {
			shared_ddgi[i] = vec4(0.0, 0.0, 0.0, 1.0);
		}
	}
	barrier();

	bool in_range = all(lessThan(trace_pixel, rtgi.trace_size));
	ivec2 scale = ivec2(max(rtgi.scale, 1));
	ivec2 pixel = min(trace_pixel * scale + scale / 2, rtgi.screen_size - 1);
	float depth = in_range ? texelFetch(depth_texture, pixel, 0).x : 0.0;
	bool geometry = in_range && depth != 0.0;
	vec4 normal_roughness = vec4(0.0);
	vec3 view_position = vec3(0.0);
	if (geometry) {
		normal_roughness = texelFetch(normal_roughness_texture, pixel, 0);
		view_position = rtgi_view_position(pixel, depth);
		atomicAdd(shared_geometry, 1u);
		bool ssgi_valid = (rtgi.flags & RTGI_FRAME_FLAG_SSGI_VALID_TEXTURE) != 0u && texelFetch(ssgi_length_texture, pixel, 0).r * 255.0 >= rtgi.ssgi_min_frames;
		if (ssgi_valid) {
			atomicAdd(shared_ssgi_valid, 1u);
		}
		bool ddgi_probe = (local.x == 2 || local.x == 6) && (local.y == 2 || local.y == 6);
		if (ddgi_probe) {
			mat3 to_world = mat3(rtgi.cam_transform);
			vec3 world_position = to_world * view_position;
			vec3 world_normal = normalize(to_world * rtgi_decode_normal(normal_roughness));
			uint slot = uint(local.x == 6) + uint(local.y == 6) * 2u;
			shared_ddgi[slot] = ddgi_sample(world_position, world_normal, -normalize(world_position));
			atomicOr(shared_ddgi_taps, 1u << slot);
		}
		if ((rtgi.ddgi_wrong_bits & RTGI_DDGI_WRONG_EVENTS) != 0u) {
			for (uint i = 0u; i < rtgi.restart_count; i++) {
				vec3 offset = view_position - rtgi.restart_spheres[i].xyz;
				if (dot(offset, offset) < rtgi.restart_spheres[i].w * rtgi.restart_spheres[i].w) {
					atomicOr(shared_events, 1u);
					break;
				}
			}
		}
		if ((rtgi.ddgi_wrong_bits & RTGI_DDGI_WRONG_READ) != 0u && all(equal(local, min(ivec2(RTGI_TILE / 2), rtgi.trace_size - 1 - (trace_pixel - local))))) {
			ivec2 previous_pixel;
			if (rtgi_previous_trace_pixel(view_position, rtgi_decode_normal(normal_roughness), previous_pixel)) {
				shared_memory = texelFetch(tile_memory_previous, previous_pixel / RTGI_TILE, 0).r;
			}
		}
	}
	if ((rtgi.ddgi_wrong_bits & RTGI_DDGI_WRONG_MEMORY) != 0u) {
		vec3 history_sample = vec3(0.0);
		ivec2 previous_pixel;
		if (geometry && rtgi_previous_trace_pixel(view_position, rtgi_decode_normal(normal_roughness), previous_pixel)) {
			vec4 history = texelFetch(history_previous, previous_pixel, 0);
			if (history.a > 0.0 && rtgi_is_finite(history.rgb)) {
				float luma = dot(history.rgb, rtgi_luma);
				history_sample = vec3(luma, luma * luma, 1.0);
			}
		}
		shared_history[local_index] = history_sample;
	}
	if (in_range) {
		imageStore(geo_position, trace_pixel, vec4(view_position, geometry ? 1.0 : 0.0));
		imageStore(geo_normal, trace_pixel, vec4(geometry ? rtgi_decode_normal(normal_roughness) * 0.5 + 0.5 : vec3(0.0), normal_roughness.w));
	}
	barrier();

	if (local_index == 0u && all(lessThan(gl_WorkGroupID.xy, uvec2(rtgi.tile_count)))) {
		uint count = shared_geometry;
		uint value = 0u;
		uint memory = 0u;
		if (count > 0u) {
			value |= RTGI_TILE_HAS_GEOMETRY;
			float ddgi_min = min(min(shared_ddgi[0].a, shared_ddgi[1].a), min(shared_ddgi[2].a, shared_ddgi[3].a));
			if (ddgi_min < rtgi.ddgi_alpha_min) {
				value |= RTGI_TILE_REASON_DDGI;
			}
			if (float(shared_ssgi_valid) < 0.5 * float(count)) {
				value |= RTGI_TILE_REASON_SSGI;
			}
			uint tile_hash = rtgi_pcg(gl_WorkGroupID.x + rtgi_pcg(gl_WorkGroupID.y + rtgi.seed));
			if (rtgi.check_period > 0u && ((tile_hash + rtgi.frame) % rtgi.check_period) == 0u) {
				value |= RTGI_TILE_REASON_CHECK;
			}
			if (rtgi.force_trace != 0u) {
				value |= RTGI_TILE_REASON_FORCED;
			}
			if ((rtgi.ddgi_wrong_bits & RTGI_DDGI_WRONG_ACTIVE) != 0u) {
				memory = rtgi_tile_memory(value);
				if ((memory & RTGI_MEMORY_FRAMES) != 0u) {
					value |= RTGI_TILE_REASON_DDGI_WRONG;
				}
			}
			if ((value & (RTGI_TILE_REASON_DDGI | RTGI_TILE_REASON_SSGI | RTGI_TILE_REASON_CHECK | RTGI_TILE_REASON_FORCED | RTGI_TILE_REASON_DDGI_WRONG)) != 0u) {
				value |= RTGI_TILE_TRACE;
			}
			RTGI_COUNT(RTGI_COUNTER_TILES_TOTAL, 1u);
			if ((value & RTGI_TILE_TRACE) != 0u) {
				RTGI_COUNT(RTGI_COUNTER_TILES_TRACED, 1u);
				RTGI_COUNT(RTGI_COUNTER_TRACE_PIXELS, count);
			}
			if ((memory & RTGI_MEMORY_EVENTS) != 0u) {
				RTGI_COUNT(RTGI_COUNTER_TILES_WRONG_EVENTS, 1u);
			}
			if ((memory & RTGI_MEMORY_DISAGREE) != 0u) {
				RTGI_COUNT(RTGI_COUNTER_TILES_WRONG_MEMORY, 1u);
			}
		}
		if ((rtgi.ddgi_wrong_bits & RTGI_DDGI_WRONG_ACTIVE) != 0u) {
			imageStore(tile_memory_current, ivec2(gl_WorkGroupID.xy), uvec4(memory));
		}
		imageStore(tile_output, ivec2(gl_WorkGroupID.xy), uvec4(value));
	}
}

#endif

#ifdef MODE_TEMPORAL

layout(set = 0, binding = 1) uniform utexture2D candidate_second;
layout(set = 0, binding = 2) uniform texture2D geo_position_current;
layout(set = 0, binding = 3) uniform texture2D geo_normal_current;
layout(set = 0, binding = 4) uniform texture2D geo_position_previous;
layout(set = 0, binding = 5) uniform texture2D geo_normal_previous;
layout(set = 0, binding = 6) uniform texture2D history_previous;
layout(set = 0, binding = 7) uniform texture2D moments_previous;
layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2D history_current;
layout(rgba16f, set = 0, binding = 9) uniform restrict writeonly image2D moments_current;

void main() {
	ivec2 trace_pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(trace_pixel, rtgi.trace_size))) {
		return;
	}

	uvec4 second = texelFetch(candidate_second, trace_pixel, 0);
	uint candidate_flags = rtgi_candidate_flags(second);
#ifdef RTGI_COUNTERS
	if ((candidate_flags & RTGI_FLAG_NO_CANDIDATE) == 0u) {
		RTGI_COUNT(RTGI_COUNTER_RAYS, 1u);
		RTGI_COUNT((candidate_flags & RTGI_FLAG_MISS_SKY) != 0u ? RTGI_COUNTER_SKY_MISSES : RTGI_COUNTER_HITS, 1u);
		if ((candidate_flags & RTGI_FLAG_BACKFACE) != 0u) {
			RTGI_COUNT(RTGI_COUNTER_BACKFACE, 1u);
		}
	}
	if ((candidate_flags & RTGI_FLAG_NONFINITE_ORIGIN) != 0u) {
		RTGI_COUNT(RTGI_COUNTER_NONFINITE_ORIGIN, 1u);
	}
	if ((candidate_flags & RTGI_FLAG_NONFINITE_NORMAL) != 0u) {
		RTGI_COUNT(RTGI_COUNTER_NONFINITE_NORMAL, 1u);
	}
#endif
	vec3 radiance = rtgi_candidate_radiance(second);
	vec4 position_current = texelFetch(geo_position_current, trace_pixel, 0);
	bool carry = (candidate_flags & RTGI_FLAG_THINNED) != 0u;
	if (position_current.w < 0.5 || (!carry && ((candidate_flags & RTGI_FLAG_NO_CANDIDATE) != 0u || !rtgi_is_finite(radiance)))) {
		imageStore(history_current, trace_pixel, vec4(0.0));
		imageStore(moments_current, trace_pixel, vec4(0.0));
		return;
	}

	vec4 normal_encoded_current = texelFetch(geo_normal_current, trace_pixel, 0);
	vec3 normal_current = rtgi_decode_normal(normal_encoded_current);
	bool dynamic_current = normal_encoded_current.w > 0.5;

	vec3 accumulated_radiance = vec3(0.0);
	float accumulated_count = 0.0;
	float accumulated_m1 = 0.0;
	float accumulated_m2 = 0.0;
	float accumulated_confidence = 0.0;
	float accepted_weight = 0.0;
	float history_existed = 0.0;
	bool module_output = (rtgi.flags & RTGI_FRAME_FLAG_MODULE_OUTPUT) != 0u;

	bool allow_history = (rtgi.flags & (RTGI_FRAME_FLAG_HISTORY_CUT | RTGI_FRAME_FLAG_CAMERA_CUT | RTGI_FRAME_FLAG_RESIZE | RTGI_FRAME_FLAG_BACKEND_SWITCH)) == 0u;
	if (allow_history) {
		vec4 previous_clip = rtgi.view_to_prev_clip * vec4(position_current.xyz, 1.0);
		vec3 previous_position = (rtgi.cur_to_prev_view * vec4(position_current.xyz, 1.0)).xyz;
		vec3 previous_normal = normalize(mat3(rtgi.cur_to_prev_view) * normal_current);
		if (previous_clip.w > 0.0) {
			vec2 previous_uv = previous_clip.xy / previous_clip.w * 0.5 + 0.5;
			int scale = max(rtgi.scale, 1);
			vec2 tap_position = (previous_uv * vec2(rtgi.screen_size) - 0.5 - float(scale / 2)) / float(scale);
			ivec2 base = ivec2(floor(tap_position));
			vec2 fraction = tap_position - vec2(base);
			float tolerance = rtgi.depth_sigma * 0.01 * max(-previous_position.z, 0.1) + 0.005;
			for (int k = 0; k < 4; k++) {
				ivec2 offset = ivec2(k & 1, k >> 1);
				ivec2 tap = base + offset;
				float bilinear = (offset.x == 1 ? fraction.x : 1.0 - fraction.x) * (offset.y == 1 ? fraction.y : 1.0 - fraction.y);
				if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, rtgi.trace_size)) || bilinear <= 0.0) {
					continue;
				}
				vec4 tap_position_previous = texelFetch(geo_position_previous, tap, 0);
				vec4 tap_history = texelFetch(history_previous, tap, 0);
				if (tap_position_previous.w < 0.5 || tap_history.a <= 0.0) {
					continue;
				}
				history_existed += bilinear;
				vec4 tap_normal_encoded = texelFetch(geo_normal_previous, tap, 0);
				vec3 tap_normal = rtgi_decode_normal(tap_normal_encoded);
				float plane_distance = abs(dot(tap_normal, previous_position - tap_position_previous.xyz));
				if (plane_distance > tolerance || dot(tap_normal, previous_normal) < 0.9 || (tap_normal_encoded.w > 0.5) != dynamic_current) {
					continue;
				}
				vec4 tap_moments = texelFetch(moments_previous, tap, 0);
				accumulated_radiance += tap_history.rgb * bilinear;
				accumulated_count += tap_history.a * bilinear;
				accumulated_m1 += tap_moments.x * bilinear;
				accumulated_m2 += tap_moments.y * bilinear;
				if (module_output) {
					accumulated_confidence += tap_moments.w * bilinear;
				}
				accepted_weight += bilinear;
			}
		}
	}

	float count_previous = 0.0;
	vec3 radiance_previous = vec3(0.0);
	float m1_previous = 0.0;
	float m2_previous = 0.0;
	float confidence_previous = 0.0;
	float validity = 0.0;
	if (accepted_weight > 0.01) {
		float inverse_weight = 1.0 / accepted_weight;
		radiance_previous = accumulated_radiance * inverse_weight;
		count_previous = accumulated_count * inverse_weight;
		if (!module_output && (rtgi.flags & RTGI_FRAME_FLAG_SOFT_CUT) != 0u) {
			count_previous = min(count_previous, float(rtgi.soft_cut_count));
		}
		if (!module_output && (rtgi.flags & RTGI_FRAME_FLAG_RESTART_SPHERES) != 0u) {
			for (uint i = 0u; i < rtgi.restart_count; i++) {
				vec3 offset = position_current.xyz - rtgi.restart_spheres[i].xyz;
				if (dot(offset, offset) < rtgi.restart_spheres[i].w * rtgi.restart_spheres[i].w) {
					count_previous = min(count_previous, float(rtgi.soft_cut_count));
					RTGI_COUNT(RTGI_COUNTER_RESTARTED, 1u);
					break;
				}
			}
		}
		m1_previous = accumulated_m1 * inverse_weight;
		m2_previous = accumulated_m2 * inverse_weight;
		confidence_previous = accumulated_confidence * inverse_weight;
		validity = clamp(accepted_weight, 0.0, 1.0);
	}
	if (allow_history && history_existed > 0.01 && accepted_weight <= 0.01) {
		RTGI_COUNT(RTGI_COUNTER_HISTORY_REJECTS, 1u);
	}

	if (carry) {
		if (count_previous <= 0.0) {
			imageStore(history_current, trace_pixel, vec4(0.0));
			imageStore(moments_current, trace_pixel, vec4(0.0));
			return;
		}
		imageStore(history_current, trace_pixel, vec4(radiance_previous, count_previous));
		imageStore(moments_current, trace_pixel, vec4(m1_previous, m2_previous, validity, module_output ? confidence_previous : 0.0));
		return;
	}

	float blend = max(1.0 / (count_previous + 1.0), 1.0 / max(rtgi.history_cap, 1.0));
	float count_current = min(count_previous + 1.0, max(rtgi.history_cap, 1.0));
	vec3 radiance_current = count_previous > 0.0 ? mix(radiance_previous, radiance, blend) : radiance;
	float luma = dot(radiance, rtgi_luma);
	float m1_current = count_previous > 0.0 ? mix(m1_previous, luma, blend) : luma;
	float m2_current = count_previous > 0.0 ? mix(m2_previous, luma * luma, blend) : luma * luma;
	float confidence_current = 0.0;
	if (module_output) {
		float m_packed = uintBitsToFloat(second.z);
		float confidence = rtgi.n_target > 0u && m_packed > 0.0 ? min(m_packed / float(rtgi.n_target), 1.0) : 0.0;
		confidence_current = count_previous > 0.0 ? mix(confidence_previous, confidence, blend) : confidence;
	}

	imageStore(history_current, trace_pixel, vec4(radiance_current, count_current));
	imageStore(moments_current, trace_pixel, vec4(m1_current, m2_current, validity, confidence_current));
}

#endif

#ifdef MODE_VARIANCE

layout(set = 0, binding = 1) uniform texture2D history_texture;
layout(set = 0, binding = 2) uniform texture2D moments_texture;
layout(set = 0, binding = 3) uniform texture2D geo_position_texture;
layout(set = 0, binding = 4) uniform texture2D geo_normal_texture;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D filter_output;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, rtgi.trace_size))) {
		return;
	}
	vec4 history = texelFetch(history_texture, pixel, 0);
	if (history.a <= 0.0) {
		imageStore(filter_output, pixel, vec4(0.0));
		return;
	}
	vec4 moments = texelFetch(moments_texture, pixel, 0);
	float variance;
	if (history.a >= 4.0) {
		variance = max(moments.y - moments.x * moments.x, 0.0) / history.a;
	} else {
		vec4 center_position = texelFetch(geo_position_texture, pixel, 0);
		vec3 center_normal = rtgi_decode_normal(texelFetch(geo_normal_texture, pixel, 0));
		float tolerance = rtgi.depth_sigma * 0.01 * max(-center_position.z, 0.1) + 0.005;
		float sum_weight = 0.0;
		float sum_luma = 0.0;
		float sum_luma_squared = 0.0;
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec2 tap = pixel + ivec2(x, y);
				if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, rtgi.trace_size))) {
					continue;
				}
				vec4 tap_history = texelFetch(history_texture, tap, 0);
				vec4 tap_position = texelFetch(geo_position_texture, tap, 0);
				if (tap_history.a <= 0.0 || tap_position.w < 0.5) {
					continue;
				}
				vec3 tap_normal = rtgi_decode_normal(texelFetch(geo_normal_texture, tap, 0));
				if (abs(dot(center_normal, tap_position.xyz - center_position.xyz)) > tolerance || dot(center_normal, tap_normal) < 0.8) {
					continue;
				}
				float luma = dot(tap_history.rgb, rtgi_luma);
				sum_weight += 1.0;
				sum_luma += luma;
				sum_luma_squared += luma * luma;
			}
		}
		float mean = sum_luma / max(sum_weight, 1.0);
		variance = max(sum_luma_squared / max(sum_weight, 1.0) - mean * mean, 0.0);
	}
	imageStore(filter_output, pixel, vec4(history.rgb, variance));
}

#endif

#ifdef MODE_ATROUS

layout(set = 0, binding = 1) uniform texture2D filter_input;
layout(set = 0, binding = 2) uniform texture2D geo_position_texture;
layout(set = 0, binding = 3) uniform texture2D geo_normal_texture;
layout(set = 0, binding = 4) uniform texture2D history_texture;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D filter_output;

const float rtgi_kernel[3] = float[](0.375, 0.25, 0.0625);

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, rtgi.trace_size))) {
		return;
	}
	vec4 center = texelFetch(filter_input, pixel, 0);
	vec4 center_position = texelFetch(geo_position_texture, pixel, 0);
	if (texelFetch(history_texture, pixel, 0).a <= 0.0 || center_position.w < 0.5) {
		imageStore(filter_output, pixel, vec4(0.0));
		return;
	}
	vec3 center_normal = rtgi_decode_normal(texelFetch(geo_normal_texture, pixel, 0));
	float center_luma = dot(center.rgb, rtgi_luma);

	float prefiltered_variance = 0.0;
	float prefilter_weight = 0.0;
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			ivec2 tap = pixel + ivec2(x, y);
			if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, rtgi.trace_size))) {
				continue;
			}
			if (texelFetch(history_texture, tap, 0).a <= 0.0) {
				continue;
			}
			float w = rtgi_kernel[abs(x)] * rtgi_kernel[abs(y)];
			prefiltered_variance += texelFetch(filter_input, tap, 0).a * w;
			prefilter_weight += w;
		}
	}
	prefiltered_variance /= max(prefilter_weight, 1e-6);
	float luma_scale = rtgi.variance_sigma * sqrt(max(prefiltered_variance, 0.0)) + 1e-4;
	float luma_dead_zone = rtgi.luma_dead_zone * sqrt(max(prefiltered_variance, 0.0));
	float tolerance = rtgi.depth_sigma * 0.01 * max(-center_position.z, 0.1) + 0.005;

	vec3 sum_radiance = center.rgb;
	float sum_variance = center.a;
	float sum_weight = 1.0;
	for (int y = -2; y <= 2; y++) {
		for (int x = -2; x <= 2; x++) {
			if (x == 0 && y == 0) {
				continue;
			}
			ivec2 tap = pixel + ivec2(x, y) * rtgi.atrous_step;
			if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, rtgi.trace_size))) {
				continue;
			}
			if (texelFetch(history_texture, tap, 0).a <= 0.0) {
				continue;
			}
			vec4 tap_position = texelFetch(geo_position_texture, tap, 0);
			if (tap_position.w < 0.5) {
				continue;
			}
			vec3 tap_normal = rtgi_decode_normal(texelFetch(geo_normal_texture, tap, 0));
			vec4 tap_value = texelFetch(filter_input, tap, 0);
			float plane_distance = abs(dot(center_normal, tap_position.xyz - center_position.xyz));
			float w = rtgi_kernel[abs(x)] * rtgi_kernel[abs(y)];
			w *= exp(-plane_distance / tolerance);
			w *= pow(max(dot(center_normal, tap_normal), 0.0), rtgi.normal_sigma);
			w *= exp(-max(abs(center_luma - dot(tap_value.rgb, rtgi_luma)) - luma_dead_zone, 0.0) / luma_scale);
			sum_radiance += tap_value.rgb * w;
			sum_variance += tap_value.a * w * w;
			sum_weight += w;
		}
	}
	float inverse_weight = 1.0 / sum_weight;
	imageStore(filter_output, pixel, vec4(sum_radiance * inverse_weight, sum_variance * inverse_weight * inverse_weight));
}

#endif

#ifdef MODE_COMPOSE

layout(set = 0, binding = 1) uniform texture2D depth_texture;
layout(set = 0, binding = 2) uniform texture2D normal_roughness_texture;
layout(set = 0, binding = 3) uniform texture2D filter_input;
layout(set = 0, binding = 4) uniform texture2D geo_position_texture;
layout(set = 0, binding = 5) uniform texture2D geo_normal_texture;
layout(set = 0, binding = 6) uniform texture2D history_texture;
layout(set = 0, binding = 7) uniform texture2D moments_texture;
layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2D slot_radiance;
layout(r16f, set = 0, binding = 9) uniform restrict writeonly image2D slot_coverage;
layout(set = 0, binding = 10) uniform utexture2D tile_texture;

float rtgi_route_confidence(vec2 p_tap_position, float p_count, float p_confidence, float p_m1, float p_m2) {
	float target = max(rtgi.target_samples, 1.0);
	float share = 1.0;
	if ((rtgi.ddgi_wrong_bits & RTGI_DDGI_WRONG_ACTIVE) != 0u && p_count >= 2.0) {
		ivec2 trace_pixel = clamp(ivec2(floor(p_tap_position + 0.5)), ivec2(0), rtgi.trace_size - 1);
		bool marked = (texelFetch(tile_texture, trace_pixel / RTGI_TILE, 0).r & RTGI_TILE_REASON_DDGI_WRONG) != 0u;
		float deviation = sqrt(max(p_m2 - p_m1 * p_m1, 0.0)) / max(p_m1, rtgi.ddgi_wrong_floor);
		if (marked && deviation < rtgi.ddgi_wrong_noise) {
			target = 1.0;
			share = clamp(rtgi.ddgi_wrong_target / float(rtgi.n_target), 1e-3, 1.0);
		}
	}
	return min(clamp(p_count / target, 0.0, 1.0), clamp(p_confidence / share, 0.0, 1.0));
}

void main() {
	ivec2 slot_pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(slot_pixel, rtgi.slot_size))) {
		return;
	}
	ivec2 pixel = min(slot_pixel * rtgi.slot_scale, rtgi.screen_size - 1);
	float depth = texelFetch(depth_texture, pixel, 0).x;
	if (depth == 0.0) {
		imageStore(slot_radiance, slot_pixel, vec4(0.0));
		imageStore(slot_coverage, slot_pixel, vec4(0.0));
		return;
	}
	vec3 position = rtgi_view_position(pixel, depth);
	vec3 normal = rtgi_decode_normal(texelFetch(normal_roughness_texture, pixel, 0));
	int scale = max(rtgi.scale, 1);
	vec2 tap_position = (vec2(pixel) - float(scale / 2)) / float(scale);
	ivec2 base = ivec2(floor(tap_position));
	vec2 fraction = tap_position - vec2(base);
	float tolerance = rtgi.depth_sigma * 0.01 * max(-position.z, 0.1) + 0.005;

	vec3 sum_radiance = vec3(0.0);
	float sum_weight = 0.0;
	float sum_count = 0.0;
	float sum_validity = 0.0;
	float sum_bilinear = 0.0;
	float sum_m1 = 0.0;
	float sum_m2 = 0.0;
	float sum_confidence = 0.0;
	for (int k = 0; k < 4; k++) {
		ivec2 offset = ivec2(k & 1, k >> 1);
		ivec2 tap = base + offset;
		float bilinear = (offset.x == 1 ? fraction.x : 1.0 - fraction.x) * (offset.y == 1 ? fraction.y : 1.0 - fraction.y);
		if (any(lessThan(tap, ivec2(0))) || any(greaterThanEqual(tap, rtgi.trace_size))) {
			continue;
		}
		sum_bilinear += bilinear;
		vec4 tap_geometry = texelFetch(geo_position_texture, tap, 0);
		vec4 tap_history = texelFetch(history_texture, tap, 0);
		if (tap_geometry.w < 0.5 || tap_history.a <= 0.0) {
			continue;
		}
		vec3 tap_normal = rtgi_decode_normal(texelFetch(geo_normal_texture, tap, 0));
		float plane_distance = abs(dot(normal, position - tap_geometry.xyz));
		float w = bilinear * exp(-plane_distance / tolerance) * pow(max(dot(normal, tap_normal), 0.0), rtgi.normal_sigma);
		if (w <= 1e-5) {
			continue;
		}
		sum_radiance += texelFetch(filter_input, tap, 0).rgb * w;
		sum_count += tap_history.a * w;
		vec4 tap_moments = texelFetch(moments_texture, tap, 0);
		sum_validity += tap_moments.z * w;
		sum_m1 += tap_moments.x * w;
		sum_m2 += tap_moments.y * w;
		sum_confidence += tap_moments.w * w;
		sum_weight += w;
	}
	if (sum_weight <= 1e-5 || sum_bilinear <= 1e-5) {
		imageStore(slot_radiance, slot_pixel, vec4(0.0));
		imageStore(slot_coverage, slot_pixel, vec4(0.0));
		return;
	}
	float inverse_weight = 1.0 / sum_weight;
	float confidence = clamp(sum_count * inverse_weight / max(rtgi.target_samples, 1.0), 0.0, 1.0);
	if ((rtgi.flags & RTGI_FRAME_FLAG_MODULE_OUTPUT) != 0u && rtgi.n_target > 0u) {
		confidence = rtgi_route_confidence(tap_position, sum_count * inverse_weight, sum_confidence * inverse_weight, sum_m1 * inverse_weight, sum_m2 * inverse_weight);
	}
	float validity = clamp(sum_validity * inverse_weight, 0.0, 1.0);
	float support = clamp(sum_weight / sum_bilinear, 0.0, 1.0);
	float coverage = confidence * validity * support;
	imageStore(slot_radiance, slot_pixel, vec4(max(sum_radiance * inverse_weight, vec3(0.0)) * rtgi.energy, 1.0));
	imageStore(slot_coverage, slot_pixel, vec4(coverage));
}

#endif

#ifdef MODE_DEBUG

layout(set = 0, binding = 1) uniform utexture2D tile_texture;
layout(set = 0, binding = 2) uniform utexture2D candidate_second;
layout(set = 0, binding = 3) uniform texture2D history_texture;
layout(set = 0, binding = 4) uniform texture2D moments_texture;
layout(set = 0, binding = 5) uniform texture2D filter_input;
layout(set = 0, binding = 6) uniform texture2D geo_position_texture;
layout(rgba16f, set = 0, binding = 7) uniform restrict writeonly image2D debug_output;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, rtgi.trace_size))) {
		return;
	}
	uint tile = texelFetch(tile_texture, pixel / RTGI_TILE, 0).r;
	uint candidate_flags = rtgi_candidate_flags(texelFetch(candidate_second, pixel, 0));
	vec4 history = texelFetch(history_texture, pixel, 0);
	vec4 moments = texelFetch(moments_texture, pixel, 0);
	vec4 filtered = texelFetch(filter_input, pixel, 0);
	bool geometry = texelFetch(geo_position_texture, pixel, 0).w > 0.5;
	bool traced = (candidate_flags & RTGI_FLAG_NO_CANDIDATE) == 0u;
	vec3 color = vec3(0.0);
	if (rtgi.debug_mode == RTGI_DEBUG_TILE_CLASS) {
		if (geometry) {
			color = vec3(0.08);
			if ((tile & RTGI_TILE_TRACE) != 0u) {
				bool ddgi = (tile & RTGI_TILE_REASON_DDGI) != 0u;
				bool ssgi = (tile & RTGI_TILE_REASON_SSGI) != 0u;
				if (ddgi && ssgi) {
					color = vec3(1.0, 0.1, 0.9);
				} else if (ddgi) {
					color = vec3(1.0, 0.15, 0.1);
				} else if (ssgi) {
					color = vec3(0.1, 0.3, 1.0);
				} else if ((tile & RTGI_TILE_REASON_CHECK) != 0u) {
					color = vec3(1.0, 0.9, 0.1);
				} else {
					color = vec3(1.0);
				}
			}
		}
	} else if (rtgi.debug_mode == RTGI_DEBUG_RAY_VALIDITY) {
		if (traced) {
			if ((candidate_flags & RTGI_FLAG_BACKFACE) != 0u) {
				color = vec3(1.0, 0.0, 0.7);
			} else if ((candidate_flags & RTGI_FLAG_MISS_SKY) != 0u) {
				color = vec3(0.2, 0.4, 1.0);
			} else if (moments.z < 0.5) {
				color = vec3(1.0, 0.5, 0.0);
			} else {
				color = vec3(0.1, 0.3 + 0.7 * clamp(history.a / max(rtgi.target_samples, 1.0), 0.0, 1.0), 0.2);
			}
		}
	} else if (rtgi.debug_mode == RTGI_DEBUG_CONTRIBUTION) {
		if (traced && history.a > 0.0) {
			float coverage = clamp(history.a / max(rtgi.target_samples, 1.0), 0.0, 1.0) * clamp(moments.z, 0.0, 1.0);
			float relative_deviation = clamp(sqrt(max(filtered.a, 0.0)) / (dot(filtered.rgb, rtgi_luma) + 0.05), 0.0, 1.0);
			color = vec3(coverage, relative_deviation, 0.0);
		}
	} else if (rtgi.debug_mode == RTGI_DEBUG_HISTORY_COUNT) {
		float stored = !geometry ? -100.0 : ((tile & RTGI_TILE_TRACE) != 0u ? history.a : -1.0 - history.a);
		imageStore(debug_output, pixel, vec4(texelFetch(geo_position_texture, pixel, 0).xyz, stored));
		return;
	}
	imageStore(debug_output, pixel, vec4(color, 1.0));
}

#endif
