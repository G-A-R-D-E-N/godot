#[compute]

#version 450

#VERSION_DEFINES

#ifdef USE_DDGI
#extension GL_EXT_samplerless_texture_functions : enable
#endif

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define M_PI 3.14159265359
#define MISS_DISTANCE 1000.0
#define MAX_SAMPLE_WEIGHT 4.0
#define REACTIVE_FRACTION 0.25
#define NEIGHBORHOOD_RADIUS 4
#define NEIGHBORHOOD_GAUSSIAN_K 3.0
#define NOISE_SIGMAS 4.0
#define FAST_BLEND 0.25
#define FAST_NOISE_SCALE 0.378

#define STATE_DISOCCLUDED -1.0
#define STATE_OFF_SCREEN -2.0
#define STATE_OPEN -3.0
#define STATE_STEPS -4.0
#define STATE_REJECTED -5.0

#define TRACE_HIT 0
#define TRACE_OFF_SCREEN 1
#define TRACE_OPEN 2
#define TRACE_STEPS 3

#define DEBUG_TERMINATION 1u
#define DEBUG_CONFIDENCE 2u

layout(set = 0, binding = 0, std140) uniform SceneData {
	mat4 projection[2];
	mat4 inv_projection[2];
	mat4 reprojection[2];
	vec4 eye_offset[2];
	mat4 prev_inv_projection[2];
	mat4 cur_to_prev;
}
scene_data;

#ifdef MODE_TRACE

layout(set = 0, binding = 1) uniform sampler2D source_color;
layout(set = 0, binding = 2) uniform sampler2D source_hiz;
layout(set = 0, binding = 3) uniform sampler2D source_normal_roughness;
layout(rgba16f, set = 0, binding = 4) uniform restrict writeonly image2D ray_radiance;
layout(rgba16f, set = 0, binding = 5) uniform restrict writeonly image2D ray_hit;

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	int mipmaps;
	int num_steps;
	float max_distance;
	float thickness;
	bool orthogonal;
	int view_index;
	uint frame;
	int rays;
	bool validate_source;
	float source_mip;
	vec4 view_to_world[3];
}
params;

#include "screen_space_source_validation_inc.glsl"

#ifdef USE_DDGI
#define DDGI_SET 1
#define DDGI_UBO_BINDING 0
#define DDGI_SAMPLING
layout(set = 1, binding = 1) uniform utexture2DArray ddgi_probe_texture;
layout(set = 1, binding = 2) uniform texture2DArray ddgi_irradiance_texture;
layout(set = 1, binding = 3) uniform texture2DArray ddgi_distance_texture;
layout(set = 1, binding = 4) uniform sampler ddgi_linear_sampler;
#include "../environment/ddgi_inc.glsl"

vec3 view_to_world(vec3 p_vector) {
	return vec3(dot(params.view_to_world[0].xyz, p_vector), dot(params.view_to_world[1].xyz, p_vector), dot(params.view_to_world[2].xyz, p_vector));
}
#endif

#else

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	ivec2 trace_size;
	int rays;
	uint frame;
	bool temporal;
	bool history_valid;
	bool orthogonal;
	int view_index;
	float exposure;
	float firefly_luminance;
	int history_frames;
	uint debug_mode;
	int pad;
	int pad2;
}
params;

#ifdef MODE_RESOLVE

layout(set = 0, binding = 1) uniform sampler2D ray_radiance;
layout(set = 0, binding = 2) uniform sampler2D ray_hit;
layout(set = 0, binding = 3) uniform sampler2D source_hiz;
layout(set = 0, binding = 4) uniform sampler2D source_normal_roughness_trace;
layout(set = 0, binding = 5) uniform sampler2D source_depth;
layout(set = 0, binding = 6) uniform sampler2D source_normal_roughness;
layout(rgba16f, set = 0, binding = 7) uniform restrict writeonly image2D output_resolved;
layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2D output_info;
layout(set = 0, binding = 9) uniform sampler2D source_exposure;

#else

layout(set = 0, binding = 1) uniform sampler2D resolved;
layout(set = 0, binding = 2) uniform sampler2D resolved_info;
layout(set = 0, binding = 3) uniform sampler2D source_depth;
layout(set = 0, binding = 4) uniform sampler2D source_normal_roughness;
layout(set = 0, binding = 5) uniform sampler2D history;
layout(set = 0, binding = 6) uniform sampler2D history_length;
layout(set = 0, binding = 7) uniform sampler2D source_previous;
layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2D output_color;
#ifndef MODE_FAST
layout(r8, set = 0, binding = 9) uniform restrict writeonly image2D output_length;
layout(set = 0, binding = 10) uniform sampler2D fast_history;
#endif

