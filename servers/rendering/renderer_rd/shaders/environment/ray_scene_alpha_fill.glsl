#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_texture;
layout(r8, set = 0, binding = 1) uniform restrict writeonly image2DArray alpha_layers;

layout(push_constant, std430) uniform Params {
	uint layer;
	uint layer_size;
	uint source_width;
	uint source_height;
	uint cutoff;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

void main() {
	uvec2 id = gl_GlobalInvocationID.xy;
	if (id.x >= params.layer_size || id.y >= params.layer_size) {
		return;
	}
	float value = 0.0;
	if (params.source_width >= params.layer_size && params.source_height >= params.layer_size) {
		uint x0 = (id.x * params.source_width) / params.layer_size;
		uint x1 = ((id.x + 1u) * params.source_width) / params.layer_size;
		uint y0 = (id.y * params.source_height) / params.layer_size;
		uint y1 = ((id.y + 1u) * params.source_height) / params.layer_size;
		uint count = 0u;
		for (uint y = y0; y < y1; y++) {
			for (uint x = x0; x < x1; x++) {
				float alpha = texelFetch(source_texture, ivec2(x, y), 0).a;
				count += uint(round(alpha * 255.0)) >= params.cutoff ? 1u : 0u;
			}
		}
		value = float(count) / float((x1 - x0) * (y1 - y0));
	} else {
		vec2 uv = (vec2(id) + vec2(0.5)) / float(params.layer_size);
		float alpha = textureLod(source_texture, uv, 0.0).a;
		value = (alpha * 255.0 >= float(params.cutoff)) ? 1.0 : 0.0;
	}
	imageStore(alpha_layers, ivec3(id, params.layer), vec4(value, 0.0, 0.0, 1.0));
}
