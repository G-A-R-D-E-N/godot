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

} // namespace AutoExposureGuard
