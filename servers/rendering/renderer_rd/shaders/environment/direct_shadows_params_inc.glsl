struct DirectLamp {
	uint caster_mask;
	float size;
	uint reset_frame;
	uint dynamic;
	float clearance;
	uint exclude;
	float anchor_offset_x;
	float anchor_offset_y;
	float anchor_offset_z;
	float mean_r;
	float mean_g;
	float mean_b;
};

layout(set = 0, binding = 4, std140) uniform Params {
	mat4 inv_projection;
	mat4 prev_view_from_view;
	mat4 prev_projection;
	vec4 camera_basis[3];
	vec3 region_half;
	float bias;
	ivec2 size;
	vec2 pixel_size;
	uint cluster_shift;
	uint cluster_width;
	uint cluster_type_size;
	uint max_cluster_element_count_div_32;
	float z_far;
	uint frame;
	uint last_frame;
	uint entries;
	uint rays;
	uint reference;
	uint debug_view;
	uint history_valid;
	uint filter_on;
	uint force_one;
	float history_cap;
	float clamp_sigmas;
	float clamp_min_rays;
	float kernel_scale;
	float kernel_floor;
	float world_per_pixel;
	float range_scale;
	float dynamic_cap;
	uint count;
	uint head_entries;
	uint tail_visibility;
	uint cold_frames;
	ivec2 full_size;
	vec2 jitter_delta;
	uint pad0;
	uint pad1;
	vec4 anchor;
}
params;

#ifdef QUARTER
ivec2 direct_block_pixel(ivec2 p_block, uint p_phase) {
	return p_block * 2 + ivec2(int(p_phase & 1u), int(p_phase >> 1u));
}

float direct_guide_depth(uint p_word) {
	return uintBitsToFloat(p_word & ~3u);
}

vec3 direct_rebuild_position(ivec2 p_full_pixel, float p_depth) {
	vec2 uv = (vec2(p_full_pixel) + 0.5) * params.pixel_size * 2.0 - 1.0;
	vec2 at_unit = vec2(params.inv_projection[0][0], params.inv_projection[1][1]) * uv;
	return params.inv_projection[2][3] == 0.0 ? vec3(at_unit + params.inv_projection[3].xy, -p_depth) : vec3(at_unit * p_depth, -p_depth);
}

float direct_surface_weight(vec3 p_center_position, vec3 p_center_normal, float p_center_depth, vec3 p_other_position, vec3 p_other_normal) {
	float plane = abs(dot(p_center_normal, p_other_position - p_center_position));
	float facing = max(dot(p_center_normal, p_other_normal), 0.0);
	return exp(-plane / max(0.01 * p_center_depth, 1e-4)) * facing * facing;
}
#endif

vec2 direct_oct_encode(vec3 p_normal) {
	vec2 folded = p_normal.xy / (abs(p_normal.x) + abs(p_normal.y) + abs(p_normal.z));
	return p_normal.z >= 0.0 ? folded : (1.0 - abs(folded.yx)) * vec2(folded.x >= 0.0 ? 1.0 : -1.0, folded.y >= 0.0 ? 1.0 : -1.0);
}

vec3 direct_oct_decode(vec2 p_oct) {
	vec3 normal = vec3(p_oct, 1.0 - abs(p_oct.x) - abs(p_oct.y));
	float fold = max(-normal.z, 0.0);
	normal.x += normal.x >= 0.0 ? -fold : fold;
	normal.y += normal.y >= 0.0 ? -fold : fold;
	return normalize(normal);
}
