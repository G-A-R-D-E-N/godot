#define RAY_SCENE_LEAF_SIZE 4u
#define RAY_SCENE_NO_HIT 0xFFFFFFFFu
#define RAY_SCENE_EXACT_LIGHTS 4u

#define RAY_SCENE_LIGHT_DIRECTIONAL 0u
#define RAY_SCENE_LIGHT_SPOT 2u
#define RAY_SCENE_LIGHT_AREA 3u
#define RAY_SCENE_ALL_CASTERS 0xFFFFFFFFu

#define RAY_SCENE_MATERIAL_TWO_SIDED 1u
#define RAY_SCENE_MATERIAL_SKIP 2u
#define RAY_SCENE_MATERIAL_SHADOW_PASS 8u

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
	float pad;
	vec3 emission;
	uint flags;
};

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

layout(set = RAY_SCENE_SET, binding = 0, std430) restrict readonly buffer RaySceneBlasNodes {
	RaySceneNode data[];
}
ray_scene_blas_nodes;

layout(set = RAY_SCENE_SET, binding = 1, std430) restrict readonly buffer RaySceneTriangles {
	RaySceneTriangle data[];
}
ray_scene_triangles;

layout(set = RAY_SCENE_SET, binding = 2, std430) restrict readonly buffer RaySceneInstances {
	RaySceneInstance data[];
}
ray_scene_instances;

layout(set = RAY_SCENE_SET, binding = 3, std430) restrict readonly buffer RaySceneTlasNodes {
	RaySceneNode data[];
}
ray_scene_tlas_nodes;

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
	uint pad0;
	uint pad1;
	uint pad2;
}
ray_scene;

struct RaySceneHit {
	float t;
	uint instance;
	uint triangle;
	bool backface;
};

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

void ray_scene_trace_blas(uint p_root, uint p_material_offset, uint p_instance, vec3 p_origin, vec3 p_direction, bool p_any, inout RaySceneHit r_hit) {
	vec3 inv_direction = ray_scene_safe_inverse(p_direction);
	uint skip_mask = p_any ? (RAY_SCENE_MATERIAL_SKIP | RAY_SCENE_MATERIAL_SHADOW_PASS) : RAY_SCENE_MATERIAL_SKIP;
	uint stack[RAY_SCENE_BLAS_STACK_SIZE];
	uint sp = 0;

	float t_near;
	RaySceneNode node = ray_scene_blas_nodes.data[p_root];
	if (!ray_scene_box(p_origin, inv_direction, node.bmin, node.bmax, r_hit.t, t_near)) {
		return;
	}

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
				if (t > 0.0 && t < r_hit.t) {
					if ((ray_scene_materials.data[p_material_offset + triangle.surface].flags & skip_mask) != 0u) {
						continue;
					}
					r_hit.t = t;
					r_hit.instance = p_instance;
					r_hit.triangle = triangle_index;
					r_hit.backface = det > 0.0;
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
			bool hit_a = ray_scene_box(p_origin, inv_direction, a.bmin, a.bmax, r_hit.t, ta);
			bool hit_b = ray_scene_box(p_origin, inv_direction, b.bmin, b.bmax, r_hit.t, tb);
			if (hit_a && hit_b) {
				bool a_first = ta <= tb;
				stack[sp++] = a_first ? left + 1u : left;
				node = a_first ? a : b;
				continue;
			} else if (hit_a || hit_b) {
				node = hit_a ? a : b;
				continue;
			}
		}

		if (sp == 0u) {
			break;
		}
		node = ray_scene_blas_nodes.data[stack[--sp]];
	}
}

