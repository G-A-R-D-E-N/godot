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
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

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
