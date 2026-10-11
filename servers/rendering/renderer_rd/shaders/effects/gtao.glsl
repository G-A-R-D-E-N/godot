#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#include "xegtao_inc.glsl"

layout(push_constant, std430) uniform Params {
	GTAOConstants consts;
	float slice_count;
	float steps_per_slice;
	ivec2 full_size;
	vec2 full_pixel_size;
	bool final_apply;
	uint pad;
}
params;

#ifdef MODE_PREFILTER

layout(set = 0, binding = 0) uniform sampler2D source_depth;
layout(rgba8, set = 0, binding = 1) uniform restrict readonly image2D source_normal;

layout(r32f, set = 1, binding = 0) uniform restrict writeonly image2D dest_depth0;
layout(r32f, set = 1, binding = 1) uniform restrict writeonly image2D dest_depth1;
layout(r32f, set = 1, binding = 2) uniform restrict writeonly image2D dest_depth2;
layout(r32f, set = 1, binding = 3) uniform restrict writeonly image2D dest_depth3;
layout(r32f, set = 1, binding = 4) uniform restrict writeonly image2D dest_depth4;
layout(r32ui, set = 1, binding = 5) uniform restrict writeonly uimage2D dest_normal;

shared float scratch_depths[8][8];

float prefilter_view_depth(ivec2 p_pixel) {
	return XeGTAO_ClampDepth(XeGTAO_ScreenSpaceToViewSpaceDepth(texelFetch(source_depth, p_pixel, 0).x, params.consts));
}

uint prefilter_normal(ivec2 p_pixel) {
	vec3 normal = normalize(imageLoad(source_normal, p_pixel).xyz * 2.0 - 1.0);
	return XeGTAO_FLOAT3_to_R11G11B10_UNORM(vec3(normal.xy, -normal.z) * 0.5 + 0.5);
}

float prefilter_texel(ivec2 p_texel, out uint r_normal) {
	ivec2 source_max = params.full_size - ivec2(1);
#ifdef HALF_SIZE
	ivec2 footprint[4];
	float footprint_depths[4];
	for (int i = 0; i < 4; i++) {
		footprint[i] = min(p_texel * 2 + ivec2(i % 2, i / 2), source_max);
		footprint_depths[i] = prefilter_view_depth(footprint[i]);
	}
	float filtered_depth = XeGTAO_DepthMIPFilter(footprint_depths[0], footprint_depths[1], footprint_depths[2], footprint_depths[3], params.consts);
	int closest = 0;
	for (int i = 1; i < 4; i++) {
		if (abs(footprint_depths[i] - filtered_depth) < abs(footprint_depths[closest] - filtered_depth)) {
			closest = i;
		}
	}
	r_normal = prefilter_normal(footprint[closest]);
	return filtered_depth;
#else
	ivec2 pixel = min(p_texel, source_max);
	r_normal = prefilter_normal(pixel);
	return prefilter_view_depth(pixel);
#endif
}

void store_mip0(ivec2 p_texel, float p_depth, uint p_normal) {
	if (all(lessThan(p_texel, params.consts.ViewportSize))) {
		imageStore(dest_depth0, p_texel, vec4(p_depth));
		imageStore(dest_normal, p_texel, uvec4(p_normal));
	}
}

bool inside_mip(ivec2 p_texel, int p_mip) {
	return all(lessThan(p_texel, max(params.consts.ViewportSize >> p_mip, ivec2(1))));
}

