/**************************************************************************/
/*  test_fog_ddgi.cpp                                                     */
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

TEST_FORCE_LINK(test_fog_ddgi)

#ifdef RD_ENABLED

#include "core/math/random_pcg.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/environment/fog_ddgi.h"

namespace TestFogDDGI {

using namespace RendererRD;

static constexpr uint32_t MAX_TEST_RAYS = 256;
static constexpr uint32_t DENSE_RAYS = 20000;
static constexpr uint32_t FIELD_TERMS = 20;

static const double FIELD_MEANS[FIELD_TERMS] = { 1.0, 0.0, 0.0, 0.0, 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

static double field_value(const double p_coefficients[FIELD_TERMS], const double p_direction[3]) {
	const double x = p_direction[0];
	const double y = p_direction[1];
	const double z = p_direction[2];
	const double terms[FIELD_TERMS] = { 1.0, x, y, z, x * x, y * y, z * z, x * y, y * z, z * x, x * x * x, y * y * y, z * z * z, x * x * y, x * x * z, y * y * x, y * y * z, z * z * x, z * z * y, x * y * z };
	double value = 0.0;
	for (uint32_t i = 0; i < FIELD_TERMS; i++) {
		value += p_coefficients[i] * terms[i];
	}
	return value;
}

static double field_mean(const double p_coefficients[FIELD_TERMS]) {
	double mean = 0.0;
	for (uint32_t i = 0; i < FIELD_TERMS; i++) {
		mean += p_coefficients[i] * FIELD_MEANS[i];
	}
	return mean;
}

static void random_field(RandomPCG &p_rng, double r_coefficients[FIELD_TERMS]) {
	for (uint32_t i = 0; i < FIELD_TERMS; i++) {
		r_coefficients[i] = p_rng.randd() * 2.0 - 1.0;
	}
	r_coefficients[0] = 3.0;
}

static double field_spread(const double p_coefficients[FIELD_TERMS]) {
	const double mean = field_mean(p_coefficients);
	double spread = 0.0;
	for (uint32_t i = 0; i < DENSE_RAYS; i++) {
		double direction[3];
		fog_ddgi_ray_direction<double>(i, DENSE_RAYS, nullptr, direction);
		spread = MAX(spread, Math::abs(field_value(p_coefficients, direction) - mean));
	}
	return spread;
}

static void random_rotation(RandomPCG &p_rng, double r_rotation[3][3]) {
	const double u1 = p_rng.randd();
	const double u2 = p_rng.randd() * Math::TAU;
	const double u3 = p_rng.randd() * Math::TAU;
	const double x = Math::sqrt(1.0 - u1) * Math::sin(u2);
	const double y = Math::sqrt(1.0 - u1) * Math::cos(u2);
	const double z = Math::sqrt(u1) * Math::sin(u3);
	const double w = Math::sqrt(u1) * Math::cos(u3);
	r_rotation[0][0] = 1.0 - 2.0 * (y * y + z * z);
	r_rotation[0][1] = 2.0 * (x * y - z * w);
	r_rotation[0][2] = 2.0 * (x * z + y * w);
	r_rotation[1][0] = 2.0 * (x * y + z * w);
	r_rotation[1][1] = 1.0 - 2.0 * (x * x + z * z);
	r_rotation[1][2] = 2.0 * (y * z - x * w);
	r_rotation[2][0] = 2.0 * (x * z - y * w);
	r_rotation[2][1] = 2.0 * (y * z + x * w);
	r_rotation[2][2] = 1.0 - 2.0 * (x * x + y * y);
}

template <typename T>
static void make_directions(uint32_t p_count, const T *p_rotation, T r_directions[][3]) {
	for (uint32_t i = 0; i < p_count; i++) {
		fog_ddgi_ray_direction<T>(i, p_count, p_rotation, r_directions[i]);
	}
}

template <typename T>
static T length3(const T p_vector[3]) {
	return Math::sqrt(p_vector[0] * p_vector[0] + p_vector[1] * p_vector[1] + p_vector[2] * p_vector[2]);
}

static float henyey_greenstein(float p_cos_theta, float p_g) {
	const float k = 0.0795774715459f;
	return k * (1.0f - p_g * p_g) / Math::pow(1.0f + p_g * p_g - 2.0f * p_g * p_cos_theta, 1.5f);
}

static float mix_value(float p_a, float p_b, float p_t) {
	return p_a * (1.0f - p_t) + p_b * p_t;
}

TEST_CASE("[FogDDGI] The GI source never counts GI twice and otherwise keeps the stock SDFGI and VoxelGI pair") {
	const float injects[] = { 0.0f, 5e-5f, 1e-4f, 1.5e-4f, 5e-4f, 1e-3f, 1.5e-3f, 1.0f };
	const uint32_t counts[] = { 0, 1, 8 };
	for (uint32_t bits = 0; bits < 32; bits++) {
		const bool setting_on = (bits & 1) != 0;
		const bool ddgi_valid = (bits & 2) != 0;
		const bool shader_ok = (bits & 4) != 0;
		const bool sdfgi_enabled = (bits & 8) != 0;
		const bool sdfgi_valid = (bits & 16) != 0;
		for (const float inject : injects) {
			for (const uint32_t count : counts) {
				const FogGIChoice choice = fog_gi_source(inject, setting_on, ddgi_valid, shader_ok, sdfgi_enabled, sdfgi_valid, count);
				CHECK_FALSE((choice.use_ddgi && choice.bind_sdfgi));
				CHECK(choice.use_ddgi == (setting_on && ddgi_valid && shader_ok && inject > 0.0001));
				if (choice.use_ddgi) {
					CHECK(choice.voxel_gi_count == 0);
				} else {
					CHECK(choice.bind_sdfgi == (inject > 0.0001 && sdfgi_enabled && sdfgi_valid));
					CHECK(choice.voxel_gi_count == (inject > 0.001 ? count : 0u));
				}
			}
		}
	}

	CHECK(double(1e-4f) < 0.0001);
	CHECK(double(1e-3f) > 0.001);
	CHECK_FALSE(fog_gi_source(1e-4f, true, true, true, true, true, 8).use_ddgi);
	CHECK_FALSE(fog_gi_source(1e-4f, true, true, true, true, true, 8).bind_sdfgi);
	CHECK(fog_gi_source(1e-4f, true, true, true, true, true, 8).voxel_gi_count == 0);
	CHECK(fog_gi_source(1.5e-4f, true, true, true, true, true, 8).use_ddgi);
	CHECK(fog_gi_source(1.5e-4f, false, true, true, true, true, 8).bind_sdfgi);
	CHECK(fog_gi_source(1.5e-4f, false, true, true, true, true, 8).voxel_gi_count == 0);
	CHECK(fog_gi_source(5e-4f, true, true, false, true, true, 8).bind_sdfgi);
	CHECK(fog_gi_source(5e-4f, true, true, false, true, true, 8).voxel_gi_count == 0);
	CHECK(fog_gi_source(1e-3f, false, false, false, false, false, 8).voxel_gi_count == 8);
	CHECK(fog_gi_source(1e-3f, true, true, true, true, true, 8).voxel_gi_count == 0);
}

TEST_CASE("[FogDDGI] The ambient split adds to one and keeps the stock ambient where DDGI has no coverage") {
	const float alphas[] = { 0.0f, 0.25f, 0.5f, 0.999f, 1.0f };
	for (const float alpha : alphas) {
		CHECK(fog_ddgi_ambient_share(alpha) + alpha == doctest::Approx(1.0f).epsilon(1e-6));
	}
	const float ambient_colors[] = { 0.37f, 1.0f / 3.0f, 12.5f, 0.0f };
	const float ambient_inject = 1.3f;
	for (const float ambient : ambient_colors) {
		const float stock = ambient * ambient_inject;
		CHECK(ambient * ambient_inject * fog_ddgi_ambient_share(0.0f) == stock);
	}
}

TEST_CASE("[FogDDGI] The ray mean of the blend matches the field mean") {
	const uint32_t ray_counts[] = { 32, 64, 128, 256 };
	static double radiance[MAX_TEST_RAYS][4];
	static double directions[MAX_TEST_RAYS][3];

	for (const uint32_t n : ray_counts) {
		make_directions<double>(n, nullptr, directions);
		for (uint32_t r = 0; r < n; r++) {
			radiance[r][0] = 2.5;
			radiance[r][1] = 0.125;
			radiance[r][2] = 7.0;
			radiance[r][3] = 1.0;
		}
		const FogDDGIRayMoments<double> constant = fog_ddgi_ray_moments<double>(&radiance[0][0], &directions[0][0], n);
		CHECK(constant.count == n);
		CHECK(Math::abs(constant.mean[0] - 2.5) <= 1e-12 * 2.5);
		CHECK(Math::abs(constant.mean[1] - 0.125) <= 1e-12 * 0.125);
		CHECK(Math::abs(constant.mean[2] - 7.0) <= 1e-12 * 7.0);

		const double a = 1.75;
		const double b = -0.6;
		for (uint32_t r = 0; r < n; r++) {
			radiance[r][0] = a + b * directions[r][2];
			radiance[r][1] = radiance[r][0];
			radiance[r][2] = radiance[r][0];
		}
		const FogDDGIRayMoments<double> dipole = fog_ddgi_ray_moments<double>(&radiance[0][0], &directions[0][0], n);
		CHECK(Math::abs(dipole.mean[0] - a) <= 1e-12 * (Math::abs(a) + Math::abs(b)));
	}

	{
		static float radiance_f[MAX_TEST_RAYS][4];
		static float directions_f[MAX_TEST_RAYS][3];
		const float a = 1.75f;
		const float b = -0.6f;
		for (const uint32_t n : ray_counts) {
			make_directions<float>(n, nullptr, directions_f);
			for (uint32_t r = 0; r < n; r++) {
				radiance_f[r][0] = a + b * directions_f[r][2];
				radiance_f[r][1] = radiance_f[r][0];
				radiance_f[r][2] = radiance_f[r][0];
				radiance_f[r][3] = 1.0f;
			}
			const FogDDGIRayMoments<float> dipole = fog_ddgi_ray_moments<float>(&radiance_f[0][0], &directions_f[0][0], n);
			CHECK(Math::abs(dipole.mean[0] - a) <= 1e-5f * (Math::abs(a) + Math::abs(b)));
		}
	}

	{
		RandomPCG rng(1234);
		double coefficients[FIELD_TERMS];
		random_field(rng, coefficients);
		const double mean = field_mean(coefficients);
		const double spread = field_spread(coefficients);
		double previous_rms = 0.0;
		for (uint32_t k = 0; k < 4; k++) {
			const uint32_t n = ray_counts[k];
			RandomPCG rotation_rng(5678);
			double worst = 0.0;
			double square_sum = 0.0;
			for (uint32_t trial = 0; trial < 1000; trial++) {
				double rotation[3][3];
				random_rotation(rotation_rng, rotation);
				make_directions<double>(n, &rotation[0][0], directions);
				for (uint32_t r = 0; r < n; r++) {
					radiance[r][0] = field_value(coefficients, directions[r]);
					radiance[r][1] = radiance[r][0];
					radiance[r][2] = radiance[r][0];
					radiance[r][3] = 1.0;
				}
				const double error = Math::abs(fog_ddgi_ray_moments<double>(&radiance[0][0], &directions[0][0], n).mean[0] - mean);
				worst = MAX(worst, error);
				square_sum += error * error;
			}
			const double rms = Math::sqrt(square_sum / 1000.0);
			CHECK(worst <= 4.0 / double(n) * spread);
			if (k > 0) {
				CHECK(rms < previous_rms);
			}
			previous_rms = rms;
		}
	}

	{
		const uint32_t n = 128;
		make_directions<double>(n, nullptr, directions);
		for (uint32_t r = 0; r < n; r++) {
			const bool back_face = r % 5 == 0;
			radiance[r][0] = back_face ? 1000.0 : 1.0;
			radiance[r][1] = radiance[r][0];
			radiance[r][2] = radiance[r][0];
			radiance[r][3] = back_face ? -0.5 : 2.0;
		}
		const FogDDGIRayMoments<double> moments = fog_ddgi_ray_moments<double>(&radiance[0][0], &directions[0][0], n);
		CHECK(moments.count == n - (n + 4) / 5);
		CHECK(moments.mean[0] == doctest::Approx(1.0).epsilon(1e-12));
	}
}

TEST_CASE("[FogDDGI] The first moment is bounded by the mean, and its ray-set bias is small") {
	static float radiance[MAX_TEST_RAYS][4];
	static float directions[MAX_TEST_RAYS][3];
	RandomPCG rng(42);
	for (uint32_t trial = 0; trial < 200; trial++) {
		double rotation_d[3][3];
		random_rotation(rng, rotation_d);
		float rotation[3][3];
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				rotation[i][j] = float(rotation_d[i][j]);
			}
		}
		const uint32_t n = 32 + (trial % 7) * 32;
		make_directions<float>(n, &rotation[0][0], directions);
		for (uint32_t r = 0; r < n; r++) {
			radiance[r][0] = rng.randf() * 4.0f;
			radiance[r][1] = rng.randf();
			radiance[r][2] = rng.randf() < 0.3f ? 0.0f : rng.randf() * 100.0f;
			radiance[r][3] = rng.randf() < 0.1f ? -1.0f : 1.0f;
		}
		const FogDDGIRayMoments<float> moments = fog_ddgi_ray_moments<float>(&radiance[0][0], &directions[0][0], n);
		for (int c = 0; c < 3; c++) {
			CHECK(length3<float>(moments.moment[c]) <= moments.mean[c] * (1.0f + 1e-6f));
		}
	}

