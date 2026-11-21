/**************************************************************************/
/*  test_restir_settings.h                                                */
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

#include "../restir_gi_core.h"
#include "../restir_settings.h"

#include "core/math/math_funcs.h"
#include "tests/test_macros.h"

namespace TestRestirSettings {

struct ReuseRow {
	const char *name;
	double RestirReuseValues::*value;
	float RestirFrameParams::*field;
};

const ReuseRow REUSE_ROWS[9] = {
	{ "gi/plane_tolerance", &RestirReuseValues::plane_tolerance, &RestirFrameParams::tol_plane },
	{ "gi/normal_angle", &RestirReuseValues::normal_angle, &RestirFrameParams::cos_normal },
	{ "gi/roughness_tolerance", &RestirReuseValues::roughness_tolerance, &RestirFrameParams::class_tol },
	{ "gi/jacobian_max", &RestirReuseValues::jacobian_max, &RestirFrameParams::jac_max },
	{ "gi/min_distance", &RestirReuseValues::min_distance, &RestirFrameParams::d_min },
	{ "gi/min_cosine", &RestirReuseValues::min_cosine, &RestirFrameParams::cos_min },
	{ "gi/weight_clamp", &RestirReuseValues::weight_clamp, &RestirFrameParams::w_clamp },
	{ "gi/sky_distance", &RestirReuseValues::sky_distance, &RestirFrameParams::sky_distance },
	{ "gi/dirty_margin", &RestirReuseValues::dirty_margin, &RestirFrameParams::dirty_margin },
};

float applied(const ReuseRow &p_row, double p_value) {
	RestirReuseValues values = restir_reuse_defaults();
	values.*(p_row.value) = p_value;
	RestirFrameParams params;
	restir_reuse_apply(values, params);
	return params.*(p_row.field);
}

float expected(const ReuseRow &p_row, double p_value) {
	if (p_row.field == &RestirFrameParams::cos_normal) {
		return float(Math::cos(Math::deg_to_rad(p_value)));
	}
	return float(p_value);
}

TEST_CASE("[RestirSettings] The reuse limit defaults are the frame constants, bit for bit") {
	const RestirFrameParams constants;
	RestirFrameParams params;
	for (const ReuseRow &row : REUSE_ROWS) {
		params.*(row.field) = -1.0f;
	}
	restir_reuse_apply(restir_reuse_defaults(), params);
	for (const ReuseRow &row : REUSE_ROWS) {
		CHECK_MESSAGE(params.*(row.field) == constants.*(row.field), row.name);
	}
	CHECK(constants.tol_plane == 0.02f);
	CHECK(constants.cos_normal == 0.9063078f);
	CHECK(constants.class_tol == 0.35f);
	CHECK(constants.jac_max == 8.0f);
	CHECK(constants.d_min == 0.03f);
	CHECK(constants.cos_min == 0.05f);
	CHECK(constants.w_clamp == 20.0f);
	CHECK(constants.sky_distance == 1000.0f);
	CHECK(constants.dirty_margin == 0.01f);
}

TEST_CASE("[RestirSettings] Each reuse limit is held to its hinted range and its default lies inside") {
	const RestirReuseValues defaults = restir_reuse_defaults();
	for (const ReuseRow &row : REUSE_ROWS) {
		double low = 0.0;
		double high = 0.0;
		REQUIRE_MESSAGE(restir_setting_range(row.name, low, high), row.name);
		CHECK_MESSAGE(low < high, row.name);
		CHECK_MESSAGE(defaults.*(row.value) >= low, row.name);
		CHECK_MESSAGE(defaults.*(row.value) <= high, row.name);
		const float at_low = applied(row, low);
		const float at_high = applied(row, high);
		const float inside = applied(row, (low + high) * 0.5);
		CHECK_MESSAGE(at_low == expected(row, low), row.name);
		CHECK_MESSAGE(at_high == expected(row, high), row.name);
		CHECK_MESSAGE(inside == expected(row, (low + high) * 0.5), row.name);
		CHECK_MESSAGE(at_low != at_high, row.name);
		CHECK_MESSAGE(inside != at_low, row.name);
		CHECK_MESSAGE(inside != at_high, row.name);
		CHECK_MESSAGE(applied(row, low - (high - low)) == at_low, row.name);
		CHECK_MESSAGE(applied(row, high + (high - low)) == at_high, row.name);
	}
}

TEST_CASE("[RestirSettings] The normal angle is given in degrees and stored as a cosine") {
	const ReuseRow &row = REUSE_ROWS[1];
	CHECK(applied(row, 25.0) == 0.9063078f);
	CHECK(applied(row, 60.0) == doctest::Approx(0.5).epsilon(1e-6));
	CHECK(applied(row, 90.0) == doctest::Approx(0.0).epsilon(1e-6));
	CHECK(applied(row, 10.0) > applied(row, 25.0));
	CHECK(applied(row, 0.0) == applied(row, 1.0));
	CHECK(applied(row, 95.0) == applied(row, 90.0));
}

TEST_CASE("[RestirSettings] A choice setting names a value outside its list") {
	CHECK(restir_setting_invalid_choice("gi/mode", "B2").is_empty());
	CHECK(restir_setting_invalid_choice("gi/mode", "U1").is_empty());
	CHECK(restir_setting_invalid_choice("gi/revalidate", "share").is_empty());
	CHECK(restir_setting_invalid_choice("gi/denoise", "r2").is_empty());
	const String bad = restir_setting_invalid_choice("gi/mode", "b2");
	CHECK(bad.contains("restir/gi/mode=b2"));
	CHECK(bad.contains("B1,B2,B2k,U1"));
	CHECK_FALSE(restir_setting_invalid_choice("gi/denoise", "").is_empty());
	CHECK(restir_setting_invalid_choice("gi/spatial_radius", "anything").is_empty());
	CHECK(restir_setting_invalid_choice("gi/no_such_setting", "anything").is_empty());
}

}
