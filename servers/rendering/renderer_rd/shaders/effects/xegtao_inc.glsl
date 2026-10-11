///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2016-2021, Intel Corporation
//
// SPDX-License-Identifier: MIT
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// XeGTAO is based on GTAO/GTSO "Jimenez et al. / Practical Real-Time Strategies for Accurate Indirect Occlusion",
// https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf
//
// Implementation:  Filip Strugar (filip.strugar@intel.com), Steve Mccalla <stephen.mccalla@intel.com>
// Version:         1.30
// Details:         https://github.com/GameTechDev/XeGTAO
//
// File changes (yyyy-mm-dd)
// 2026-10-03: GLSL port for Godot RenderingDevice: fp32 depths, dynamic constants, outputs returned to the caller.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define XE_GTAO_PI (3.1415926535897932384626433832795)
#define XE_GTAO_PI_HALF (1.5707963267948966192313216916398)
#define XE_GTAO_DEPTH_MIP_LEVELS 5
#define XE_GTAO_OCCLUSION_TERM_SCALE (1.5)
#define XE_HILBERT_LEVEL 6u
#define XE_HILBERT_WIDTH (1u << XE_HILBERT_LEVEL)

struct GTAOConstants {
	ivec2 ViewportSize;
	vec2 ViewportPixelSize;

	vec2 DepthUnpackConsts;
	vec2 CameraTanHalfFOV;

	vec2 NDCToViewMul;
	vec2 NDCToViewAdd;

	vec2 NDCToViewMul_x_PixelSize;
	float EffectRadius;
	float EffectFalloffRange;

	float RadiusMultiplier;
	float Padding0;
	float FinalValuePower;
	float DenoiseBlurBeta;

	float SampleDistributionPower;
	float ThinOccluderCompensation;
	float DepthMIPSamplingOffset;
	int NoiseIndex;
};

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
#define AOTermType vec4
#else
#define AOTermType float
#endif

vec3 XeGTAO_R11G11B10_UNORM_to_FLOAT3(uint packedInput) {
	vec3 unpackedOutput;
	unpackedOutput.x = float((packedInput) & 0x000007ffu) / 2047.0;
	unpackedOutput.y = float((packedInput >> 11) & 0x000007ffu) / 2047.0;
	unpackedOutput.z = float((packedInput >> 22) & 0x000003ffu) / 1023.0;
	return unpackedOutput;
}

uint XeGTAO_FLOAT3_to_R11G11B10_UNORM(vec3 unpackedInput) {
	uint packedOutput;
	packedOutput = ((uint(clamp(unpackedInput.x, 0.0, 1.0) * 2047.0 + 0.5)) |
			(uint(clamp(unpackedInput.y, 0.0, 1.0) * 2047.0 + 0.5) << 11) |
			(uint(clamp(unpackedInput.z, 0.0, 1.0) * 1023.0 + 0.5) << 22));
	return packedOutput;
}

vec4 XeGTAO_R8G8B8A8_UNORM_to_FLOAT4(uint packedInput) {
	vec4 unpackedOutput;
	unpackedOutput.x = float(packedInput & 0x000000ffu) / 255.0;
	unpackedOutput.y = float(((packedInput >> 8) & 0x000000ffu)) / 255.0;
	unpackedOutput.z = float(((packedInput >> 16) & 0x000000ffu)) / 255.0;
	unpackedOutput.w = float(packedInput >> 24) / 255.0;
	return unpackedOutput;
}

uint XeGTAO_FLOAT4_to_R8G8B8A8_UNORM(vec4 unpackedInput) {
	return ((uint(clamp(unpackedInput.x, 0.0, 1.0) * 255.0 + 0.5)) |
			(uint(clamp(unpackedInput.y, 0.0, 1.0) * 255.0 + 0.5) << 8) |
			(uint(clamp(unpackedInput.z, 0.0, 1.0) * 255.0 + 0.5) << 16) |
			(uint(clamp(unpackedInput.w, 0.0, 1.0) * 255.0 + 0.5) << 24));
}

