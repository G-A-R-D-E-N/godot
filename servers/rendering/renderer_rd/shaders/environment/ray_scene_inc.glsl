#define RAY_SCENE_LEAF_SIZE 4u
#define RAY_SCENE_NO_HIT 0xFFFFFFFFu
#define RAY_SCENE_EXACT_LIGHTS 4u

#define RAY_SCENE_LIGHT_DIRECTIONAL 0u
#define RAY_SCENE_LIGHT_SPOT 2u
#define RAY_SCENE_LIGHT_AREA 3u
#define RAY_SCENE_ALL_CASTERS 0xFFFFFFFFu

#define RAY_SCENE_MATERIAL_TWO_SIDED 1u
#define RAY_SCENE_MATERIAL_SKIP 2u
#define RAY_SCENE_MATERIAL_BLEND 4u
#define RAY_SCENE_MATERIAL_SHADOW_PASS 8u
#define RAY_SCENE_MATERIAL_SHADOW_ONLY 16u
#define RAY_SCENE_BLEND_TIE 1e-4
#define RAY_SCENE_TIE_ULPS 4.0
#define RAY_SCENE_FLOAT_EPSILON 1.1920929e-7
#define RAY_SCENE_BAND_NODE 0x80000000u

#define RAY_SCENE_SKIP_MASK(m_shadow, m_skip) (RAY_SCENE_MATERIAL_SKIP | ((m_shadow) ? RAY_SCENE_MATERIAL_SHADOW_PASS : RAY_SCENE_MATERIAL_SHADOW_ONLY) | (m_skip))
#define RAY_SCENE_CASTER_SKIPS(m_layer_mask, m_caster_mask) ((m_caster_mask) != RAY_SCENE_ALL_CASTERS && ((m_layer_mask) & (m_caster_mask)) == 0u)
#define RAY_SCENE_TIE_SCALE(m_origin) max(max(abs((m_origin).x), abs((m_origin).y)), abs((m_origin).z))
#define RAY_SCENE_TIE_ABS(m_t, m_scale) (RAY_SCENE_TIE_ULPS * RAY_SCENE_FLOAT_EPSILON * max((m_scale), (m_t)))
#define RAY_SCENE_TIE_REACH(m_t, m_scale) ((m_t) * (1.0 + RAY_SCENE_BLEND_TIE) + RAY_SCENE_TIE_ABS(m_t, m_scale))
#define RAY_SCENE_TIE_LIMIT(m_limit, m_hit, m_blend, m_scale) \
	float m_limit = (m_hit).t; \
	if ((m_hit).instance != RAY_SCENE_NO_HIT && (m_blend) != (m_hit).blend) { \
		m_limit = (m_blend) ? RAY_SCENE_TIE_REACH(m_limit, m_scale) : m_limit * (1.0 - RAY_SCENE_BLEND_TIE) - RAY_SCENE_TIE_ABS(m_limit, m_scale); \
	}
#define RAY_SCENE_COVERAGE_SEED(m_seed) ray_scene_hash((m_seed) ^ 0x6a09e667u)
#define RAY_SCENE_COVERAGE_MISSES(m_seed, m_triangle, m_instance, m_coverage) (float(ray_scene_hash(ray_scene_hash((m_seed) + (m_triangle)) + (m_instance)) >> 8u) >= (m_coverage) * 16777216.0)

const vec3 luminance_weights = vec3(0.2126, 0.7152, 0.0722);

struct RaySceneNode {
	vec3 bmin;
	uint left_first;
	vec3 bmax;
	uint count;
};

struct RaySceneTriangle {
	vec3 v0;
	uint surface;
	vec4 e1;
	vec4 e2;
};

struct RaySceneInstance {
	vec4 world_to_object[3];
	uint blas_root;
	uint material_offset;
	uint layer_mask;
	uint pad;
};

struct RaySceneMaterial {
	vec3 albedo;
	float coverage;
	vec3 emission;
	uint flags;
	uint emission_texture;
	uint emission_uv_set;
	vec2 emission_uv_scale;
	vec2 emission_uv_offset;
	vec2 emission_nits_range;
	float emission_texel_scale;
	uint emission_flags;
	float emission_mean_luminance;
	float pad;
};

#define RAY_SCENE_EMISSION_TEXTURE_RANGES 1

struct RaySceneLight {
	vec3 color;
	float energy;
	vec3 direction;
	uint type;
	vec3 position;
	float attenuation;
	float cos_spot_angle;
	float inv_spot_attenuation;
	float inv_radius;
	uint caster_mask;
	vec3 half_a;
	float area;
	vec3 half_b;
	uint pad;
};

