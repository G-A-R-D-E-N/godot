/**************************************************************************/
/*  fog_ddgi.h                                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/math_funcs.h"
#include "core/typedefs.h"

namespace RendererRD {

static constexpr bool FOG_DDGI_DEFAULT_ENABLED = true;
static constexpr bool FOG_DDGI_DEFAULT_VISIBILITY = true;
static constexpr bool FOG_DDGI_DEFAULT_DIRECTIONAL = false;

struct FogGIChoice {
	bool use_ddgi = false;
	bool bind_sdfgi = false;
	uint32_t voxel_gi_count = 0;
};

inline FogGIChoice fog_gi_source(float p_gi_inject, bool p_setting_on, bool p_ddgi_valid, bool p_ddgi_shader_ok, bool p_sdfgi_enabled, bool p_sdfgi_valid, uint32_t p_voxel_gi_count) {
	FogGIChoice choice;
	if (p_setting_on && p_ddgi_valid && p_ddgi_shader_ok && p_gi_inject > 0.0001) {
		choice.use_ddgi = true;
		return choice;
	}
	choice.bind_sdfgi = p_gi_inject > 0.0001 && p_sdfgi_enabled && p_sdfgi_valid;
	choice.voxel_gi_count = p_gi_inject > 0.001 ? p_voxel_gi_count : 0;
	return choice;
}

inline uint32_t fog_ddgi_directional(bool p_setting_on, float p_phase_g, bool p_shader_ok) {
	return (p_setting_on && p_shader_ok && p_phase_g != 0.0f) ? 1 : 0;
}

inline int fog_ddgi_variant(int p_fog_variant, uint32_t p_visibility) {
	return 2 * p_fog_variant + int(p_visibility);
}

inline float fog_ddgi_ambient_share(float p_alpha) {
	return 1.0f - p_alpha;
}

inline float fog_ddgi_visibility(float p_distance, float p_mean, float p_mean_squared) {
	float weight = 1.0f;
	if (p_distance > p_mean) {
		const float variance = Math::abs(p_mean_squared - p_mean * p_mean);
		const float delta = p_distance - p_mean;
		const float chebyshev = variance / (variance + delta * delta);
		weight *= chebyshev * chebyshev * chebyshev;
		weight *= weight < 0.5f ? Math::pow(weight, 7.0f) * 128.0f : 1.0f;
	}
	weight = MAX(1e-6f, weight);
	if (weight < 0.2f) {
		weight *= weight * weight * 25.0f;
	}
	return weight;
}

struct FogDDGICorner {
	bool skipped = false;
	float trilinear = 0.0f;
	float visibility = 1.0f;
};

struct FogDDGILevelWeight {
	float weight = 0.0f;
	float confidence = 0.0f;
};

inline FogDDGILevelWeight fog_ddgi_level_weight(const FogDDGICorner *p_corners, uint32_t p_count) {
	FogDDGILevelWeight result;
	float trilinear_sum = 0.0f;
	for (uint32_t i = 0; i < p_count; i++) {
		if (p_corners[i].skipped) {
			continue;
		}
		result.weight += p_corners[i].visibility * p_corners[i].trilinear;
		trilinear_sum += p_corners[i].trilinear;
	}
	result.confidence = trilinear_sum > 0.0f ? CLAMP(result.weight / trilinear_sum, 0.0f, 1.0f) : 0.0f;
	return result;
}

inline float fog_ddgi_irradiance_hysteresis(float p_probe_hysteresis, float p_keep, float p_previous_alpha) {
	const float updates = p_probe_hysteresis > 0.0f ? p_previous_alpha : 0.0f;
	return MIN(p_probe_hysteresis * p_keep, updates / (updates + 1.0f));
}

inline float fog_ddgi_tile_alpha(float p_probe_hysteresis, float p_previous_alpha) {
	const float updates = p_probe_hysteresis > 0.0f ? p_previous_alpha : 0.0f;
	return MIN(updates + 1.0f, 255.0f);
}

inline float fog_ddgi_mean_hysteresis(float p_probe_hysteresis, float p_keep, float p_tile_alpha) {
	const float updates = p_probe_hysteresis > 0.0f ? p_tile_alpha - 1.0f : 0.0f;
	return MIN(p_probe_hysteresis * p_keep, updates / (updates + 1.0f));
}

inline bool fog_ddgi_change_halves(const float p_result[3], const float p_previous[3], float p_change_threshold) {
	const float change = MAX(Math::abs(p_result[0] - p_previous[0]), MAX(Math::abs(p_result[1] - p_previous[1]), Math::abs(p_result[2] - p_previous[2])));
	const float largest = MAX(MAX(p_previous[0], MAX(p_previous[1], p_previous[2])), MAX(p_result[0], MAX(p_result[1], p_result[2])));
	return change > p_change_threshold * largest;
}

template <typename T>
inline void fog_ddgi_ray_direction(uint32_t p_index, uint32_t p_count, const T *p_rotation, T r_direction[3]) {
	const T golden_ratio = T(1.6180339887);
	const T n = T(p_count);
	const T z = T(1) - (T(2) * T(p_index) + T(1)) / n;
	const T r = Math::sqrt(MAX(T(0), T(1) - z * z));
	const T turns = T(p_index) / golden_ratio;
	const T phi = T(6.2831853) * (turns - Math::floor(turns));
	const T local[3] = { r * Math::cos(phi), r * Math::sin(phi), z };
	for (int i = 0; i < 3; i++) {
		r_direction[i] = p_rotation ? p_rotation[i * 3 + 0] * local[0] + p_rotation[i * 3 + 1] * local[1] + p_rotation[i * 3 + 2] * local[2] : local[i];
	}
}

template <typename T>
struct FogDDGIRayMoments {
	T mean[3] = {};
	T moment[3][3] = {};
	uint32_t count = 0;
};

template <typename T>
inline FogDDGIRayMoments<T> fog_ddgi_ray_moments(const T *p_radiance, const T *p_directions, uint32_t p_rays) {
	T sum[3] = {};
	T moment[3][3] = {};
	T count = T(0);
	for (uint32_t r = 0; r < p_rays; r++) {
		const T *ray = p_radiance + r * 4;
		const T *direction = p_directions + r * 3;
		if (ray[3] < T(0)) {
			continue;
		}
		for (int c = 0; c < 3; c++) {
			sum[c] += ray[c];
			for (int k = 0; k < 3; k++) {
				moment[c][k] += ray[c] * direction[k];
			}
		}
		count += T(1);
	}
	FogDDGIRayMoments<T> result;
	result.count = uint32_t(count);
	for (int c = 0; c < 3; c++) {
		result.mean[c] = count > T(0) ? sum[c] / count : T(0);
		for (int k = 0; k < 3; k++) {
			result.moment[c][k] = count > T(0) ? moment[c][k] / count : T(0);
		}
	}
	return result;
}

inline void fog_ddgi_inscatter(const float *p_mean, const float *p_moment, const float *p_view, float p_phase_g, float *r_light) {
	for (int c = 0; c < 3; c++) {
		const float first = p_view[0] * p_moment[c * 3 + 0] + p_view[1] * p_moment[c * 3 + 1] + p_view[2] * p_moment[c * 3 + 2];
		r_light[c] = MAX(p_mean[c] + 3.0f * p_phase_g * first, 0.0f);
	}
}

} // namespace RendererRD
