float far_shadow_below(float p_angle, float p_center, float p_radius) {
	if (p_radius <= 0.0) {
		return p_angle > p_center ? 1.0 : 0.0;
	}
	float x = clamp((p_angle - p_center) / p_radius, -1.0, 1.0);
	return 0.5 + (x * sqrt(1.0 - x * x) + asin(x)) / M_PI;
}

float far_shadow_horizon(float p_height, float p_distance, float p_tan_elevation, float p_z) {
	float d = max(p_distance, 0.001);
	return atan((p_height + d * p_tan_elevation - p_z) / d);
}

float far_shadow_tap(uint p_slot, vec3 p_uvw, float p_world_y, float p_tan_elevation, float p_elevation, float p_radius, bool p_near) {
	float terrain = textureLod(sampler2DArray(far_shadow_terrain[p_slot], SAMPLER_LINEAR_CLAMP), p_uvw, 0.0).r;
	float z = max(p_world_y - terrain, 0.0);
	if (p_near) {
		vec2 near = textureLod(sampler2DArray(far_shadow_near[p_slot], SAMPLER_LINEAR_CLAMP), p_uvw, 0.0).rg;
		return 1.0 - far_shadow_below(far_shadow_horizon(near.r, near.g, p_tan_elevation, z), p_elevation, p_radius);
	}
	vec4 heights = textureLod(sampler2DArray(far_shadow_heights[p_slot], SAMPLER_LINEAR_CLAMP), p_uvw, 0.0);
	float solid = far_shadow_horizon(heights.r, heights.g, p_tan_elevation, z);
	float blocked = far_shadow_below(solid, p_elevation, p_radius);
	if (heights.a > heights.b) {
		float band_distance = textureLod(sampler2DArray(far_shadow_bands[p_slot], SAMPLER_LINEAR_CLAMP), p_uvw, 0.0).r;
		float low = far_shadow_horizon(heights.b, band_distance, p_tan_elevation, z);
		float high = far_shadow_horizon(heights.a, band_distance, p_tan_elevation, z);
		blocked += max(0.0, far_shadow_below(high, p_elevation, p_radius) - far_shadow_below(max(low, solid), p_elevation, p_radius));
	}
	return 1.0 - min(blocked, 1.0);
}

float far_shadow_level(uint p_slot, uint p_level, vec2 p_offset, float p_world_y, vec2 p_across, bool p_near) {
	FarShadowSlot slot = far_shadow.slots[p_slot];
	vec4 level = slot.levels[p_level];
	vec3 uvw = vec3((p_offset - level.xy) * level.z + 0.5, float(p_level));
	float tan_elevation = slot.direction.w;
	float elevation = atan(tan_elevation);
	float radius = atan(slot.tan_radius);
	if (slot.tan_radius <= 0.0) {
		return far_shadow_tap(p_slot, uvw, p_world_y, tan_elevation, elevation, radius, p_near);
	}
	float caster_distance = p_near ? textureLod(sampler2DArray(far_shadow_near[p_slot], SAMPLER_LINEAR_CLAMP), uvw, 0.0).g
								   : textureLod(sampler2DArray(far_shadow_heights[p_slot], SAMPLER_LINEAR_CLAMP), uvw, 0.0).g;
	float spread = max(caster_distance, 0.0) * slot.tan_radius * level.z;
	float jitter = (fract(scene_data_block.data.taa_frame_count * 0.618034 + 0.5) - 0.5) * 0.5;
	float visible = 0.0;
	float weights = 0.0;
	for (uint k = 0; k < 4; k++) {
		float u = -0.75 + 0.5 * float(k) + jitter;
		float w = sqrt(max(1.0 - u * u, 0.0));
		vec3 tap = vec3(uvw.xy + p_across * (u * spread), uvw.z);
		visible += w * far_shadow_tap(p_slot, tap, p_world_y, tan_elevation, elevation, radius, p_near);
		weights += w;
	}
	return visible / max(weights, 0.0001);
}

float far_shadow_visibility(uint p_slot, vec3 p_offset, float p_world_y, vec3 p_normal, bool p_near) {
	FarShadowSlot slot = far_shadow.slots[p_slot];
	vec2 flat_normal = p_normal.xz;
	vec2 sun = slot.direction.xz;
	vec2 across = dot(sun, sun) > 0.0 ? normalize(vec2(-sun.y, sun.x)) : vec2(1.0, 0.0);
	uint levels = p_near ? slot.near_level_count : slot.level_count;
	for (uint k = 0; k < levels; k++) {
		vec4 level = slot.levels[k];
		vec2 lookup = p_offset.xz + flat_normal * level.w;
		vec2 edge = abs((lookup - level.xy) * level.z) * 2.0;
		float outer = max(edge.x, edge.y);
		if (outer >= 1.0) {
			continue;
		}
		float visible = far_shadow_level(p_slot, k, lookup, p_world_y, across, p_near);
		float blend = clamp((outer - 0.875) / 0.125, 0.0, 1.0);
		if (blend > 0.0 && k + 1 < levels) {
			vec4 next = slot.levels[k + 1];
			vec2 next_lookup = p_offset.xz + flat_normal * next.w;
			visible = mix(visible, far_shadow_level(p_slot, k + 1, next_lookup, p_world_y, across, p_near), blend);
		}
		return visible;
	}
	return 1.0;
}

float far_shadow_mix(uint p_light, float p_shadow, float p_far, float p_fade, vec3 p_vertex, vec3 p_normal) {
	int slot = sc_use_far_shadow() ? far_shadow.slot_of_light[p_light >> 2][p_light & 3] : -1;
	if (slot < 0) {
		return mix(p_shadow, p_far, p_fade);
	}
	mat3 inv_view = transpose(mat3(scene_data_block.data.inv_view_matrix[0].xyz,
			scene_data_block.data.inv_view_matrix[1].xyz,
			scene_data_block.data.inv_view_matrix[2].xyz));
	vec3 offset = inv_view * p_vertex;
	float world_y = offset.y + far_shadow.slots[slot].eye_y;
	vec3 normal = inv_view * p_normal;
	bool probe = bool(scene_data_block.data.flags & SCENE_DATA_FLAGS_IN_REFLECTION_PROBE);
	float all_casters = (p_fade > 0.0 || probe) ? far_shadow_visibility(uint(slot), offset, world_y, normal, false) : 1.0;
	float far_casters = probe ? all_casters : (p_fade < 1.0 ? far_shadow_visibility(uint(slot), offset, world_y, normal, true) : 1.0);
	return mix(min(p_shadow, far_casters), min(p_far, all_casters), p_fade);
}
