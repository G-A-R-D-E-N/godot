/**************************************************************************/
/*  test_ray_scene_lights.cpp                                             */
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

TEST_FORCE_LINK(test_ray_scene_lights)

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/environment/ray_scene.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"

namespace TestRaySceneLights {

using RendererRD::LightStorage;
using RendererRD::RayScene;

constexpr float ENERGY = 3.0f;
constexpr float INTENSITY = 1200.0f;
constexpr float EXPOSURE = 0.37f;
constexpr float CHANNEL = 0.8f;
const Vector2 SIZE(0.5f, 0.25f);

float raster_area_color(bool p_physical, bool p_normalize) {
	float energy = LightStorage::light_render_energy(ENERGY, INTENSITY, RSE::LIGHT_AREA, p_physical);
	energy *= EXPOSURE;
	float color = CHANNEL * energy;
	if (p_normalize) {
		color /= SIZE.x * SIZE.y;
	}
	return color;
}

TEST_CASE("[RayScene] Area light energy matches the raster light colour") {
	for (const bool physical : { false, true }) {
		for (const bool normalize : { false, true }) {
			const float packed = RayScene::light_energy(ENERGY, INTENSITY, RSE::LIGHT_AREA, physical, EXPOSURE, SIZE, normalize) * CHANNEL;
			const float raster = raster_area_color(physical, normalize);
			INFO("physical ", physical, " normalize ", normalize, " packed ", packed, " raster ", raster);
			CHECK(Math::abs(packed / raster - 1.0f) < 1e-6f);
		}
	}
}

TEST_CASE("[RayScene] Area light energy known answers") {
	const double area = double(SIZE.x) * SIZE.y;
	CHECK(RayScene::light_energy(1.0f, 1000.0f, RSE::LIGHT_AREA, true, 1.0f, SIZE, true) == doctest::Approx(1000.0 / (2.0 * Math::PI) / area).epsilon(1e-6));
	CHECK(RayScene::light_energy(1.0f, 1000.0f, RSE::LIGHT_AREA, true, 1.0f, SIZE, false) == doctest::Approx(1000.0 / (2.0 * Math::PI)).epsilon(1e-6));
	CHECK(RayScene::light_energy(1.0f, 1000.0f, RSE::LIGHT_AREA, false, 1.0f, SIZE, true) == doctest::Approx(Math::PI / area).epsilon(1e-6));
	CHECK(RayScene::light_energy(1.0f, 1000.0f, RSE::LIGHT_AREA, false, 1.0f, SIZE, false) == doctest::Approx(Math::PI).epsilon(1e-6));
}

TEST_CASE("[RayScene] Point, spot and directional energy is unchanged") {
	CHECK(RayScene::light_energy(1.0f, 1000.0f, RSE::LIGHT_OMNI, true, 1.0f, Vector2(), false) == float(1000.0f * (1.0 / (Math::PI * 4.0))));
	CHECK(RayScene::light_energy(1.0f, 1000.0f, RSE::LIGHT_SPOT, true, 1.0f, Vector2(), false) == float(1000.0f * (1.0 / Math::PI)));
	CHECK(RayScene::light_energy(1.0f, 1000.0f, RSE::LIGHT_DIRECTIONAL, true, 1.0f, Vector2(), false) == 1000.0f);
	CHECK(RayScene::light_energy(2.0f, 1000.0f, RSE::LIGHT_OMNI, false, 1.0f, SIZE, true) == float(2.0f * Math::PI));
}

TEST_CASE("[RayScene] Area light half axes ignore node scale") {
	const Basis basis = Basis(Vector3(0, 1, 0), 0.7) * Basis::from_scale(Vector3(3.0, 2.0, 5.0));
	RayScene::Light light = {};
	RayScene::pack_area(basis, SIZE, light);
	const Vector3 half_a(light.half_a[0], light.half_a[1], light.half_a[2]);
	const Vector3 half_b(light.half_b[0], light.half_b[1], light.half_b[2]);
	CHECK(half_a.length() == doctest::Approx(0.25));
	CHECK(half_b.length() == doctest::Approx(0.125));
	CHECK(half_a.normalized().is_equal_approx(basis.get_column(Vector3::AXIS_X).normalized()));
	CHECK(half_b.normalized().is_equal_approx(basis.get_column(Vector3::AXIS_Y).normalized()));
	CHECK(light.area == doctest::Approx(0.125));
}

} // namespace TestRaySceneLights

#endif
