#define RAY_SCENE_HW_GROUP_COUNT 7u
#define RAY_SCENE_HW_BLEND_INSTANCES 8u
#define RAY_SCENE_HW_ALL_GROUPS 0x80u
#define RAY_SCENE_HW_REMAP_SHIFT 2u

uint ray_scene_hw_triangle(uint p_instance, uint p_geometry, uint p_primitive) {
	RaySceneInstance instance = ray_scene_instances.data[p_instance];
	uint region = instance.pad >> RAY_SCENE_HW_REMAP_SHIFT;
	if (region == 0u) {
		return instance.blas_root + p_primitive;
	}
	uint base = region - 1u;
	uint position = p_geometry == 0u ? p_primitive : ray_scene_hw_remap.data[base] + p_primitive;
	return instance.blas_root + ray_scene_hw_remap.data[base + 1u + position];
}

uint ray_scene_hw_material(uint p_instance, uint p_triangle) {
	return ray_scene_instances.data[p_instance].material_offset + ray_scene_triangles.data[p_triangle].surface;
}

bool ray_scene_hw_blend(uint p_instance, uint p_triangle) {
	return (ray_scene_materials.data[ray_scene_hw_material(p_instance, p_triangle)].flags & RAY_SCENE_MATERIAL_BLEND) != 0u;
}

uint ray_scene_hw_cull_mask(uint p_caster_mask) {
	if (p_caster_mask == RAY_SCENE_ALL_CASTERS) {
		return 0xFFu;
	}
	uint count = ray_scene_hw_remap.data[RAY_SCENE_HW_GROUP_COUNT];
	for (uint g = 0u; g < count; g++) {
		if (ray_scene_hw_remap.data[g] == p_caster_mask) {
			return 1u << g;
		}
	}
	return RAY_SCENE_HW_ALL_GROUPS;
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

	uint seed = RAY_SCENE_COVERAGE_SEED(p_seed);
	float tie_scale = RAY_SCENE_TIE_SCALE(p_origin);
	uint skip_mask = RAY_SCENE_SKIP_MASK(p_shadow, p_skip);
	uint cull_mask = ray_scene_hw_cull_mask(p_caster_mask);
	bool exact = cull_mask != RAY_SCENE_HW_ALL_GROUPS;
	uint ray_flags = (p_any ? gl_RayFlagsTerminateOnFirstHitEXT : gl_RayFlagsNoneEXT) | (exact ? gl_RayFlagsNoneEXT : gl_RayFlagsNoOpaqueEXT);
#ifdef RAY_SCENE_EXCLUDE
	if (exclude_count != 0u) {
		ray_flags |= gl_RayFlagsNoOpaqueEXT;
	}
#endif

	float near_backface = min(p_near_backface, p_t_max);
	rayQueryEXT query;
	for (uint pass = near_backface > 0.0 ? 0u : 1u; pass < 2u; pass++) {
		bool near_pass = pass == 0u;
		if (!near_pass && near_backface > 0.0 && near_backface >= p_t_max) {
			break;
		}
		rayQueryInitializeEXT(query, ray_scene_tlas, near_pass ? ray_flags | gl_RayFlagsNoOpaqueEXT : ray_flags, cull_mask, p_origin, near_pass ? 0.0 : near_backface, p_direction, near_pass ? near_backface : p_t_max);
		while (rayQueryProceedEXT(query)) {
			uint index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false));
			if (!exact && RAY_SCENE_CASTER_SKIPS(ray_scene_instances.data[index].layer_mask, p_caster_mask)) {
				continue;
			}
			uint triangle = ray_scene_hw_triangle(index, uint(rayQueryGetIntersectionGeometryIndexEXT(query, false)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, false)));
#ifdef RAY_SCENE_EXCLUDE
			if (exclude_count != 0u && ray_scene_excluded(ray_scene_hw_material(index, triangle))) {
				continue;
			}
