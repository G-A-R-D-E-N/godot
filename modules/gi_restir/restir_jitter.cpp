/**************************************************************************/
/*  restir_jitter.cpp                                                     */
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

#include "restir_jitter.h"

#include "core/math/math_funcs.h"

static RestirJitterShift _restir_jitter_unmatched() {
	RestirJitterShift shift;
	shift.unmatched = true;
	return shift;
}

RestirJitterShift restir_jitter_shift(const Projection &p_projection, const Projection &p_drawn_projection) {
	RestirJitterShift shift;
	Projection correction;
	correction.set_depth_correction(true);
	const Projection plain = correction * p_projection;

	double m[4][4];
	double d[4][4];
	bool same = true;
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			m[c][r] = double(plain.columns[c][r]);
			d[c][r] = double(p_drawn_projection.columns[c][r]);
			same = same && plain.columns[c][r] == p_drawn_projection.columns[c][r];
		}
	}
	if (same) {
		return shift;
	}

	int pivot = 0;
	for (int c = 1; c < 4; c++) {
		if (Math::abs(m[c][3]) > Math::abs(m[pivot][3])) {
			pivot = c;
		}
	}
	if (!(Math::abs(m[pivot][3]) > 0.0)) {
		return _restir_jitter_unmatched();
	}
	const double jitter[2] = {
		(d[pivot][0] - m[pivot][0]) / m[pivot][3],
		(d[pivot][1] - m[pivot][1]) / m[pivot][3],
	};
	if (!Math::is_finite(jitter[0]) || !Math::is_finite(jitter[1])) {
		return _restir_jitter_unmatched();
	}

	for (int r = 0; r < 2; r++) {
		double scale = 0.0;
		for (int c = 0; c < 4; c++) {
			scale = MAX(scale, MAX(Math::abs(m[c][r]), MAX(Math::abs(d[c][r]), Math::abs(m[c][3]))));
		}
		for (int c = 0; c < 4; c++) {
			const double residual = d[c][r] - m[c][r] - jitter[r] * m[c][3];
			if (!(Math::abs(residual) <= RESTIR_JITTER_TOLERANCE * scale)) {
				return _restir_jitter_unmatched();
			}
		}
	}
	for (int r = 2; r < 4; r++) {
		double scale = 0.0;
		for (int c = 0; c < 4; c++) {
			scale = MAX(scale, MAX(Math::abs(m[c][r]), Math::abs(d[c][r])));
		}
		for (int c = 0; c < 4; c++) {
			if (!(Math::abs(d[c][r] - m[c][r]) <= RESTIR_JITTER_TOLERANCE * scale)) {
				return _restir_jitter_unmatched();
			}
		}
		if (!(Math::abs(m[0][r]) <= RESTIR_JITTER_TOLERANCE * scale) || !(Math::abs(m[1][r]) <= RESTIR_JITTER_TOLERANCE * scale)) {
			return _restir_jitter_unmatched();
		}
	}

	const double det = m[0][0] * m[1][1] - m[1][0] * m[0][1];
	const double det_scale = Math::abs(m[0][0] * m[1][1]) + Math::abs(m[1][0] * m[0][1]);
	if (!(Math::abs(det) > RESTIR_JITTER_TOLERANCE * det_scale)) {
		return _restir_jitter_unmatched();
	}

	shift.jitter_x = jitter[0];
	shift.jitter_y = jitter[1];
	if (jitter[0] == 0.0 && jitter[1] == 0.0) {
		return shift;
	}
	const double qx = (m[1][1] * jitter[0] - m[1][0] * jitter[1]) / det;
	const double qy = (m[0][0] * jitter[1] - m[0][1] * jitter[0]) / det;
	shift.ax = 0.0 - qx * m[2][3];
	shift.ay = 0.0 - qy * m[2][3];
	shift.bx = 0.0 - qx * m[3][3];
	shift.by = 0.0 - qy * m[3][3];
	shift.jittered = true;
	return shift;
}