// Ray scene space is world minus the anchor; layout must match RayScene::TriangleEmitter: v0 at offset 0, instance at 12, v1 at 16, material at 28, v2 at 32, uv_index at 44.
struct RaySceneTriangleEmitter {
	vec3 v0;
	uint instance;
	vec3 v1;
	uint material;
	vec3 v2;
	uint uv_index;
	uint flags;
	uint pad0;
	uint pad1;
	uint pad2;
};

#define RAY_SCENE_EMITTER_PROXIED 1u

#ifndef RAY_SCENE_HW
layout(set = RAY_SCENE_SET, binding = 0, std430) restrict readonly buffer RaySceneBlasNodes {
	RaySceneNode data[];
}
ray_scene_blas_nodes;
#else
layout(set = RAY_SCENE_SET, binding = 0, std430) restrict readonly buffer RaySceneHwRemap {
	uint data[];
}
ray_scene_hw_remap;
#endif

layout(set = RAY_SCENE_SET, binding = 1, std430) restrict readonly buffer RaySceneTriangles {
	RaySceneTriangle data[];
}
ray_scene_triangles;

layout(set = RAY_SCENE_SET, binding = 2, std430) restrict readonly buffer RaySceneInstances {
	RaySceneInstance data[];
}
ray_scene_instances;

#ifndef RAY_SCENE_HW
layout(set = RAY_SCENE_SET, binding = 3, std430) restrict readonly buffer RaySceneTlasNodes {
	RaySceneNode data[];
}
ray_scene_tlas_nodes;
#endif

layout(set = RAY_SCENE_SET, binding = 4, std430) restrict readonly buffer RaySceneMaterials {
	RaySceneMaterial data[];
}
ray_scene_materials;

layout(set = RAY_SCENE_SET, binding = 5, std430) restrict readonly buffer RaySceneLights {
	RaySceneLight data[];
}
ray_scene_lights;

layout(set = RAY_SCENE_SET, binding = 6, std140) uniform RaySceneData {
	vec3 camera_to_anchor;
	uint instance_count;
	uint light_count;
	uint triangle_emitter_count;
	uint triangle_emitters_capped;
	float emissive_exposure_normalization;
	uint triangle_emitter_generation;
	uint emission_texture_count;
	uint proxy_emitters_traced;
	uint pad2;
}
ray_scene;

#ifdef RAY_SCENE_HW
layout(set = RAY_SCENE_SET, binding = 7) uniform accelerationStructureEXT ray_scene_tlas;
#endif

layout(set = RAY_SCENE_SET, binding = 8, std430) restrict readonly buffer RaySceneTriangleEmitters {
	RaySceneTriangleEmitter data[];
}
ray_scene_triangle_emitters;

layout(set = RAY_SCENE_SET, binding = 15) uniform sampler2DArray ray_scene_emission_textures;

layout(set = RAY_SCENE_SET, binding = 9, std430) restrict readonly buffer RaySceneTriangleUvs {
	uint data[];
}
ray_scene_triangle_uvs;

#ifdef RAY_SCENE_ALPHA
#define RAY_SCENE_ALPHA_SHIFT 8u
#define RAY_SCENE_ALPHA_STATE_READY 2u
#define RAY_SCENE_MATERIAL_SHADOW_TWO_SIDED 32u
#define RAY_SCENE_ALPHA_CULL_MASK 3u
#define RAY_SCENE_ALPHA_CULL_BACK 2u

layout(set = RAY_SCENE_SET, binding = 10) uniform sampler2DArray ray_scene_alpha_mean;

struct RaySceneAlphaEntry {
	uint layer;
	uint state;
	vec2 scale;
	vec2 offset;
	uint bits;
	float far_gain;
	float far_level;
	float far_veil_alpha;
	vec2 far_affine;
	float density;
	uint far_state;
};

layout(set = RAY_SCENE_SET, binding = 13, std430) restrict readonly buffer RaySceneAlphaTable {
	RaySceneAlphaEntry data[];
}
ray_scene_alpha_table;

layout(set = RAY_SCENE_SET, binding = 14) uniform sampler2DArray ray_scene_alpha_layers;
#endif

struct RaySceneHit {
	float t;
	uint instance;
	uint triangle;
	bool backface;
	bool blend;
};

RaySceneTriangleEmitter ray_scene_triangle_emitter(uint p_index) {
	return ray_scene_triangle_emitters.data[p_index];
}

bool ray_scene_triangle_emitter_proxied(uint p_index) {
	return (ray_scene_triangle_emitters.data[p_index].flags & RAY_SCENE_EMITTER_PROXIED) != 0u;
}

