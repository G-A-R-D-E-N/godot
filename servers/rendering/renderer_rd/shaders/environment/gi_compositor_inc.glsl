const uint GI_COMPOSITOR_PATTERN_CONSTANT = 0u;
const uint GI_COMPOSITOR_PATTERN_RAMP = 1u;
const uint GI_COMPOSITOR_PATTERN_CHECKER = 2u;
const uint GI_COMPOSITOR_CHECKER_CELL = 8u;

float gi_compositor_clamp_unit(float p_value) {
	return clamp(p_value, 0.0, 1.0);
}

vec4 gi_compositor_blend(vec4 p_lower, vec3 p_provider, float p_coverage) {
	if (!(p_coverage > 0.0)) {
		return p_lower;
	}
	float c = min(p_coverage, 1.0);
	float alpha = p_lower.a + c - p_lower.a * c;
	if (!(alpha > 0.0)) {
		return p_lower;
	}
	vec3 rgb = (p_lower.rgb * (p_lower.a * (1.0 - c)) + p_provider * c) / alpha;
	return vec4(rgb, alpha);
}

float gi_compositor_pattern_coverage(uint p_pattern, float p_coverage, uvec2 p_pos, uvec2 p_size) {
	if (p_pattern == GI_COMPOSITOR_PATTERN_RAMP) {
		return gi_compositor_clamp_unit(p_coverage) * float(p_pos.x) / float(max(p_size.x, 2u) - 1u);
	} else if (p_pattern == GI_COMPOSITOR_PATTERN_CHECKER) {
		uvec2 cell = p_pos / GI_COMPOSITOR_CHECKER_CELL;
		return ((cell.x + cell.y) & 1u) == 0u ? gi_compositor_clamp_unit(p_coverage) : 0.0;
	}
	return gi_compositor_clamp_unit(p_coverage);
}
