/**************************************************************************/
/*  test_fog_precision.cpp                                                */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_fog_precision)

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/environment/fog_precision.h"

#include <cmath>

namespace TestFogPrecision {

using namespace RendererRD::FogPrecision;

static float half_step() {
	return 0.5f / DENSITY_SCALE;
}

static uint32_t stack_density(float p_density, int p_count) {
	uint32_t word = 0;
	for (int i = 0; i < p_count; i++) {
		word += uint32_t(quantize_density(p_density));
	}
	return word;
}

static uint32_t stack_color(float p_amount, int p_count) {
	uint32_t word = 0;
	for (int i = 0; i < p_count; i++) {
		word += quantize_color(p_amount);
	}
	return word;
}

static float perspective_corner_scale(float p_vertical_fov_degrees, float p_aspect, float p_near, float p_fog_end, const Vector2 &p_corner = Vector2(1.0f, -1.0f)) {
	const float t = Math::tan(Math::deg_to_rad(p_vertical_fov_degrees) * 0.5f);
	const Vector2 begin = Vector2(p_aspect * t, t) * p_near;
	const Vector2 end = Vector2(p_aspect * t, t) * p_fog_end;
	return path_scale(p_corner, begin, end, p_fog_end);
}

TEST_CASE("[FogPrecision] Indoor dust densities round trip within half a step") {
	const float densities[] = { 1e-6f, 1e-3f, 1.5e-3f, 3e-3f, 1e-2f, 2.0f };
	for (float density : densities) {
		const float decoded = decode_density(uint32_t(quantize_density(density)));
		CHECK(Math::abs(decoded - density) <= half_step());
		const float negative = decode_density(uint32_t(quantize_density(-density)));
		CHECK(negative == -decoded);
	}
	CHECK(quantize_density(1.5e-3f) == 1573);
	CHECK(quantize_density(1e-3f) == 1049);
	CHECK(quantize_density(-1.5e-3f) == -1573);
}

TEST_CASE("[FogPrecision] Ten overlapped volumes sum without loss") {
	const uint32_t word = stack_density(1.5e-3f, 10);
	CHECK(word == 15730u);
	CHECK(decode_density(word) == doctest::Approx(0.0150013).epsilon(1e-6));
}

TEST_CASE("[FogPrecision] 63 volumes at the density limit fit and a 64th wraps") {
	CHECK(quantize_density(40.0f) == (1 << 25));
	CHECK(quantize_density(-40.0f) == -(1 << 25));
	const uint32_t fits = stack_density(40.0f, 63);
	CHECK(total_density(0.0f, fits) == 2016.0f);
	const uint32_t wraps = fits + uint32_t(quantize_density(40.0f));
	CHECK(total_density(0.0f, wraps) == 0.0f);
}

TEST_CASE("[FogPrecision] NaN, zero and infinite densities") {
	CHECK(quantize_density(float(Math::NaN)) == 0);
	CHECK(quantize_density(0.0f) == 0);
	CHECK(quantize_density(-0.0f) == 0);
	CHECK(quantize_density(float(Math::INF)) == (1 << 25));
	CHECK(quantize_density(float(-Math::INF)) == -(1 << 25));
	CHECK(quantize_color(float(Math::NaN)) == 0u);
	CHECK(quantize_color(-1.0f) == 0u);
}

TEST_CASE("[FogPrecision] Colour words keep albedo above one out and clip emission at the colour limit") {
	CHECK(scattering_amount(1.5f, 2.0f) == 2.0f);
	CHECK(scattering_amount(-0.5f, 2.0f) == 0.0f);
	CHECK(scattering_amount(0.9f, -1.0f) == 0.0f);
	CHECK(emission_amount(-1.0f, 1.0f) == 0.0f);
	CHECK(quantize_color(emission_amount(100.0f, 1.0f)) == (1u << 26));
	CHECK(quantize_color(emission_amount(1e4f, 1.0f)) == (1u << 26));
	CHECK(decode_color(stack_color(emission_amount(100.0f, 1.0f), 15)) == 960.0f);
	const uint32_t fits = stack_color(COLOR_LIMIT, 63);
	CHECK(decode_color(fits) == 4032.0f);
	CHECK(fits + quantize_color(COLOR_LIMIT) == 0u);
}

TEST_CASE("[FogPrecision] Dust at 3e-3 with albedo 0.9 keeps all three channels") {
	const uint32_t word = quantize_color(scattering_amount(0.9f, 3e-3f));
	CHECK(Math::abs(decode_color(word) - 0.9f * 3e-3f) <= half_step());
}

TEST_CASE("[FogPrecision] A carve-out removes the medium's light in proportion") {
	{
		const uint32_t density_word = uint32_t(quantize_density(1.0f)) + uint32_t(quantize_density(-1.0f));
		const uint32_t positive_word = positive_density_word(quantize_density(1.0f));
		const float emission = decode_color(quantize_color(emission_amount(2.0f, 1.0f)));
		const float total = total_density(0.0f, density_word);
		const float f = carve_fraction(0.0f, total, decode_color(positive_word));
		CHECK(total == 0.0f);
		CHECK(emission == 2.0f);
		CHECK(emission * f == 0.0f);
	}
	{
		const int32_t dust = quantize_density(2.0f);
		const int32_t lit = quantize_density(1.0f);
		const int32_t carve = quantize_density(-1.0f);
		const uint32_t density_word = uint32_t(dust) + uint32_t(lit) + uint32_t(carve);
		const uint32_t positive_word = positive_density_word(dust) + positive_density_word(lit) + positive_density_word(carve);
		const float scattering = decode_color(quantize_color(scattering_amount(0.9f, 1.0f)));
		const float total = total_density(0.0f, density_word);
		const float f = carve_fraction(0.0f, total, decode_color(positive_word));
		CHECK(f == doctest::Approx(2.0 / 3.0).epsilon(1e-6));
		CHECK(scattering * f / total == doctest::Approx(scattering / 3.0f).epsilon(1e-6));
	}
	{
		const float base = 0.01f;
		const int32_t a = quantize_density(0.02f);
		const int32_t b = quantize_density(0.003f);
		const uint32_t density_word = uint32_t(a) + uint32_t(b);
		const uint32_t positive_word = positive_density_word(a) + positive_density_word(b);
		const float total = total_density(base, density_word);
		CHECK(carve_fraction(base, total, decode_color(positive_word)) == 1.0f);
	}
	{
		const float base = 0.01f;
		const int32_t carve = quantize_density(-0.005f);
		const float total = total_density(base, uint32_t(carve));
		CHECK(carve_fraction(base, total, decode_color(positive_density_word(carve))) == doctest::Approx(0.5).epsilon(1e-4));
	}
}

TEST_CASE("[FogPrecision] A single rounding remnant gets no light and two steps do") {
	CHECK_FALSE(gets_light(decode_density(1u)));
	CHECK(gets_light(decode_density(2u)));
	CHECK(gets_light(1e-3f));
	CHECK_FALSE(gets_light(0.0f));
}

TEST_CASE("[FogPrecision] The path scale matches the view ray at the corners") {
	const float aspect = 16.0f / 9.0f;
	CHECK(perspective_corner_scale(50.0f, aspect, 1e-6f, 64.0f) == doctest::Approx(1.380).epsilon(1e-3));
	CHECK(perspective_corner_scale(60.0f, aspect, 1e-6f, 64.0f) == doctest::Approx(1.545).epsilon(1e-3));
	CHECK(perspective_corner_scale(75.0f, aspect, 1e-6f, 64.0f) == doctest::Approx(1.857).epsilon(1e-3));
	const Vector2 corners[] = { Vector2(1.0f, 1.0f), Vector2(-1.0f, 1.0f), Vector2(-1.0f, -1.0f) };
	for (const Vector2 &corner : corners) {
		CHECK(perspective_corner_scale(60.0f, aspect, 1e-6f, 64.0f, corner) == perspective_corner_scale(60.0f, aspect, 1e-6f, 64.0f));
	}
	const float t = Math::tan(Math::deg_to_rad(60.0f) * 0.5f);
	const float near_scale = perspective_corner_scale(60.0f, aspect, 0.05f, 64.0f);
	const float shrink = (64.0f - 0.05f) / 64.0f;
	CHECK(near_scale == doctest::Approx(Math::sqrt(1.0f + shrink * shrink * t * t * (aspect * aspect + 1.0f))).epsilon(1e-6));
	const Vector2 half = Vector2(8.0f, 4.5f);
	CHECK(path_scale(Vector2(1.0f, 1.0f), half, half, 20.0f) == 1.0f);
	CHECK(path_scale(Vector2(column_ndc(40, 81), column_ndc(25, 51)), Vector2(0.05f, 0.03f), Vector2(60.0f, 34.0f), 64.0f) == 1.0f);
	CHECK(column_ndc(80, 81) == doctest::Approx(0.98765).epsilon(1e-4));
	CHECK(column_ndc(49, 50) == doctest::Approx(0.98).epsilon(1e-6));
}

TEST_CASE("[FogPrecision] The in-scatter weight matches the exact integral from thin dust to thick fog") {
	const float steps[] = { 0.0039f, 0.5f, 1.0f, 4.0f };
	for (float step : steps) {
		for (int e = -90; e <= 10; e++) {
			const float extinction = float(std::pow(10.0, e / 10.0));
			const double tau = double(step) * double(extinction);
			const double reference = -std::expm1(-tau) / double(extinction);
			const float weight = in_scatter_weight(step, extinction);
			CHECK(std::abs(double(weight) - reference) <= 4e-6 * reference);
		}
	}
	const float switch_extinction = SERIES_SWITCH / 0.5f;
	const float below = in_scatter_weight(0.5f, switch_extinction);
	const float above = in_scatter_weight(0.5f, std::nextafter(switch_extinction, 1.0f));
	CHECK(std::abs(double(above) - double(below)) <= 4e-6 * double(below));
	volatile float zero_extinction = 0.0f;
	CHECK(in_scatter_weight(1.0f, zero_extinction) == 0.0f);
	CHECK(in_scatter_weight(1.0f, -1.0f) == 0.0f);
	CHECK(in_scatter_weight(1.0f, float(Math::NaN)) == 0.0f);
	CHECK(in_scatter_weight(0.0f, 1.0f) == 0.0f);
}

} // namespace TestFogPrecision

#endif
