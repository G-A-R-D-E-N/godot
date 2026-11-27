/**************************************************************************/
/*  fog_precision.h                                                       */
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
#include "core/math/vector2.h"

#include <stdint.h>

namespace RendererRD {

namespace FogPrecision {

constexpr float DENSITY_SCALE = 1048576.0f;
constexpr float DENSITY_LIMIT = 32.0f;
constexpr float COLOR_LIMIT = 64.0f;
constexpr float LIGHT_GATE = 1e-6f;
constexpr float SERIES_SWITCH = 0.05f;
constexpr uint32_t LIGHT_WORDS = 4;
constexpr uint32_t EMISSION_WORDS = 3;

inline int32_t quantize_density(float p_density) {
	if (!(Math::abs(p_density) > 0.0f)) {
		return 0;
	}
	const float density = CLAMP(p_density, -DENSITY_LIMIT, DENSITY_LIMIT);
	return int32_t(Math::floor(density * DENSITY_SCALE + 0.5f));
}

inline uint32_t quantize_color(float p_amount) {
	if (!(p_amount > 0.0f)) {
		return 0;
	}
	const float amount = MIN(p_amount, COLOR_LIMIT);
	return uint32_t(Math::floor(amount * DENSITY_SCALE + 0.5f));
}

inline float scattering_amount(float p_albedo, float p_density) {
	return CLAMP(p_albedo, 0.0f, 1.0f) * MAX(p_density, 0.0f);
}

inline float emission_amount(float p_emission, float p_density) {
	return MAX(p_emission, 0.0f) * MAX(p_density, 0.0f);
}

inline uint32_t positive_density_word(int32_t p_quantized_density) {
	return p_quantized_density > 0 ? uint32_t(p_quantized_density) : 0u;
}

inline float decode_density(uint32_t p_word) {
	return float(int32_t(p_word)) / DENSITY_SCALE;
}

inline float decode_color(uint32_t p_word) {
	return float(p_word) / DENSITY_SCALE;
}

inline float total_density(float p_base_density, uint32_t p_density_word) {
	return MAX(0.0f, p_base_density + decode_density(p_density_word));
}

inline float carve_fraction(float p_base_density, float p_total_density, float p_positive_volume_density) {
	const float medium = p_base_density + p_positive_volume_density;
	if (!(medium > 0.0f)) {
		return 0.0f;
	}
	return MIN(1.0f, p_total_density / medium);
}

inline bool gets_light(float p_total_density) {
	return p_total_density > LIGHT_GATE;
}

inline float column_ndc(uint32_t p_index, uint32_t p_count) {
	return (float(p_index) + 0.5f) / float(p_count) * 2.0f - 1.0f;
}

inline float path_scale(const Vector2 &p_ndc, const Vector2 &p_size_begin, const Vector2 &p_size_end, float p_fog_end) {
	const Vector2 slope = p_ndc * (p_size_end - p_size_begin) / p_fog_end;
	return Math::sqrt(1.0f + slope.dot(slope));
}

inline float in_scatter_weight(float p_step, float p_extinction) {
	if (!(p_extinction > 0.0f)) {
		return 0.0f;
	}
	const float tau = p_step * p_extinction;
	if (tau > SERIES_SWITCH) {
		return (1.0f - Math::exp(-tau)) / p_extinction;
	}
	return p_step * (1.0f - tau * (0.5f - tau * (1.0f / 6.0f - tau * (1.0f / 24.0f))));
}

} // namespace FogPrecision

} // namespace RendererRD
