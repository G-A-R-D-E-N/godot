/**************************************************************************/
/*  local_exposure_limits.h                                               */
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

namespace LocalExposureLimits {

constexpr float MAX_PIVOT_STOPS = 8.0f;
constexpr float MAX_SHIFT_STOPS = 6.0f;
constexpr float MIN_CONTRAST = 0.25f;

constexpr float limit(float p_value, float p_min, float p_max) {
	return p_value < p_min ? p_min : (p_value > p_max ? p_max : p_value);
}

constexpr float strength(float p_strength) {
	return limit(p_strength, 0.0f, 1.0f);
}

constexpr float contrast(float p_contrast) {
	return limit(p_contrast, MIN_CONTRAST, 1.0f);
}

constexpr float shift(float p_stops) {
	return limit(p_stops, 0.0f, MAX_SHIFT_STOPS);
}

constexpr float pivot(float p_stops) {
	return limit(p_stops, -MAX_PIVOT_STOPS, MAX_PIVOT_STOPS);
}

constexpr float median_log(const float *p_weight, const float *p_sum, int p_count, float p_none) {
	float total = 0.0f;
	for (int i = 0; i < p_count; i++) {
		total += p_weight[i];
	}
	if (!(total > 1e-5f)) {
		return p_none;
	}
	float running = 0.0f;
	for (int i = 0; i < p_count; i++) {
		running += p_weight[i];
		if (running >= 0.5f * total) {
			return p_sum[i] / p_weight[i];
		}
	}
	return p_none;
}

} // namespace LocalExposureLimits
