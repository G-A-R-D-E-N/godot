/**************************************************************************/
/*  test_restir_jitter.h                                                  */
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

#include "../restir_harness.h"
#include "../restir_jitter.h"

#include "core/math/math_funcs.h"
#include "tests/test_macros.h"

namespace TestRestirJitter {

constexpr double JITTER_X = 0.3 / 640.0 * 2.0;
constexpr double JITTER_Y = -0.45 / 360.0 * 2.0;
constexpr double MAP_TOLERANCE = sizeof(real_t) == 8 ? 1e-9 : 1e-4;
constexpr double JITTER_TOLERANCE = sizeof(real_t) == 8 ? 1e-12 : 1e-6;
constexpr double SIGNS[4][2] = { { 1.0, 1.0 }, { -1.0, 1.0 }, { 1.0, -1.0 }, { -1.0, -1.0 } };
constexpr double UVS[5][2] = { { 0.5, 0.5 }, { 0.03, 0.11 }, { 0.97, 0.2 }, { 0.25, 0.9 }, { 0.71, 0.64 } };
constexpr double DEPTHS[5] = { 0.95, 0.5, 0.1, 0.01, 0.0005 };

struct Point {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

Projection plain_projection(const Projection &p_projection) {
	Projection correction;
	correction.set_depth_correction(true);
	return correction * p_projection;
}

Projection drawn_projection(const Projection &p_projection, double p_jitter_x, double p_jitter_y, bool p_flip_y = true) {
	Projection correction;
	correction.set_depth_correction(p_flip_y);
	correction.add_jitter_offset(Vector2(p_jitter_x, p_jitter_y));
	return correction * p_projection;
}

Point rebuild(const Projection &p_inverse, double p_u, double p_v, double p_depth) {
	const Vector4 h = p_inverse.xform(Vector4(p_u * 2.0 - 1.0, p_v * 2.0 - 1.0, p_depth, 1.0));
	return { double(h.x) / double(h.w), double(h.y) / double(h.w), double(h.z) / double(h.w) };
}

Point shifted(const Point &p_view, const RestirJitterShift &p_shift) {
	return { p_view.x + p_view.z * p_shift.ax + p_shift.bx, p_view.y + p_view.z * p_shift.ay + p_shift.by, p_view.z };
}

bool maps_onto(const Point &p_view, const Point &p_truth, const RestirJitterShift &p_shift) {
	const Point moved = shifted(p_view, p_shift);
	const double scale = MAX(Math::sqrt(p_truth.x * p_truth.x + p_truth.y * p_truth.y + p_truth.z * p_truth.z), 1e-30);
	const double limit = MAP_TOLERANCE * scale;
	return Math::abs(moved.x - p_truth.x) <= limit && Math::abs(moved.y - p_truth.y) <= limit && Math::abs(moved.z - p_truth.z) <= limit;
}

bool all_zero(const RestirJitterShift &p_shift) {
	return p_shift.ax == 0.0 && p_shift.ay == 0.0 && p_shift.bx == 0.0 && p_shift.by == 0.0;
}

bool check_projection(const Projection &p_projection) {
	const Projection plain_inverse = plain_projection(p_projection).inverse();
	bool good = true;
	for (const double *sign : SIGNS) {
		const double jx = JITTER_X * sign[0];
		const double jy = JITTER_Y * sign[1];
		const Projection drawn = drawn_projection(p_projection, jx, jy);
		const Projection drawn_inverse = drawn.inverse();
		const RestirJitterShift shift = restir_jitter_shift(p_projection, drawn);
		good = good && !shift.unmatched && shift.jittered;
		good = good && Math::abs(shift.jitter_x - jx) <= JITTER_TOLERANCE && Math::abs(shift.jitter_y - jy) <= JITTER_TOLERANCE;
		for (const double *uv : UVS) {
			for (const double depth : DEPTHS) {
				const Point view = rebuild(plain_inverse, uv[0], uv[1], depth);
				const Point truth = rebuild(drawn_inverse, uv[0], uv[1], depth);
				good = good && maps_onto(view, truth, shift);
				good = good && !maps_onto(view, truth, RestirJitterShift());
			}
		}
	}
	return good;
}

TEST_CASE("[RestirJitter] Perspective: the shift maps the plain rebuild onto the drawn one") {
	CHECK(check_projection(Projection::create_perspective(60.0, 16.0 / 9.0, 0.05, 2000.0)));
}

TEST_CASE("[RestirJitter] Off-axis perspective: the shift maps the plain rebuild onto the drawn one") {
	CHECK(check_projection(Projection::create_frustum(-0.04, 0.05, -0.02, 0.03, 0.05, 2000.0)));
}

TEST_CASE("[RestirJitter] Orthogonal: the shift maps the plain rebuild onto the drawn one") {
	CHECK(check_projection(Projection::create_orthogonal_aspect(20.0, 16.0 / 9.0, 0.05, 2000.0)));
	CHECK(check_projection(Projection::create_orthogonal(-12.0, 8.0, -5.0, 6.0, 0.1, 500.0)));
}

TEST_CASE("[RestirJitter] Orthogonal: the shift has no z term") {
	const Projection projection = Projection::create_orthogonal_aspect(20.0, 16.0 / 9.0, 0.05, 2000.0);
	const RestirJitterShift shift = restir_jitter_shift(projection, drawn_projection(projection, JITTER_X, JITTER_Y));
	CHECK(shift.ax == 0.0);
	CHECK(shift.ay == 0.0);
	CHECK(shift.bx != 0.0);
	CHECK(shift.by != 0.0);
}

TEST_CASE("[RestirJitter] Zero jitter gives exact zeros") {
	const Projection projections[2] = { Projection::create_perspective(60.0, 16.0 / 9.0, 0.05, 2000.0), Projection::create_orthogonal_aspect(20.0, 16.0 / 9.0, 0.05, 2000.0) };
	for (const Projection &projection : projections) {
		const RestirJitterShift shift = restir_jitter_shift(projection, drawn_projection(projection, 0.0, 0.0));
		CHECK(all_zero(shift));
		CHECK(!shift.unmatched);
		CHECK(!shift.jittered);
		CHECK(shift.jitter_x == 0.0);
		CHECK(shift.jitter_y == 0.0);
		const RestirJitterShift same = restir_jitter_shift(projection, plain_projection(projection));
		CHECK(all_zero(same));
		CHECK(!same.unmatched);
	}
}

TEST_CASE("[RestirJitter] A y-flipped drawn projection is unmatched and gives zeros") {
	const Projection projections[2] = { Projection::create_perspective(60.0, 16.0 / 9.0, 0.05, 2000.0), Projection::create_orthogonal_aspect(20.0, 16.0 / 9.0, 0.05, 2000.0) };
	for (const Projection &projection : projections) {
		const RestirJitterShift flipped = restir_jitter_shift(projection, drawn_projection(projection, JITTER_X, JITTER_Y, false));
		CHECK(flipped.unmatched);
		CHECK(!flipped.jittered);
		CHECK(all_zero(flipped));
		const RestirJitterShift flipped_still = restir_jitter_shift(projection, drawn_projection(projection, 0.0, 0.0, false));
		CHECK(flipped_still.unmatched);
		CHECK(all_zero(flipped_still));
	}
}

TEST_CASE("[RestirJitter] A drawn projection that differs in more than an xy jitter is unmatched") {
	const Projection projection = Projection::create_perspective(60.0, 16.0 / 9.0, 0.05, 2000.0);
	Projection depth_changed = drawn_projection(projection, JITTER_X, JITTER_Y);
	depth_changed.columns[3][2] *= 1.01;
	const RestirJitterShift depth_shift = restir_jitter_shift(projection, depth_changed);
	CHECK(depth_shift.unmatched);
	CHECK(all_zero(depth_shift));
	Projection zoomed = drawn_projection(projection, JITTER_X, JITTER_Y);
	zoomed.columns[0][0] *= 1.01;
	const RestirJitterShift zoom_shift = restir_jitter_shift(projection, zoomed);
	CHECK(zoom_shift.unmatched);
	CHECK(all_zero(zoom_shift));
	const RestirJitterShift other_camera = restir_jitter_shift(projection, drawn_projection(Projection::create_perspective(70.0, 16.0 / 9.0, 0.05, 2000.0), JITTER_X, JITTER_Y));
	CHECK(other_camera.unmatched);
	CHECK(all_zero(other_camera));
}

TEST_CASE("[RestirJitter] Sign: a positive x jitter moves the point toward -x, a positive y jitter toward +y (view space)") {
	const Projection projections[2] = { Projection::create_perspective(60.0, 16.0 / 9.0, 0.05, 2000.0), Projection::create_orthogonal_aspect(20.0, 16.0 / 9.0, 0.05, 2000.0) };
	for (const Projection &projection : projections) {
		const Projection plain_inverse = plain_projection(projection).inverse();
		const Point view = rebuild(plain_inverse, 0.4, 0.6, 0.1);
		CHECK(view.z < 0.0);
		const RestirJitterShift right = restir_jitter_shift(projection, drawn_projection(projection, 0.001, 0.0));
		const Point moved_x = shifted(view, right);
		CHECK(moved_x.x < view.x);
		CHECK(moved_x.y == view.y);
		const RestirJitterShift down = restir_jitter_shift(projection, drawn_projection(projection, 0.0, 0.001));
		const Point moved_y = shifted(view, down);
		CHECK(moved_y.y > view.y);
		CHECK(moved_y.x == view.x);
		const Point truth_x = rebuild(drawn_projection(projection, 0.001, 0.0).inverse(), 0.4, 0.6, 0.1);
		CHECK(truth_x.x < view.x);
		const Point truth_y = rebuild(drawn_projection(projection, 0.0, 0.001).inverse(), 0.4, 0.6, 0.1);
		CHECK(truth_y.y > view.y);
	}
}

TEST_CASE("[RestirJitter] Harness: the shifts survive the dict round trip and default to zero") {
	RestirFrameParams params;
	const float shift[4] = { 1.5e-4f, -2.5e-4f, 3.0e-3f, -4.0e-3f };
	for (int i = 0; i < 4; i++) {
		params.jitter_shift[i] = shift[i];
		params.previous_jitter_shift[i] = -shift[i];
	}
	const RestirFrameParams back = RestirHarness::params_from_dict(RestirHarness::params_to_dict(params));
	bool same = true;
	for (int i = 0; i < 4; i++) {
		same = same && back.jitter_shift[i] == shift[i] && back.previous_jitter_shift[i] == -shift[i];
	}
	CHECK(same);
	Dictionary old_capture = RestirHarness::params_to_dict(params);
	old_capture.erase("jitter_shift");
	old_capture.erase("previous_jitter_shift");
	const RestirFrameParams old = RestirHarness::params_from_dict(old_capture);
	bool zero = true;
	for (int i = 0; i < 4; i++) {
		zero = zero && old.jitter_shift[i] == 0.0f && old.previous_jitter_shift[i] == 0.0f;
	}
	CHECK(zero);
}

} //namespace TestRestirJitter