	{
		const uint32_t n = 128;
		make_directions<float>(n, nullptr, directions);
		for (uint32_t r = 0; r < n; r++) {
			radiance[r][0] = r == 77 ? 3.0f : 0.0f;
			radiance[r][1] = r == 77 ? 0.5f : 0.0f;
			radiance[r][2] = r == 77 ? 9.0f : 0.0f;
			radiance[r][3] = 1.0f;
		}
		const FogDDGIRayMoments<float> moments = fog_ddgi_ray_moments<float>(&radiance[0][0], &directions[0][0], n);
		for (int c = 0; c < 3; c++) {
			CHECK(length3<float>(moments.moment[c]) == doctest::Approx(moments.mean[c]).epsilon(1e-6));
		}
	}

	{
		static double radiance_d[MAX_TEST_RAYS][4];
		static double directions_d[MAX_TEST_RAYS][3];
		for (uint32_t n = 32; n <= 256; n++) {
			make_directions<double>(n, nullptr, directions_d);
			for (uint32_t r = 0; r < n; r++) {
				radiance_d[r][0] = 1.0;
				radiance_d[r][1] = 1.0;
				radiance_d[r][2] = 1.0;
				radiance_d[r][3] = 1.0;
			}
			const FogDDGIRayMoments<double> moments = fog_ddgi_ray_moments<double>(&radiance_d[0][0], &directions_d[0][0], n);
			CHECK(Math::abs(moments.moment[0][2]) <= 1e-12);
			CHECK(length3<double>(moments.moment[0]) <= 2.15 / double(n));
		}

		RandomPCG rotation_rng(99);
		const uint32_t rotated_counts[] = { 32, 64, 128, 256 };
		for (const uint32_t n : rotated_counts) {
			for (uint32_t trial = 0; trial < 1000; trial++) {
				double rotation[3][3];
				random_rotation(rotation_rng, rotation);
				make_directions<double>(n, &rotation[0][0], directions_d);
				for (uint32_t r = 0; r < n; r++) {
					radiance_d[r][0] = 1.0;
					radiance_d[r][1] = 1.0;
					radiance_d[r][2] = 1.0;
					radiance_d[r][3] = 1.0;
				}
				const FogDDGIRayMoments<double> moments = fog_ddgi_ray_moments<double>(&radiance_d[0][0], &directions_d[0][0], n);
				CHECK(length3<double>(moments.moment[0]) <= 2.15 / double(n));
			}
		}
	}
}