#endif

#endif

vec3 compute_view_pos(vec3 screen_pos) {
	vec4 pos;
	pos.xy = screen_pos.xy * 2.0 - 1.0;
	pos.z = screen_pos.z;
	pos.w = 1.0;
	pos = scene_data.inv_projection[params.view_index] * pos;
	return pos.xyz / pos.w;
}

vec3 compute_screen_pos(vec3 pos) {
	vec4 screen_pos = scene_data.projection[params.view_index] * vec4(pos, 1.0);
	screen_pos.xyz /= screen_pos.w;
	screen_pos.xy = screen_pos.xy * 0.5 + 0.5;
	return screen_pos.xyz;
}

float interleaved_gradient_noise(vec2 p_pos) {
	return fract(52.9829189 * fract(dot(p_pos, vec2(0.06711056, 0.00583715))));
}

float luminance(vec3 p_color) {
	return dot(p_color, vec3(0.2126, 0.7152, 0.0722));
}

#ifdef MODE_TRACE

vec2 compute_cell_count(int level) {
	int cell_count_x = max(1, params.screen_size.x >> level);
	int cell_count_y = max(1, params.screen_size.y >> level);
	return vec2(cell_count_x, cell_count_y);
}

float linearize_depth(float depth) {
	vec4 pos = vec4(0.0, 0.0, depth, 1.0);
	pos = scene_data.inv_projection[params.view_index] * pos;
	return pos.z / pos.w;
}

vec3 compute_geometric_normal(ivec2 pixel_pos, float depth_c, vec3 view_c, float pixel_offset) {
	ivec2 last = params.screen_size - 1;
	vec4 H = vec4(
			texelFetch(source_hiz, clamp(pixel_pos + ivec2(-1, 0), ivec2(0), last), 0).x,
			texelFetch(source_hiz, clamp(pixel_pos + ivec2(-2, 0), ivec2(0), last), 0).x,
			texelFetch(source_hiz, clamp(pixel_pos + ivec2(1, 0), ivec2(0), last), 0).x,
			texelFetch(source_hiz, clamp(pixel_pos + ivec2(2, 0), ivec2(0), last), 0).x);

	vec4 V = vec4(
			texelFetch(source_hiz, clamp(pixel_pos + ivec2(0, -1), ivec2(0), last), 0).x,
			texelFetch(source_hiz, clamp(pixel_pos + ivec2(0, -2), ivec2(0), last), 0).x,
			texelFetch(source_hiz, clamp(pixel_pos + ivec2(0, 1), ivec2(0), last), 0).x,
			texelFetch(source_hiz, clamp(pixel_pos + ivec2(0, 2), ivec2(0), last), 0).x);

	vec2 he = abs((2.0 * H.xz - H.yw) - depth_c);
	vec2 ve = abs((2.0 * V.xz - V.yw) - depth_c);

	int h_sign = he.x < he.y ? -1 : 1;
	int v_sign = ve.x < ve.y ? -1 : 1;

	vec3 view_h = compute_view_pos(vec3((pixel_pos + vec2(h_sign, 0) + pixel_offset) / params.screen_size, H[1 + int(h_sign)]));
	vec3 view_v = compute_view_pos(vec3((pixel_pos + vec2(0, v_sign) + pixel_offset) / params.screen_size, V[1 + int(v_sign)]));

	vec3 h_der = h_sign * (view_h - view_c);
	vec3 v_der = v_sign * (view_v - view_c);

	return cross(v_der, h_der);
}

vec2 sample_sequence(ivec2 p_pixel, uint p_index) {
	vec2 offset = vec2(interleaved_gradient_noise(vec2(p_pixel)), interleaved_gradient_noise(vec2(p_pixel) + vec2(47.0, 17.0)));
	return fract(offset + float(p_index + 1u) * vec2(0.7548776662466927, 0.5698402909980532));
}