vec3 ray_scene_triangle_emitter_emission(uint p_material) {
	return ray_scene_materials.data[p_material].emission;
}

vec2 ray_scene_triangle_emitter_uv(uint p_uv_index, vec3 p_bary) {
	uint base = p_uv_index * 4u;
	float w0 = 1.0 - p_bary.x - p_bary.y;
	return unpackHalf2x16(ray_scene_triangle_uvs.data[base]) * w0 + unpackHalf2x16(ray_scene_triangle_uvs.data[base + 1u]) * p_bary.x + unpackHalf2x16(ray_scene_triangle_uvs.data[base + 2u]) * p_bary.y;
}

vec3 ray_scene_triangle_emitter_emission_texel(uint p_material, vec2 p_uv) {
	RaySceneMaterial material = ray_scene_materials.data[p_material];
	if (material.emission_texture == 0u || material.emission_texture > ray_scene.emission_texture_count) {
		return material.emission;
	}
	vec2 uv = p_uv * material.emission_uv_scale + material.emission_uv_offset;
	vec4 texel = textureLod(ray_scene_emission_textures, vec3(uv, float(material.emission_texture - 1u)), 0.0);
	float nits = (material.emission_flags & RAY_SCENE_EMISSION_TEXTURE_RANGES) != 0u ? mix(material.emission_nits_range.x, material.emission_nits_range.y, texel.a) : texel.a;
	return texel.rgb * nits * material.emission_texel_scale;
}

float ray_scene_triangle_emitter_mean_luminance(uint p_material) {
	return ray_scene_materials.data[p_material].emission_mean_luminance;
}

float ray_scene_emissive_exposure_normalization() {
	return ray_scene.emissive_exposure_normalization;
}

vec3 ray_scene_triangle_emitter_emission_light_space(uint p_material) {
	return ray_scene_triangle_emitter_emission(p_material) * ray_scene.emissive_exposure_normalization;
}

uint ray_scene_triangle_emitter_count() {
	return ray_scene.triangle_emitter_count;
}

bool ray_scene_triangle_emitters_capped() {
	return ray_scene.triangle_emitters_capped != 0u;
}

uint ray_scene_triangle_emitter_generation() {
	return ray_scene.triangle_emitter_generation;
}

uint ray_scene_hash(uint p_value) {
	p_value ^= p_value >> 16u;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15u;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16u;
	return p_value;
}

#ifdef RAY_SCENE_REPLAY
bool ray_scene_replay = false;

#define RAY_SCENE_MIN_TARGET 5.421011e-20

#define RAY_SCENE_LIGHT_KEY(m_index, m_id) (ray_scene_replay ? (m_id) : (m_index))
#define RAY_SCENE_PICKS(m_stream, m_race) (ray_scene_replay ? (m_race) : (m_stream))

float ray_scene_precise_divide(float p_a, float p_b) {
	precise float quotient = p_a / p_b;
	precise float residual = fma(-quotient, p_b, p_a);
	precise float corrected = fma(residual, 1.0 / p_b, quotient);
	return corrected;
}

float ray_scene_race_exponential(uint p_seed_hash, uint p_id) {
	uint k = ray_scene_hash(p_seed_hash ^ ray_scene_hash(p_id)) >> 9u;
	if (k < 4194304u) {
		return -log((float(k) + 0.5) * exp2(-23.0));
	}
	precise float v = (8388607.5 - float(k)) * exp2(-23.0);
	precise float t = v / (2.0 - v);
	precise float t2 = t * t;
	precise float series = 1.0 / 13.0;
	series = fma(series, t2, 1.0 / 11.0);
	series = fma(series, t2, 1.0 / 9.0);
	series = fma(series, t2, 1.0 / 7.0);
	series = fma(series, t2, 1.0 / 5.0);
	series = fma(series, t2, 1.0 / 3.0);
	series = fma(series, t2, 1.0);
	return 2.0 * t * series;
}

bool ray_scene_race_step(inout float r_key, inout uint r_id, uint p_seed_hash, uint p_id, float p_weight) {
	float key = ray_scene_precise_divide(ray_scene_race_exponential(p_seed_hash, p_id), p_weight);
	if (key < r_key || (key == r_key && p_id < r_id)) {
		r_key = key;
		r_id = p_id;
		return true;
	}
	return false;
}
#else
#define RAY_SCENE_LIGHT_KEY(m_index, m_id) (m_index)
#define RAY_SCENE_PICKS(m_stream, m_race) (m_stream)
#endif

