struct SurfelLookup {
	vec4 sh_r;
	vec4 sh_g;
	vec4 sh_b;
	float light_weight;
	float coverage;
	uint nearest;
	bool saturated;
};

SurfelLookup surfel_lookup(vec3 p_position, vec3 p_normal, uint p_skip) {
	SurfelLookup result = SurfelLookup(vec4(0.0), vec4(0.0), vec4(0.0), 0.0, 0.0, SURFEL_INVALID, false);

	uint level = surfel_level(length(p_position - surfel_data.camera_to_anchor));
	uint bucket = surfel_bucket(level, ivec3(floor(p_position / surfel_cell_size(level))));
	uint begin = min(surfel_offsets.data[bucket], surfel_data.cell_capacity);
	uint stored = min(surfel_offsets.data[bucket + 1u], surfel_data.cell_capacity);
	result.saturated = stored > begin + SURFEL_MAX_PER_LOOKUP;
	uint end = min(stored, begin + SURFEL_MAX_PER_LOOKUP);
	float best = 0.0;
	for (uint i = begin; i < end; i++) {
		uint index = surfel_cells.data[i];
		if (index == p_skip) {
			continue;
		}
		vec3 delta = p_position - surfels.data[index].position;
		float radius = surfels.data[index].radius;
		float reach = radius * SURFEL_REACH;
		float distance2 = dot(delta, delta);
		if (distance2 >= reach * reach) {
			continue;
		}
		vec3 surfel_normal = surfels.data[index].normal;
		float alignment = dot(p_normal, surfel_normal);
		if (alignment < 0.5 || abs(dot(delta, surfel_normal)) > radius * 0.5) {
			continue;
		}
		uint clip = surfels.data[index].clip;
		if (clip != SURFEL_NO_CLIP && surfel_clipped(delta, surfel_normal, reach, clip)) {
			continue;
		}
		float falloff = 1.0 - sqrt(distance2) / reach;
		float facing = (alignment - 0.5) * 2.0;
		result.coverage += falloff * facing;
		float weight = falloff * falloff * facing;
		if (weight > best) {
			best = weight;
			result.nearest = index;
		}
		weight *= min(1.0, float(surfels.data[index].samples) / SURFEL_CONFIDENT_SAMPLES);
		result.sh_r += surfels.data[index].sh_r * weight;
		result.sh_g += surfels.data[index].sh_g * weight;
		result.sh_b += surfels.data[index].sh_b * weight;
		result.light_weight += weight;
	}
	if (result.light_weight > 0.0) {
		float inv = 1.0 / result.light_weight;
		result.sh_r *= inv;
		result.sh_g *= inv;
		result.sh_b *= inv;
	}
	return result;
}

float surfel_density(vec3 p_position, vec3 p_normal, uint p_skip, float p_limit, out bool r_saturated) {
	uint level = surfel_level(length(p_position - surfel_data.camera_to_anchor));
	uint bucket = surfel_bucket(level, ivec3(floor(p_position / surfel_cell_size(level))));
	uint begin = min(surfel_offsets.data[bucket], surfel_data.cell_capacity);
	uint stored = min(surfel_offsets.data[bucket + 1u], surfel_data.cell_capacity);
	r_saturated = stored > begin + SURFEL_MAX_PER_LOOKUP;
	uint end = min(stored, begin + SURFEL_MAX_PER_LOOKUP);
	float coverage = 0.0;
	for (uint i = begin; i < end; i++) {
		uint index = surfel_cells.data[i];
		if (index == p_skip) {
			continue;
		}
		vec3 delta = p_position - surfels.data[index].position;
		float radius = surfels.data[index].radius;
		float reach = radius * SURFEL_REACH;
		float distance2 = dot(delta, delta);
		if (distance2 >= reach * reach) {
			continue;
		}
		vec3 surfel_normal = surfels.data[index].normal;
		float alignment = dot(p_normal, surfel_normal);
		if (alignment < 0.5 || abs(dot(delta, surfel_normal)) > radius * 0.5) {
			continue;
		}
		uint clip = surfels.data[index].clip;
		if (clip != SURFEL_NO_CLIP && surfel_clipped(delta, surfel_normal, reach, clip)) {
			continue;
		}
		coverage += (1.0 - sqrt(distance2) / reach) * ((alignment - 0.5) * 2.0);
		if (coverage > p_limit) {
			break;
		}
	}
	return coverage;
}

float surfel_blend(SurfelLookup p_lookup) {
	return clamp(p_lookup.light_weight / SURFEL_FULL_WEIGHT, 0.0, 1.0);
}