int trace_ray(vec3 p_pos, vec3 p_screen_pos, vec3 p_ray_dir, out vec3 r_hit_screen_pos, out vec3 r_free_screen_pos) {
	vec3 end_pos = p_pos + p_ray_dir * params.max_distance;
	if (!params.orthogonal && end_pos.z > -0.00001) {
		end_pos -= p_ray_dir / p_ray_dir.z * (end_pos.z + 0.00001);
	}

	vec3 screen_end_pos = compute_screen_pos(end_pos);
	vec3 screen_ray_dir = screen_end_pos - p_screen_pos;
	float t_end = max(abs(screen_ray_dir.z), 1e-9);
	screen_ray_dir /= t_end;

	bool facing_camera = screen_ray_dir.z >= 0.0;

	vec2 t0 = (vec2(0.0) - p_screen_pos.xy) / screen_ray_dir.xy;
	vec2 t1 = (vec2(1.0) - p_screen_pos.xy) / screen_ray_dir.xy;
	vec2 t2 = max(t0, t1);
	float t_screen = min(t2.x, t2.y);
	float t_max = min(t_screen, t_end);
	float t_free = t_max;

	vec2 cell_step = vec2(screen_ray_dir.x < 0.0 ? -1.0 : 1.0, screen_ray_dir.y < 0.0 ? -1.0 : 1.0);

	int cur_level = 0;
	int cur_iteration = params.num_steps;

	float t;
	{
		vec2 cell_index = floor(p_screen_pos.xy * params.screen_size);
		vec2 new_cell_index = cell_index + clamp(cell_step, vec2(0.0), vec2(1.0));
		vec2 new_cell_pos = (new_cell_index / params.screen_size) + cell_step * 0.000001;
		vec2 pos_t = (new_cell_pos - p_screen_pos.xy) / screen_ray_dir.xy;
		t = min(pos_t.x, pos_t.y);
	}

	while (cur_level >= 0 && cur_iteration > 0 && t < t_max) {
		vec3 cur_screen_pos = p_screen_pos + screen_ray_dir * t;

		vec2 cell_count = compute_cell_count(cur_level);
		vec2 cell_index = floor(cur_screen_pos.xy * cell_count);
		float cell_depth = texelFetch(source_hiz, ivec2(cell_index), cur_level).x;
		float depth_t = (cell_depth - p_screen_pos.z) * screen_ray_dir.z;

		vec2 new_cell_index = cell_index + clamp(cell_step, vec2(0.0), vec2(1.0));
		vec2 new_cell_pos = (new_cell_index / cell_count) + cell_step * 0.000001;
		vec2 pos_t = (new_cell_pos - p_screen_pos.xy) / screen_ray_dir.xy;
		float edge_t = min(pos_t.x, pos_t.y);

		bool hit = facing_camera ? (t <= depth_t) : (depth_t <= edge_t);
		int mip_offset = hit ? -1 : +1;

		if (cur_level == 0) {
			float z0 = linearize_depth(cell_depth);
			float z1 = linearize_depth(cur_screen_pos.z);
			if ((z0 - z1) > params.thickness) {
				hit = false;
				mip_offset = 0;
				t_free = min(t_free, t);
			}
		}

		if (hit) {
			if (!facing_camera) {
				t = max(t, depth_t);
			}
		} else {
			t = edge_t;
		}

		cur_level = min(cur_level + mip_offset, params.mipmaps - 1);
		--cur_iteration;
	}

	r_hit_screen_pos = p_screen_pos + screen_ray_dir * t;
	r_free_screen_pos = p_screen_pos + screen_ray_dir * min(t_free, t);
	if (t < t_max && cur_level < 0) {
		return TRACE_HIT;
	}
	if (t < t_max) {
		return TRACE_STEPS;
	}
	return t_screen <= t_end ? TRACE_OFF_SCREEN : TRACE_OPEN;
}

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);

	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	vec3 screen_pos;
	screen_pos.xy = vec2(pixel_pos + 0.5) / params.screen_size;
	screen_pos.z = texelFetch(source_hiz, pixel_pos, 0).x;

	if (screen_pos.z == 0.0) {
		for (int r = 0; r < params.rays; r++) {
			imageStore(ray_radiance, ivec2(pixel_pos.x + r * params.screen_size.x, pixel_pos.y), vec4(0.0));
			imageStore(ray_hit, ivec2(pixel_pos.x + r * params.screen_size.x, pixel_pos.y), vec4(0.0));
		}
		return;
	}

	vec3 origin = compute_view_pos(screen_pos);
	vec3 normal = normalize(texelFetch(source_normal_roughness, pixel_pos, 0).xyz * 2.0 - 1.0);
	vec3 geom_normal = normalize(compute_geometric_normal(pixel_pos, screen_pos.z, origin, 0.5));
	float view_depth = params.orthogonal ? 1.0 : max(-origin.z, 1e-3);
	vec3 pos = origin + geom_normal * max(view_depth * 0.002, 0.005);
	vec3 ray_screen_pos = compute_screen_pos(pos);
	vec2 source_size = vec2(textureSize(source_color, 0));

	vec3 tangent = normalize(cross(abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), normal));
	vec3 bitangent = cross(normal, tangent);

	for (int r = 0; r < params.rays; r++) {
		ivec2 slot = ivec2(pixel_pos.x + r * params.screen_size.x, pixel_pos.y);
		vec2 u = sample_sequence(pixel_pos, params.frame * uint(params.rays) + uint(r));
		float sin_theta = sqrt(u.x);
		float cos_theta = sqrt(max(1.0 - u.x, 0.0));
		float phi = 2.0 * M_PI * u.y;
		vec3 ray_dir = normalize(tangent * (sin_theta * cos(phi)) + bitangent * (sin_theta * sin(phi)) + normal * cos_theta);
		float cos_source = max(dot(ray_dir, normal), 1e-3);

		if (dot(ray_dir, geom_normal) <= 0.0) {
			imageStore(ray_radiance, slot, vec4(0.0, 0.0, 0.0, STATE_REJECTED));
			imageStore(ray_hit, slot, vec4(ray_dir * MISS_DISTANCE, cos_source));
			continue;
		}

		vec3 hit_screen_pos;
		vec3 free_screen_pos;
		int trace = trace_ray(pos, ray_screen_pos, ray_dir, hit_screen_pos, free_screen_pos);

		vec4 radiance = vec4(0.0, 0.0, 0.0, trace == TRACE_OFF_SCREEN ? STATE_OFF_SCREEN : (trace == TRACE_STEPS ? STATE_STEPS : STATE_OPEN));
		vec3 hit_vector = ray_dir * MISS_DISTANCE;

		if (trace == TRACE_HIT) {
			ivec2 hit_pixel = clamp(ivec2(hit_screen_pos.xy * params.screen_size), ivec2(0), params.screen_size - 1);
			float hit_depth = texelFetch(source_hiz, hit_pixel, 0).x;
			if (hit_depth != 0.0) {
				vec3 cur_pos = compute_view_pos(hit_screen_pos);
				vec3 hit_pos = compute_view_pos(vec3(hit_screen_pos.xy, hit_depth));
				vec3 hit_normal = normalize(texelFetch(source_normal_roughness, hit_pixel, 0).xyz * 2.0 - 1.0);

				float confidence = all(lessThan(abs(hit_screen_pos.xy - screen_pos.xy) * params.screen_size, vec2(1.0))) ? 0.0 : 1.0;
				confidence *= smoothstep(0.0, 0.1, dot(hit_normal, -ray_dir));
				float thickness = 1.0 - smoothstep(0.0, params.thickness, length(cur_pos - hit_pos));
				confidence *= thickness * thickness;
				vec2 border_size = 0.05 * vec2(float(params.screen_size.y) / float(params.screen_size.x), 1.0);
				vec2 border = smoothstep(vec2(0.0), border_size, hit_screen_pos.xy) * (1.0 - smoothstep(vec2(1.0) - border_size, vec2(1.0), hit_screen_pos.xy));
				confidence *= border.x * border.y;
				confidence *= 1.0 - smoothstep(0.8 * params.max_distance, params.max_distance, length(hit_pos - origin));

				hit_vector = hit_pos - origin;
				if (confidence <= 0.0) {
					radiance = vec4(0.0, 0.0, 0.0, STATE_REJECTED);
				} else {
					vec4 hit_reprojected = scene_data.reprojection[params.view_index] * vec4(hit_screen_pos.xy * 2.0 - 1.0, hit_depth, 1.0);
					vec2 previous_uv = hit_reprojected.xy / hit_reprojected.w * 0.5 + 0.5;
					bool valid = hit_reprojected.w > 0.0 && all(greaterThanEqual(previous_uv, vec2(0.0))) && all(lessThanEqual(previous_uv, vec2(1.0)));
					if (valid && params.validate_source) {
						float expected_depth = params.orthogonal ? -(scene_data.cur_to_prev * vec4(hit_pos, 1.0)).z : -hit_pos.z;
						valid = source_sample_valid(source_color, hit_reprojected, expected_depth, source_size);
					}
					radiance = valid ? vec4(textureLod(source_color, previous_uv, params.source_mip).rgb, confidence) : vec4(0.0, 0.0, 0.0, STATE_DISOCCLUDED);
				}
			} else {
				radiance = vec4(0.0, 0.0, 0.0, STATE_OPEN);
			}
		}

#ifdef USE_DDGI
		if (radiance.a == STATE_OFF_SCREEN || radiance.a == STATE_OPEN || radiance.a == STATE_STEPS) {
			float free_distance = max(dot(compute_view_pos(free_screen_pos) - pos, ray_dir), 0.0);
			vec3 world_position = view_to_world(pos + ray_dir * free_distance);
			vec4 probe_light = ddgi_sample_radiance(world_position, view_to_world(normal), -normalize(world_position), view_to_world(ray_dir), DDGI_NO_VIEW_LIMIT);
			if (probe_light.a > 0.0) {
				radiance = vec4(max(probe_light.rgb, vec3(0.0)) * ddgi.energy, probe_light.a);
			}
		}
#endif

		imageStore(ray_radiance, slot, radiance);
		imageStore(ray_hit, slot, vec4(hit_vector, cos_source));
	}
}