void main() {
	uvec2 base_coord = gl_GlobalInvocationID.xy;
	uvec2 group_thread = gl_LocalInvocationID.xy;
	ivec2 pix_coord = ivec2(base_coord) * 2;

	uint normal0, normal1, normal2, normal3;
	float depth0 = prefilter_texel(pix_coord + ivec2(0, 0), normal0);
	float depth1 = prefilter_texel(pix_coord + ivec2(1, 0), normal1);
	float depth2 = prefilter_texel(pix_coord + ivec2(0, 1), normal2);
	float depth3 = prefilter_texel(pix_coord + ivec2(1, 1), normal3);
	store_mip0(pix_coord + ivec2(0, 0), depth0, normal0);
	store_mip0(pix_coord + ivec2(1, 0), depth1, normal1);
	store_mip0(pix_coord + ivec2(0, 1), depth2, normal2);
	store_mip0(pix_coord + ivec2(1, 1), depth3, normal3);

	float dm1 = XeGTAO_DepthMIPFilter(depth0, depth1, depth2, depth3, params.consts);
	if (inside_mip(ivec2(base_coord), 1)) {
		imageStore(dest_depth1, ivec2(base_coord), vec4(dm1));
	}
	scratch_depths[group_thread.x][group_thread.y] = dm1;

	memoryBarrierShared();
	barrier();

	if (all(equal(group_thread % uvec2(2), uvec2(0)))) {
		float in_tl = scratch_depths[group_thread.x + 0][group_thread.y + 0];
		float in_tr = scratch_depths[group_thread.x + 1][group_thread.y + 0];
		float in_bl = scratch_depths[group_thread.x + 0][group_thread.y + 1];
		float in_br = scratch_depths[group_thread.x + 1][group_thread.y + 1];

		float dm2 = XeGTAO_DepthMIPFilter(in_tl, in_tr, in_bl, in_br, params.consts);
		if (inside_mip(ivec2(base_coord / 2u), 2)) {
			imageStore(dest_depth2, ivec2(base_coord / 2u), vec4(dm2));
		}
		scratch_depths[group_thread.x][group_thread.y] = dm2;
	}

	memoryBarrierShared();
	barrier();

	if (all(equal(group_thread % uvec2(4), uvec2(0)))) {
		float in_tl = scratch_depths[group_thread.x + 0][group_thread.y + 0];
		float in_tr = scratch_depths[group_thread.x + 2][group_thread.y + 0];
		float in_bl = scratch_depths[group_thread.x + 0][group_thread.y + 2];
		float in_br = scratch_depths[group_thread.x + 2][group_thread.y + 2];

		float dm3 = XeGTAO_DepthMIPFilter(in_tl, in_tr, in_bl, in_br, params.consts);
		if (inside_mip(ivec2(base_coord / 4u), 3)) {
			imageStore(dest_depth3, ivec2(base_coord / 4u), vec4(dm3));
		}
		scratch_depths[group_thread.x][group_thread.y] = dm3;
	}

	memoryBarrierShared();
	barrier();

	if (all(equal(group_thread % uvec2(8), uvec2(0)))) {
		float in_tl = scratch_depths[group_thread.x + 0][group_thread.y + 0];
		float in_tr = scratch_depths[group_thread.x + 4][group_thread.y + 0];
		float in_bl = scratch_depths[group_thread.x + 0][group_thread.y + 4];
		float in_br = scratch_depths[group_thread.x + 4][group_thread.y + 4];

		float dm4 = XeGTAO_DepthMIPFilter(in_tl, in_tr, in_bl, in_br, params.consts);
		if (inside_mip(ivec2(base_coord / 8u), 4)) {
			imageStore(dest_depth4, ivec2(base_coord / 8u), vec4(dm4));
		}
	}
}

#endif

#ifdef MODE_MAIN

layout(set = 0, binding = 0) uniform sampler2D source_working_depth;
layout(r32ui, set = 0, binding = 1) uniform restrict readonly uimage2D source_working_normal;

layout(r32ui, set = 1, binding = 0) uniform restrict writeonly uimage2D dest_ao_term;
layout(r8, set = 1, binding = 1) uniform restrict writeonly image2D dest_edges;

void main() {
	uvec2 pix_coord = gl_GlobalInvocationID.xy;
	if (any(greaterThanEqual(pix_coord, uvec2(params.consts.ViewportSize)))) {
		return;
	}

	vec3 viewspace_normal = normalize(XeGTAO_R11G11B10_UNORM_to_FLOAT3(imageLoad(source_working_normal, ivec2(pix_coord)).x) * 2.0 - 1.0);
	vec2 local_noise = XeGTAO_SpatioTemporalNoise(pix_coord, uint(params.consts.NoiseIndex));

	uint ao_term;
	float edges;
	XeGTAO_MainPass(pix_coord, params.slice_count, params.steps_per_slice, local_noise, viewspace_normal, params.consts, source_working_depth, ao_term, edges);

	imageStore(dest_ao_term, ivec2(pix_coord), uvec4(ao_term));
	imageStore(dest_edges, ivec2(pix_coord), vec4(edges));
}

#endif

#ifdef MODE_DENOISE

layout(set = 0, binding = 0) uniform usampler2D source_ao_term;
layout(set = 0, binding = 1) uniform sampler2D source_edges;

layout(r32ui, set = 1, binding = 0) uniform restrict writeonly uimage2D dest_ao_term;