#ifdef RAY_SCENE_ALPHA
bool ray_scene_alpha_culled(uint p_bits, uint p_flags, bool p_backface, bool p_shadow) {
	uint cull = p_bits & RAY_SCENE_ALPHA_CULL_MASK;
	if (cull == 0u || (p_shadow && (p_flags & RAY_SCENE_MATERIAL_SHADOW_TWO_SIDED) != 0u)) {
		return false;
	}
	bool seen_back = p_backface != p_shadow;
	return cull == RAY_SCENE_ALPHA_CULL_BACK ? seen_back : !seen_back;
}

bool ray_scene_alpha_covered(RaySceneAlphaEntry p_entry, uint p_triangle, uint p_instance, vec2 p_bary, uint p_seed) {
	uint base = p_triangle * 4u;
	float w0 = 1.0 - p_bary.x - p_bary.y;
	vec2 uv = unpackHalf2x16(ray_scene_triangle_uvs.data[base]) * w0 + unpackHalf2x16(ray_scene_triangle_uvs.data[base + 1u]) * p_bary.x + unpackHalf2x16(ray_scene_triangle_uvs.data[base + 2u]) * p_bary.y;
	uv = uv * p_entry.scale + p_entry.offset;
	float coverage = textureLod(ray_scene_alpha_layers, vec3(uv, float(p_entry.layer)), 0.0).r;
#ifdef RAY_SCENE_ALPHA_THRESHOLD
	return coverage >= 0.5;
#else
	return coverage > float(ray_scene_hash(ray_scene_hash(p_seed ^ (p_triangle * 0x9e3779b9u)) + p_instance) >> 8u) / 16777216.0;
#endif
}

bool ray_scene_alpha_passes(uint p_flags, uint p_triangle, uint p_instance, vec2 p_bary, uint p_seed, bool p_backface, bool p_shadow) {
	uint index = p_flags >> RAY_SCENE_ALPHA_SHIFT;
	if (index == 0u) {
		return true;
	}
	RaySceneAlphaEntry entry = ray_scene_alpha_table.data[index];
	if (entry.state != RAY_SCENE_ALPHA_STATE_READY) {
		return true;
	}
	if (ray_scene_alpha_culled(entry.bits, p_flags, p_backface, p_shadow)) {
		return false;
	}
	return ray_scene_alpha_covered(entry, p_triangle, p_instance, p_bary, p_seed);
}
#endif

vec3 ray_scene_safe_inverse(vec3 p_direction) {
	vec3 safe = mix(vec3(1e-20), abs(p_direction), greaterThan(abs(p_direction), vec3(1e-20)));
	return 1.0 / (safe * mix(vec3(-1.0), vec3(1.0), greaterThanEqual(p_direction, vec3(0.0))));
}

bool ray_scene_box(vec3 p_origin, vec3 p_inv_direction, vec3 p_min, vec3 p_max, float p_t_max, out float r_t_near) {
	vec3 t0 = (p_min - p_origin) * p_inv_direction;
	vec3 t1 = (p_max - p_origin) * p_inv_direction;
	vec3 t_min = min(t0, t1);
	vec3 t_max = max(t0, t1);
	r_t_near = max(max(t_min.x, t_min.y), max(t_min.z, 0.0));
	float t_far = min(min(t_max.x, t_max.y), min(t_max.z, p_t_max));
	return r_t_near <= t_far;
}