#elif defined(MODE_RESOLVE)

const vec2 RING[8] = vec2[](
		vec2(0.3535534, 0.0),
		vec2(-0.4330127, 0.4330127),
		vec2(0.0, -0.7071068),
		vec2(0.5590170, 0.5590170),
		vec2(-0.8660254, 0.0),
		vec2(0.4677072, -0.7180700),
		vec2(-0.2213320, 0.9184020),
		vec2(-0.6614378, -0.6614378));

vec3 state_color(float p_state) {
	if (p_state > 0.0) {
		return vec3(0.0, p_state, 0.0);
	}
	if (p_state == STATE_DISOCCLUDED) {
		return vec3(1.0, 0.0, 1.0);
	}
	if (p_state == STATE_OFF_SCREEN) {
		return vec3(0.0, 0.0, 1.0);
	}
	if (p_state == STATE_OPEN) {
		return vec3(0.5);
	}
	if (p_state == STATE_STEPS) {
		return vec3(1.0, 1.0, 0.0);
	}
	return vec3(1.0, 0.0, 0.0);
}

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);

	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	float depth = texelFetch(source_depth, pixel_pos, 0).x;

	vec4 current = vec4(0.0);
	float reactive = 0.0;
	vec2 noise = vec2(0.0);

	vec2 trace_pos = (vec2(pixel_pos) + 0.5) * vec2(params.trace_size) / vec2(params.screen_size) - 0.5;
	ivec2 base = ivec2(floor(trace_pos));

	if (depth != 0.0) {
		vec2 uv = (vec2(pixel_pos) + 0.5) / vec2(params.screen_size);
		vec3 pos = compute_view_pos(vec3(uv, depth));
		vec3 normal = normalize(texelFetch(source_normal_roughness, pixel_pos, 0).xyz * 2.0 - 1.0);
		float view_depth = params.orthogonal ? 1.0 : max(-pos.z, 1e-3);
		float exposure_scale = params.exposure / max(texelFetch(source_exposure, ivec2(0, 0), 0).r, 1e-6);

		vec2 bilinear = trace_pos - vec2(base);
		float radius = 2.5;
		float angle = 2.0 * M_PI * interleaved_gradient_noise(vec2(pixel_pos) + 5.588238 * float(params.frame & 63u));
		mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));

		float weight_sum = 0.0;
		float confidence_sum = 0.0;
		float rejected_sum = 0.0;
		float hit_weight_sum = 0.0;
		vec3 radiance_sum = vec3(0.0);
		float weight_square_sum = 0.0;
		vec2 sample_sum = vec2(0.0);
		vec2 sample_square_sum = vec2(0.0);

		for (int s = 0; s < 12; s++) {
			ivec2 trace_pixel;
			float kernel;
			if (s < 4) {
				ivec2 offset = ivec2(s & 1, s >> 1);
				trace_pixel = base + offset;
				kernel = 0.25 + (offset.x == 1 ? bilinear.x : 1.0 - bilinear.x) * (offset.y == 1 ? bilinear.y : 1.0 - bilinear.y);
			} else {
				trace_pixel = ivec2(floor(trace_pos + 0.5 + rotation * RING[s - 4] * radius));
				kernel = 0.5;
			}
			if (any(lessThan(trace_pixel, ivec2(0))) || any(greaterThanEqual(trace_pixel, params.trace_size))) {
				continue;
			}

			float trace_depth = texelFetch(source_hiz, trace_pixel, 0).x;
			if (trace_depth == 0.0) {
				continue;
			}
			vec3 trace_origin = compute_view_pos(vec3((vec2(trace_pixel) + 0.5) / vec2(params.trace_size), trace_depth));
			vec3 trace_normal = normalize(texelFetch(source_normal_roughness_trace, trace_pixel, 0).xyz * 2.0 - 1.0);
			float plane_distance = abs(dot(normal, trace_origin - pos)) / view_depth;
			float geometry = (1.0 - smoothstep(0.005, 0.03, plane_distance)) * pow(clamp(dot(normal, trace_normal), 0.0, 1.0), 8.0);
			if (geometry <= 0.0) {
				continue;
			}

			for (int r = 0; r < params.rays; r++) {
				vec4 hit = texelFetch(ray_hit, ivec2(trace_pixel.x + r * params.trace_size.x, trace_pixel.y), 0);
				if (hit.w <= 0.0) {
					continue;
				}
				vec4 radiance = texelFetch(ray_radiance, ivec2(trace_pixel.x + r * params.trace_size.x, trace_pixel.y), 0);
				radiance.rgb = min(radiance.rgb, vec3(65504.0));
				vec3 hit_point = trace_origin + hit.xyz;
				vec3 light = normalize(hit_point - pos);
				float n_dot_l = dot(normal, light);
				if (n_dot_l <= 0.0) {
					continue;
				}

				float weight = kernel * geometry * min(n_dot_l / hit.w, MAX_SAMPLE_WEIGHT);
				float confidence = max(radiance.a, 0.0);
				weight_sum += weight;
				confidence_sum += weight * confidence;
				rejected_sum += radiance.a == STATE_DISOCCLUDED ? weight : 0.0;
				if (confidence > 0.0) {
					radiance.rgb *= min(1.0, params.firefly_luminance / max(abs(luminance(radiance.rgb)) * exposure_scale, 1e-6));
					hit_weight_sum += weight * confidence;
					radiance_sum += weight * confidence * radiance.rgb;
				}
				vec2 sample_value = vec2(confidence > 0.0 ? confidence * luminance(radiance.rgb) : 0.0, confidence);
				weight_square_sum += weight * weight;
				sample_sum += weight * sample_value;
				sample_square_sum += weight * sample_value * sample_value;
			}
		}

		if (weight_sum > 0.0) {
			float confidence = confidence_sum / weight_sum;
			vec3 radiance = hit_weight_sum > 0.0 ? radiance_sum / hit_weight_sum : vec3(0.0);
			current = vec4(radiance * confidence, confidence);
			reactive = rejected_sum / weight_sum;
			vec2 sample_mean = sample_sum / weight_sum;
			noise = sqrt(max(sample_square_sum / weight_sum - sample_mean * sample_mean, vec2(0.0)) * weight_square_sum) / weight_sum;
		}
	}

	if (params.debug_mode == DEBUG_TERMINATION) {
		ivec2 trace_pixel = clamp(ivec2(floor(trace_pos + 0.5)), ivec2(0), params.trace_size - 1);
		vec4 hit = texelFetch(ray_hit, trace_pixel, 0);
		current = vec4(hit.w > 0.0 ? state_color(texelFetch(ray_radiance, trace_pixel, 0).a) : vec3(0.0), 1.0);
	} else if (params.debug_mode == DEBUG_CONFIDENCE) {
		current = vec4(vec3(current.a), 1.0);
	}

	imageStore(output_resolved, pixel_pos, current);
	imageStore(output_info, pixel_pos, vec4(reactive > REACTIVE_FRACTION ? -1.0 : 0.0, noise, 0.0));
}

