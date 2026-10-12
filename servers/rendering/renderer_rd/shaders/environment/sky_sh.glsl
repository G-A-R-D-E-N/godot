#[compute]

#version 450

#VERSION_DEFINES

#define GROUP_SIZE 128
#define SH_COEFFS 9
#define CHANNELS 29
#define WEIGHT_CHANNEL 27
#define COUNT_CHANNEL 28
#define SUBDIV 10

#include "../oct_inc.glsl"

layout(local_size_x = GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D source_octmap;

layout(set = 1, binding = 0, std430) restrict writeonly buffer SkySH {
	vec4 coefficients[SH_COEFFS];
	uvec4 meta;
}
sh_out;

layout(push_constant, std430) uniform Params {
	float border_size;
	uint size;
	uint lod;
	uint generation_low;
	uint generation_high;
	uint pad0;
	uint pad1;
	uint pad2;
}
params;

shared float reduce_sums[CHANNELS][GROUP_SIZE];

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

	float inv_size = 1.0 / float(params.size);
	float oct_scale = 2.0 / params.border_size;
	float sample_area = (oct_scale * inv_size) * (oct_scale * inv_size) / float(SUBDIV * SUBDIV);
	uint total = params.size * params.size;

	for (uint i = tid; i < total; i += GROUP_SIZE) {
		uvec2 texel = uvec2(i % params.size, i / params.size);
		vec2 center_uv = (vec2(texel) + 0.5) * inv_size;
		vec2 center_e = (center_uv - 0.5) * oct_scale;
		if (abs(center_e.x) >= 1.0 || abs(center_e.y) >= 1.0) {
			continue;
		}

		float basis_weight[SH_COEFFS];
		for (int l = 0; l < SH_COEFFS; l++) {
			basis_weight[l] = 0.0;
		}
		float texel_weight = 0.0;

		for (int sy = 0; sy < SUBDIV; sy++) {
			for (int sx = 0; sx < SUBDIV; sx++) {
				vec2 offset = (vec2(sx, sy) + 0.5) / float(SUBDIV) - 0.5;
				vec2 uv = (vec2(texel) + 0.5 + offset) * inv_size;
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
			acc[l * 3 + 0] += term.r;
			acc[l * 3 + 1] += term.g;
			acc[l * 3 + 2] += term.b;
		}
		acc[WEIGHT_CHANNEL] += texel_weight;
		acc[COUNT_CHANNEL] += 1.0;
	}

	for (int k = 0; k < CHANNELS; k++) {
		reduce_sums[k][tid] = acc[k];
	}
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

	if (tid == 0) {
		float weight_sum = reduce_sums[WEIGHT_CHANNEL][0];
		float norm = weight_sum > 0.0 ? (4.0 * 3.14159265358979) / weight_sum : 0.0;
		for (int l = 0; l < SH_COEFFS; l++) {
			sh_out.coefficients[l] = vec4(reduce_sums[l * 3 + 0][0], reduce_sums[l * 3 + 1][0], reduce_sums[l * 3 + 2][0], 0.0) * norm;
		}
		sh_out.meta = uvec4(params.generation_low, params.generation_high, weight_sum > 0.0 ? 1u : 0u, uint(reduce_sums[COUNT_CHANNEL][0]));
	}
}