vec3 XeGTAO_ComputeViewspacePosition(const vec2 screenPos, const float viewspaceDepth, const GTAOConstants consts) {
	vec3 ret;
	ret.xy = (consts.NDCToViewMul * screenPos.xy + consts.NDCToViewAdd) * viewspaceDepth;
	ret.z = viewspaceDepth;
	return ret;
}

float XeGTAO_ScreenSpaceToViewSpaceDepth(const float screenDepth, const GTAOConstants consts) {
	float depthLinearizeMul = consts.DepthUnpackConsts.x;
	float depthLinearizeAdd = consts.DepthUnpackConsts.y;
	return depthLinearizeMul / (depthLinearizeAdd - screenDepth);
}

vec4 XeGTAO_CalculateEdges(const float centerZ, const float leftZ, const float rightZ, const float topZ, const float bottomZ) {
	vec4 edgesLRTB = vec4(leftZ, rightZ, topZ, bottomZ) - centerZ;

	float slopeLR = (edgesLRTB.y - edgesLRTB.x) * 0.5;
	float slopeTB = (edgesLRTB.w - edgesLRTB.z) * 0.5;
	vec4 edgesLRTBSlopeAdjusted = edgesLRTB + vec4(slopeLR, -slopeLR, slopeTB, -slopeTB);
	edgesLRTB = min(abs(edgesLRTB), abs(edgesLRTBSlopeAdjusted));
	return vec4(clamp((1.25 - edgesLRTB / (centerZ * 0.011)), 0.0, 1.0));
}