#else

bool find_history(vec2 p_uv, float p_depth, vec3 p_normal, vec3 p_pos, out vec2 r_history_uv) {
	vec4 surface_reprojected = scene_data.reprojection[params.view_index] * vec4(p_uv * 2.0 - 1.0, p_depth, 1.0);
	r_history_uv = surface_reprojected.xy / surface_reprojected.w * 0.5 + 0.5;

	bool history_found = surface_reprojected.w > 0.0 && all(greaterThanEqual(r_history_uv, vec2(0.0))) && all(lessThanEqual(r_history_uv, vec2(1.0)));
	if (history_found && !params.orthogonal) {
		vec2 previous_size = vec2(textureSize(source_previous, 0));
		vec2 gather_uv = (floor(r_history_uv * previous_size - 0.5) + 1.0) / previous_size;
		vec4 previous_depths = textureGather(source_previous, gather_uv, 3);
		vec3 previous_normal = normalize(mat3(scene_data.cur_to_prev) * p_normal);
		vec3 previous_pos = (scene_data.cur_to_prev * vec4(p_pos, 1.0)).xyz;
		history_found = false;
		for (int i = 0; i < 4; i++) {
			vec2 texel_uv = gather_uv + (vec2(i == 0 || i == 3 ? -0.5 : 0.5, i < 2 ? 0.5 : -0.5)) / previous_size;
			vec4 ray = scene_data.prev_inv_projection[params.view_index] * vec4(texel_uv * 2.0 - 1.0, 0.5, 1.0);
			vec3 ray_dir = ray.xyz / ray.w;
			vec3 previous_point = ray_dir * (previous_depths[i] / max(-ray_dir.z, 1e-6));
			if (previous_depths[i] > 0.0 && abs(dot(previous_normal, previous_point - previous_pos)) <= 0.02 * previous_depths[i] + 0.01) {
				history_found = true;
			}
		}
	}
	return history_found;
}

