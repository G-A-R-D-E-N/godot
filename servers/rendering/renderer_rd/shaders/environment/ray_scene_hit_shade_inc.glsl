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
	vec3 irradiance = gi_hit_direct_light(position, normal, bias, RAY_SCENE_SHADE_MAX_DISTANCE, p_seed);
	if (RAY_SCENE_SHADE_BOUNCE_ENERGY > 0.0) {
		vec3 bounce = vec3(0.0);
		float near_origin = 0.0;
#ifdef MODE_SURFEL_TRACE
		SurfelLookup lookup = surfel_lookup(position, normal, SURFEL_INVALID);
		float surfel_weight = surfel_blend(lookup);
		if (surfel_weight < 1.0) {
			float spacing = ddgi.levels[0].spacing;
			near_origin = (1.0 - smoothstep(spacing, spacing * 2.0, hit.t)) * (1.0 - smoothstep(spacing * 0.4, spacing, max(dot(position - origin_surfel_position, origin_surfel_normal), 0.0)));
			if (near_origin > 0.0) {
				bounce = near_origin * ddgi_sample_direction(origin_surfel_position - ray_scene.camera_to_anchor, origin_surfel_normal, origin_surfel_normal, normal, 0.0, DDGI_NO_VIEW_LIMIT).rgb;
			}
			if (near_origin < 1.0) {
				bounce += (1.0 - near_origin) * ddgi_sample_direction(position - ray_scene.camera_to_anchor, normal, -p_direction, normal, 0.0, hit.t * 0.5).rgb;
			}
			bounce = mix(bounce, surfel_eval_irradiance(lookup.sh_r, lookup.sh_g, lookup.sh_b, normal), surfel_weight);
		} else {
			bounce = surfel_eval_irradiance(lookup.sh_r, lookup.sh_g, lookup.sh_b, normal);
		}
#else
		if (near_origin < 1.0) {
			bounce += (1.0 - near_origin) * ddgi_sample_direction(position - ray_scene.camera_to_anchor, normal, -p_direction, normal, 0.0, hit.t * 0.5).rgb;
		}
#endif
		irradiance += bounce * RAY_SCENE_SHADE_BOUNCE_ENERGY;
	}
	vec3 radiance = material.emission * RAY_SCENE_SHADE_EMISSION_EXPOSURE + material.albedo * irradiance;
	return vec4(min(radiance, vec3(RAY_SCENE_SHADE_MAX_RADIANCE)), hit.t);
}