#ifndef RAY_SCENE_HW
void ray_scene_trace_blas(uint p_root, uint p_material_offset, uint p_instance, vec3 p_origin, vec3 p_direction, bool p_any, bool p_shadow, uint p_skip, uint p_seed, bool p_band, float p_tie_scale, float p_near_backface, inout RaySceneHit r_hit) {
	vec3 inv_direction = ray_scene_safe_inverse(p_direction);
	uint skip_mask = RAY_SCENE_SKIP_MASK(p_shadow, p_skip);
	uint stack[RAY_SCENE_BLAS_STACK_SIZE];
	uint sp = 0;

	float t_near;
	RaySceneNode node = ray_scene_blas_nodes.data[p_root];
	if (!ray_scene_box(p_origin, inv_direction, node.bmin, node.bmax, RAY_SCENE_TIE_REACH(r_hit.t, p_tie_scale), t_near)) {
		return;
	}
	bool band = p_band || t_near > r_hit.t;

	while (true) {
		if (node.count > 0u) {
			for (uint i = 0u; i < min(node.count, RAY_SCENE_LEAF_SIZE); i++) {
				uint triangle_index = node.left_first + i;
				RaySceneTriangle triangle = ray_scene_triangles.data[triangle_index];
				vec3 p = cross(p_direction, triangle.e2.xyz);
				float det = dot(triangle.e1.xyz, p);
				if (abs(det) < 1e-14) {
					continue;
				}
				float inv_det = 1.0 / det;
				vec3 s = p_origin - triangle.v0;
				float u = dot(s, p) * inv_det;
				if (u < 0.0 || u > 1.0) {
					continue;
				}
				vec3 q = cross(s, triangle.e1.xyz);
				float v = dot(p_direction, q) * inv_det;
				if (v < 0.0 || u + v > 1.0) {
					continue;
				}
				float t = dot(triangle.e2.xyz, q) * inv_det;
				if (t > 0.0 && t < RAY_SCENE_TIE_REACH(r_hit.t, p_tie_scale)) {
					uint material_index = p_material_offset + triangle.surface;
#ifdef RAY_SCENE_EXCLUDE
					if (exclude_count != 0u && ray_scene_excluded(material_index)) {
						continue;
					}
#endif
					uint flags = ray_scene_materials.data[material_index].flags;
					if ((flags & skip_mask) != 0u) {
						continue;
					}
					if (t < p_near_backface && det > 0.0 && (flags & (RAY_SCENE_MATERIAL_TWO_SIDED | RAY_SCENE_MATERIAL_BLEND)) == 0u) {
						continue;
					}
#ifdef RAY_SCENE_ALPHA
					if (!ray_scene_alpha_passes(flags, triangle_index, p_instance, vec2(u, v), p_seed, det > 0.0, p_shadow)) {
						continue;
					}
#endif
					bool blend = (flags & RAY_SCENE_MATERIAL_BLEND) != 0u;
					if (band && !blend) {
						continue;
					}
					RAY_SCENE_TIE_LIMIT(limit, r_hit, blend, p_tie_scale);
					if (t >= limit) {
						continue;
					}
					if (blend && RAY_SCENE_COVERAGE_MISSES(p_seed, triangle_index, p_instance, ray_scene_materials.data[material_index].coverage)) {
						continue;
					}
					r_hit.t = t;
					r_hit.instance = p_instance;
					r_hit.triangle = triangle_index;
					r_hit.backface = det > 0.0;
					r_hit.blend = blend;
					if (p_any) {
						return;
					}
				}
			}
		} else {
			uint left = node.left_first;
			RaySceneNode a = ray_scene_blas_nodes.data[left];
			RaySceneNode b = ray_scene_blas_nodes.data[left + 1u];
			float ta;
			float tb;
			float reach = RAY_SCENE_TIE_REACH(r_hit.t, p_tie_scale);
			bool hit_a = ray_scene_box(p_origin, inv_direction, a.bmin, a.bmax, reach, ta);
			bool hit_b = ray_scene_box(p_origin, inv_direction, b.bmin, b.bmax, reach, tb);
			bool band_a = band || ta > r_hit.t;
			bool band_b = band || tb > r_hit.t;
			if (hit_a && hit_b) {
				bool a_first = ta <= tb;
				stack[sp++] = a_first ? (left + 1u) | (band_b ? RAY_SCENE_BAND_NODE : 0u) : left | (band_a ? RAY_SCENE_BAND_NODE : 0u);
				node = a_first ? a : b;
				band = a_first ? band_a : band_b;
				continue;
			} else if (hit_a || hit_b) {
				node = hit_a ? a : b;
				band = hit_a ? band_a : band_b;
				continue;
			}
		}

		if (sp == 0u) {
			break;
		}
		uint entry = stack[--sp];
		band = (entry & RAY_SCENE_BAND_NODE) != 0u;
		node = ray_scene_blas_nodes.data[entry & ~RAY_SCENE_BAND_NODE];
	}
}