bool ray_scene_trace_masked(vec3 p_origin, vec3 p_direction, float p_t_max, bool p_any, uint p_caster_mask, out RaySceneHit r_hit) {
	r_hit.t = p_t_max;
	r_hit.instance = RAY_SCENE_NO_HIT;
	r_hit.triangle = 0u;
	r_hit.backface = false;
	if (ray_scene.instance_count == 0u) {
		return false;
	}

	vec3 inv_direction = ray_scene_safe_inverse(p_direction);
	uint stack[RAY_SCENE_TLAS_STACK_SIZE];
	uint sp = 0;

	float t_near;
	RaySceneNode node = ray_scene_tlas_nodes.data[0u];
	if (!ray_scene_box(p_origin, inv_direction, node.bmin, node.bmax, r_hit.t, t_near)) {
		return false;
	}

	while (true) {
		if (node.count > 0u) {
			for (uint i = 0u; i < min(node.count, RAY_SCENE_LEAF_SIZE); i++) {
				RaySceneInstance instance = ray_scene_instances.data[node.left_first + i];
				if (p_caster_mask != RAY_SCENE_ALL_CASTERS && (instance.layer_mask & p_caster_mask) == 0u) {
					continue;
				}
				vec4 origin = vec4(p_origin, 1.0);
				vec3 local_origin = vec3(dot(instance.world_to_object[0], origin), dot(instance.world_to_object[1], origin), dot(instance.world_to_object[2], origin));
				vec3 local_direction = vec3(dot(instance.world_to_object[0].xyz, p_direction), dot(instance.world_to_object[1].xyz, p_direction), dot(instance.world_to_object[2].xyz, p_direction));
				ray_scene_trace_blas(instance.blas_root, instance.material_offset, node.left_first + i, local_origin, local_direction, p_any, r_hit);
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
			bool hit_a = ray_scene_box(p_origin, inv_direction, a.bmin, a.bmax, r_hit.t, ta);
			bool hit_b = ray_scene_box(p_origin, inv_direction, b.bmin, b.bmax, r_hit.t, tb);
			if (hit_a && hit_b) {
				bool a_first = ta <= tb;
				stack[sp++] = a_first ? left + 1u : left;
				node = a_first ? a : b;
				continue;
			} else if (hit_a || hit_b) {
				node = hit_a ? a : b;
				continue;
			}
		}

		if (sp == 0u) {
			break;
		}
		node = ray_scene_tlas_nodes.data[stack[--sp]];
	}
	return r_hit.instance != RAY_SCENE_NO_HIT;
}

bool ray_scene_trace(vec3 p_origin, vec3 p_direction, float p_t_max, bool p_any, out RaySceneHit r_hit) {
	return ray_scene_trace_masked(p_origin, p_direction, p_t_max, p_any, RAY_SCENE_ALL_CASTERS, r_hit);
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

uint ray_scene_hash(uint p_value) {
	p_value ^= p_value >> 16u;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15u;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16u;
	return p_value;
}

bool ray_scene_unshadowed(vec3 p_position, vec3 p_normal, float p_bias, vec3 p_to_light, float p_distance, uint p_caster_mask) {
	RaySceneHit shadow;
	return !ray_scene_trace_masked(p_position + p_normal * p_bias, p_to_light, max(p_distance - p_bias, 0.0), true, p_caster_mask, shadow);
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
			uint rect_seed = ray_scene_hash(p_seed + 0x68bc21ebu) ^ (i * 0x9e3779b9u);
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
			if (ray_scene_unshadowed(p_position, p_normal, p_bias, to_light, distance, light.caster_mask)) {
				light_sum += light_value;
			}
			continue;
		}
		float weight = dot(light_value, luminance_weights);
		if (weight <= 0.0) {
			continue;
		}
		total_weight += weight;
		if (local_count < RAY_SCENE_EXACT_LIGHTS) {
			exact_light[local_count] = light_value;
			exact_direction[local_count] = to_light;
			exact_distance[local_count] = distance;
			exact_mask[local_count] = light.caster_mask;
		}
		local_count++;
		if (float(ray_scene_hash(p_seed ^ (i * 0x9e3779b9u)) >> 8u) * total_weight < weight * 16777216.0) {
			chosen_light = light_value;
			chosen_direction = to_light;
			chosen_distance = distance;
			chosen_weight = weight;
			chosen_mask = light.caster_mask;
		}
	}
	if (local_count <= RAY_SCENE_EXACT_LIGHTS) {
		for (uint i = 0u; i < local_count; i++) {
			if (ray_scene_unshadowed(p_position, p_normal, p_bias, exact_direction[i], exact_distance[i], exact_mask[i])) {
				light_sum += exact_light[i];
			}
		}
	} else if (ray_scene_unshadowed(p_position, p_normal, p_bias, chosen_direction, chosen_distance, chosen_mask)) {
		light_sum += chosen_light * (total_weight / chosen_weight);
	}
	return light_sum;
}
