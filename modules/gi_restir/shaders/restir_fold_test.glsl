#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

#include "restir_core_rtgi_inc.glsl"
#include "restir_fold_inc.glsl"

#define RS_FOLD_TEST_DRAW 0u
#define RS_FOLD_TEST_PHI 1u
#define RS_FOLD_TEST_PSI 2u
#define RS_FOLD_TEST_ISQRT 3u
#define RS_FOLD_TEST_GROUP 64u

layout(push_constant, std430) uniform FoldTestParams {
	uint sub;
	uint k;
	uint count;
	uint v0;
	uint grid_w;
	uint grid_h;
	uint stride;
	uint frames;
}
params;

layout(set = 0, binding = 0, std430) restrict buffer FoldTestOut {
	uint words[];
}
test_out;

void main() {
	uint t = gl_WorkGroupID.y * gl_NumWorkGroups.x * RS_FOLD_TEST_GROUP + gl_GlobalInvocationID.x;
	if (params.sub == RS_FOLD_TEST_DRAW) {
		if (t < params.count) {
			test_out.words[t] = rs_fold_draw(t, params.k);
		}
	} else if (params.sub == RS_FOLD_TEST_PHI) {
		if (t < params.grid_w * params.grid_h) {
			uvec2 pixel = uvec2(t % params.grid_w, t / params.grid_w);
			atomicAdd(test_out.words[rs_fold_phi(pixel) % params.k], 1u);
		}
	} else if (params.sub == RS_FOLD_TEST_PSI) {
		if (t < params.count * params.frames) {
			uint i = t / params.frames;
			uint v = params.v0 + t % params.frames;
			uint linear = i * params.stride;
			uvec2 pixel = uvec2(linear % params.grid_w, linear / params.grid_w);
			uint phi = rs_fold_phi(pixel) % params.k;
			uint psi_r = RS_FOLD_PSI_R(pixel, v) % params.k;
			uint s = rs_fold_draw(RS_FOLD_PSI_S(pixel, v), params.k);
			test_out.words[t] = phi | (psi_r << 8u) | (s << 16u);
		}
	} else if (params.sub == RS_FOLD_TEST_ISQRT) {
		if (t < params.count) {
			uint s = rs_fold_isqrt(t);
			if (!(s * s <= t && t < (s + 1u) * (s + 1u))) {
				atomicAdd(test_out.words[0], 1u);
				atomicMin(test_out.words[1], t);
			}
		}
	}
}
