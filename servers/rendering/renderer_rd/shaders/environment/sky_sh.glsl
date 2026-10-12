#[compute]

#version 450

#VERSION_DEFINES

#define GROUP_SIZE 128
#define TILE_WIDTH 16
#define SH_COEFFS 9
#define CHANNELS 29
#define WEIGHT_CHANNEL 27
#define COUNT_CHANNEL 28
#define PARTIAL_STRIDE 32
#define SUBDIV 2

#include "../oct_inc.glsl"

layout(local_size_x = GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

#ifdef SH_REDUCE

layout(set = 0, binding = 0, std430) restrict readonly buffer SkySHPartials {
	float values[];
}
partials;

layout(set = 1, binding = 0, std430) restrict writeonly buffer SkySH {
	vec4 coefficients[SH_COEFFS];
	uvec4 meta;
}
sh_out;

#else

layout(set = 0, binding = 0) uniform sampler2D source_octmap;

layout(set = 1, binding = 0, std430) restrict writeonly buffer SkySHPartials {
	float values[];
}
partials;

#endif

layout(push_constant, std430) uniform Params {
	float border_size;
	uint size;
	uint lod;
	uint origin;
	uint extent;
	uint group_count;
	uint generation_low;
	uint generation_high;
}
params;

shared float reduce_sums[CHANNELS][GROUP_SIZE];

void reduce_group(uint tid) {
	memoryBarrierShared();
	barrier();

	for (uint s = GROUP_SIZE / 2; s > 0; s >>= 1) {
		if (tid < s) {
			for (int k = 0; k < CHANNELS; k++) {
				reduce_sums[k][tid] += reduce_sums[k][tid + s];
			}
		}
		memoryBarrierShared();
		barrier();
	}
}

#ifndef SH_REDUCE

void sh_basis(vec3 d, out float y[SH_COEFFS]) {
	y[0] = 0.282095;
	y[1] = 0.488603 * d.y;
	y[2] = 0.488603 * d.z;
	y[3] = 0.488603 * d.x;
	y[4] = 1.092548 * d.x * d.y;
	y[5] = 1.092548 * d.y * d.z;
	y[6] = 0.315392 * (3.0 * d.z * d.z - 1.0);
	y[7] = 1.092548 * d.x * d.z;
	y[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

void main() {
	uint tid = gl_LocalInvocationIndex;

	float acc[CHANNELS];
	for (int k = 0; k < CHANNELS; k++) {
		acc[k] = 0.0;
	}

	uvec2 local = uvec2(tid % TILE_WIDTH, tid / TILE_WIDTH);
	uvec2 offset = gl_WorkGroupID.xy * uvec2(TILE_WIDTH, GROUP_SIZE / TILE_WIDTH) + local;
	uvec2 texel = uvec2(params.origin) + offset;

	float inv_size = 1.0 / float(params.size);
	float oct_scale = 2.0 / params.border_size;
	vec2 center_e = ((vec2(texel) + 0.5) * inv_size - 0.5) * oct_scale;

	if (offset.x < params.extent && offset.y < params.extent && abs(center_e.x) < 1.0 && abs(center_e.y) < 1.0) {
		float sample_area = (oct_scale * inv_size) * (oct_scale * inv_size) / float(SUBDIV * SUBDIV);

		float basis_weight[SH_COEFFS];
		for (int l = 0; l < SH_COEFFS; l++) {
			basis_weight[l] = 0.0;
		}
		float texel_weight = 0.0;

		for (int sy = 0; sy < SUBDIV; sy++) {
			for (int sx = 0; sx < SUBDIV; sx++) {
				vec2 sub_offset = (vec2(sx, sy) + 0.5) / float(SUBDIV) - 0.5;
				vec2 uv = (vec2(texel) + 0.5 + sub_offset) * inv_size;
				vec2 e = (uv - 0.5) * oct_scale;
				vec3 d = oct_to_vec3_with_border(uv, params.border_size);

				float pz = 1.0 - abs(e.x) - abs(e.y);
				vec2 q = pz < 0.0 ? sign(e) * (1.0 - abs(e.yx)) : e;
				float r2 = dot(q, q) + pz * pz;
				float w = sample_area / (r2 * sqrt(r2));

				float y[SH_COEFFS];
				sh_basis(d, y);
				for (int l = 0; l < SH_COEFFS; l++) {
					basis_weight[l] += w * y[l];
				}
				texel_weight += w;
			}
		}

		vec3 c = texelFetch(source_octmap, ivec2(texel), int(params.lod)).rgb;
		if (any(isnan(c)) || any(isinf(c))) {
			c = vec3(0.0);
		}

		for (int l = 0; l < SH_COEFFS; l++) {
			vec3 term = c * basis_weight[l];
			acc[l * 3 + 0] = term.r;
			acc[l * 3 + 1] = term.g;
			acc[l * 3 + 2] = term.b;
		}
		acc[WEIGHT_CHANNEL] = texel_weight;
		acc[COUNT_CHANNEL] = 1.0;
	}

	for (int k = 0; k < CHANNELS; k++) {
		reduce_sums[k][tid] = acc[k];
	}
	reduce_group(tid);

	if (tid == 0) {
		uint groups_x = (params.extent + TILE_WIDTH - 1) / TILE_WIDTH;
		uint group = gl_WorkGroupID.y * groups_x + gl_WorkGroupID.x;
		for (int k = 0; k < CHANNELS; k++) {
			partials.values[group * PARTIAL_STRIDE + uint(k)] = reduce_sums[k][0];
		}
	}
}

#else

void main() {
	uint tid = gl_LocalInvocationIndex;

	float acc[CHANNELS];
	for (int k = 0; k < CHANNELS; k++) {
		acc[k] = 0.0;
	}

	for (uint g = tid; g < params.group_count; g += GROUP_SIZE) {
		for (int k = 0; k < CHANNELS; k++) {
			acc[k] += partials.values[g * PARTIAL_STRIDE + uint(k)];
		}
	}

	for (int k = 0; k < CHANNELS; k++) {
		reduce_sums[k][tid] = acc[k];
	}
	reduce_group(tid);

	if (tid == 0) {
		float weight_sum = reduce_sums[WEIGHT_CHANNEL][0];
		float norm = weight_sum > 0.0 ? (4.0 * 3.14159265358979) / weight_sum : 0.0;
		for (int l = 0; l < SH_COEFFS; l++) {
			sh_out.coefficients[l] = vec4(reduce_sums[l * 3 + 0][0], reduce_sums[l * 3 + 1][0], reduce_sums[l * 3 + 2][0], 0.0) * norm;
		}
		sh_out.meta = uvec4(params.generation_low, params.generation_high, weight_sum > 0.0 ? 1u : 0u, uint(reduce_sums[COUNT_CHANNEL][0]));
	}
}

#endif