float read_history_length(vec2 p_history_uv) {
	return floor(texelFetch(history_length, min(ivec2(p_history_uv * vec2(params.screen_size)), params.screen_size - 1), 0).r * 255.0 + 0.5);
}

#ifdef MODE_FAST

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	vec4 current = texelFetch(resolved, pixel_pos, 0);
	float depth = texelFetch(source_depth, pixel_pos, 0).x;
	if (!params.history_valid || depth == 0.0 || texelFetch(resolved_info, pixel_pos, 0).x < 0.0) {
		imageStore(output_color, pixel_pos, current);
		return;
	}

	vec3 normal = normalize(texelFetch(source_normal_roughness, pixel_pos, 0).xyz * 2.0 - 1.0);
	vec2 uv = (vec2(pixel_pos) + 0.5) / vec2(params.screen_size);
	vec2 history_uv;
	vec4 result = current;
	if (find_history(uv, depth, normal, compute_view_pos(vec3(uv, depth)), history_uv)) {
		float blend = max(1.0 / (read_history_length(history_uv) + 1.0), FAST_BLEND);
		result = mix(textureLod(history, history_uv, 0.0), current, blend);
	}
	imageStore(output_color, pixel_pos, result);
}

#else

vec4 clip_to_box(vec4 p_box_min, vec4 p_box_max, vec4 p_value) {
	vec4 center = 0.5 * (p_box_max + p_box_min);
	vec4 extent = 0.5 * (p_box_max - p_box_min) + 0.001;
	vec4 offset = p_value - center;
	vec4 units = abs(offset / extent);
	float largest = max(max(units.x, units.y), max(units.z, units.w));
	return largest > 1.0 ? center + offset / largest : p_value;
}