bool ray_scene_trace_masked_near(vec3 p_origin, vec3 p_direction, float p_t_max, bool p_any, bool p_shadow, uint p_skip, uint p_caster_mask, uint p_seed, float p_near_backface, out RaySceneHit r_hit) {
	r_hit.t = p_t_max;
	r_hit.instance = RAY_SCENE_NO_HIT;
	r_hit.triangle = 0u;
	r_hit.backface = false;
	r_hit.blend = false;
	if (ray_scene.instance_count == 0u) {
		return false;
	}

	vec3 inv_direction = ray_scene_safe_inverse(p_direction);
	uint seed = RAY_SCENE_COVERAGE_SEED(p_seed);
	float tie_scale = RAY_SCENE_TIE_SCALE(p_origin);
	uint stack[RAY_SCENE_TLAS_STACK_SIZE];
	uint sp = 0;

	float t_near;
	RaySceneNode node = ray_scene_tlas_nodes.data[0u];
	if (!ray_scene_box(p_origin, inv_direction, node.bmin, node.bmax, RAY_SCENE_TIE_REACH(r_hit.t, tie_scale), t_near)) {
		return false;
	}
	bool band = t_near > r_hit.t;

	while (true) {
		if (node.count > 0u) {
			for (uint i = 0u; i < min(node.count, RAY_SCENE_LEAF_SIZE); i++) {
				RaySceneInstance instance = ray_scene_instances.data[node.left_first + i];
				if (RAY_SCENE_CASTER_SKIPS(instance.layer_mask, p_caster_mask)) {
					continue;
				}
				vec4 origin = vec4(p_origin, 1.0);
				vec3 local_origin = vec3(dot(instance.world_to_object[0], origin), dot(instance.world_to_object[1], origin), dot(instance.world_to_object[2], origin));
				vec3 local_direction = vec3(dot(instance.world_to_object[0].xyz, p_direction), dot(instance.world_to_object[1].xyz, p_direction), dot(instance.world_to_object[2].xyz, p_direction));
				ray_scene_trace_blas(instance.blas_root, instance.material_offset, node.left_first + i, local_origin, local_direction, p_any, p_shadow, p_skip, seed, band, tie_scale, p_near_backface, r_hit);
				if (p_any && r_hit.instance != RAY_SCENE_NO_HIT) {
					return true;
				}
			}
		} else {
			uint left = node.left_first;
			RaySceneNode a = ray_scene_tlas_nodes.data[left];
			RaySceneNode b = ray_scene_tlas_nodes.data[left + 1u];
			float ta;
			float tb;
			float reach = RAY_SCENE_TIE_REACH(r_hit.t, tie_scale);
			bool hit_a = ray_scene_box(p_origin, inv_direction, a.bmin, a.bmax, reach, ta);
			bool hit_b = ray_scene_box(p_origin, inv_direction, b.bmin, b.bmax, reach, tb);
			bool band_a = band || ta > r_hit.t;
			bool band_b = band || tb > r_hit.t;
			if (hit_a && hit_b) {
				bool a_first = ta <= tb;
				stack[sp++] = a_first ? (left + 1u) | (band_b ? RAY_SCENE_BAND_NODE : 0u) : left | (band_a ? RAY_SCENE_BAND_NODE : 0u);
				node = a_first ? a : b;
				band = a_first ? band_a : band_b;
				continue;
			} else if (hit_a || hit_b) {
				node = hit_a ? a : b;
				band = hit_a ? band_a : band_b;
				continue;
			}
		}

		if (sp == 0u) {
			break;
		}
		uint entry = stack[--sp];
		band = (entry & RAY_SCENE_BAND_NODE) != 0u;
		node = ray_scene_tlas_nodes.data[entry & ~RAY_SCENE_BAND_NODE];
	}
	return r_hit.instance != RAY_SCENE_NO_HIT;
}
#else
#include "ray_scene_hw_inc.glsl"
#endif

// Ray scene space only; the caller biases p_from off the emitter surface. Requires the ray scene TLAS bindings when RAY_SCENE_HW is set.
bool ray_scene_sampled_visibility(vec3 p_from, vec3 p_to, uint p_instance_mask, uint p_seed) {
	vec3 offset = p_to - p_from;
	float distance = length(offset);
	if (distance <= 0.0) {
		return true;
	}
	RaySceneHit hit;
	return !ray_scene_trace_masked_near(p_from, offset / distance, distance, true, true, 0u, p_instance_mask, p_seed, 0.0, hit);
}
bool ray_scene_sampled_visibility_blocker(vec3 p_from, vec3 p_to, uint p_instance_mask, uint p_seed, int p_skip_instance, out uint r_blocker, out float r_blocker_distance) {
	r_blocker = 0xFFFFFFFFu;
	vec3 offset = p_to - p_from;
	float distance = length(offset);
	if (distance <= 0.0) {
		return true;
	}
	vec3 direction = offset / distance;
	float travelled = 0.0;
	for (int i = 0; i < 4; i++) {
		RaySceneHit hit;
		if (!ray_scene_trace_masked_near(p_from + direction * travelled, direction, distance - travelled, true, true, 0u, p_instance_mask, p_seed, 0.0, hit)) {
			return true;
		}
		if (p_skip_instance < 0 || int(hit.instance) != p_skip_instance) {
			r_blocker = hit.instance;
			r_blocker_distance = travelled + hit.t;
			return false;
		}
		travelled += hit.t + 1e-4;
		if (travelled >= distance) {
			return true;
		}
	}
	return true;
}
bool ray_scene_sampled_visibility_skipping(vec3 p_from, vec3 p_to, uint p_instance_mask, uint p_seed, int p_skip_instance) {
	vec3 offset = p_to - p_from;
	float distance = length(offset);
	if (distance <= 0.0) {
		return true;
	}
	vec3 direction = offset / distance;
	float travelled = 0.0;
	for (int i = 0; i < 4; i++) {
		RaySceneHit hit;
		if (!ray_scene_trace_masked_near(p_from + direction * travelled, direction, distance - travelled, true, true, 0u, p_instance_mask, p_seed, 0.0, hit)) {
			return true;
		}
		if (p_skip_instance < 0 || int(hit.instance) != p_skip_instance) {
			return false;
		}
		travelled += hit.t + 1e-4;
		if (travelled >= distance) {
			return true;
		}
	}
	return true;
}

