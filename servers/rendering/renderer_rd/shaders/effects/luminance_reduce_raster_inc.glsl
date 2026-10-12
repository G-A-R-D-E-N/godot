layout(push_constant, std430) uniform PushConstant {
	ivec2 source_size;

	float exposure_adjust;
	float min_luminance;
	float max_luminance;
	float block_scale;
	float highlight_limit;
	float pad;
}
settings;