TEST_CASE("[FogDDGI] The rejected six-axis estimate has the stated narrow-source error and is exact to band 3") {
	const double axes[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	const double inv_sqrt3 = 1.0 / Math::sqrt(3.0);
	const double sources[2][3] = { { 1, 0, 0 }, { inv_sqrt3, inv_sqrt3, inv_sqrt3 } };
	const double expected[2] = { 2.0 / 3.0, 2.0 / Math::sqrt(3.0) };
	for (int s = 0; s < 2; s++) {
		double estimate = 0.0;
		for (int a = 0; a < 6; a++) {
			const double cosine = axes[a][0] * sources[s][0] + axes[a][1] * sources[s][1] + axes[a][2] * sources[s][2];
			estimate += MAX(0.0, cosine) / Math::PI / 6.0;
		}
		const double truth = 1.0 / (4.0 * Math::PI);
		CHECK(estimate / truth == doctest::Approx(expected[s]).epsilon(1e-12));
	}

	RandomPCG rng(7);
	double coefficients[FIELD_TERMS];
	random_field(rng, coefficients);
	double estimate = 0.0;
	for (int a = 0; a < 6; a++) {
		double irradiance = 0.0;
		for (uint32_t i = 0; i < DENSE_RAYS; i++) {
			double direction[3];
			fog_ddgi_ray_direction<double>(i, DENSE_RAYS, nullptr, direction);
			const double cosine = axes[a][0] * direction[0] + axes[a][1] * direction[1] + axes[a][2] * direction[2];
			irradiance += MAX(0.0, cosine) * field_value(coefficients, direction);
		}
		estimate += irradiance * 4.0 / double(DENSE_RAYS) / 6.0;
	}
	const double mean = field_mean(coefficients);
	CHECK(Math::abs(estimate - mean) <= 1e-5 * Math::abs(mean));
}

TEST_CASE("[FogDDGI] The mid-air visibility weight and level confidence") {
	CHECK(fog_ddgi_visibility(0.5f, 1.0f, 1.1f) == 1.0f);
	CHECK(fog_ddgi_visibility(1.0f, 1.0f, 1.1f) == 1.0f);

	float previous = 1.0f;
	for (int i = 1; i <= 400; i++) {
		const float weight = fog_ddgi_visibility(1.0f + float(i) * 0.025f, 1.0f, 1.25f);
		CHECK(weight <= previous);
		CHECK(weight > 0.0f);
		previous = weight;
	}

	CHECK(Math::abs(fog_ddgi_visibility(2.0f, 1.0f, 1.25f) - 2.5e-17f) <= 1e-5f * 2.5e-17f);
	{
		const float variance = 0.25f;
		const float distance = 1.0f + Math::sqrt(variance / 0.9f - variance);
		const float delta = distance - 1.0f;
		const float chebyshev = variance / (variance + delta * delta);
		const float cube = chebyshev * chebyshev * chebyshev;
		CHECK(cube >= 0.5f);
		CHECK(Math::abs(fog_ddgi_visibility(distance, 1.0f, 1.0f + variance) - cube) <= 1e-6f * cube);
	}
	{
		const float variance = 0.25f;
		const float target_chebyshev = Math::pow(0.45f, 1.0f / 3.0f);
		const float distance = 1.0f + Math::sqrt(variance / target_chebyshev - variance);
		const float delta = distance - 1.0f;
		const float chebyshev = variance / (variance + delta * delta);
		const float cube = chebyshev * chebyshev * chebyshev;
		const float crushed = 128.0f * Math::pow(cube, 8.0f);
		CHECK(cube < 0.5f);
		CHECK(crushed >= 0.2f);
		CHECK(Math::abs(fog_ddgi_visibility(distance, 1.0f, 1.0f + variance) - crushed) <= 1e-5f * crushed);
	}
	CHECK(128.0f * Math::pow(0.5f, 8.0f) == doctest::Approx(0.5f));
	CHECK(0.2f * 0.2f * 0.2f * 25.0f == doctest::Approx(0.2f));

	const float alpha[3] = { 0.3f, 0.6f, 0.2f };
	FogDDGICorner corners[8];
	for (int i = 0; i < 8; i++) {
		const int corner[3] = { i & 1, (i >> 1) & 1, (i >> 2) & 1 };
		float trilinear = 1.0f;
		for (int k = 0; k < 3; k++) {
			trilinear *= corner[k] ? alpha[k] : 1.0f - alpha[k];
		}
		corners[i].trilinear = trilinear;
		corners[i].visibility = 1.0f;
		corners[i].skipped = false;
	}

	FogDDGILevelWeight level = fog_ddgi_level_weight(corners, 8);
	CHECK(level.confidence == doctest::Approx(1.0f).epsilon(1e-6));
	CHECK(level.weight == doctest::Approx(1.0f).epsilon(1e-6));

	for (int i = 0; i < 8; i++) {
		corners[i].visibility = fog_ddgi_visibility(20.0f, 1.0f, 1.01f);
	}
	level = fog_ddgi_level_weight(corners, 8);
	CHECK(level.confidence < 1e-6f);
	CHECK(level.weight > 0.0f);

	for (int i = 0; i < 8; i++) {
		corners[i].visibility = 1.0f;
		corners[i].skipped = i == 0 || i == 3 || i == 6;
	}
	level = fog_ddgi_level_weight(corners, 8);
	CHECK(level.confidence == doctest::Approx(1.0f).epsilon(1e-6));
	CHECK(level.weight < 1.0f);

	SUBCASE("Visibility define off: every valid corner counts as visible") {
		float valid_trilinear = 0.0f;
		for (int i = 0; i < 8; i++) {
			corners[i].skipped = i == 1 || i == 4;
			corners[i].visibility = 1.0f;
			valid_trilinear += corners[i].skipped ? 0.0f : corners[i].trilinear;
		}
		const FogDDGILevelWeight off = fog_ddgi_level_weight(corners, 8);
		CHECK(off.confidence == doctest::Approx(1.0f).epsilon(1e-6));
		CHECK(off.weight == doctest::Approx(valid_trilinear).epsilon(1e-6));
	}

	for (int i = 0; i < 8; i++) {
		corners[i].skipped = i != 5;
		corners[i].visibility = i == 5 ? 1.0f : fog_ddgi_visibility(20.0f, 1.0f, 1.01f);
	}
	level = fog_ddgi_level_weight(corners, 8);
	CHECK(level.confidence == doctest::Approx(1.0f).epsilon(1e-6));

	for (int i = 0; i < 8; i++) {
		corners[i].skipped = true;
	}
	level = fog_ddgi_level_weight(corners, 8);
	CHECK(level.weight == 0.0f);
	CHECK(level.confidence == 0.0f);
}

TEST_CASE("[FogDDGI] The mean history follows the irradiance update count and keeps the moment bound") {
	const float hysteresis_values[] = { 0.0f, 0.5f, 0.97f, 0.999f };
	const float keeps[] = { 1.0f, 0.75f, 0.2f };
	for (const float hysteresis : hysteresis_values) {
		for (const float keep : keeps) {
			CHECK(fog_ddgi_mean_hysteresis(hysteresis, keep, 1.0f) == 0.0f);
			for (int n = 0; n <= 254; n++) {
				const float tile_alpha = fog_ddgi_tile_alpha(hysteresis, float(n));
				CHECK(fog_ddgi_mean_hysteresis(hysteresis, keep, tile_alpha) == fog_ddgi_irradiance_hysteresis(hysteresis, keep, float(n)));
			}
			const float capped = fog_ddgi_tile_alpha(hysteresis, 255.0f);
			CHECK(Math::abs(fog_ddgi_mean_hysteresis(hysteresis, keep, capped) - fog_ddgi_irradiance_hysteresis(hysteresis, keep, 255.0f)) <= 1.6e-5f);
		}
	}
	CHECK(fog_ddgi_mean_hysteresis(1.0f, 1.0f, 255.0f) == doctest::Approx(254.0f / 255.0f));
	CHECK(fog_ddgi_irradiance_hysteresis(1.0f, 1.0f, 255.0f) == doctest::Approx(255.0f / 256.0f));

	static float radiance[MAX_TEST_RAYS][4];
	static float directions[MAX_TEST_RAYS][3];
	RandomPCG rng(2026);
	float mean[3] = {};
	float moment[3][3] = {};
	float irradiance_alpha = 0.0f;
	const uint32_t n = 64;
	uint32_t halvings = 0;
	for (int step = 0; step < 200; step++) {
		double rotation_d[3][3];
		random_rotation(rng, rotation_d);
		float rotation[3][3];
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				rotation[i][j] = float(rotation_d[i][j]);
			}
		}
		make_directions<float>(n, &rotation[0][0], directions);
		const float scale = step % 17 == 0 ? 8.0f : 1.0f;
		for (uint32_t r = 0; r < n; r++) {
			radiance[r][0] = rng.randf() * scale;
			radiance[r][1] = rng.randf() * 0.5f * scale;
			radiance[r][2] = rng.randf() < 0.5f ? 0.0f : rng.randf() * 2.0f * scale;
			radiance[r][3] = rng.randf() < 0.1f ? -1.0f : 1.0f;
		}
		const FogDDGIRayMoments<float> rays = fog_ddgi_ray_moments<float>(&radiance[0][0], &directions[0][0], n);

		if (step == 50) {
			irradiance_alpha = 0.0f;
		}
		const float probe_hysteresis = rng.randf() < 0.1f ? 0.0f : 0.97f;
		const float keep = rng.randf() < 0.2f ? 0.25f + rng.randf() * 0.75f : 1.0f;
		const float tile_alpha = fog_ddgi_tile_alpha(probe_hysteresis, irradiance_alpha);
		irradiance_alpha = tile_alpha;

		float hysteresis = fog_ddgi_mean_hysteresis(probe_hysteresis, keep, tile_alpha);
		if (fog_ddgi_change_halves(rays.mean, mean, 0.25f)) {
			hysteresis *= 0.5f;
			halvings++;
		}
		for (int c = 0; c < 3; c++) {
			mean[c] = mix_value(rays.mean[c], mean[c], hysteresis);
			for (int k = 0; k < 3; k++) {
				moment[c][k] = mix_value(rays.moment[c][k], moment[c][k], hysteresis);
			}
		}

		for (int c = 0; c < 3; c++) {
			CHECK(length3<float>(moment[c]) <= mean[c] * (1.0f + 1e-5f) + 1e-30f);
		}
		if (step == 50) {
			CHECK(hysteresis == 0.0f);
			for (int c = 0; c < 3; c++) {
				CHECK(mean[c] == rays.mean[c]);
				for (int k = 0; k < 3; k++) {
					CHECK(moment[c][k] == rays.moment[c][k]);
				}
			}
		}
	}
	CHECK(halvings > 0);
}