bool ray_scene_trace_masked(vec3 p_origin, vec3 p_direction, float p_t_max, bool p_any, bool p_shadow, uint p_skip, uint p_caster_mask, uint p_seed, out RaySceneHit r_hit) {
	return ray_scene_trace_masked_near(p_origin, p_direction, p_t_max, p_any, p_shadow, p_skip, p_caster_mask, p_seed, 0.0, r_hit);
}

bool ray_scene_trace(vec3 p_origin, vec3 p_direction, float p_t_max, bool p_any, bool p_shadow, uint p_seed, out RaySceneHit r_hit) {
	return ray_scene_trace_masked(p_origin, p_direction, p_t_max, p_any, p_shadow, 0u, RAY_SCENE_ALL_CASTERS, p_seed, r_hit);
}

bool ray_scene_trace_opaque(vec3 p_origin, vec3 p_direction, float p_t_max, uint p_seed, out RaySceneHit r_hit) {
	return ray_scene_trace_masked(p_origin, p_direction, p_t_max, false, false, RAY_SCENE_MATERIAL_BLEND, RAY_SCENE_ALL_CASTERS, p_seed, r_hit);
}

vec3 ray_scene_hit_normal(RaySceneHit p_hit) {
	RaySceneTriangle triangle = ray_scene_triangles.data[p_hit.triangle];
	RaySceneInstance instance = ray_scene_instances.data[p_hit.instance];
	mat3 normal_matrix = mat3(instance.world_to_object[0].xyz, instance.world_to_object[1].xyz, instance.world_to_object[2].xyz);
	return normalize(normal_matrix * cross(triangle.e2.xyz, triangle.e1.xyz));
}

RaySceneMaterial ray_scene_hit_material(RaySceneHit p_hit) {
	RaySceneTriangle triangle = ray_scene_triangles.data[p_hit.triangle];
	RaySceneInstance instance = ray_scene_instances.data[p_hit.instance];
	return ray_scene_materials.data[instance.material_offset + triangle.surface];
}

float ray_scene_omni_attenuation(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(p_distance, 0.0001), -p_decay);
}

bool ray_scene_unshadowed(vec3 p_position, vec3 p_normal, float p_bias, vec3 p_to_light, float p_distance, uint p_caster_mask, uint p_seed) {
	RaySceneHit shadow;
	return !ray_scene_trace_masked_near(p_position + p_normal * p_bias, p_to_light, max(p_distance - p_bias, 0.0), true, true, 0u, p_caster_mask, p_seed, 0.25 * p_bias, shadow);
}

