#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(push_constant, std430) uniform Params {
	uint layer;
	uint layer_size;
	uint source_width;
	uint source_height;
	uint cutoff;
	uint invert;
	uint pad1;
	uint pad2;
}
params;

#ifdef MODE_MEAN_MIP
layout(r8, set = 0, binding = 0) uniform restrict readonly image2DArray source_mip;
layout(r8, set = 0, binding = 1) uniform restrict writeonly image2DArray dest_mip;

void main() {
	uvec2 id = gl_GlobalInvocationID.xy;
	if (id.x >= params.layer_size || id.y >= params.layer_size) {
		return;
	}
	ivec3 base = ivec3(id * 2u, params.layer);
	float sum = imageLoad(source_mip, base).r +
			imageLoad(source_mip, base + ivec3(1, 0, 0)).r +
			imageLoad(source_mip, base + ivec3(0, 1, 0)).r +
			imageLoad(source_mip, base + ivec3(1, 1, 0)).r;
	imageStore(dest_mip, ivec3(id, params.layer), vec4(sum * 0.25, 0.0, 0.0, 1.0));
}
#else
layout(set = 0, binding = 0) uniform sampler2D source_texture;
layout(r8, set = 0, binding = 1) uniform restrict writeonly image2DArray alpha_layers;
layout(r8, set = 0, binding = 2) uniform restrict writeonly image2DArray mean_layers;

shared float alpha_sums[8][8];
shared uint sample_counts[8][8];

void main() {
	uvec2 id = gl_GlobalInvocationID.xy;
	float alpha_sum = 0.0;
	uint samples = 1u;
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
				alpha_sum += alpha;
				bool pass = params.invert != 0u ? uint(round(alpha * 255.0)) <= params.cutoff : uint(round(alpha * 255.0)) >= params.cutoff;
				count += pass ? 1u : 0u;
			}
		}
		samples = (x1 - x0) * (y1 - y0);
		value = float(count) / float(samples);
	} else {
		vec2 uv = (vec2(id) + vec2(0.5)) / float(params.layer_size);
		float alpha = textureLod(source_texture, uv, 0.0).a;
		alpha_sum = alpha;
		bool pass = params.invert != 0u ? alpha * 255.0 <= float(params.cutoff) : alpha * 255.0 >= float(params.cutoff);
		value = pass ? 1.0 : 0.0;
	}
	imageStore(alpha_layers, ivec3(id, params.layer), vec4(value, 0.0, 0.0, 1.0));

	uvec2 local = gl_LocalInvocationID.xy;
	alpha_sums[local.y][local.x] = alpha_sum;
	sample_counts[local.y][local.x] = samples;
	memoryBarrierShared();
	barrier();

	uint block_size = params.layer_size / uint(imageSize(mean_layers).x);
	if ((local.x % block_size) == 0u && (local.y % block_size) == 0u) {
		float block_sum = 0.0;
		uint block_count = 0u;
		for (uint y = 0u; y < block_size; y++) {
			for (uint x = 0u; x < block_size; x++) {
				block_sum += alpha_sums[local.y + y][local.x + x];
				block_count += sample_counts[local.y + y][local.x + x];
			}
		}
		imageStore(mean_layers, ivec3(id / block_size, params.layer), vec4(block_sum / float(block_count), 0.0, 0.0, 1.0));
	}
}
#endif