TEST_CASE("[FogDDGI] The DDGI variant index and the directional pick") {
	bool used[8] = {};
	for (int fog_variant = 0; fog_variant < 4; fog_variant++) {
		for (uint32_t visibility = 0; visibility < 2; visibility++) {
			const int index = fog_ddgi_variant(fog_variant, visibility);
			CHECK(index >= 0);
			CHECK(index < 8);
			CHECK_FALSE(used[index]);
			used[index] = true;
		}
	}

	int listed = 0;
	int group = 0;
	for (int vk_memory_model = 0; vk_memory_model < 2; vk_memory_model++) {
		for (int no_atomics = 0; no_atomics < 2; no_atomics++) {
			const bool use_vulkan_memory_model = vk_memory_model != 0;
			const bool use_32_bit_atomics = no_atomics == 0;
			const int fog_variant = (use_vulkan_memory_model ? 2 : 0) + (use_32_bit_atomics ? 0 : 1);
			CHECK(fog_variant == group);
			for (uint32_t visibility = 0; visibility < 2; visibility++) {
				CHECK(fog_ddgi_variant(fog_variant, visibility) == listed);
				listed++;
			}
			group++;
		}
	}

	CHECK(fog_ddgi_directional(true, 0.0f, true) == 0);
	CHECK(fog_ddgi_directional(true, -0.0f, true) == 0);
	const float on_values[] = { 1e-30f, -0.5f, 0.9f };
	for (const float g : on_values) {
		CHECK(fog_ddgi_directional(true, g, true) == 1);
		CHECK(fog_ddgi_directional(false, g, true) == 0);
		CHECK(fog_ddgi_directional(true, g, false) == 0);
	}
}