float neighborhood_weight(int p_offset) {
	return exp(-NEIGHBORHOOD_GAUSSIAN_K * float(p_offset * p_offset) / float((NEIGHBORHOOD_RADIUS + 1) * (NEIGHBORHOOD_RADIUS + 1)));
}

shared vec4 neighborhood[16][16];
shared vec2 neighborhood_noise[16][16];
shared vec3 neighborhood_normal[16][16];

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);
	ivec2 local_pos = ivec2(gl_LocalInvocationID.xy);

	ivec2 tile_origin = ivec2(gl_WorkGroupID.xy) * 8 - NEIGHBORHOOD_RADIUS;
	for (int i = 0; i < 4; i++) {
		ivec2 tile_pos = local_pos + ivec2(i & 1, i >> 1) * 8;
		ivec2 tile_pixel = clamp(tile_origin + tile_pos, ivec2(0), params.screen_size - 1);
		neighborhood[tile_pos.y][tile_pos.x] = texelFetch(fast_history, tile_pixel, 0);
		neighborhood_noise[tile_pos.y][tile_pos.x] = texelFetch(resolved_info, tile_pixel, 0).yz;
		neighborhood_normal[tile_pos.y][tile_pos.x] = normalize(texelFetch(source_normal_roughness, tile_pixel, 0).xyz * 2.0 - 1.0);
	}
	memoryBarrierShared();
	barrier();

	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	vec4 current = texelFetch(resolved, pixel_pos, 0);
	float depth = texelFetch(source_depth, pixel_pos, 0).x;
	if (!params.temporal || !params.history_valid || depth == 0.0) {
		imageStore(output_color, pixel_pos, current);
		imageStore(output_length, pixel_pos, vec4(depth == 0.0 ? 0.0 : 1.0 / 255.0));
		return;
	}

	vec3 normal = normalize(texelFetch(source_normal_roughness, pixel_pos, 0).xyz * 2.0 - 1.0);
	vec2 uv = (vec2(pixel_pos) + 0.5) / vec2(params.screen_size);
	bool reactive = texelFetch(resolved_info, pixel_pos, 0).x < 0.0;

	vec3 centre_normal = neighborhood_normal[local_pos.y + NEIGHBORHOOD_RADIUS][local_pos.x + NEIGHBORHOOD_RADIUS];
	vec4 moment1 = vec4(0.0);
	vec4 moment2 = vec4(0.0);
	vec2 noise = vec2(0.0);
	float moment_weight = 0.0;
	for (int y = -NEIGHBORHOOD_RADIUS; y <= NEIGHBORHOOD_RADIUS; y++) {
		for (int x = -NEIGHBORHOOD_RADIUS; x <= NEIGHBORHOOD_RADIUS; x++) {
			ivec2 tile_pos = local_pos + ivec2(x, y) + NEIGHBORHOOD_RADIUS;
			float same_surface = clamp(dot(centre_normal, neighborhood_normal[tile_pos.y][tile_pos.x]), 0.0, 1.0);
			same_surface *= same_surface;
			same_surface *= same_surface;
			same_surface *= same_surface;
			float weight = neighborhood_weight(x) * neighborhood_weight(y) * same_surface;
			vec4 neighbor = neighborhood[tile_pos.y][tile_pos.x];
			moment1 += neighbor * weight;
			moment2 += neighbor * neighbor * weight;
			noise += neighborhood_noise[tile_pos.y][tile_pos.x] * weight;
			moment_weight += weight;
		}
	}
	moment1 /= moment_weight;
	moment2 /= moment_weight;
	noise /= moment_weight;

	vec2 history_uv;
	vec4 result = current;
	float length_out = 1.0;
	if (find_history(uv, depth, normal, compute_view_pos(vec3(uv, depth)), history_uv)) {
		vec4 previous = textureLod(history, history_uv, 0.0);
		float previous_length = read_history_length(history_uv);
		vec4 extent = max(2.0 * sqrt(max(moment2 - moment1 * moment1, vec4(0.0))), NOISE_SIGMAS * FAST_NOISE_SCALE * noise.xxxy);
		extent = max(extent, vec4(0.5 * abs(moment1.rgb), 0.0));
		vec4 clipped = clip_to_box(vec4(moment1.rgb - extent.rgb, max(moment1.a - extent.a, 0.0)), min(moment1 + extent, vec4(vec3(65504.0), 1.0)), previous);
		float previous_luma = luminance(previous.rgb);
		float clipped_luma = luminance(clipped.rgb);
		previous_length *= max(1.0 - abs(clipped_luma - previous_luma) / max(max(abs(clipped_luma), abs(previous_luma)), 1e-6), 0.0);
		float blend = max(1.0 / (previous_length + 1.0), 1.0 / float(max(params.history_frames, 1)));
		blend = reactive ? 1.0 : blend;
		result = mix(clipped, current, blend);
		length_out = reactive ? 1.0 : min(previous_length + 1.0, 255.0);
	}

	imageStore(output_color, pixel_pos, result);
	imageStore(output_length, pixel_pos, vec4(length_out / 255.0));
}

#endif

#endif