void main() {
	uvec2 pix_coord_base = gl_GlobalInvocationID.xy * uvec2(2, 1);
	if (any(greaterThanEqual(pix_coord_base, uvec2(params.consts.ViewportSize)))) {
		return;
	}

	uint values[2];
	XeGTAO_Denoise(pix_coord_base, params.consts, source_ao_term, source_edges, params.final_apply, values);

	imageStore(dest_ao_term, ivec2(pix_coord_base), uvec4(values[0]));
	if (pix_coord_base.x + 1u < uint(params.consts.ViewportSize.x)) {
		imageStore(dest_ao_term, ivec2(pix_coord_base) + ivec2(1, 0), uvec4(values[1]));
	}
}

#endif

#ifdef MODE_RESOLVE

layout(set = 0, binding = 0) uniform usampler2D source_ao_term;
#ifdef HALF_SIZE
layout(set = 0, binding = 1) uniform sampler2D source_working_depth;
layout(r32ui, set = 0, binding = 2) uniform restrict readonly uimage2D source_working_normal;
layout(set = 0, binding = 3) uniform sampler2D source_depth;
layout(rgba8, set = 0, binding = 4) uniform restrict readonly image2D source_normal;
const float GTAO_UPSAMPLE_DEPTH_SIGMA = 0.02;
#endif

layout(r8, set = 1, binding = 0) uniform restrict writeonly image2D dest_ao;
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
layout(rgba8, set = 1, binding = 1) uniform restrict writeonly image2D dest_bent_normal;
#endif

AOTermType resolve_term(ivec2 p_texel) {
	uint packed_value = texelFetch(source_ao_term, p_texel, 0).x;
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	AOTermType term;
	XeGTAO_DecodeVisibilityBentNormal(packed_value, term.w, term.xyz);
	return term;
#else
	return float(packed_value) / 255.0;
#endif
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(pixel, params.full_size))) {
		return;
	}

#ifdef HALF_SIZE
	float reference_z = XeGTAO_ScreenSpaceToViewSpaceDepth(texelFetch(source_depth, pixel, 0).x, params.consts);
	vec3 reference_normal = normalize(imageLoad(source_normal, pixel).xyz * 2.0 - 1.0);
	reference_normal.z = -reference_normal.z;

	vec2 source_position = (vec2(pixel) + 0.5) * params.full_pixel_size * vec2(params.consts.ViewportSize) - 0.5;
	ivec2 base = ivec2(floor(source_position));
	vec2 fraction = source_position - vec2(base);
	ivec2 work_max = params.consts.ViewportSize - ivec2(1);

	AOTermType sum = AOTermType(0.0);
	float weight_sum = 0.0;
	for (int y = 0; y <= 1; y++) {
		for (int x = 0; x <= 1; x++) {
			ivec2 tap = clamp(base + ivec2(x, y), ivec2(0), work_max);
			float bilinear = (x == 0 ? 1.0 - fraction.x : fraction.x) * (y == 0 ? 1.0 - fraction.y : fraction.y);
			float tap_z = texelFetch(source_working_depth, tap, 0).x;
			vec3 tap_normal = normalize(XeGTAO_R11G11B10_UNORM_to_FLOAT3(imageLoad(source_working_normal, tap).x) * 2.0 - 1.0);
			float depth_weight = exp(-abs(tap_z - reference_z) / max(reference_z * GTAO_UPSAMPLE_DEPTH_SIGMA, 1e-6));
			float normal_weight = clamp(dot(tap_normal, reference_normal), 0.0, 1.0);
			float weight = bilinear * depth_weight * normal_weight * normal_weight;
			sum += resolve_term(tap) * weight;
			weight_sum += weight;
		}
	}

	AOTermType term = weight_sum > 1e-5 ? sum / weight_sum : resolve_term(clamp(ivec2(round(source_position)), ivec2(0), work_max));
#else
	AOTermType term = resolve_term(pixel);
#endif

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	imageStore(dest_ao, pixel, vec4(clamp(term.w, 0.0, 1.0)));
	vec3 bent_normal = normalize(term.xyz);
	imageStore(dest_bent_normal, pixel, vec4(vec3(bent_normal.xy, -bent_normal.z) * 0.5 + 0.5, 1.0));
#else
	imageStore(dest_ao, pixel, vec4(clamp(term, 0.0, 1.0)));
#endif
}

#endif