TEST_CASE("[FogDDGI] The settings defaults") {
	CHECK(FOG_DDGI_DEFAULT_ENABLED);
	CHECK(FOG_DDGI_DEFAULT_VISIBILITY);
	CHECK_FALSE(FOG_DDGI_DEFAULT_DIRECTIONAL);
}

TEST_CASE("[FogDDGI] The clamped band-1 in-scatter of a single collimated source") {
	const float mean[3] = { 1.0f, 1.0f, 1.0f };
	const float moment[3][3] = { { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f } };
	float light[3];

	LocalVector<float> gs;
	for (int i = 0; i <= 19; i++) {
		gs.push_back(float(i) * 0.05f);
	}
	for (int i = 1; i <= 18; i++) {
		gs.push_back(-float(i) * 0.05f);
	}
	RandomPCG rng(314);
	for (const float g : gs) {
		for (int i = 0; i <= 40; i++) {
			const float c = -1.0f + float(i) * 0.05f;
			const float view[3] = { Math::sqrt(MAX(0.0f, 1.0f - c * c)), 0.0f, c };
			fog_ddgi_inscatter(mean, &moment[0][0], view, g, light);
			for (int k = 0; k < 3; k++) {
				CHECK(light[k] >= 0.0f);
				CHECK(light[k] == doctest::Approx(MAX(0.0f, 1.0f + 3.0f * g * c)).epsilon(1e-6));
			}
		}
		for (int i = 0; i < 1000; i++) {
			const float z = rng.randf() * 2.0f - 1.0f;
			const float phi = rng.randf() * float(Math::TAU);
			const float r = Math::sqrt(MAX(0.0f, 1.0f - z * z));
			const float view[3] = { r * Math::cos(phi), r * Math::sin(phi), z };
			fog_ddgi_inscatter(mean, &moment[0][0], view, g, light);
			CHECK(light[0] >= 0.0f);
			CHECK(Math::abs(light[0] - MAX(0.0f, 1.0f + 3.0f * g * z)) <= 1e-6f);
		}
	}

	struct Known {
		float g;
		float plus_one;
		float zero;
		float minus_half;
		float minus_one;
		float minus_one_unclamped;
	};
	const Known known[] = {
		{ 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
		{ 1.0f / 3.0f, 2.0f, 1.0f, 0.5f, 0.0f, 0.0f },
		{ 0.5f, 2.5f, 1.0f, 0.25f, 0.0f, -0.5f },
		{ 0.8f, 3.4f, 1.0f, 0.0f, 0.0f, -1.4f },
		{ 0.95f, 3.85f, 1.0f, 0.0f, 0.0f, -1.85f },
	};
	const float cosines[4] = { 1.0f, 0.0f, -0.5f, -1.0f };
	for (const Known &k : known) {
		const float expected[4] = { k.plus_one, k.zero, k.minus_half, k.minus_one };
		for (int i = 0; i < 4; i++) {
			const float c = cosines[i];
			const float view[3] = { Math::sqrt(MAX(0.0f, 1.0f - c * c)), 0.0f, c };
			fog_ddgi_inscatter(mean, &moment[0][0], view, k.g, light);
			CHECK(Math::abs(light[0] - expected[i]) <= 1e-6f);
		}
		CHECK(Math::abs((1.0f + 3.0f * k.g * -1.0f) - k.minus_one_unclamped) <= 1e-6f);
	}
	CHECK(3.0f * (1.0f / 3.0f) == 1.0f);
	{
		const float view[3] = { 0.0f, 0.0f, -1.0f };
		fog_ddgi_inscatter(mean, &moment[0][0], view, 1.0f / 3.0f, light);
		CHECK(Math::abs(light[0]) <= 1e-7f);
	}

	{
		static float radiance[MAX_TEST_RAYS][4];
		static float directions[MAX_TEST_RAYS][3];
		const uint32_t n = 128;
		make_directions<float>(n, nullptr, directions);
		for (uint32_t r = 0; r < n; r++) {
			const float value = r == 127 ? 128.0f : 0.0f;
			radiance[r][0] = value;
			radiance[r][1] = value;
			radiance[r][2] = value;
			radiance[r][3] = 1.0f;
		}
		const FogDDGIRayMoments<float> rays = fog_ddgi_ray_moments<float>(&radiance[0][0], &directions[0][0], n);
		CHECK(rays.mean[0] == 1.0f);
		for (int k = 0; k < 3; k++) {
			CHECK(rays.moment[0][k] == directions[127][k]);
		}
		CHECK(length3<float>(rays.moment[0]) == doctest::Approx(1.0f).epsilon(1e-6));
		const float toward[3] = { directions[127][0], directions[127][1], directions[127][2] };
		const float away[3] = { -directions[127][0], -directions[127][1], -directions[127][2] };
		for (const float g : gs) {
			fog_ddgi_inscatter(rays.mean, &rays.moment[0][0], toward, g, light);
			CHECK(light[0] >= 0.0f);
			CHECK(Math::abs(light[0] - MAX(0.0f, 1.0f + 3.0f * g)) <= 1e-6f);
			fog_ddgi_inscatter(rays.mean, &rays.moment[0][0], away, g, light);
			CHECK(light[0] >= 0.0f);
			CHECK(Math::abs(light[0] - MAX(0.0f, 1.0f - 3.0f * g)) <= 1e-6f);
		}
	}

	{
		const float big_mean[3] = { 0.75f, 2.0f, 0.0f };
		const float big_moment[3][3] = { { 65504.0f, 65504.0f, 65504.0f }, { -65504.0f, 65504.0f, -65504.0f }, { 65504.0f, -65504.0f, 65504.0f } };
		const float view[3] = { 0.6f, 0.0f, 0.8f };
		fog_ddgi_inscatter(big_mean, &big_moment[0][0], view, 0.0f, light);
		CHECK(light[0] == 0.75f);
		CHECK(light[1] == 2.0f);
		CHECK(light[2] == 0.0f);
	}
}

TEST_CASE("[FogDDGI] Looking toward a lit floor patch is brighter than looking away") {
	const float rotation[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, -1.0f, 0.0f } };
	static float radiance[MAX_TEST_RAYS][4];
	static float directions[MAX_TEST_RAYS][3];
	const uint32_t n = 128;
	make_directions<float>(n, &rotation[0][0], directions);
	for (uint32_t r = 0; r < n; r++) {
		const float value = r == 127 ? 128.0f : 0.0f;
		radiance[r][0] = value;
		radiance[r][1] = value;
		radiance[r][2] = value;
		radiance[r][3] = 1.0f;
	}
	CHECK(directions[127][1] == -0.9921875f);
	const FogDDGIRayMoments<float> rays = fog_ddgi_ray_moments<float>(&radiance[0][0], &directions[0][0], n);
	CHECK(rays.mean[0] == 1.0f);
	CHECK(rays.moment[0][1] == -0.9921875f);

	const float position_a[3] = { 0.0f, -2.0f, 0.0f };
	const float position_b[3] = { 0.0f, 1.0f, 0.0f };
	float view_a[3];
	float view_b[3];
	for (int k = 0; k < 3; k++) {
		view_a[k] = position_a[k] / length3<float>(position_a);
		view_b[k] = position_b[k] / length3<float>(position_b);
	}
	const float c_a = view_a[0] * rays.moment[0][0] + view_a[1] * rays.moment[0][1] + view_a[2] * rays.moment[0][2];
	const float c_b = view_b[0] * rays.moment[0][0] + view_b[1] * rays.moment[0][1] + view_b[2] * rays.moment[0][2];
	CHECK(c_a == 0.9921875f);
	CHECK(c_b == -0.9921875f);

	float light_a[3];
	float light_b[3];
	fog_ddgi_inscatter(rays.mean, &rays.moment[0][0], view_a, 0.3f, light_a);
	fog_ddgi_inscatter(rays.mean, &rays.moment[0][0], view_b, 0.3f, light_b);
	CHECK(Math::abs(light_a[0] - 1.89296875f) <= 1e-6f);
	CHECK(Math::abs(light_b[0] - 0.10703125f) <= 1e-6f);
	CHECK(light_a[0] / light_b[0] == doctest::Approx(17.69f).epsilon(1e-3));

	fog_ddgi_inscatter(rays.mean, &rays.moment[0][0], view_a, 0.0f, light_a);
	fog_ddgi_inscatter(rays.mean, &rays.moment[0][0], view_b, 0.0f, light_b);
	CHECK(light_a[0] == 1.0f);
	CHECK(light_b[0] == 1.0f);

	fog_ddgi_inscatter(rays.mean, &rays.moment[0][0], view_a, -0.3f, light_a);
	fog_ddgi_inscatter(rays.mean, &rays.moment[0][0], view_b, -0.3f, light_b);
	CHECK(Math::abs(light_a[0] - 0.10703125f) <= 1e-6f);
	CHECK(Math::abs(light_b[0] - 1.89296875f) <= 1e-6f);

	const float gs[] = { 0.1f, 0.3f, 0.6f, 0.9f, -0.1f, -0.3f, -0.6f, -0.9f };
	for (const float g : gs) {
		const bool stock_a_brighter = henyey_greenstein(c_a, g) > henyey_greenstein(c_b, g);
		CHECK(stock_a_brighter == (g > 0.0f));
		fog_ddgi_inscatter(rays.mean, &rays.moment[0][0], view_a, g, light_a);
		fog_ddgi_inscatter(rays.mean, &rays.moment[0][0], view_b, g, light_b);
		CHECK((light_a[0] > light_b[0]) == stock_a_brighter);
	}
}

} // namespace TestFogDDGI

#endif
