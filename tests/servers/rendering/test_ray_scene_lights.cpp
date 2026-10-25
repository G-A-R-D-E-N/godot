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

#include "servers/rendering/renderer_rd/environment/ddgi_light_tracker.h"
#include "servers/rendering/renderer_rd/environment/ray_scene.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"

namespace TestRaySceneLights {

using RendererRD::DDGILightTracker;
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

constexpr float RANGE = 5.0f;

RayScene::Light tracked_light(RSE::LightType p_type, uint32_t p_mask) {
	RayScene::Light light = {};
	light.type = uint32_t(p_type);
	light.energy = 1.0f;
	light.inv_radius = 1.0f / RANGE;
	light.caster_mask = p_mask;
	light.color[0] = light.color[1] = light.color[2] = 1.0f;
	return light;
}

RayScene::Light tracked_rect(float p_yaw, float p_roll, float p_half_a, uint32_t p_mask) {
	const Basis basis = Basis(Vector3(0, 1, 0), Math::deg_to_rad(p_yaw)) * Basis(Vector3(0, 0, 1), Math::deg_to_rad(p_roll));
	const Vector3 facing = basis.xform(Vector3(0, 0, -1));
	const Vector3 half_a = basis.xform(Vector3(p_half_a, 0, 0));
	const Vector3 half_b = basis.xform(Vector3(0, 0.125, 0));
	RayScene::Light light = tracked_light(RSE::LIGHT_AREA, p_mask);
	for (int k = 0; k < 3; k++) {
		light.direction[k] = float(facing[k]);
		light.half_a[k] = float(half_a[k]);
		light.half_b[k] = float(half_b[k]);
	}
	return light;
}

RayScene::Light tracked_omni(float p_x) {
	RayScene::Light light = tracked_light(RSE::LIGHT_OMNI, 0xFFFFFFFF);
	light.position[0] = p_x;
	return light;
}

uint32_t tracker_frame(uint64_t p_frame, const RayScene::Light &p_rect, const RayScene::Light *p_omni, LocalVector<DDGILightTracker::Box> &r_boxes) {
	DDGILightTracker::begin(true, p_frame, 1.0f);
	DDGILightTracker::observe(RID::from_uint64(1), p_rect, Vector3());
	if (p_omni) {
		DDGILightTracker::observe(RID::from_uint64(2), *p_omni, Vector3());
	}
	DDGILightTracker::end();
	DDGILightTracker::take(r_boxes);
	return r_boxes.size();
}

TEST_CASE("[RayScene] DDGI light tracker resets for a turned, rolled or resized rect, a caster mask change and a moved omni") {
	const float reach = RANGE + Math::sqrt(0.25f * 0.25f + 0.125f * 0.125f);
	const float wide_reach = RANGE + Math::sqrt(0.3f * 0.3f + 0.125f * 0.125f);
	LocalVector<DDGILightTracker::Box> boxes;
	DDGILightTracker::begin(false, 0, 1.0f);

	CHECK(tracker_frame(0, tracked_rect(0, 0, 0.25f, 0xFFFFFFFF), nullptr, boxes) == 0);
	CHECK(tracker_frame(8, tracked_rect(0, 0, 0.25f, 0xFFFFFFFF), nullptr, boxes) == 0);
	CHECK(tracker_frame(16, tracked_rect(5, 0, 0.25f, 0xFFFFFFFF), nullptr, boxes) == 0);

	REQUIRE(tracker_frame(24, tracked_rect(20, 0, 0.25f, 0xFFFFFFFF), nullptr, boxes) == 1);
	CHECK(boxes[0].center.is_zero_approx());
	CHECK(boxes[0].half == doctest::Approx(reach));

	CHECK(tracker_frame(25, tracked_rect(40, 0, 0.25f, 0xFFFFFFFF), nullptr, boxes) == 0);
	CHECK(tracker_frame(32, tracked_rect(40, 0, 0.25f, 0xFFFFFFFF), nullptr, boxes) == 1);
	CHECK(tracker_frame(40, tracked_rect(40, 90, 0.25f, 0xFFFFFFFF), nullptr, boxes) == 1);

	REQUIRE(tracker_frame(48, tracked_rect(40, 90, 0.3f, 0xFFFFFFFF), nullptr, boxes) == 2);
	CHECK(MIN(boxes[0].half, boxes[1].half) == doctest::Approx(reach));
	CHECK(MAX(boxes[0].half, boxes[1].half) == doctest::Approx(wide_reach));

	CHECK(tracker_frame(56, tracked_rect(40, 90, 0.3f, 0x1), nullptr, boxes) == 1);

	const RayScene::Light omni = tracked_omni(2.0f);
	REQUIRE(tracker_frame(64, tracked_rect(40, 90, 0.3f, 0x1), &omni, boxes) == 1);
	CHECK(boxes[0].center.is_equal_approx(Vector3(2, 0, 0)));
	CHECK(boxes[0].half == doctest::Approx(RANGE));

	const RayScene::Light moved = tracked_omni(2.5f);
	CHECK(tracker_frame(72, tracked_rect(60, 90, 0.3f, 0x1), &moved, boxes) == 3);

	DDGILightTracker::begin(false, 0, 1.0f);
}

} // namespace TestRaySceneLights

#endif