float XeGTAO_PackEdges(vec4 edgesLRTB) {
	edgesLRTB = roundEven(clamp(edgesLRTB, 0.0, 1.0) * 2.9);
	return dot(edgesLRTB, vec4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

float XeGTAO_FastSqrt(float x) {
	return intBitsToFloat(0x1fbd1df5 + (floatBitsToInt(x) >> 1));
}

float XeGTAO_FastACos(float inX) {
	const float PI = 3.141593;
	const float HALF_PI = 1.570796;
	float x = abs(inX);
	float res = -0.156583 * x + HALF_PI;
	res *= XeGTAO_FastSqrt(1.0 - x);
	return (inX >= 0.0) ? res : PI - res;
}

uint XeGTAO_EncodeVisibilityBentNormal(float visibility, vec3 bentNormal) {
	return XeGTAO_FLOAT4_to_R8G8B8A8_UNORM(vec4(bentNormal * 0.5 + 0.5, visibility));
}

void XeGTAO_DecodeVisibilityBentNormal(const uint packedValue, out float visibility, out vec3 bentNormal) {
	vec4 decoded = XeGTAO_R8G8B8A8_UNORM_to_FLOAT4(packedValue);
	bentNormal = decoded.xyz * vec3(2.0) - vec3(1.0);
	visibility = decoded.w;
}

uint XeGTAO_OutputWorkingTerm(float visibility, vec3 bentNormal) {
	visibility = clamp(visibility / float(XE_GTAO_OCCLUSION_TERM_SCALE), 0.0, 1.0);
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	return XeGTAO_EncodeVisibilityBentNormal(visibility, bentNormal);
#else
	return uint(visibility * 255.0 + 0.5);
#endif
}

mat3 XeGTAO_RotFromToMatrix(vec3 from, vec3 to) {
	const float e = dot(from, to);
	const float f = abs(e);

	if (f > float(1.0 - 0.0003)) {
		return mat3(1, 0, 0, 0, 1, 0, 0, 0, 1);
	}

	const vec3 v = cross(from, to);
	const float h = (1.0) / (1.0 + e);
	const float hvx = h * v.x;
	const float hvz = h * v.z;
	const float hvxy = hvx * v.y;
	const float hvxz = hvx * v.z;
	const float hvyz = hvz * v.y;

	mat3 mtx;
	mtx[0][0] = e + hvx * v.x;
	mtx[0][1] = hvxy - v.z;
	mtx[0][2] = hvxz + v.y;

	mtx[1][0] = hvxy + v.z;
	mtx[1][1] = e + h * v.y * v.y;
	mtx[1][2] = hvyz - v.x;

	mtx[2][0] = hvxz - v.y;
	mtx[2][1] = hvyz + v.x;
	mtx[2][2] = e + hvz * v.z;

	return mtx;
}

void XeGTAO_MainPass(const uvec2 pixCoord, float sliceCount, float stepsPerSlice, const vec2 localNoise, vec3 viewspaceNormal, const GTAOConstants consts,
		sampler2D sourceViewspaceDepth, out uint outWorkingAOTerm, out float outWorkingEdges) {
	vec2 normalizedScreenPos = (vec2(pixCoord) + vec2(0.5)) * consts.ViewportPixelSize;

	vec4 valuesUL = textureGather(sourceViewspaceDepth, vec2(pixCoord) * consts.ViewportPixelSize, 0);
	vec4 valuesBR = textureGatherOffset(sourceViewspaceDepth, vec2(pixCoord) * consts.ViewportPixelSize, ivec2(1, 1), 0);

	float viewspaceZ = valuesUL.y;

	const float pixLZ = valuesUL.x;
	const float pixTZ = valuesUL.z;
	const float pixRZ = valuesBR.z;
	const float pixBZ = valuesBR.x;

	vec4 edgesLRTB = XeGTAO_CalculateEdges(viewspaceZ, pixLZ, pixRZ, pixTZ, pixBZ);
	outWorkingEdges = XeGTAO_PackEdges(edgesLRTB);

	viewspaceZ *= 0.99999;

	const vec3 pixCenterPos = XeGTAO_ComputeViewspacePosition(normalizedScreenPos, viewspaceZ, consts);
	const vec3 viewVec = normalize(-pixCenterPos);

	const float effectRadius = consts.EffectRadius * consts.RadiusMultiplier;
	const float sampleDistributionPower = consts.SampleDistributionPower;
	const float thinOccluderCompensation = consts.ThinOccluderCompensation;
	const float falloffRange = consts.EffectFalloffRange * effectRadius;

	const float falloffFrom = effectRadius * (1.0 - consts.EffectFalloffRange);

	const float falloffMul = -1.0 / (falloffRange);
	const float falloffAdd = falloffFrom / (falloffRange) + 1.0;

	float visibility = 0.0;
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	vec3 bentNormal = vec3(0.0);
#else
	vec3 bentNormal = viewspaceNormal;
#endif

	{
		const float noiseSlice = localNoise.x;
		const float noiseSample = localNoise.y;

		const float pixelTooCloseThreshold = 1.3;

		const vec2 pixelDirRBViewspaceSizeAtCenterZ = viewspaceZ * consts.NDCToViewMul_x_PixelSize;

		float screenspaceRadius = effectRadius / pixelDirRBViewspaceSizeAtCenterZ.x;

		visibility += clamp((10.0 - screenspaceRadius) / 100.0, 0.0, 1.0) * 0.5;

		const float minS = pixelTooCloseThreshold / screenspaceRadius;

		for (float slice = 0.0; slice < sliceCount; slice++) {
			float sliceK = (slice + noiseSlice) / sliceCount;
			float phi = sliceK * XE_GTAO_PI;
			float cosPhi = cos(phi);
			float sinPhi = sin(phi);
			vec2 omega = vec2(cosPhi, -sinPhi);

			omega *= screenspaceRadius;

			const vec3 directionVec = vec3(cosPhi, sinPhi, 0.0);

			const vec3 orthoDirectionVec = directionVec - (dot(directionVec, viewVec) * viewVec);

			const vec3 axisVec = normalize(cross(orthoDirectionVec, viewVec));

			vec3 projectedNormalVec = viewspaceNormal - axisVec * dot(viewspaceNormal, axisVec);

			float signNorm = sign(dot(orthoDirectionVec, projectedNormalVec));

			float projectedNormalVecLength = length(projectedNormalVec);
			float cosNorm = clamp(dot(projectedNormalVec, viewVec) / projectedNormalVecLength, 0.0, 1.0);

			float n = signNorm * XeGTAO_FastACos(cosNorm);

			const float lowHorizonCos0 = cos(n + XE_GTAO_PI_HALF);
			const float lowHorizonCos1 = cos(n - XE_GTAO_PI_HALF);

			float horizonCos0 = lowHorizonCos0;
			float horizonCos1 = lowHorizonCos1;

			for (float stepIndex = 0.0; stepIndex < stepsPerSlice; stepIndex++) {
				const float stepBaseNoise = float(slice + stepIndex * stepsPerSlice) * 0.6180339887498948482;
				float stepNoise = fract(noiseSample + stepBaseNoise);

				float s = (stepIndex + stepNoise) / (stepsPerSlice);

				s = pow(s, sampleDistributionPower);

				s += minS;

				vec2 sampleOffset = s * omega;

				float sampleOffsetLength = length(sampleOffset);

				const float mipLevel = clamp(log2(sampleOffsetLength) - consts.DepthMIPSamplingOffset, 0.0, float(XE_GTAO_DEPTH_MIP_LEVELS));

				sampleOffset = roundEven(sampleOffset) * consts.ViewportPixelSize;

				vec2 sampleScreenPos0 = normalizedScreenPos + sampleOffset;
				float SZ0 = textureLod(sourceViewspaceDepth, sampleScreenPos0, mipLevel).x;
				vec3 samplePos0 = XeGTAO_ComputeViewspacePosition(sampleScreenPos0, SZ0, consts);

				vec2 sampleScreenPos1 = normalizedScreenPos - sampleOffset;
				float SZ1 = textureLod(sourceViewspaceDepth, sampleScreenPos1, mipLevel).x;
				vec3 samplePos1 = XeGTAO_ComputeViewspacePosition(sampleScreenPos1, SZ1, consts);

				vec3 sampleDelta0 = (samplePos0 - pixCenterPos);
				vec3 sampleDelta1 = (samplePos1 - pixCenterPos);
				float sampleDist0 = length(sampleDelta0);
				float sampleDist1 = length(sampleDelta1);

				vec3 sampleHorizonVec0 = sampleDelta0 / sampleDist0;
				vec3 sampleHorizonVec1 = sampleDelta1 / sampleDist1;

				float falloffBase0 = length(vec3(sampleDelta0.x, sampleDelta0.y, sampleDelta0.z * (1.0 + thinOccluderCompensation)));
				float falloffBase1 = length(vec3(sampleDelta1.x, sampleDelta1.y, sampleDelta1.z * (1.0 + thinOccluderCompensation)));
				float weight0 = clamp(falloffBase0 * falloffMul + falloffAdd, 0.0, 1.0);
				float weight1 = clamp(falloffBase1 * falloffMul + falloffAdd, 0.0, 1.0);

				float shc0 = dot(sampleHorizonVec0, viewVec);
				float shc1 = dot(sampleHorizonVec1, viewVec);

				shc0 = mix(lowHorizonCos0, shc0, weight0);
				shc1 = mix(lowHorizonCos1, shc1, weight1);

				horizonCos0 = max(horizonCos0, shc0);
				horizonCos1 = max(horizonCos1, shc1);
			}

			projectedNormalVecLength = mix(projectedNormalVecLength, 1.0, 0.05);

			float h0 = -XeGTAO_FastACos(horizonCos1);
			float h1 = XeGTAO_FastACos(horizonCos0);

			float iarc0 = (cosNorm + 2.0 * h0 * sin(n) - cos(2.0 * h0 - n)) / 4.0;
			float iarc1 = (cosNorm + 2.0 * h1 * sin(n) - cos(2.0 * h1 - n)) / 4.0;
			float localVisibility = projectedNormalVecLength * (iarc0 + iarc1);
			visibility += localVisibility;

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
			float t0 = (6.0 * sin(h0 - n) - sin(3.0 * h0 - n) + 6.0 * sin(h1 - n) - sin(3.0 * h1 - n) + 16.0 * sin(n) - 3.0 * (sin(h0 + n) + sin(h1 + n))) / 12.0;
			float t1 = (-cos(3.0 * h0 - n) - cos(3.0 * h1 - n) + 8.0 * cos(n) - 3.0 * (cos(h0 + n) + cos(h1 + n))) / 12.0;
			vec3 localBentNormal = vec3(directionVec.x * t0, directionVec.y * t0, -t1);
			localBentNormal = (localBentNormal * XeGTAO_RotFromToMatrix(vec3(0.0, 0.0, -1.0), viewVec)) * projectedNormalVecLength;
			bentNormal += localBentNormal;
#endif
		}
		visibility /= sliceCount;
		visibility = pow(visibility, consts.FinalValuePower);
		visibility = max(0.03, visibility);

#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
		bentNormal = normalize(bentNormal);
#endif
	}

	outWorkingAOTerm = XeGTAO_OutputWorkingTerm(visibility, bentNormal);
}

float XeGTAO_DepthMIPFilter(float depth0, float depth1, float depth2, float depth3, const GTAOConstants consts) {
	float maxDepth = max(max(depth0, depth1), max(depth2, depth3));

	const float depthRangeScaleFactor = 0.75;
	const float effectRadius = depthRangeScaleFactor * consts.EffectRadius * consts.RadiusMultiplier;
	const float falloffRange = consts.EffectFalloffRange * effectRadius;
	const float falloffFrom = effectRadius * (1.0 - consts.EffectFalloffRange);
	const float falloffMul = -1.0 / (falloffRange);
	const float falloffAdd = falloffFrom / (falloffRange) + 1.0;

	float weight0 = clamp((maxDepth - depth0) * falloffMul + falloffAdd, 0.0, 1.0);
	float weight1 = clamp((maxDepth - depth1) * falloffMul + falloffAdd, 0.0, 1.0);
	float weight2 = clamp((maxDepth - depth2) * falloffMul + falloffAdd, 0.0, 1.0);
	float weight3 = clamp((maxDepth - depth3) * falloffMul + falloffAdd, 0.0, 1.0);

	float weightSum = weight0 + weight1 + weight2 + weight3;
	return (weight0 * depth0 + weight1 * depth1 + weight2 * depth2 + weight3 * depth3) / weightSum;
}

float XeGTAO_ClampDepth(float depth) {
	return clamp(depth, 0.0, 65504.0);
}

vec4 XeGTAO_UnpackEdges(float _packedVal) {
	uint packedVal = uint(_packedVal * 255.5);
	vec4 edgesLRTB;
	edgesLRTB.x = float((packedVal >> 6) & 0x03u) / 3.0;
	edgesLRTB.y = float((packedVal >> 4) & 0x03u) / 3.0;
	edgesLRTB.z = float((packedVal >> 2) & 0x03u) / 3.0;
	edgesLRTB.w = float((packedVal >> 0) & 0x03u) / 3.0;

	return clamp(edgesLRTB, 0.0, 1.0);
}

void XeGTAO_AddSample(AOTermType ssaoValue, float edgeValue, inout AOTermType sum, inout float sumWeight) {
	float weight = edgeValue;

	sum += (weight * ssaoValue);
	sumWeight += weight;
}

uint XeGTAO_Output(AOTermType outputValue, const bool finalApply) {
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
	float visibility = outputValue.w * ((finalApply) ? (float(XE_GTAO_OCCLUSION_TERM_SCALE)) : (1.0));
	vec3 bentNormal = normalize(outputValue.xyz);
	return XeGTAO_EncodeVisibilityBentNormal(visibility, bentNormal);
#else
	outputValue *= (finalApply) ? (float(XE_GTAO_OCCLUSION_TERM_SCALE)) : (1.0);
	return uint(outputValue * 255.0 + 0.5);
#endif
}

void XeGTAO_DecodeGatherPartial(const uvec4 packedValue, out AOTermType outDecoded[4]) {
	for (int i = 0; i < 4; i++) {
#ifdef XE_GTAO_COMPUTE_BENT_NORMALS
		XeGTAO_DecodeVisibilityBentNormal(packedValue[i], outDecoded[i].w, outDecoded[i].xyz);
#else
		outDecoded[i] = float(packedValue[i]) / float(255.0);
#endif
	}
}

void XeGTAO_Denoise(const uvec2 pixCoordBase, const GTAOConstants consts, usampler2D sourceAOTerm, sampler2D sourceEdges, const bool finalApply, out uint outputValue[2]) {
	const float blurAmount = (finalApply) ? (consts.DenoiseBlurBeta) : (consts.DenoiseBlurBeta / 5.0);
	const float diagWeight = 0.85 * 0.5;

	AOTermType aoTerm[2];
	vec4 edgesC_LRTB[2];
	float weightTL[2];
	float weightTR[2];
	float weightBL[2];
	float weightBR[2];

	const vec2 gatherCenter = vec2(pixCoordBase.x, pixCoordBase.y) * consts.ViewportPixelSize;
	vec4 edgesQ0 = textureGatherOffset(sourceEdges, gatherCenter, ivec2(0, 0), 0);
	vec4 edgesQ1 = textureGatherOffset(sourceEdges, gatherCenter, ivec2(2, 0), 0);
	vec4 edgesQ2 = textureGatherOffset(sourceEdges, gatherCenter, ivec2(1, 2), 0);

	AOTermType visQ0[4];
	XeGTAO_DecodeGatherPartial(textureGatherOffset(sourceAOTerm, gatherCenter, ivec2(0, 0), 0), visQ0);
	AOTermType visQ1[4];
	XeGTAO_DecodeGatherPartial(textureGatherOffset(sourceAOTerm, gatherCenter, ivec2(2, 0), 0), visQ1);
	AOTermType visQ2[4];
	XeGTAO_DecodeGatherPartial(textureGatherOffset(sourceAOTerm, gatherCenter, ivec2(0, 2), 0), visQ2);
	AOTermType visQ3[4];
	XeGTAO_DecodeGatherPartial(textureGatherOffset(sourceAOTerm, gatherCenter, ivec2(2, 2), 0), visQ3);

	for (int side = 0; side < 2; side++) {
		vec4 edgesL_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ0.x) : (edgesQ0.y));
		vec4 edgesT_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ0.z) : (edgesQ1.w));
		vec4 edgesR_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ1.x) : (edgesQ1.y));
		vec4 edgesB_LRTB = XeGTAO_UnpackEdges((side == 0) ? (edgesQ2.w) : (edgesQ2.z));

		edgesC_LRTB[side] = XeGTAO_UnpackEdges((side == 0) ? (edgesQ0.y) : (edgesQ1.x));

		edgesC_LRTB[side] *= vec4(edgesL_LRTB.y, edgesR_LRTB.x, edgesT_LRTB.w, edgesB_LRTB.z);

		const float leak_threshold = 2.5;
		const float leak_strength = 0.5;
		float edginess = (clamp(4.0 - leak_threshold - dot(edgesC_LRTB[side], vec4(1.0)), 0.0, 1.0) / (4.0 - leak_threshold)) * leak_strength;
		edgesC_LRTB[side] = clamp(edgesC_LRTB[side] + edginess, 0.0, 1.0);

		weightTL[side] = diagWeight * (edgesC_LRTB[side].x * edgesL_LRTB.z + edgesC_LRTB[side].z * edgesT_LRTB.x);
		weightTR[side] = diagWeight * (edgesC_LRTB[side].z * edgesT_LRTB.y + edgesC_LRTB[side].y * edgesR_LRTB.z);
		weightBL[side] = diagWeight * (edgesC_LRTB[side].w * edgesB_LRTB.x + edgesC_LRTB[side].x * edgesL_LRTB.w);
		weightBR[side] = diagWeight * (edgesC_LRTB[side].y * edgesR_LRTB.w + edgesC_LRTB[side].w * edgesB_LRTB.y);

		AOTermType ssaoValue = (side == 0) ? (visQ0[1]) : (visQ1[0]);
		AOTermType ssaoValueL = (side == 0) ? (visQ0[0]) : (visQ0[1]);
		AOTermType ssaoValueT = (side == 0) ? (visQ0[2]) : (visQ1[3]);
		AOTermType ssaoValueR = (side == 0) ? (visQ1[0]) : (visQ1[1]);
		AOTermType ssaoValueB = (side == 0) ? (visQ2[2]) : (visQ3[3]);
		AOTermType ssaoValueTL = (side == 0) ? (visQ0[3]) : (visQ0[2]);
		AOTermType ssaoValueBR = (side == 0) ? (visQ3[3]) : (visQ3[2]);
		AOTermType ssaoValueTR = (side == 0) ? (visQ1[3]) : (visQ1[2]);
		AOTermType ssaoValueBL = (side == 0) ? (visQ2[3]) : (visQ2[2]);

		float sumWeight = blurAmount;
		AOTermType sum = ssaoValue * sumWeight;

		XeGTAO_AddSample(ssaoValueL, edgesC_LRTB[side].x, sum, sumWeight);
		XeGTAO_AddSample(ssaoValueR, edgesC_LRTB[side].y, sum, sumWeight);
		XeGTAO_AddSample(ssaoValueT, edgesC_LRTB[side].z, sum, sumWeight);
		XeGTAO_AddSample(ssaoValueB, edgesC_LRTB[side].w, sum, sumWeight);

		XeGTAO_AddSample(ssaoValueTL, weightTL[side], sum, sumWeight);
		XeGTAO_AddSample(ssaoValueTR, weightTR[side], sum, sumWeight);
		XeGTAO_AddSample(ssaoValueBL, weightBL[side], sum, sumWeight);
		XeGTAO_AddSample(ssaoValueBR, weightBR[side], sum, sumWeight);

		aoTerm[side] = sum / sumWeight;

		outputValue[side] = XeGTAO_Output(aoTerm[side], finalApply);
	}
}

uint XeGTAO_HilbertIndex(uint posX, uint posY) {
	uint index = 0u;
	for (uint curLevel = XE_HILBERT_WIDTH / 2u; curLevel > 0u; curLevel /= 2u) {
		uint regionX = (posX & curLevel) > 0u ? 1u : 0u;
		uint regionY = (posY & curLevel) > 0u ? 1u : 0u;
		index += curLevel * curLevel * ((3u * regionX) ^ regionY);
		if (regionY == 0u) {
			if (regionX == 1u) {
				posX = uint((XE_HILBERT_WIDTH - 1u)) - posX;
				posY = uint((XE_HILBERT_WIDTH - 1u)) - posY;
			}

			uint temp = posX;
			posX = posY;
			posY = temp;
		}
	}
	return index;
}

vec2 XeGTAO_SpatioTemporalNoise(uvec2 pixCoord, uint temporalIndex) {
	uint index = XeGTAO_HilbertIndex(pixCoord.x % 64u, pixCoord.y % 64u);
	index += 288u * (temporalIndex % 64u);
	return vec2(fract(0.5 + float(index) * vec2(0.75487766624669276005, 0.5698402909980532659114)));
}
