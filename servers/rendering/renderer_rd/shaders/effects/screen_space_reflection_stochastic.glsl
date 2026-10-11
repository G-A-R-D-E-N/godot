#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#define M_PI 3.14159265359
#define MIRROR_ROUGHNESS 0.1
#define REDUCED_RATE_ROUGHNESS 0.35
#define MISS_DISTANCE 1000.0
#define REJECTED_CONFIDENCE -1.0
#define MAX_SAMPLE_WEIGHT 4.0
#define MAX_SAMPLE_LUMINANCE 32.0
#define REACTIVE_FRACTION 0.25
#define NEIGHBORHOOD_RADIUS 4
#define NEIGHBORHOOD_GAUSSIAN_K 3.0
#define NOISE_SIGMAS 4.0

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
	float distance_fade;
	float curve_fade_in;
	float depth_tolerance;
	bool orthogonal;
	int view_index;
	uint frame;
	int rays;
	float roughness_cutoff;
	bool validate_source;
	float source_mip;
	int pad;
	int pad2;
}
params;

#include "screen_space_source_validation_inc.glsl"

#else

layout(push_constant, std430) uniform Params {
	ivec2 screen_size;
	ivec2 trace_size;
	int rays;
	uint frame;
	float roughness_cutoff;
	bool temporal;
	bool history_valid;
	bool orthogonal;
	int view_index;
	float exposure;
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
#ifdef WRITE_HIT_DISTANCE
layout(r16f, set = 0, binding = 10) uniform restrict writeonly image2D output_hit_distance;
#endif

#else

layout(set = 0, binding = 1) uniform sampler2D resolved;
layout(set = 0, binding = 2) uniform sampler2D resolved_info;
layout(set = 0, binding = 3) uniform sampler2D source_depth;
layout(set = 0, binding = 4) uniform sampler2D source_normal_roughness;
layout(set = 0, binding = 5) uniform sampler2D history;
layout(set = 0, binding = 6) uniform sampler2D history_length;
layout(set = 0, binding = 7) uniform sampler2D source_previous;
layout(rgba16f, set = 0, binding = 8) uniform restrict writeonly image2D output_color;
layout(r8, set = 0, binding = 9) uniform restrict writeonly image2D output_length;

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

float decode_roughness(float p_encoded) {
	float roughness = p_encoded;
	if (roughness > 0.5) {
		roughness = 1.0 - roughness;
	}
	return roughness / (127.0 / 255.0);
}

float interleaved_gradient_noise(vec2 p_pos) {
	return fract(52.9829189 * fract(dot(p_pos, vec2(0.06711056, 0.00583715))));
}

float ggx_d(float p_n_dot_h, float p_alpha) {
	float a2 = p_alpha * p_alpha;
	float d = p_n_dot_h * p_n_dot_h * (a2 - 1.0) + 1.0;
	return a2 / (M_PI * d * d);
}

float smith_lambda(float p_n_dot_x, float p_alpha) {
	float x2 = max(p_n_dot_x * p_n_dot_x, 1e-6);
	return 0.5 * (sqrt(1.0 + p_alpha * p_alpha * (1.0 - x2) / x2) - 1.0);
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
	vec4 H = vec4(
			texelFetch(source_hiz, pixel_pos + ivec2(-1, 0), 0).x,
			texelFetch(source_hiz, pixel_pos + ivec2(-2, 0), 0).x,
			texelFetch(source_hiz, pixel_pos + ivec2(1, 0), 0).x,
			texelFetch(source_hiz, pixel_pos + ivec2(2, 0), 0).x);

	vec4 V = vec4(
			texelFetch(source_hiz, pixel_pos + ivec2(0, -1), 0).x,
			texelFetch(source_hiz, pixel_pos + ivec2(0, -2), 0).x,
			texelFetch(source_hiz, pixel_pos + ivec2(0, 1), 0).x,
			texelFetch(source_hiz, pixel_pos + ivec2(0, 2), 0).x);

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

vec3 sample_ggx_vndf(vec3 p_view, float p_alpha, vec2 p_u) {
	vec3 vh = normalize(vec3(p_alpha * p_view.x, p_alpha * p_view.y, p_view.z));
	float lensq = vh.x * vh.x + vh.y * vh.y;
	vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
	vec3 t2 = cross(vh, t1);
	float r = sqrt(p_u.x);
	float phi = 2.0 * M_PI * p_u.y;
	float s1 = r * cos(phi);
	float s2 = r * sin(phi);
	float s = 0.5 * (1.0 + vh.z);
	s2 = (1.0 - s) * sqrt(1.0 - s1 * s1) + s * s2;
	vec3 nh = s1 * t1 + s2 * t2 + sqrt(max(0.0, 1.0 - s1 * s1 - s2 * s2)) * vh;
	return normalize(vec3(p_alpha * nh.x, p_alpha * nh.y, max(0.0, nh.z)));
}

vec2 sample_sequence(ivec2 p_pixel, uint p_index) {
	vec2 offset = vec2(interleaved_gradient_noise(vec2(p_pixel)), interleaved_gradient_noise(vec2(p_pixel) + vec2(47.0, 17.0)));
	return fract(offset + float(p_index + 1u) * vec2(0.7548776662466927, 0.5698402909980532));
}

bool trace_ray(vec3 p_pos, vec3 p_screen_pos, vec3 p_ray_dir, out vec3 r_hit_screen_pos, out float r_screen_length) {
	vec3 end_pos = p_pos + p_ray_dir;
	if (end_pos.z > 0.0) {
		end_pos -= p_ray_dir / p_ray_dir.z * (end_pos.z + 0.00001);
	}

	vec3 screen_end_pos = compute_screen_pos(end_pos);
	vec3 screen_ray_dir = screen_end_pos - p_screen_pos;
	screen_ray_dir /= abs(screen_ray_dir.z);

	bool facing_camera = screen_ray_dir.z >= 0.0;

	vec2 t0 = (vec2(0.0) - p_screen_pos.xy) / screen_ray_dir.xy;
	vec2 t1 = (vec2(1.0) - p_screen_pos.xy) / screen_ray_dir.xy;
	vec2 t2 = max(t0, t1);
	float t_max = min(t2.x, t2.y);

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
			if ((z0 - z1) > params.depth_tolerance) {
				hit = false;
				mip_offset = 0;
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
	r_screen_length = length(screen_ray_dir.xy * t);
	return t < t_max && cur_level < 0;
}

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);

	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	vec3 screen_pos;
	screen_pos.xy = vec2(pixel_pos + 0.5) / params.screen_size;
	screen_pos.z = texelFetch(source_hiz, pixel_pos, 0).x;

	float roughness = 1.0;
	vec3 origin = vec3(0.0);
	vec3 normal = vec3(0.0, 0.0, 1.0);
	if (screen_pos.z != 0.0) {
		origin = compute_view_pos(screen_pos);
		vec4 normal_roughness = texelFetch(source_normal_roughness, pixel_pos, 0);
		normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
		roughness = decode_roughness(normal_roughness.w);
	}

	bool mirror = roughness < MIRROR_ROUGHNESS;
	bool traced = screen_pos.z != 0.0 && roughness < params.roughness_cutoff;
	if (traced && !mirror && roughness >= REDUCED_RATE_ROUGHNESS) {
		traced = ((pixel_pos.x + pixel_pos.y + int(params.frame)) & 1) == 0;
	}

	if (!traced) {
		for (int r = 0; r < params.rays; r++) {
			imageStore(ray_radiance, ivec2(pixel_pos.x + r * params.screen_size.x, pixel_pos.y), vec4(0.0));
			imageStore(ray_hit, ivec2(pixel_pos.x + r * params.screen_size.x, pixel_pos.y), vec4(0.0));
		}
		return;
	}

	vec3 geom_normal = normalize(compute_geometric_normal(pixel_pos, screen_pos.z, origin, 0.5));
	vec3 pos = origin + geom_normal * (1.0 - pow(clamp(dot(normal, geom_normal), 0.0, 1.0), 8.0));
	vec3 ray_screen_pos = compute_screen_pos(pos);
	vec3 view_dir = params.orthogonal ? vec3(0.0, 0.0, -1.0) : normalize(pos - scene_data.eye_offset[params.view_index].xyz);
	float alpha = max(roughness * roughness, 1e-4);
	float n_dot_v = max(dot(normal, -view_dir), 1e-4);

	vec3 tangent = normalize(cross(abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), normal));
	vec3 bitangent = cross(normal, tangent);
	vec3 local_view = vec3(dot(-view_dir, tangent), dot(-view_dir, bitangent), n_dot_v);

	for (int r = 0; r < params.rays; r++) {
		vec3 ray_dir;
		float inv_pdf;
		if (mirror) {
			ray_dir = normalize(reflect(view_dir, normal));
			inv_pdf = -1.0;
		} else {
			vec3 local_half = sample_ggx_vndf(local_view, alpha, sample_sequence(pixel_pos, params.frame * uint(params.rays) + uint(r)));
			vec3 half_vector = normalize(tangent * local_half.x + bitangent * local_half.y + normal * local_half.z);
			ray_dir = normalize(reflect(view_dir, half_vector));
			float pdf = ggx_d(max(local_half.z, 0.0), alpha) / ((1.0 + smith_lambda(n_dot_v, alpha)) * 4.0 * n_dot_v);
			inv_pdf = 1.0 / max(pdf, 1e-4);
		}

		if (dot(ray_dir, normal) <= 0.0 || dot(ray_dir, geom_normal) <= 0.0) {
			imageStore(ray_radiance, ivec2(pixel_pos.x + r * params.screen_size.x, pixel_pos.y), vec4(0.0));
			imageStore(ray_hit, ivec2(pixel_pos.x + r * params.screen_size.x, pixel_pos.y), vec4(0.0));
			continue;
		}

		vec3 cur_screen_pos;
		float screen_length;
		bool found = trace_ray(pos, ray_screen_pos, ray_dir, cur_screen_pos, screen_length);

		ivec2 hit_pixel = ivec2(cur_screen_pos.xy * params.screen_size);
		float hit_depth = texelFetch(source_hiz, hit_pixel, 0).x;
		float confidence = (found && hit_depth != 0.0 && all(greaterThanEqual(cur_screen_pos.xy, vec2(0.0))) && all(lessThanEqual(cur_screen_pos.xy, vec2(1.0)))) ? 1.0 : 0.0;

		if (all(lessThan(abs(cur_screen_pos.xy - screen_pos.xy) * params.screen_size, vec2(2.0)))) {
			confidence = 0.0;
		}

		vec3 hit_normal = normalize(texelFetch(source_normal_roughness, hit_pixel, 0).xyz * 2.0 - 1.0);
		confidence *= smoothstep(0.0, 0.15, dot(hit_normal, -ray_dir));

		vec3 cur_pos = compute_view_pos(cur_screen_pos);
		vec3 hit_pos = compute_view_pos(vec3(cur_screen_pos.xy, hit_depth));
		float thickness = 1.0 - smoothstep(0.0, params.depth_tolerance, length(cur_pos - hit_pos));
		confidence *= thickness * thickness;

		vec2 border_size = 0.05 * vec2(float(params.screen_size.y) / float(params.screen_size.x), 1.0);
		vec2 border = smoothstep(vec2(0.0), border_size, cur_screen_pos.xy) * (1.0 - smoothstep(vec2(1.0) - border_size, vec2(1.0), cur_screen_pos.xy));
		confidence *= border.x * border.y;

		float fade_in = params.curve_fade_in == 0.0 ? 1.0 : pow(clamp(screen_length, 0.0, 1.0), params.curve_fade_in);
		float fade_out = params.distance_fade == 0.0 ? 1.0 : pow(clamp(1.0 - screen_length, 0.0, 1.0), params.distance_fade);
		confidence *= fade_in * fade_out;

		vec4 radiance = vec4(0.0);
		vec3 hit_vector = ray_dir * MISS_DISTANCE;
		if (confidence > 0.0) {
			hit_vector = hit_pos - origin;
			vec4 hit_reprojected = scene_data.reprojection[params.view_index] * vec4(cur_screen_pos.xy * 2.0 - 1.0, hit_depth, 1.0);
			if (params.validate_source && !params.orthogonal && !source_sample_valid(source_color, hit_reprojected, -hit_pos.z, vec2(params.screen_size))) {
				radiance = vec4(0.0, 0.0, 0.0, REJECTED_CONFIDENCE);
			} else {
				radiance = vec4(textureLod(source_color, hit_reprojected.xy / hit_reprojected.w * 0.5 + 0.5, params.source_mip).rgb, confidence);
			}
		}

		imageStore(ray_radiance, ivec2(pixel_pos.x + r * params.screen_size.x, pixel_pos.y), radiance);
		imageStore(ray_hit, ivec2(pixel_pos.x + r * params.screen_size.x, pixel_pos.y), vec4(hit_vector, inv_pdf));
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

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);

	if (any(greaterThanEqual(pixel_pos, params.screen_size))) {
		return;
	}

	float depth = texelFetch(source_depth, pixel_pos, 0).x;
	vec4 normal_roughness = texelFetch(source_normal_roughness, pixel_pos, 0);
	float roughness = decode_roughness(normal_roughness.w);
	float fade = 1.0 - smoothstep(params.roughness_cutoff - 0.1, params.roughness_cutoff, roughness);

	vec4 current = vec4(0.0);
	float hit_distance = 0.0;
	float reactive = 0.0;
	vec2 noise = vec2(0.0);

	if (depth != 0.0 && fade > 0.0) {
		vec2 uv = (vec2(pixel_pos) + 0.5) / vec2(params.screen_size);
		vec3 pos = compute_view_pos(vec3(uv, depth));
		vec3 normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
		vec3 view = params.orthogonal ? vec3(0.0, 0.0, 1.0) : normalize(scene_data.eye_offset[params.view_index].xyz - pos);
		float n_dot_v = max(dot(normal, view), 1e-4);
		float alpha = max(roughness * roughness, 1e-4);
		float lambda_v = smith_lambda(n_dot_v, alpha);
		bool mirror = roughness < MIRROR_ROUGHNESS;
		float view_depth = max(-pos.z, 1e-3);
		float exposure_scale = params.exposure / max(texelFetch(source_exposure, ivec2(0, 0), 0).r, 1e-6);

		vec2 trace_pos = (vec2(pixel_pos) + 0.5) * vec2(params.trace_size) / vec2(params.screen_size) - 0.5;
		ivec2 base = ivec2(floor(trace_pos));
		vec2 bilinear = trace_pos - vec2(base);
		float radius = 2.0 + 2.0 * clamp((roughness - MIRROR_ROUGHNESS) / 0.5, 0.0, 1.0);
		float angle = 2.0 * M_PI * interleaved_gradient_noise(vec2(pixel_pos) + 5.588238 * float(params.frame & 63u));
		mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));

		float weight_sum = 0.0;
		float confidence_sum = 0.0;
		float rejected_sum = 0.0;
		float hit_weight_sum = 0.0;
		vec3 radiance_sum = vec3(0.0);
		float distance_sum = 0.0;
		float weight_square_sum = 0.0;
		vec2 sample_sum = vec2(0.0);
		vec2 sample_square_sum = vec2(0.0);

		int sample_count = mirror ? 4 : 12;
		for (int s = 0; s < sample_count; s++) {
			ivec2 trace_pixel;
			float kernel;
			if (s < 4) {
				ivec2 offset = ivec2(s & 1, s >> 1);
				trace_pixel = base + offset;
				kernel = (offset.x == 1 ? bilinear.x : 1.0 - bilinear.x) * (offset.y == 1 ? bilinear.y : 1.0 - bilinear.y);
				kernel = mirror ? kernel : 0.25 + kernel;
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
				if (hit.w == 0.0) {
					continue;
				}
				vec4 radiance = texelFetch(ray_radiance, ivec2(trace_pixel.x + r * params.trace_size.x, trace_pixel.y), 0);
				vec3 hit_point = trace_origin + hit.xyz;
				vec3 light = normalize(hit_point - pos);

				float weight = kernel * geometry;
				if (!mirror) {
					float n_dot_l = dot(normal, light);
					if (hit.w < 0.0 || n_dot_l <= 0.0) {
						continue;
					}
					float n_dot_h = clamp(dot(normal, normalize(view + light)), 0.0, 1.0);
					float g2 = 1.0 / (1.0 + lambda_v + smith_lambda(n_dot_l, alpha));
					weight *= min(ggx_d(n_dot_h, alpha) * g2 / (4.0 * n_dot_v) * hit.w, MAX_SAMPLE_WEIGHT);
				}
				if (weight <= 0.0) {
					continue;
				}

				float confidence = max(radiance.a, 0.0);
				weight_sum += weight;
				confidence_sum += weight * confidence;
				rejected_sum += radiance.a < 0.0 ? weight : 0.0;
				if (confidence > 0.0) {
					radiance.rgb *= min(1.0, MAX_SAMPLE_LUMINANCE / max(luminance(radiance.rgb) * exposure_scale, 1e-6));
					hit_weight_sum += weight * confidence;
					radiance_sum += weight * confidence * radiance.rgb;
					distance_sum += weight * confidence * length(hit_point - pos);
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
			current = vec4(radiance * confidence, confidence) * fade;
			hit_distance = confidence_sum > 0.0 ? distance_sum / confidence_sum : 0.0;
			reactive = rejected_sum / weight_sum;
			vec2 sample_mean = sample_sum / weight_sum;
			noise = sqrt(max(sample_square_sum / weight_sum - sample_mean * sample_mean, vec2(0.0)) * weight_square_sum) / weight_sum * fade;
		}
	}

	imageStore(output_resolved, pixel_pos, current);
	imageStore(output_info, pixel_pos, vec4(reactive > REACTIVE_FRACTION ? -1.0 - hit_distance : hit_distance, noise, 0.0));
#ifdef WRITE_HIT_DISTANCE
	imageStore(output_hit_distance, pixel_pos, vec4(hit_distance / (hit_distance + 2.0)));
#endif
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
shared vec4 row_moment1[16][8];
shared vec4 row_moment2[16][8];
shared vec2 row_noise[16][8];

void main() {
	ivec2 pixel_pos = ivec2(gl_GlobalInvocationID.xy);
	ivec2 local_pos = ivec2(gl_LocalInvocationID.xy);

	ivec2 tile_origin = ivec2(gl_WorkGroupID.xy) * 8 - NEIGHBORHOOD_RADIUS;
	for (int i = 0; i < 4; i++) {
		ivec2 tile_pos = local_pos + ivec2(i & 1, i >> 1) * 8;
		ivec2 tile_pixel = clamp(tile_origin + tile_pos, ivec2(0), params.screen_size - 1);
		neighborhood[tile_pos.y][tile_pos.x] = texelFetch(resolved, tile_pixel, 0);
		neighborhood_noise[tile_pos.y][tile_pos.x] = texelFetch(resolved_info, tile_pixel, 0).yz;
	}
	memoryBarrierShared();
	barrier();

	for (int i = 0; i < 2; i++) {
		int row = local_pos.y + i * 8;
		vec4 sum1 = vec4(0.0);
		vec4 sum2 = vec4(0.0);
		vec2 noise_sum = vec2(0.0);
		for (int x = -NEIGHBORHOOD_RADIUS; x <= NEIGHBORHOOD_RADIUS; x++) {
			vec4 neighbor = neighborhood[row][local_pos.x + NEIGHBORHOOD_RADIUS + x];
			sum1 += neighbor * neighborhood_weight(x);
			sum2 += neighbor * neighbor * neighborhood_weight(x);
			noise_sum += neighborhood_noise[row][local_pos.x + NEIGHBORHOOD_RADIUS + x] * neighborhood_weight(x);
		}
		row_moment1[row][local_pos.x] = sum1;
		row_moment2[row][local_pos.x] = sum2;
		row_noise[row][local_pos.x] = noise_sum;
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

	vec4 normal_roughness = texelFetch(source_normal_roughness, pixel_pos, 0);
	float roughness = decode_roughness(normal_roughness.w);
	vec3 normal = normalize(normal_roughness.xyz * 2.0 - 1.0);
	vec2 uv = (vec2(pixel_pos) + 0.5) / vec2(params.screen_size);
	vec3 pos = compute_view_pos(vec3(uv, depth));
	vec3 view = params.orthogonal ? vec3(0.0, 0.0, 1.0) : normalize(scene_data.eye_offset[params.view_index].xyz - pos);
	vec4 info = texelFetch(resolved_info, pixel_pos, 0);
	bool reactive = info.x < 0.0;
	float hit_distance = reactive ? -1.0 - info.x : info.x;

	vec4 moment1 = vec4(0.0);
	vec4 moment2 = vec4(0.0);
	vec2 noise = vec2(0.0);
	float moment_weight = 0.0;
	for (int y = -NEIGHBORHOOD_RADIUS; y <= NEIGHBORHOOD_RADIUS; y++) {
		moment1 += row_moment1[local_pos.y + NEIGHBORHOOD_RADIUS + y][local_pos.x] * neighborhood_weight(y);
		moment2 += row_moment2[local_pos.y + NEIGHBORHOOD_RADIUS + y][local_pos.x] * neighborhood_weight(y);
		noise += row_noise[local_pos.y + NEIGHBORHOOD_RADIUS + y][local_pos.x] * neighborhood_weight(y);
		moment_weight += neighborhood_weight(y);
	}
	moment1 /= moment_weight * moment_weight;
	moment2 /= moment_weight * moment_weight;
	noise /= moment_weight * moment_weight;

	vec4 surface_reprojected = scene_data.reprojection[params.view_index] * vec4(uv * 2.0 - 1.0, depth, 1.0);
	vec2 history_uv = surface_reprojected.xy / surface_reprojected.w * 0.5 + 0.5;
	if (hit_distance > 0.0 && !params.orthogonal) {
		vec3 virtual_pos = pos - view * hit_distance;
		vec4 virtual_reprojected = scene_data.reprojection[params.view_index] * vec4(uv * 2.0 - 1.0, compute_screen_pos(virtual_pos).z, 1.0);
		vec2 virtual_uv = virtual_reprojected.xy / virtual_reprojected.w * 0.5 + 0.5;
		history_uv = mix(virtual_uv, history_uv, smoothstep(0.2, 0.6, roughness));
	}

	bool history_found = all(greaterThanEqual(history_uv, vec2(0.0))) && all(lessThanEqual(history_uv, vec2(1.0)));
	if (history_found) {
		vec2 previous_size = vec2(textureSize(source_previous, 0));
		vec2 gather_uv = (floor(history_uv * previous_size - 0.5) + 1.0) / previous_size;
		vec4 previous_depths = textureGather(source_previous, gather_uv, 3);
		vec3 previous_normal = normalize(mat3(scene_data.cur_to_prev) * normal);
		vec3 previous_pos = (scene_data.cur_to_prev * vec4(pos, 1.0)).xyz;
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

	vec4 result = current;
	float length_out = 1.0;
	if (history_found) {
		vec4 previous = textureLod(history, history_uv, 0.0);
		float previous_length = floor(texelFetch(history_length, min(ivec2(history_uv * vec2(params.screen_size)), params.screen_size - 1), 0).r * 255.0 + 0.5);
		vec4 extent = max((1.0 + roughness) * sqrt(max(moment2 - moment1 * moment1, vec4(0.0))), NOISE_SIGMAS * noise.xxxy);
		previous = clip_to_box(max(moment1 - extent, vec4(0.0)), min(moment1 + extent, vec4(vec3(65504.0), 1.0)), previous);
		float blend = max(1.0 / (previous_length + 1.0), roughness < MIRROR_ROUGHNESS ? 0.2 : 0.04);
		blend = reactive ? 1.0 : blend;
		result = mix(previous, current, blend);
		length_out = reactive ? 1.0 : min(previous_length + 1.0, 255.0);
	}

	imageStore(output_color, pixel_pos, result);
	imageStore(output_length, pixel_pos, vec4(length_out / 255.0));
}

#endif
