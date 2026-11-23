/**************************************************************************/
/*  auto_exposure_guard.h                                                 */
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

namespace AutoExposureGuard {

inline float light_factor(float p_stops_per_second, double p_time_step, bool p_off) {
	if (p_off || !(p_stops_per_second > 0.0f) || !(p_time_step > 0.0)) {
		return 0.0f;
	}
	return float(Math::pow(2.0, MIN(double(p_stops_per_second) * p_time_step, 100.0)));
}

inline float limited(float p_previous, float p_stepped, float p_factor, float p_min_luminance = 0.0f) {
	if (p_factor > 0.0f && p_previous > 0.0f) {
		return MAX(MIN(p_stepped, p_previous * p_factor), p_min_luminance);
	}
	return p_stepped;
}

inline float held(float p_previous, float p_stepped, bool p_hold) {
	return (p_hold && p_previous > 0.0f) ? p_previous : p_stepped;
}

inline float highlight_rate(float p_time, double p_time_step, bool p_reset) {
	if (p_reset || !(p_time > 0.0f)) {
		return 1.0f;
	}
	if (!(p_time_step > 0.0)) {
		return 0.0f;
	}
	return float(1.0 - Math::exp(-p_time_step / double(p_time)));
}

inline bool highlight_valid(bool p_stored, bool p_prev_positive, bool p_immediate, bool p_stale) {
	return p_stored && p_prev_positive && !p_immediate && !p_stale;
}

inline bool highlight_stored(bool p_valid, bool p_kept) {
	return p_valid || p_kept;
}

inline float highlight_bounded(float p_stored, float p_raw, float p_max_shift) {
	return p_stored >= 0.0f ? MIN(p_stored, p_max_shift) : p_raw;
}

inline float highlight_shift(float p_previous, float p_raw, float p_rise, float p_fall, bool p_valid, bool p_hold) {
	if (!p_valid) {
		return p_raw;
	}
	if (p_hold) {
		return p_previous;
	}
	const float rate = p_raw > p_previous ? p_rise : p_fall;
	return rate >= 1.0f ? p_raw : p_previous + (p_raw - p_previous) * rate;
}

struct HighlightStep {
	float log_target = 0.0f;
	float shift = 0.0f;
};

inline HighlightStep highlight_step(float p_log_mean, float p_log_high, bool p_has_high, float p_ceiling, float p_max_shift, float p_previous, float p_rise, float p_fall, bool p_valid, bool p_hold) {
	float stepped = p_log_mean;
	float raw = 0.0f;
	if (p_has_high) {
		stepped = MIN(MAX(p_log_high - p_ceiling, p_log_mean), p_log_mean + p_max_shift);
		raw = MIN(MAX(p_log_high - p_ceiling - p_log_mean, 0.0f), p_max_shift);
	}
	HighlightStep out;
	const float previous = p_valid ? highlight_bounded(p_previous, raw, p_max_shift) : p_previous;
	out.shift = highlight_shift(previous, raw, p_rise, p_fall, p_valid, p_hold);
	out.log_target = out.shift == raw ? stepped : p_log_mean + out.shift;
	return out;
}

} // namespace AutoExposureGuard