#endif
			RaySceneMaterial material = ray_scene_materials.data[ray_scene_hw_material(index, triangle)];
			if ((material.flags & skip_mask) != 0u) {
				continue;
			}
			if (near_pass && !rayQueryGetIntersectionFrontFaceEXT(query, false) && (material.flags & (RAY_SCENE_MATERIAL_TWO_SIDED | RAY_SCENE_MATERIAL_BLEND)) == 0u) {
				continue;
			}
			bool blend = (material.flags & RAY_SCENE_MATERIAL_BLEND) != 0u;
			if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
				r_hit.t = rayQueryGetIntersectionTEXT(query, true);
				r_hit.instance = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, true));
				r_hit.blend = ray_scene_hw_blend(r_hit.instance, ray_scene_hw_triangle(r_hit.instance, uint(rayQueryGetIntersectionGeometryIndexEXT(query, true)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, true))));
			}
			RAY_SCENE_TIE_LIMIT(limit, r_hit, blend, tie_scale);
			if (rayQueryGetIntersectionTEXT(query, false) >= limit) {
				continue;
			}
#ifdef RAY_SCENE_ALPHA
			if (!ray_scene_alpha_passes(material.flags, triangle, index, rayQueryGetIntersectionBarycentricsEXT(query, false).yx, seed, !rayQueryGetIntersectionFrontFaceEXT(query, false), p_shadow)) {
				continue;
			}
#endif
			if (blend && RAY_SCENE_COVERAGE_MISSES(seed, triangle, index, material.coverage)) {
				continue;
			}
			rayQueryConfirmIntersectionEXT(query);
		}
		if (rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
			break;
		}
	}

	if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
		r_hit.t = p_t_max;
		r_hit.instance = RAY_SCENE_NO_HIT;
		r_hit.blend = false;
		return false;
	}
	r_hit.t = rayQueryGetIntersectionTEXT(query, true);
	r_hit.instance = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(query, true));
	r_hit.triangle = ray_scene_hw_triangle(r_hit.instance, uint(rayQueryGetIntersectionGeometryIndexEXT(query, true)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, true)));
	r_hit.backface = !rayQueryGetIntersectionFrontFaceEXT(query, true);
	r_hit.blend = ray_scene_hw_blend(r_hit.instance, r_hit.triangle);
	if (p_any || r_hit.blend || (p_skip & RAY_SCENE_MATERIAL_BLEND) != 0u || ray_scene_hw_remap.data[RAY_SCENE_HW_BLEND_INSTANCES] == 0u) {
		return true;
	}

	rayQueryEXT tie_query;
	float tie_floor = max(r_hit.t - RAY_SCENE_TIE_ABS(r_hit.t, tie_scale), 0.0);
	rayQueryInitializeEXT(tie_query, ray_scene_tlas, gl_RayFlagsCullOpaqueEXT, cull_mask, p_origin, tie_floor, p_direction, RAY_SCENE_TIE_REACH(r_hit.t, tie_scale));
	while (rayQueryProceedEXT(tie_query)) {
		uint index = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(tie_query, false));
		if (!exact && RAY_SCENE_CASTER_SKIPS(ray_scene_instances.data[index].layer_mask, p_caster_mask)) {
			continue;
		}
		uint triangle = ray_scene_hw_triangle(index, uint(rayQueryGetIntersectionGeometryIndexEXT(tie_query, false)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(tie_query, false)));
		RaySceneMaterial material = ray_scene_materials.data[ray_scene_hw_material(index, triangle)];
		if ((material.flags & skip_mask) != 0u || (material.flags & RAY_SCENE_MATERIAL_BLEND) == 0u) {
			continue;
		}
#ifdef RAY_SCENE_ALPHA
		if (!ray_scene_alpha_passes(material.flags, triangle, index, rayQueryGetIntersectionBarycentricsEXT(tie_query, false).yx, seed, !rayQueryGetIntersectionFrontFaceEXT(tie_query, false), p_shadow)) {
			continue;
		}
#endif
		if (RAY_SCENE_COVERAGE_MISSES(seed, triangle, index, material.coverage)) {
			continue;
		}
		rayQueryConfirmIntersectionEXT(tie_query);
	}
	if (rayQueryGetIntersectionTypeEXT(tie_query, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
		r_hit.t = rayQueryGetIntersectionTEXT(tie_query, true);
		r_hit.instance = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(tie_query, true));
		r_hit.triangle = ray_scene_hw_triangle(r_hit.instance, uint(rayQueryGetIntersectionGeometryIndexEXT(tie_query, true)), uint(rayQueryGetIntersectionPrimitiveIndexEXT(tie_query, true)));
		r_hit.backface = !rayQueryGetIntersectionFrontFaceEXT(tie_query, true);
		r_hit.blend = true;
	}
	return true;
}
