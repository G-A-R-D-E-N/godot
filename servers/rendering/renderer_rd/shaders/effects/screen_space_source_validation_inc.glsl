#define SOURCE_DEPTH_RELATIVE_TOLERANCE 0.03
#define SOURCE_DEPTH_MIN_TOLERANCE 0.02

bool source_depth_matches(vec4 p_previous, float p_expected) {
	return any(lessThanEqual(abs(p_previous - vec4(p_expected)), vec4(max(p_expected * SOURCE_DEPTH_RELATIVE_TOLERANCE, SOURCE_DEPTH_MIN_TOLERANCE))));
}

bool source_sample_valid(sampler2D p_previous_depth, vec4 p_reprojected, float p_view_depth, vec2 p_cell_count) {
	vec2 uv = p_reprojected.xy / p_reprojected.w * 0.5 + 0.5;
	if (p_reprojected.w <= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return false;
	}
	float expected = p_reprojected.w * p_view_depth;
	vec2 base = floor(uv * p_cell_count - 0.5) + 0.5;
	for (int i = 0; i < 4; i++) {
		if (source_depth_matches(textureGather(p_previous_depth, (base + vec2(i & 1, i >> 1)) / p_cell_count, 3), expected)) {
			return true;
		}
	}
	return false;
}