vec3 ray_scene_direct_light(vec3 p_position, vec3 p_normal, float p_bias, float p_max_distance, uint p_seed) {
	vec3 light_sum = vec3(0.0);
	vec3 chosen_light = vec3(0.0);
	vec3 chosen_direction = vec3(0.0);
	float chosen_distance = 0.0;
	float chosen_weight = 0.0;
	float total_weight = 0.0;
	vec3 exact_light[RAY_SCENE_EXACT_LIGHTS];
	vec3 exact_direction[RAY_SCENE_EXACT_LIGHTS];
	float exact_distance[RAY_SCENE_EXACT_LIGHTS];
	uint exact_mask[RAY_SCENE_EXACT_LIGHTS];
	uint chosen_mask = RAY_SCENE_ALL_CASTERS;
	uint local_count = 0u;
#ifdef RAY_SCENE_REPLAY
	uint exact_id[RAY_SCENE_EXACT_LIGHTS];
	uint race_seed = ray_scene_hash(p_seed);
	float race_key = uintBitsToFloat(0x7f800000u);
	uint race_id = 0xFFFFFFFFu;
#endif
	for (uint i = 0u; i < ray_scene.light_count; i++) {
		RaySceneLight light = ray_scene_lights.data[i];
		vec3 to_light;
		float attenuation = 1.0;
		float distance = p_max_distance;
		if (light.type == RAY_SCENE_LIGHT_DIRECTIONAL) {
			to_light = -light.direction;
		} else if (light.type == RAY_SCENE_LIGHT_AREA) {
			vec3 offset = p_position - light.position;
			if (dot(light.direction, offset) <= 0.0) {
				continue;
			}
			uint rect_seed = ray_scene_hash(p_seed + 0x68bc21ebu) ^ (RAY_SCENE_LIGHT_KEY(i, light.pad) * 0x9e3779b9u);
			vec2 rect_uv = vec2(float(ray_scene_hash(rect_seed) >> 8u), float(ray_scene_hash(rect_seed ^ 0x2f8f3d5bu) >> 8u)) * (2.0 / 16777216.0) - 1.0;
			vec3 relative = light.position + light.half_a * rect_uv.x + light.half_b * rect_uv.y - p_position;
			distance = length(relative);
			to_light = relative / max(distance, 1e-4);
			vec2 extent = max(vec2(dot(light.half_a, light.half_a), dot(light.half_b, light.half_b)), vec2(1e-12));
			vec2 closest = clamp(vec2(dot(offset, light.half_a), dot(offset, light.half_b)) / extent, -1.0, 1.0);
			float closest_distance = length(offset - light.half_a * closest.x - light.half_b * closest.y);
			float window = ray_scene_omni_attenuation(closest_distance, light.inv_radius, light.attenuation) * closest_distance * closest_distance;
			attenuation = window * light.area * max(dot(-to_light, light.direction), 0.0) / max(distance * distance, 1e-8);
		} else {
			vec3 relative = light.position - p_position;
			distance = length(relative);
			to_light = relative / max(distance, 1e-4);
			attenuation = ray_scene_omni_attenuation(distance, light.inv_radius, light.attenuation);
			if (light.type == RAY_SCENE_LIGHT_SPOT) {
				float spot_cos = max(dot(-to_light, light.direction), light.cos_spot_angle);
				float spot_rim = max(1e-4, (1.0 - spot_cos) / (1.0 - light.cos_spot_angle));
				attenuation *= 1.0 - pow(spot_rim, light.inv_spot_attenuation);
			}
		}
		float n_dot_l = dot(p_normal, to_light);
		if (n_dot_l <= 0.0 || attenuation <= 0.0) {
			continue;
		}
		vec3 light_value = light.color * (light.energy * attenuation * n_dot_l / 3.14159265359);
		if (light.type == RAY_SCENE_LIGHT_DIRECTIONAL) {
			if (ray_scene_unshadowed(p_position, p_normal, p_bias, to_light, distance, light.caster_mask, (p_seed + RAY_SCENE_LIGHT_KEY(i, light.pad) * 0x632be5abu) ^ 0x85ebca6bu)) {
				light_sum += light_value;
			}
			continue;
		}
		float weight = dot(light_value, luminance_weights);
		if (weight <= 0.0) {
			continue;
		}
#ifdef RAY_SCENE_REPLAY
		if (ray_scene_replay && !(weight >= RAY_SCENE_MIN_TARGET && !isinf(weight))) {
			continue;
		}
#endif
		total_weight += weight;
		if (local_count < RAY_SCENE_EXACT_LIGHTS) {
			exact_light[local_count] = light_value;
			exact_direction[local_count] = to_light;
			exact_distance[local_count] = distance;
			exact_mask[local_count] = light.caster_mask;
#ifdef RAY_SCENE_REPLAY
			exact_id[local_count] = light.pad;
#endif
		}
		local_count++;
		if (RAY_SCENE_PICKS(float(ray_scene_hash(p_seed ^ (i * 0x9e3779b9u)) >> 8u) * total_weight < weight * 16777216.0, ray_scene_race_step(race_key, race_id, race_seed, light.pad, weight))) {
			chosen_light = light_value;
			chosen_direction = to_light;
			chosen_distance = distance;
			chosen_weight = weight;
			chosen_mask = light.caster_mask;
		}
	}
	if (local_count <= RAY_SCENE_EXACT_LIGHTS) {
		for (uint i = 0u; i < local_count; i++) {
			if (ray_scene_unshadowed(p_position, p_normal, p_bias, exact_direction[i], exact_distance[i], exact_mask[i], p_seed + (RAY_SCENE_LIGHT_KEY(i, exact_id[i]) + 0x10000u) * 0x632be5abu)) {
				light_sum += exact_light[i];
			}
		}
	} else if (ray_scene_unshadowed(p_position, p_normal, p_bias, chosen_direction, chosen_distance, chosen_mask, p_seed ^ 0x27d4eb2fu)) {
		light_sum += chosen_light * (total_weight / chosen_weight);
	}
	return light_sum;
}

#define gi_hit_direct_light ray_scene_direct_light



