layout(push_constant, std430) uniform PushConstant {
	ivec2 source_size;

	float exposure_adjust;
	float min_luminance;
	float max_luminance;
	float block_scale;
	float highlight_limit;
	float adaptation;
	float adaptation_pivot;
	uint final_level;
	float light_limit;
	uint hold;
}
settings;
