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

#include "core/templates/sort_array.h"
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

bool half_axes_zero(const RayScene::Light &p_light) {
	for (int k = 0; k < 3; k++) {
		if (p_light.half_a[k] != 0.0f || p_light.half_b[k] != 0.0f) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[RayScene] Omni and spot carry the lamp size in area, area lights their rect, directional is untouched") {
	const Basis basis = Basis(Vector3(0, 1, 0), 0.7) * Basis::from_scale(Vector3(3.0, 2.0, 5.0));
	for (const RSE::LightType type : { RSE::LIGHT_OMNI, RSE::LIGHT_SPOT }) {
		INFO("type ", int(type));
		RayScene::Light light = {};
		RayScene::pack_shape(type, basis, SIZE, 0.35f, light);
		CHECK(light.area == 0.35f);
		CHECK(half_axes_zero(light));
		RayScene::Light point = {};
		point.area = 9.0f;
		RayScene::pack_shape(type, basis, Vector2(), 0.0f, point);
		CHECK(point.area == 0.0f);
	}
	RayScene::Light rect = {};
	RayScene::pack_shape(RSE::LIGHT_AREA, basis, SIZE, 0.35f, rect);
	CHECK(rect.area == doctest::Approx(0.125));
	CHECK(Vector3(rect.half_a[0], rect.half_a[1], rect.half_a[2]).length() == doctest::Approx(0.25));
	RayScene::Light sun = {};
	sun.area = 7.0f;
	RayScene::pack_shape(RSE::LIGHT_DIRECTIONAL, basis, SIZE, 0.35f, sun);
	CHECK(sun.area == 7.0f);
	CHECK(half_axes_zero(sun));
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

void list_update(uint64_t p_frame, bool p_reset, const RayScene::Light *p_omni, uint64_t p_id = 2) {
	DDGILightTracker::begin(p_reset, p_frame, 1.0f, true);
	if (p_omni) {
		DDGILightTracker::observe(RID::from_uint64(p_id), *p_omni, Vector3());
	}
	DDGILightTracker::end();
}

TEST_CASE("[RayScene] DDGI light tracker's list keeps a first view's events through a second update, and pending stays empty with the reset off") {
	const RayScene::Light omni = tracked_omni(2.0f);
	const RayScene::Light moved = tracked_omni(2.5f);
	const RayScene::Light distant = tracked_omni(50.0f);
	LocalVector<DDGILightTracker::Event> events;
	DDGILightTracker::begin(false, 0, 1.0f);

	list_update(0, false, &omni);
	uint64_t serial = DDGILightTracker::events_since(UINT64_MAX, events);
	CHECK(events.is_empty());

	list_update(8, false, &moved);
	CHECK(DDGILightTracker::pending_count() == 0);
	list_update(8, false, &moved);
	CHECK(DDGILightTracker::pending_count() == 0);
	const uint64_t after = DDGILightTracker::events_since(serial, events);
	REQUIRE(events.size() == 2);
	CHECK(after == serial + 2);
	CHECK(events[0].center.is_equal_approx(Vector3(2, 0, 0)));
	CHECK(events[1].center.is_equal_approx(Vector3(2.5f, 0, 0)));
	CHECK(events[0].half == doctest::Approx(RANGE));
	CHECK(events[1].since_last == 8);
	serial = after;

	list_update(12, false, &omni);
	CHECK(DDGILightTracker::events_since(serial, events) == serial);
	CHECK(events.is_empty());

	list_update(16, false, &omni);
	serial = DDGILightTracker::events_since(serial, events);
	REQUIRE(events.size() == 2);
	CHECK(events[0].since_last == 8);

	list_update(116, false, &moved);
	serial = DDGILightTracker::events_since(serial, events);
	REQUIRE(events.size() == 2);
	CHECK(events[0].since_last == 100);

	list_update(124, true, &omni);
	CHECK(DDGILightTracker::pending_count() == 2);
	list_update(132, false, &moved);
	CHECK(DDGILightTracker::pending_count() == 0);
	CHECK(DDGILightTracker::stats().fired == 0);
	serial = DDGILightTracker::events_since(serial, events);
	CHECK(events.size() == 4);

	DDGILightTracker::begin(false, 200, 1.0f, true);
	DDGILightTracker::observe(RID::from_uint64(2), moved, Vector3());
	DDGILightTracker::observe(RID::from_uint64(3), distant, Vector3());
	DDGILightTracker::end();
	serial = DDGILightTracker::events_since(serial, events);
	REQUIRE(events.size() == 1);
	CHECK(events[0].center.is_equal_approx(Vector3(50, 0, 0)));
	CHECK(events[0].since_last == UINT32_MAX);

	DDGILightTracker::begin(false, 0, 1.0f);
}

TEST_CASE("[RayScene] DDGI light tracker's list reports what a late reader lost, and off it records nothing") {
	const RayScene::Light omni = tracked_omni(2.0f);
	LocalVector<DDGILightTracker::Event> events;
	DDGILightTracker::begin(false, 0, 1.0f);

	list_update(0, false, &omni);
	const uint64_t start = DDGILightTracker::events_since(UINT64_MAX, events);
	for (uint32_t i = 1; i <= 40; i++) {
		const RayScene::Light step = tracked_omni(2.0f + float(i));
		list_update(i * 8, false, &step);
	}
	const uint64_t after = DDGILightTracker::events_since(start, events);
	CHECK(after == start + 80);
	REQUIRE(events.size() == DDGILightTracker::MAX_EVENTS);
	CHECK(after - start - events.size() == 16);
	for (uint32_t k = 0; k < events.size(); k++) {
		const uint32_t fired = 16 + k;
		CHECK(events[k].center.is_equal_approx(Vector3(2.0f + float(fired / 2 + fired % 2), 0, 0)));
	}

	CHECK(DDGILightTracker::events_since(after - DDGILightTracker::MAX_EVENTS, events) == after);
	CHECK(events.size() == DDGILightTracker::MAX_EVENTS);
	DDGILightTracker::events_since(after - 1, events);
	REQUIRE(events.size() == 1);
	CHECK(events[0].center.is_equal_approx(Vector3(42, 0, 0)));

	DDGILightTracker::begin(true, 1000, 1.0f);
	DDGILightTracker::observe(RID::from_uint64(2), omni, Vector3());
	DDGILightTracker::end();
	CHECK(DDGILightTracker::pending_count() == 2);
	CHECK(DDGILightTracker::events_since(after, events) == after);
	CHECK(events.is_empty());

	DDGILightTracker::begin(false, 0, 1.0f);
}

void list_update_both(uint64_t p_frame, bool p_reset, const RayScene::Light &p_first, const RayScene::Light &p_second) {
	DDGILightTracker::begin(p_reset, p_frame, 1.0f, true);
	DDGILightTracker::observe(RID::from_uint64(2), p_first, Vector3());
	DDGILightTracker::observe(RID::from_uint64(3), p_second, Vector3());
	DDGILightTracker::end();
}

TEST_CASE("[RayScene] DDGI light tracker's list serves each reader from its own serial, across updates and frames") {
	const RayScene::Light omni = tracked_omni(2.0f);
	const RayScene::Light moved = tracked_omni(2.5f);
	const RayScene::Light distant = tracked_omni(50.0f);
	const RayScene::Light distant_moved = tracked_omni(51.0f);
	LocalVector<DDGILightTracker::Event> events;
	DDGILightTracker::begin(false, 0, 1.0f);

	list_update_both(0, false, omni, distant);
	const uint64_t slow = DDGILightTracker::events_since(UINT64_MAX, events);

	list_update_both(8, false, moved, distant);
	const uint64_t fast = DDGILightTracker::events_since(slow, events);
	CHECK(events.size() == 2);

	list_update_both(8, false, moved, distant_moved);
	const uint64_t fast_after = DDGILightTracker::events_since(fast, events);
	REQUIRE(events.size() == 2);
	CHECK(events[0].center.is_equal_approx(Vector3(50, 0, 0)));
	CHECK(events[1].center.is_equal_approx(Vector3(51, 0, 0)));

	list_update_both(9, false, moved, distant_moved);
	CHECK(DDGILightTracker::events_since(fast_after, events) == fast_after);
	CHECK(events.is_empty());
	CHECK(DDGILightTracker::events_since(slow, events) == fast_after);
	REQUIRE(events.size() == 4);
	CHECK(events[0].center.is_equal_approx(Vector3(2, 0, 0)));
	CHECK(events[3].center.is_equal_approx(Vector3(51, 0, 0)));

	DDGILightTracker::begin(false, 0, 1.0f);
}

TEST_CASE("[RayScene] DDGI light tracker with the reset and the list both on gives each the same lamps") {
	const RayScene::Light omni = tracked_omni(2.0f);
	const RayScene::Light moved = tracked_omni(2.5f);
	const RayScene::Light distant = tracked_omni(50.0f);
	LocalVector<DDGILightTracker::Event> events;
	LocalVector<DDGILightTracker::Box> boxes;
	DDGILightTracker::begin(false, 0, 1.0f);

	list_update(0, true, &omni);
	uint64_t serial = DDGILightTracker::events_since(UINT64_MAX, events);
	DDGILightTracker::take(boxes);
	CHECK(boxes.is_empty());

	list_update(8, true, &moved);
	serial = DDGILightTracker::events_since(serial, events);
	DDGILightTracker::take(boxes);
	REQUIRE(events.size() == 2);
	REQUIRE(boxes.size() == 2);
	CHECK(events[0].center.is_equal_approx(boxes[0].center));
	CHECK(events[1].center.is_equal_approx(boxes[1].center));
	CHECK(events[1].center.is_equal_approx(Vector3(2.5f, 0, 0)));
	CHECK(events[0].since_last == 8);

	list_update_both(16, true, moved, distant);
	serial = DDGILightTracker::events_since(serial, events);
	DDGILightTracker::take(boxes);
	REQUIRE(events.size() == 1);
	REQUIRE(boxes.size() == 1);
	CHECK(events[0].center.is_equal_approx(Vector3(50, 0, 0)));
	CHECK(boxes[0].center.is_equal_approx(Vector3(50, 0, 0)));
	CHECK(events[0].since_last == UINT32_MAX);

	list_update(24, true, &moved);
	serial = DDGILightTracker::events_since(serial, events);
	DDGILightTracker::take(boxes);
	REQUIRE(events.size() == 1);
	REQUIRE(boxes.size() == 1);
	CHECK(events[0].center.is_equal_approx(Vector3(50, 0, 0)));
	CHECK(boxes[0].center.is_equal_approx(Vector3(50, 0, 0)));
	CHECK(events[0].since_last == UINT32_MAX);

	DDGILightTracker::begin(false, 0, 1.0f);
}

TEST_CASE("[RayScene] DDGI light tracker posts nothing for two views taking turns, and a removed lamp a frame later") {
	const RayScene::Light a = tracked_omni(2.0f);
	const RayScene::Light b = tracked_omni(50.0f);
	LocalVector<DDGILightTracker::Box> boxes;
	LocalVector<DDGILightTracker::Event> events;
	DDGILightTracker::begin(false, 0, 1.0f);

	list_update(10, true, &a, 2);
	list_update(10, true, &b, 3);
	DDGILightTracker::take(boxes);
	CHECK(boxes.size() == 1);

	for (uint64_t frame = 11; frame < 40; frame++) {
		list_update(frame, true, &a, 2);
		list_update(frame, true, &b, 3);
		DDGILightTracker::take(boxes);
		CHECK(boxes.is_empty());
	}
	for (uint64_t frame = 40; frame < 50; frame++) {
		list_update(frame, true, &b, 3);
		list_update(frame, true, &a, 2);
		DDGILightTracker::take(boxes);
		CHECK(boxes.is_empty());
	}
	DDGILightTracker::begin(false, 0, 1.0f);

	list_update(100, true, &a, 2);
	list_update(101, true, nullptr);
	DDGILightTracker::take(boxes);
	CHECK(boxes.is_empty());
	list_update(102, true, nullptr);
	DDGILightTracker::take(boxes);
	REQUIRE(boxes.size() == 1);
	CHECK(boxes[0].center.is_equal_approx(Vector3(2, 0, 0)));

	list_update(103, true, &a, 2);
	DDGILightTracker::take(boxes);
	CHECK(boxes.size() == 1);
	list_update(104, true, nullptr);
	list_update(105, true, &a, 2);
	DDGILightTracker::take(boxes);
	CHECK(boxes.is_empty());
	DDGILightTracker::begin(false, 0, 1.0f);

	list_update(200, false, &a, 2);
	uint64_t serial = DDGILightTracker::events_since(UINT64_MAX, events);
	list_update(201, false, nullptr);
	CHECK(DDGILightTracker::events_since(serial, events) == serial);
	list_update(202, false, nullptr);
	serial = DDGILightTracker::events_since(serial, events);
	REQUIRE(events.size() == 1);
	CHECK(events[0].center.is_equal_approx(Vector3(2, 0, 0)));
	CHECK(events[0].since_last == UINT32_MAX);
	list_update(203, false, &a, 2);
	serial = DDGILightTracker::events_since(serial, events);
	REQUIRE(events.size() == 1);
	CHECK(events[0].since_last == UINT32_MAX);

	DDGILightTracker::begin(false, 0, 1.0f);
}

TEST_CASE("[RayScene] DDGI light tracker's aging only covers a view that renders every frame, and a lamp back from one missed frame keeps its rate limit") {
	const RayScene::Light a = tracked_omni(2.0f);
	const RayScene::Light a_moved = tracked_omni(3.0f);
	const RayScene::Light b = tracked_omni(50.0f);
	LocalVector<DDGILightTracker::Box> boxes;
	DDGILightTracker::begin(false, 0, 1.0f);

	list_update(300, true, &a, 2);
	list_update(300, true, &b, 3);
	DDGILightTracker::take(boxes);
	CHECK(boxes.size() == 1);
	list_update(301, true, &a, 2);
	DDGILightTracker::take(boxes);
	CHECK(boxes.is_empty());
	list_update(302, true, &a, 2);
	DDGILightTracker::take(boxes);
	REQUIRE(boxes.size() == 1);
	CHECK(boxes[0].center.is_equal_approx(Vector3(50, 0, 0)));
	list_update(303, true, &b, 3);
	DDGILightTracker::take(boxes);
	REQUIRE(boxes.size() == 1);
	CHECK(boxes[0].center.is_equal_approx(Vector3(50, 0, 0)));
	DDGILightTracker::begin(false, 0, 1.0f);

	list_update(400, true, &a, 2);
	list_update(401, true, nullptr);
	list_update(402, true, &a_moved, 2);
	DDGILightTracker::take(boxes);
	CHECK(boxes.is_empty());
	list_update(408, true, &a_moved, 2);
	DDGILightTracker::take(boxes);
	CHECK(boxes.size() == 2);

	DDGILightTracker::begin(false, 0, 1.0f);
}

using Light = RayScene::Light;
using LightCensus = RayScene::LightCensus;

struct LightOrder {
	bool operator()(const RayScene::Light &p_a, const RayScene::Light &p_b) const {
		for (int k = 0; k < 3; k++) {
			if (p_a.position[k] != p_b.position[k]) {
				return p_a.position[k] < p_b.position[k];
			}
		}
		return p_a.inv_radius < p_b.inv_radius;
	}
};

struct LightNearer {
	Vector3 camera;
	static float score(const RayScene::Light &p_light, const Vector3 &p_camera) {
		float reach = 1.0f / p_light.inv_radius;
		if (p_light.type == uint32_t(RSE::LIGHT_AREA)) {
			reach += Math::sqrt(Vector3(p_light.half_a[0], p_light.half_a[1], p_light.half_a[2]).length_squared() + Vector3(p_light.half_b[0], p_light.half_b[1], p_light.half_b[2]).length_squared());
		}
		return Vector3(p_light.position[0], p_light.position[1], p_light.position[2]).distance_to(p_camera) - reach;
	}
	bool operator()(const RayScene::Light &p_a, const RayScene::Light &p_b) const {
		return score(p_a, camera) < score(p_b, camera);
	}
};

static double light_power(const RayScene::Light &p_light) {
	return double(p_light.energy) * (0.2126 * p_light.color[0] + 0.7152 * p_light.color[1] + 0.0722 * p_light.color[2]);
}

void golden_select(LocalVector<Light> &lights, LocalVector<Light> &local_lights, const Vector3 &p_camera, uint32_t p_max_lights, uint32_t p_reserved_area, LightCensus &census) {
	if (lights.size() + local_lights.size() > p_max_lights) {
		const uint32_t keep = p_max_lights - lights.size();
		SortArray<Light, LightNearer> nearest;
		nearest.compare.camera = p_camera;
		LocalVector<Light> area;
		LocalVector<Light> rest;
		for (const Light &light : local_lights) {
			if (light.type == uint32_t(RSE::LIGHT_AREA)) {
				area.push_back(light);
			} else {
				rest.push_back(light);
			}
		}
		const uint32_t reserved = MIN(MIN(area.size(), p_reserved_area), keep);
		if (area.size() > reserved) {
			nearest.nth_element(0, area.size(), reserved, area.ptr());
		}
		for (uint32_t i = reserved; i < area.size(); i++) {
			rest.push_back(area[i]);
		}
		const uint32_t rest_keep = keep - reserved;
		if (rest.size() > rest_keep) {
			nearest.nth_element(0, rest.size(), rest_keep, rest.ptr());
		}
		for (uint32_t i = rest_keep; i < rest.size(); i++) {
			if (rest[i].caster_mask != 0xFFFFFFFFu) {
				census.masked_cut++;
				census.masked_cut_energy += light_power(rest[i]);
			}
		}
		rest.resize(MIN(rest.size(), rest_keep));
		census.area_reserved = reserved;
		local_lights.clear();
		for (uint32_t i = 0; i < reserved; i++) {
			local_lights.push_back(area[i]);
		}
		for (const Light &light : rest) {
			local_lights.push_back(light);
		}
	}
	for (const Light &light : local_lights) {
		census.area_packed += light.type == uint32_t(RSE::LIGHT_AREA);
		if (light.caster_mask != 0xFFFFFFFFu) {
			census.masked_packed_energy += light_power(light);
		}
	}
	SortArray<Light, LightOrder> ordered;
	ordered.sort(local_lights.ptr(), local_lights.size());
	for (const Light &light : local_lights) {
		lights.push_back(light);
	}
}

constexpr double LUM = 0.2126 + 0.7152 + 0.0722;
constexpr float PICK_CELL = 16.0f;
const Vector3 PICK_HALF(144, 80, 144);

RayScene::Light pick_omni(float p_x, uint32_t p_pad, float p_energy = 1.0f, float p_y = 8.0f, uint32_t p_mask = 0xFFFFFFFF) {
	RayScene::Light light = {};
	light.type = uint32_t(RSE::LIGHT_OMNI);
	light.position[0] = p_x;
	light.position[1] = p_y;
	light.position[2] = 8.0f;
	light.color[0] = light.color[1] = light.color[2] = 1.0f;
	light.energy = p_energy;
	light.attenuation = 2.0f;
	light.inv_radius = 1.0f / 2.0f;
	light.caster_mask = p_mask;
	light.pad = p_pad;
	return light;
}

RayScene::Light pick_panel(float p_x, float p_y, uint32_t p_pad) {
	RayScene::Light light = pick_omni(p_x, p_pad, 4.0f, p_y);
	light.type = uint32_t(RSE::LIGHT_AREA);
	light.direction[2] = -1.0f;
	light.half_a[0] = 0.25f;
	light.half_b[1] = 0.125f;
	light.area = 0.125f;
	return light;
}

struct PickRun {
	LocalVector<RayScene::Light> lights;
	LocalVector<RID> ids;

	void add(const RayScene::Light &p_light, uint64_t p_rid) {
		lights.push_back(p_light);
		ids.push_back(RID::from_uint64(p_rid));
	}
};

PickRun pick_fixture(uint32_t p_count, float p_offset = 0.0f) {
	PickRun run;
	for (uint32_t i = 0; i < p_count; i++) {
		run.add(pick_omni(float(i) + 0.25f + p_offset, i), i + 1);
	}
	return run;
}

PickRun pick_reordered(const PickRun &p_run, bool p_shuffle) {
	LocalVector<uint32_t> order;
	for (uint32_t i = 0; i < p_run.lights.size(); i++) {
		order.push_back(p_shuffle ? i : p_run.lights.size() - 1 - i);
	}
	if (p_shuffle) {
		uint64_t seed = 12345;
		for (uint32_t i = order.size() - 1; i > 0; i--) {
			seed = seed * 6364136223846793005ull + 1442695040888963407ull;
			SWAP(order[i], order[uint32_t((seed >> 33) % (i + 1))]);
		}
	}
	PickRun run;
	for (const uint32_t index : order) {
		run.add(p_run.lights[index], p_run.ids[index].get_id());
	}
	return run;
}

struct PickInput : RayScene::PickParams {
	Vector3 camera;
};

struct TrackerOff {
	~TrackerOff() { DDGILightTracker::begin(false, 0, 1.0f); }
};

PickInput pick_params(const Vector3 &p_camera, uint32_t p_keep, const Vector3 &p_anchor = Vector3()) {
	PickInput params;
	params.camera = p_camera;
	params.cell_size = PICK_CELL;
	params.cell_origin = Vector3(RayScene::pick_cell_key(p_camera, PICK_CELL)) * PICK_CELL - p_anchor;
	params.keep = p_keep;
	params.reserved_area = 16;
	params.keep_share = 0.25f;
	params.keep_distance = 16.0f;
	params.inv_exposure = 1.0;
	params.want_held = true;
	return params;
}

struct PickResult {
	uint32_t slot = 0;
	LocalVector<uint32_t> selected;
	RayScene::PickCensus census;
};

PickResult pick_update(const PickRun &p_run, RayScene::PickState &r_state, const PickInput &p_input, uint64_t p_serial) {
	PickResult result;
	result.slot = RayScene::match_slot(r_state, p_input.camera, p_input.cell_size, p_serial);
	RayScene::PickParams params = p_input;
	params.slot = result.slot;
	RayScene::pick_lights(p_run.lights, p_run.ids, params, r_state, result.selected, result.census);
	return result;
}

LocalVector<uint64_t> picked_rids(const PickRun &p_run, const LocalVector<uint32_t> &p_selected) {
	LocalVector<uint64_t> rids;
	for (const uint32_t index : p_selected) {
		rids.push_back(p_run.ids[index].get_id());
	}
	return rids;
}

bool pads_are(const LocalVector<RayScene::Light> &p_lights, uint32_t p_first, uint32_t p_last) {
	if (p_lights.size() != p_last - p_first + 1) {
		return false;
	}
	LocalVector<uint32_t> pads;
	for (const RayScene::Light &light : p_lights) {
		pads.push_back(light.pad);
	}
	pads.sort();
	for (uint32_t i = 0; i < pads.size(); i++) {
		if (pads[i] != p_first + i) {
			return false;
		}
	}
	return true;
}

bool picks(const PickRun &p_run, const LocalVector<uint32_t> &p_selected, uint32_t p_first, uint32_t p_last) {
	LocalVector<RayScene::Light> chosen;
	for (const uint32_t index : p_selected) {
		chosen.push_back(p_run.lights[index]);
	}
	return pads_are(chosen, p_first, p_last);
}

LocalVector<uint32_t> packed_pads(const LocalVector<RayScene::Light> &p_lights) {
	LocalVector<uint32_t> pads;
	for (const RayScene::Light &light : p_lights) {
		pads.push_back(light.pad);
	}
	return pads;
}

template <typename T>
bool same_list(const LocalVector<T> &p_a, const LocalVector<T> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (uint32_t i = 0; i < p_a.size(); i++) {
		if (p_a[i] != p_b[i]) {
			return false;
		}
	}
	return true;
}

bool same_bytes(const LocalVector<RayScene::Light> &p_a, const LocalVector<RayScene::Light> &p_b) {
	return p_a.size() == p_b.size() && memcmp(p_a.ptr(), p_b.ptr(), p_a.size() * sizeof(RayScene::Light)) == 0;
}

void check_no_area(const RayScene::PickCensus &p_census) {
	CHECK(p_census.cut_area == 0);
	CHECK(p_census.cut_area_power == doctest::Approx(0.0));
	CHECK(p_census.packed_area_power == doctest::Approx(0.0));
}

TEST_CASE("[RayScene] Stable light pick under budget keeps every light and counts the first update's swaps") {
	const PickRun run = pick_fixture(10);
	RayScene::PickState state;
	PickResult result = pick_update(run, state, pick_params(Vector3(150.5, 8, 8), 256), 1);
	CHECK(result.selected.size() == 10);
	check_no_area(result.census);
	CHECK(result.census.cut_nonarea == 0);
	CHECK(result.census.cut_nonarea_power == doctest::Approx(0.0));
	CHECK(result.census.packed_nonarea_power == doctest::Approx(10.0 * LUM));
	CHECK(result.census.slot_new);
	CHECK(result.census.slot_in == 10);
	CHECK(result.census.slot_out == 0);
	CHECK(result.census.packed_in == 10);
	CHECK(result.census.packed_out == 0);

	result = pick_update(run, state, pick_params(Vector3(150.5, 8, 8), 256), 2);
	CHECK_FALSE(result.census.slot_new);
	CHECK(result.census.slot_in == 0);
	CHECK(result.census.slot_out == 0);
	CHECK(result.census.packed_in == 0);
	CHECK(result.census.packed_out == 0);
}

TEST_CASE("[RayScene] Stable light pick does not move while the camera stays in one cell, and the camera pick does") {
	const PickRun run = pick_fixture(300);
	RayScene::PickState state;
	uint64_t serial = 0;
	for (const float x : { 145.0f, 150.5f, 151.5f, 159.9f }) {
		const PickResult result = pick_update(run, state, pick_params(Vector3(x, 8, 8), 256), ++serial);
		INFO("camera x ", x);
		CHECK(picks(run, result.selected, 24, 279));
		CHECK(result.census.cut_nonarea == 44);
		CHECK(result.census.cut_nonarea_power == doctest::Approx(44.0 * LUM));
		CHECK(result.census.packed_nonarea_power == doctest::Approx(256.0 * LUM));
		check_no_area(result.census);
		if (serial > 1) {
			CHECK(result.census.slot_in == 0);
			CHECK(result.census.slot_out == 0);
			CHECK(result.census.packed_in == 0);
			CHECK(result.census.packed_out == 0);
		}
	}

	for (const float x : { 150.5f, 151.5f }) {
		LocalVector<RayScene::Light> lights;
		LocalVector<RayScene::Light> local(run.lights);
		RayScene::LightCensus census;
		RayScene::select_nearest(lights, local, Vector3(x, 8, 8), 256, 16, 1.0, census);
		INFO("camera x ", x);
		CHECK(pads_are(lights, x < 151.0f ? 23 : 24, x < 151.0f ? 278 : 279));
	}
}

TEST_CASE("[RayScene] Stable light pick steps one cell forward with the keep margin, and without it swaps more") {
	const PickRun run = pick_fixture(300);
	RayScene::PickState state;
	pick_update(run, state, pick_params(Vector3(150.5, 8, 8), 256), 1);
	const PickResult result = pick_update(run, state, pick_params(Vector3(165, 8, 8), 256), 2);
	CHECK(picks(run, result.selected, 32, 287));
	CHECK(result.census.slot_in == 8);
	CHECK(result.census.slot_out == 8);
	CHECK(result.census.held == 8);
	CHECK_FALSE(result.census.slot_new);

	RayScene::PickState plain;
	PickInput params = pick_params(Vector3(150.5, 8, 8), 256);
	params.keep_share = 0.0f;
	params.keep_distance = 0.0f;
	pick_update(run, plain, params, 1);
	params = pick_params(Vector3(165, 8, 8), 256);
	params.keep_share = 0.0f;
	params.keep_distance = 0.0f;
	const PickResult swapped = pick_update(run, plain, params, 2);
	CHECK(picks(run, swapped.selected, 40, 295));
	CHECK(swapped.census.slot_in == 16);
	CHECK(swapped.census.slot_out == 16);
	CHECK(swapped.census.held == 0);
}

TEST_CASE("[RayScene] Stable light pick holds when the camera dithers across a cell edge") {
	const PickRun run = pick_fixture(300);
	RayScene::PickState state;
	pick_update(run, state, pick_params(Vector3(150.5, 8, 8), 256), 1);
	const PickResult step = pick_update(run, state, pick_params(Vector3(165, 8, 8), 256), 2);
	REQUIRE(picks(run, step.selected, 32, 287));
	uint64_t serial = 2;
	for (int i = 0; i < 4; i++) {
		const float x = i % 2 == 0 ? 150.5f : 165.0f;
		const PickResult result = pick_update(run, state, pick_params(Vector3(x, 8, 8), 256), ++serial);
		INFO("camera x ", x);
		CHECK(result.slot == step.slot);
		CHECK(picks(run, result.selected, 32, 287));
		CHECK(result.census.slot_in == 0);
		CHECK(result.census.slot_out == 0);
		CHECK(result.census.packed_in == 0);
		CHECK(result.census.packed_out == 0);
	}
}

TEST_CASE("[RayScene] Stable light pick keeps a light within the share margin and breaks an exact tie for the kept light") {
	for (const float energy : { 1.24f, 1.26f }) {
		PickRun first;
		first.add(pick_omni(8, 0, 1.0f), 1);
		RayScene::PickState state;
		pick_update(first, state, pick_params(Vector3(8, 8, 8), 1), 1);
		PickRun run = first;
		run.add(pick_omni(8, 1, energy), 2);
		const PickResult result = pick_update(run, state, pick_params(Vector3(8, 8, 8), 1), 2);
		INFO("challenger energy ", energy);
		REQUIRE(result.selected.size() == 1);
		CHECK(run.ids[result.selected[0]].get_id() == (energy < 1.25f ? 1u : 2u));
	}

	PickRun first;
	first.add(pick_omni(8, 1), 2);
	RayScene::PickState state;
	PickInput params = pick_params(Vector3(8, 8, 8), 1);
	params.keep_share = 0.0f;
	params.keep_distance = 0.0f;
	pick_update(first, state, params, 1);
	PickRun run;
	run.add(pick_omni(8, 0), 1);
	run.add(pick_omni(8, 1), 2);
	const PickResult result = pick_update(run, state, params, 2);
	REQUIRE(result.selected.size() == 1);
	CHECK(run.ids[result.selected[0]].get_id() == 2);
	CHECK(result.census.held == 0);
}

TEST_CASE("[RayScene] Stable light pick ranks a negative light by distance only and counts it with no power") {
	PickRun run;
	run.add(pick_omni(8, 0, -5.0f), 1);
	run.add(pick_omni(8, 1, 3.0f), 2);
	RayScene::PickState state;
	const PickResult result = pick_update(run, state, pick_params(Vector3(8, 8, 8), 1), 1);
	REQUIRE(result.selected.size() == 1);
	CHECK(run.ids[result.selected[0]].get_id() == 2);
	CHECK(result.census.cut_nonarea == 1);
	CHECK(result.census.cut_nonarea_power == doctest::Approx(0.0));
	CHECK(result.census.packed_nonarea_power == doctest::Approx(3.0 * LUM));
	check_no_area(result.census);
}

TEST_CASE("[RayScene] Stable light pick and its packed order do not depend on the input order") {
	const PickRun run = pick_fixture(300);
	RayScene::PickState state;
	const LocalVector<uint64_t> expected = picked_rids(run, pick_update(run, state, pick_params(Vector3(150.5, 8, 8), 256), 1).selected);
	REQUIRE(expected.size() == 256);
	for (const bool shuffle : { false, true }) {
		const PickRun reordered = pick_reordered(run, shuffle);
		RayScene::PickState fresh;
		const LocalVector<uint64_t> got = picked_rids(reordered, pick_update(reordered, fresh, pick_params(Vector3(150.5, 8, 8), 256), 1).selected);
		INFO("shuffled ", shuffle);
		CHECK(same_list(got, expected));
	}

	for (const bool swap : { false, true }) {
		PickRun pair;
		pair.add(pick_omni(8, swap ? 1 : 0), swap ? 2 : 1);
		pair.add(pick_omni(8, swap ? 0 : 1), swap ? 1 : 2);
		RayScene::PickState one;
		const PickResult single = pick_update(pair, one, pick_params(Vector3(8, 8, 8), 1), 1);
		REQUIRE(single.selected.size() == 1);
		CHECK(pair.ids[single.selected[0]].get_id() == 1);
		RayScene::PickState two;
		const LocalVector<uint64_t> both = picked_rids(pair, pick_update(pair, two, pick_params(Vector3(8, 8, 8), 2), 1).selected);
		REQUIRE(both.size() == 2);
		CHECK(both[0] == 1);
		CHECK(both[1] == 2);
	}
}

TEST_CASE("[RayScene] Stable light pick reserves area lights and the rest compete in the main pool") {
	PickRun run = pick_fixture(300);
	for (uint32_t k = 0; k < 20; k++) {
		run.add(pick_panel(144.5f + 0.75f * float(k), 12.0f, 300 + k), 301 + k);
	}
	RayScene::PickState state;
	const PickResult result = pick_update(run, state, pick_params(Vector3(150.5, 8, 8), 256), 1);
	CHECK(result.selected.size() == 256);
	CHECK(result.census.area_reserved == 16);
	const uint32_t won = result.census.area_packed - 16;
	CHECK(won == 4);
	CHECK(result.census.cut_area == 4 - won);
	CHECK(result.census.cut_nonarea == 60 + won);
	CHECK(result.census.cut_area + result.census.cut_nonarea == 64);
	CHECK(result.census.cut_area + result.census.area_packed == 20);
}

TEST_CASE("[RayScene] Stable light pick keeps a slot for each of two views two cells apart") {
	const PickRun run = pick_fixture(300);
	RayScene::PickState state;
	uint64_t serial = 0;
	const PickResult first = pick_update(run, state, pick_params(Vector3(150.5, 8, 8), 256), ++serial);
	CHECK(picks(run, first.selected, 24, 279));
	const PickResult second = pick_update(run, state, pick_params(Vector3(182.5, 8, 8), 256), ++serial);
	CHECK(second.slot != first.slot);
	CHECK(second.census.slot_new);
	CHECK(picks(run, second.selected, 44, 299));
	CHECK(second.census.slot_in == 20);
	CHECK(second.census.slot_out == 20);
	CHECK(second.census.packed_in == 20);
	CHECK(second.census.packed_out == 20);

	RayScene::PickState fresh;
	CHECK(picks(run, pick_update(run, fresh, pick_params(Vector3(182.5, 8, 8), 256), 1).selected, 44, 299));

	for (int i = 0; i < 6; i++) {
		const bool back = i % 2 == 0;
		const PickResult result = pick_update(run, state, pick_params(Vector3(back ? 150.5f : 182.5f, 8, 8), 256), ++serial);
		INFO("alternation ", i);
		CHECK(result.slot == (back ? first.slot : second.slot));
		CHECK_FALSE(result.census.slot_new);
		CHECK(picks(run, result.selected, back ? 24 : 44, back ? 279 : 299));
		CHECK(result.census.slot_in == 0);
		CHECK(result.census.slot_out == 0);
		CHECK(result.census.packed_in == 20);
		CHECK(result.census.packed_out == 20);
	}
}

TEST_CASE("[RayScene] Stable light region snaps to the slot's region cell with half a cell of hysteresis") {
	RayScene::PickState state;
	uint64_t serial = 0;
	const auto region = [&](const Vector3 &p_camera) {
		const uint32_t slot = RayScene::match_slot(state, p_camera, PICK_CELL, ++serial);
		return RayScene::light_region(state.slots[slot], p_camera, PICK_HALF, PICK_CELL);
	};
	const Vector3 size(320, 192, 320);
	AABB box = region(Vector3(1, 1, 1));
	CHECK(box.position.is_equal_approx(Vector3(-152, -88, -152)));
	CHECK(box.size.is_equal_approx(size));
	CHECK(region(Vector3(15, 15, 15)).position.is_equal_approx(Vector3(-152, -88, -152)));
	CHECK(region(Vector3(16, 1, 1)).position.is_equal_approx(Vector3(-152, -88, -152)));
	box = region(Vector3(24, 1, 1));
	CHECK(box.position.is_equal_approx(Vector3(-136, -88, -152)));
	CHECK(box.size.is_equal_approx(size));
	CHECK(state.slots[0].region_key == Vector3i(1, 0, 0));
	CHECK(region(Vector3(8, 1, 1)).position.x == doctest::Approx(-136.0));
	box = region(Vector3(7.9f, 1, 1));
	CHECK(box.position.x == doctest::Approx(-152.0));
	CHECK(box.size.is_equal_approx(size));
	CHECK(state.slots[0].region_key == Vector3i(0, 0, 0));
	CHECK(region(Vector3(1, 16, 1)).position.is_equal_approx(Vector3(-152, -88, -152)));
	box = region(Vector3(1, 24, 1));
	CHECK(box.position.is_equal_approx(Vector3(-152, -72, -152)));
	CHECK(box.size.is_equal_approx(size));
	CHECK(region(Vector3(1, 24, 16)).position.is_equal_approx(Vector3(-152, -72, -152)));
	box = region(Vector3(1, 24, 24));
	CHECK(box.position.is_equal_approx(Vector3(-152, -72, -136)));
	CHECK(box.size.is_equal_approx(size));
	CHECK(state.slots[0].region_key == Vector3i(0, 1, 1));
	for (uint32_t i = 1; i < RayScene::PICK_CELL_SLOTS; i++) {
		CHECK_FALSE(state.slots[i].used);
	}
}

TEST_CASE("[RayScene] Stable light region keeps its own region for each of two views 1.9 cells apart, and a walking view keeps one slot") {
	RayScene::PickState state;
	uint64_t serial = 0;
	const auto region = [&](const Vector3 &p_camera, uint32_t &r_slot) {
		r_slot = RayScene::match_slot(state, p_camera, PICK_CELL, ++serial);
		return RayScene::light_region(state.slots[r_slot], p_camera, PICK_HALF, PICK_CELL);
	};
	const Vector3 first(145, 8, 8);
	const Vector3 second(175.4f, 8, 8);
	REQUIRE(RayScene::pick_cell_key(first, PICK_CELL) == Vector3i(9, 0, 0));
	REQUIRE(RayScene::pick_cell_key(second, PICK_CELL) == Vector3i(10, 0, 0));
	uint32_t first_slot = 0;
	uint32_t second_slot = 0;
	CHECK(region(first, first_slot).position.x == doctest::Approx(-8.0));
	CHECK(region(second, second_slot).position.x == doctest::Approx(8.0));
	CHECK(first_slot != second_slot);
	for (int i = 0; i < 6; i++) {
		INFO("alternation ", i);
		uint32_t slot = 0;
		CHECK(region(first, slot).position.x == doctest::Approx(-8.0));
		CHECK(slot == first_slot);
		CHECK(region(second, slot).position.x == doctest::Approx(8.0));
		CHECK(slot == second_slot);
		CHECK(state.slots[first_slot].region_key == Vector3i(9, 0, 0));
		CHECK(state.slots[second_slot].region_key == Vector3i(10, 0, 0));
	}

	RayScene::PickState walk;
	AABB box;
	for (int x = 1; x <= 60; x++) {
		const Vector3 camera(float(x), 1, 1);
		const uint32_t slot = RayScene::match_slot(walk, camera, PICK_CELL, uint64_t(x));
		INFO("camera x ", x);
		CHECK(slot == 0);
		box = RayScene::light_region(walk.slots[slot], camera, PICK_HALF, PICK_CELL);
	}
	CHECK(walk.slots[0].region_key == Vector3i(3, 0, 0));
	CHECK(box.position.x == doctest::Approx(-104.0));
	for (uint32_t i = 1; i < RayScene::PICK_CELL_SLOTS; i++) {
		CHECK_FALSE(walk.slots[i].used);
	}
}

TEST_CASE("[RayScene] Stable light pick swaps fire no DDGI tracker boxes, and a light that leaves still fires its gone box") {
	const PickRun run = pick_fixture(300);
	PickRun fewer;
	for (uint32_t i = 1; i < run.lights.size(); i++) {
		fewer.add(run.lights[i], run.ids[i].get_id());
	}
	LocalVector<DDGILightTracker::Box> boxes;
	RayScene::PickState state;
	TrackerOff tracker_off;
	DDGILightTracker::begin(false, 0, 1.0f);
	const float cameras[4] = { 150.5f, 165.0f, 165.0f, 165.0f };
	for (uint64_t frame = 1; frame <= 4; frame++) {
		const PickRun &frame_run = frame <= 2 ? run : fewer;
		DDGILightTracker::begin(true, frame, 1.0f);
		const uint32_t fired_before = DDGILightTracker::stats().fired;
		for (uint32_t i = 0; i < frame_run.lights.size(); i++) {
			DDGILightTracker::observe(frame_run.ids[i], frame_run.lights[i], Vector3());
		}
		const PickResult result = pick_update(frame_run, state, pick_params(Vector3(cameras[frame - 1], 8, 8), 256), frame);
		DDGILightTracker::end();
		DDGILightTracker::take(boxes);
		INFO("frame ", frame);
		if (frame == 2) {
			CHECK(result.census.slot_in == 8);
			CHECK(result.census.slot_out == 8);
		}
		if (frame < 4) {
			CHECK(boxes.is_empty());
			CHECK(DDGILightTracker::stats().fired == fired_before);
		} else {
			REQUIRE(boxes.size() == 1);
			CHECK(boxes[0].center.is_equal_approx(Vector3(0.25, 8, 8)));
			CHECK(DDGILightTracker::stats().fired == fired_before + 1);
		}
	}
	DDGILightTracker::begin(false, 0, 1.0f);
}

TEST_CASE("[RayScene] Stable light pick is the same with the lights and the cell given relative to an anchor") {
	const PickRun run = pick_fixture(300);
	const PickRun shifted = pick_fixture(300, -64.0f);
	RayScene::PickState state;
	const LocalVector<uint64_t> expected = picked_rids(run, pick_update(run, state, pick_params(Vector3(150.5, 8, 8), 256), 1).selected);
	RayScene::PickState anchored;
	const PickInput params = pick_params(Vector3(150.5, 8, 8), 256, Vector3(64, 0, 0));
	CHECK(RayScene::pick_cell_key(params.camera, PICK_CELL) == Vector3i(9, 0, 0));
	CHECK(params.cell_origin.x == 80.0f);
	CHECK(same_list(picked_rids(shifted, pick_update(shifted, anchored, params, 1).selected), expected));
}

void check_golden(const LocalVector<RayScene::Light> &p_input, const Vector3 &p_camera, uint32_t p_max_lights, uint32_t p_reserved_area, double p_inv_exposure, LocalVector<RayScene::Light> &r_lights, RayScene::LightCensus &r_census) {
	LocalVector<RayScene::Light> golden_lights;
	LocalVector<RayScene::Light> golden_local(p_input);
	RayScene::LightCensus golden_census;
	golden_select(golden_lights, golden_local, p_camera, p_max_lights, p_reserved_area, golden_census);
	r_lights.clear();
	LocalVector<RayScene::Light> local(p_input);
	r_census = RayScene::LightCensus();
	RayScene::select_nearest(r_lights, local, p_camera, p_max_lights, p_reserved_area, p_inv_exposure, r_census);
	CHECK(same_bytes(r_lights, golden_lights));
	CHECK(r_census.area_reserved == golden_census.area_reserved);
	CHECK(r_census.area_packed == golden_census.area_packed);
	CHECK(r_census.masked_cut == golden_census.masked_cut);
	CHECK(r_census.masked_cut_energy == golden_census.masked_cut_energy);
	CHECK(r_census.masked_packed_energy == golden_census.masked_packed_energy);
}

TEST_CASE("[RayScene] Camera light pick is the old pick: same lights, same order, same census") {
	PickRun mixed = pick_fixture(300);
	for (uint32_t k = 0; k < 20; k++) {
		mixed.add(pick_panel(144.5f + 0.75f * float(k), 12.0f, 300 + k), 301 + k);
	}
	for (uint32_t k = 0; k < 4; k++) {
		mixed.add(pick_omni(148.5f + float(k), 320 + k, 1.0f, 9.0f, 0x1), 321 + k);
	}
	for (uint32_t k = 0; k < 4; k++) {
		mixed.add(pick_omni(400.0f + float(k), 324 + k, 1.0f, 8.0f, 0x1), 325 + k);
	}
	for (const bool shuffle : { false, true }) {
		const PickRun input = shuffle ? pick_reordered(mixed, true) : mixed;
		for (const float x : { 150.5f, 151.5f }) {
			INFO("shuffled ", shuffle, " camera x ", x);
			LocalVector<RayScene::Light> lights;
			RayScene::LightCensus census;
			check_golden(input.lights, Vector3(x, 8, 8), 256, 16, 1.0, lights, census);
			LocalVector<RayScene::Light> omnis;
			for (const RayScene::Light &light : lights) {
				if (light.type == uint32_t(RSE::LIGHT_OMNI) && light.caster_mask == 0xFFFFFFFFu) {
					omnis.push_back(light);
				}
			}
			CHECK(pads_are(omnis, x < 151.0f ? 35 : 36, x < 151.0f ? 266 : 267));
			CHECK(lights.size() == 256);
			CHECK(census.area_reserved == 16);
			CHECK(census.area_packed == 20);
			CHECK(census.masked_cut == 4);
			CHECK(census.masked_cut_energy == doctest::Approx(4.0 * LUM));
			CHECK(census.masked_packed_energy == doctest::Approx(4.0 * LUM));
			CHECK(census.cut_area == 0);
			CHECK(census.cut_area_power == doctest::Approx(0.0));
			CHECK(census.cut_nonarea == 72);
			CHECK(census.cut_nonarea_power == doctest::Approx(72.0 * LUM));
			CHECK(census.packed_area_power == doctest::Approx(10.0 * LUM));
			CHECK(census.packed_nonarea_power == doctest::Approx(236.0 * LUM));
		}
	}

	const PickRun omni = pick_fixture(300);
	PickRun bright = omni;
	for (RayScene::Light &light : bright.lights) {
		light.energy = 2.0f;
	}
	for (const float x : { 150.5f, 151.5f }) {
		INFO("camera x ", x);
		LocalVector<RayScene::Light> lights;
		RayScene::LightCensus census;
		check_golden(omni.lights, Vector3(x, 8, 8), 256, 16, 1.0, lights, census);
		CHECK(pads_are(lights, x < 151.0f ? 23 : 24, x < 151.0f ? 278 : 279));
		CHECK(census.cut_nonarea == 44);
		CHECK(census.cut_nonarea_power == doctest::Approx(44.0 * LUM));
		CHECK(census.packed_nonarea_power == doctest::Approx(256.0 * LUM));
		CHECK(census.cut_area == 0);
		CHECK(census.cut_area_power == doctest::Approx(0.0));
		CHECK(census.packed_area_power == doctest::Approx(0.0));

		LocalVector<RayScene::Light> exposed;
		RayScene::LightCensus exposed_census;
		check_golden(bright.lights, Vector3(x, 8, 8), 256, 16, 0.5, exposed, exposed_census);
		CHECK(same_list(packed_pads(exposed), packed_pads(lights)));
		CHECK(exposed_census.cut_nonarea_power == doctest::Approx(44.0 * LUM));
		CHECK(exposed_census.packed_nonarea_power == doctest::Approx(256.0 * LUM));
	}
}

bool region_overlaps(const RayScene::Light &p_light, const AABB &p_region) {
	const float range = 1.0f / p_light.inv_radius;
	for (int k = 0; k < 3; k++) {
		if (p_light.position[k] - range >= p_region.position[k] + p_region.size[k] || p_light.position[k] + range <= p_region.position[k]) {
			return false;
		}
	}
	return true;
}

AABB floor_region(const Vector3 &p_camera) {
	const Vector3i key = RayScene::pick_cell_key(p_camera, PICK_CELL);
	const Vector3 half = PICK_HALF + Vector3(PICK_CELL, PICK_CELL, PICK_CELL);
	const Vector3 center = (Vector3(key) + Vector3(0.5, 0.5, 0.5)) * PICK_CELL;
	return AABB(center - half, half * 2.0);
}

TEST_CASE("[RayScene] An ungated tracker sees one burst of DDGI tracker boxes per Stable region step, and a dither across the edge releases none") {
	PickRun run;
	for (int y = -12; y <= 16; y += 4) {
		for (int z = -12; z <= 16; z += 4) {
			RayScene::Light light = pick_omni(-180.0f, run.lights.size());
			light.position[1] = float(y);
			light.position[2] = float(z);
			light.inv_radius = 1.0f / 40.0f;
			run.add(light, run.lights.size() + 1);
		}
	}
	REQUIRE(run.lights.size() == 64);
	TrackerOff tracker_off;
	LocalVector<Vector3> cameras;
	for (int i = 0; i < 3; i++) {
		cameras.push_back(Vector3(1, 1, 1));
	}
	cameras.push_back(Vector3(25, 1, 1));
	cameras.push_back(Vector3(25, 1, 1));
	cameras.push_back(Vector3(25, 1, 1));
	const uint32_t dither_start = cameras.size();
	for (int hold = 0; hold < 20; hold++) {
		for (int i = 0; i < 3; i++) {
			cameras.push_back(Vector3(hold % 2 == 0 ? 15 : 17, 1, 1));
		}
	}

	for (const bool snapped : { true, false }) {
		RayScene::PickState state;
		LocalVector<DDGILightTracker::Box> boxes;
		LocalVector<uint32_t> fired;
		LocalVector<uint32_t> taken;
		DDGILightTracker::begin(false, 0, 1.0f);
		for (uint32_t f = 0; f < cameras.size(); f++) {
			const uint64_t frame = f + 1;
			AABB box;
			if (snapped) {
				const uint32_t slot = RayScene::match_slot(state, cameras[f], PICK_CELL, frame);
				box = RayScene::light_region(state.slots[slot], cameras[f], PICK_HALF, PICK_CELL);
			} else {
				box = floor_region(cameras[f]);
			}
			DDGILightTracker::begin(true, frame, 1.0f);
			const uint32_t before = DDGILightTracker::stats().fired;
			for (uint32_t i = 0; i < run.lights.size(); i++) {
				if (region_overlaps(run.lights[i], box)) {
					DDGILightTracker::observe(run.ids[i], run.lights[i], Vector3());
				}
			}
			DDGILightTracker::end();
			fired.push_back(DDGILightTracker::stats().fired - before);
			DDGILightTracker::take(boxes);
			taken.push_back(boxes.size());
		}
		DDGILightTracker::begin(false, 0, 1.0f);
		uint32_t dither_fired = 0;
		for (uint32_t f = dither_start; f < fired.size(); f++) {
			dither_fired += fired[f];
		}
		INFO("snapped ", snapped);
		if (snapped) {
			CHECK(region_overlaps(run.lights[0], AABB(Vector3(-152, -88, -152), Vector3(320, 192, 320))));
			CHECK_FALSE(region_overlaps(run.lights[0], AABB(Vector3(-136, -88, -152), Vector3(320, 192, 320))));
			for (uint32_t f = 0; f < 4; f++) {
				CHECK(fired[f] == 0);
			}
			CHECK(fired[4] == 64);
			CHECK(taken[4] == 32);
			CHECK(taken[5] == 32);
			CHECK(dither_fired == 0);
		} else {
			CHECK(dither_fired == 20 * 64);
		}
	}
}

TEST_CASE("[RayScene] Area light cut gives the same census in both light pick modes") {
	PickRun run;
	run.add(pick_omni(1.25f, 0), 1);
	run.add(pick_omni(2.25f, 1), 2);
	run.add(pick_omni(3.25f, 2), 3);
	run.add(pick_panel(10.25f, 8.0f, 3), 4);
	run.add(pick_panel(20.25f, 8.0f, 4), 5);
	run.add(pick_panel(30.25f, 8.0f, 5), 6);

	LocalVector<RayScene::Light> lights;
	RayScene::LightCensus census;
	check_golden(run.lights, Vector3(0, 8, 8), 4, 2, 1.0, lights, census);
	const LocalVector<uint32_t> expected_pads = { 0, 1, 3, 4 };
	CHECK(same_list(packed_pads(lights), expected_pads));
	CHECK(census.area_reserved == 2);
	CHECK(census.area_packed == 2);
	CHECK(census.masked_cut == 0);
	CHECK(census.masked_cut_energy == 0.0);
	CHECK(census.masked_packed_energy == 0.0);
	CHECK(census.cut_area == 1);
	CHECK(census.cut_area_power == doctest::Approx(0.5 * LUM));
	CHECK(census.cut_nonarea == 1);
	CHECK(census.cut_nonarea_power == doctest::Approx(LUM));
	CHECK(census.packed_area_power == doctest::Approx(LUM));
	CHECK(census.packed_nonarea_power == doctest::Approx(2.0 * LUM));

	RayScene::PickState state;
	PickInput params = pick_params(Vector3(0, 8, 8), 4);
	params.reserved_area = 2;
	CHECK(RayScene::pick_cell_key(params.camera, PICK_CELL) == Vector3i(0, 0, 0));
	const PickResult result = pick_update(run, state, params, 1);
	const LocalVector<uint64_t> expected_rids = { 2, 3, 4, 5 };
	CHECK(same_list(picked_rids(run, result.selected), expected_rids));
	const RayScene::PickCensus &pick = result.census;
	CHECK(pick.area_reserved == 2);
	CHECK(pick.area_packed == 2);
	CHECK(pick.cut_area == 1);
	CHECK(pick.cut_area_power == doctest::Approx(0.5 * LUM));
	CHECK(pick.cut_nonarea == 1);
	CHECK(pick.cut_nonarea_power == doctest::Approx(LUM));
	CHECK(pick.packed_area_power == doctest::Approx(LUM));
	CHECK(pick.packed_nonarea_power == doctest::Approx(2.0 * LUM));
	const double cut = pick.cut_area_power + pick.cut_nonarea_power;
	const double packed = pick.packed_area_power + pick.packed_nonarea_power;
	CHECK(cut / (cut + packed) == doctest::Approx(1.0 / 3.0));
	CHECK(pick.cut_area_power / (pick.cut_area_power + pick.packed_area_power) == doctest::Approx(1.0 / 3.0));
	CHECK(pick.cut_nonarea_power / (pick.cut_nonarea_power + pick.packed_nonarea_power) == doctest::Approx(1.0 / 3.0));
}

TEST_CASE("[RayScene] Stable light pick census scales by the inverse exposure and the pick does not") {
	const PickRun omni = pick_fixture(300);
	PickRun bright = omni;
	for (RayScene::Light &light : bright.lights) {
		light.energy = 2.0f;
	}
	RayScene::PickState state;
	const PickResult plain = pick_update(omni, state, pick_params(Vector3(150.5, 8, 8), 256), 1);
	RayScene::PickState exposed_state;
	PickInput params = pick_params(Vector3(150.5, 8, 8), 256);
	params.inv_exposure = 0.5;
	const PickResult exposed = pick_update(bright, exposed_state, params, 1);
	CHECK(picks(bright, exposed.selected, 24, 279));
	CHECK(same_list(picked_rids(bright, exposed.selected), picked_rids(omni, plain.selected)));
	CHECK(exposed.census.cut_nonarea == 44);
	CHECK(exposed.census.cut_nonarea_power == doctest::Approx(44.0 * LUM));
	CHECK(exposed.census.packed_nonarea_power == doctest::Approx(256.0 * LUM));
	check_no_area(exposed.census);
}

TEST_CASE("[RayScene] Stable light pick ranks a NaN energy, a NaN position and an infinite position last") {
	PickRun run;
	run.add(pick_omni(8, 0), 1);
	run.add(pick_omni(8, 1, float(Math::NaN)), 2);
	RayScene::Light nan_position = pick_omni(8, 2);
	nan_position.position[0] = float(Math::NaN);
	run.add(nan_position, 3);
	RayScene::Light far_position = pick_omni(8, 3);
	far_position.position[0] = float(Math::INF);
	run.add(far_position, 4);
	for (uint32_t keep = 1; keep <= 4; keep++) {
		RayScene::PickState state;
		const PickResult result = pick_update(run, state, pick_params(Vector3(8, 8, 8), keep), 1);
		INFO("keep ", keep);
		REQUIRE(result.selected.size() == keep);
		LocalVector<uint64_t> rids = picked_rids(run, result.selected);
		rids.sort();
		for (uint32_t i = 0; i < keep; i++) {
			CHECK(rids[i] == i + 1);
		}
		CHECK(result.census.cut_nonarea == 4 - keep);
		CHECK(Math::is_finite(result.census.cut_nonarea_power));
		CHECK(Math::is_finite(result.census.packed_nonarea_power));
	}
}

enum StepMode {
	STEP_STABLE,
	STEP_STABLE_SNAPPED,
	STEP_STABLE_STALE,
	STEP_CAMERA,
};

struct StepRun {
	uint64_t events = 0;
	uint64_t lost = 0;
	uint64_t worst = 0;
	LocalVector<uint64_t> per_frame;
	Vector3i region_key;
};

StepRun region_step_run(StepMode p_mode) {
	LocalVector<Vector3> lamps;
	for (int x = -200; x <= 260; x += 2) {
		for (int k = 0; k < 20; k++) {
			lamps.push_back(Vector3(float(x), 0.0f, float(-152 + 16 * k)));
		}
	}
	const Vector3 reach(RANGE, RANGE, RANGE);
	RayScene::PickState state;
	LocalVector<DDGILightTracker::Event> events;
	DDGILightTracker::begin(false, 0, 1.0f);
	StepRun run;
	Vector3 last_camera(1.0f, 1.0f, 1.0f);
	uint64_t serial = DDGILightTracker::events_since(UINT64_MAX, events);
	for (uint32_t f = 0; f <= 120; f++) {
		const Vector3 camera(1.0f + 0.5f * float(f), 1.0f, 1.0f);
		const AABB region(camera - PICK_HALF, PICK_HALF * 2.0f);
		const uint32_t slot = RayScene::match_slot(state, camera, PICK_CELL, uint64_t(f) + 1);
		const AABB box = RayScene::light_region(state.slots[slot], camera, PICK_HALF, PICK_CELL);
		const bool stable = p_mode != STEP_CAMERA;
		const AABB gate = p_mode == STEP_STABLE_SNAPPED ? box : RayScene::tracker_region_for(stable, p_mode == STEP_STABLE_STALE ? last_camera : camera, PICK_HALF);
		const AABB &gather = stable ? box : region;
		last_camera = camera;
		DDGILightTracker::begin(false, uint64_t(f) + 1, 1.0f, true);
		for (uint32_t i = 0; i < lamps.size(); i++) {
			const AABB lamp(lamps[i] - reach, reach * 2.0f);
			if (!gather.intersects(lamp) || !RayScene::tracker_observes(RSE::LIGHT_OMNI, gate, lamp)) {
				continue;
			}
			RayScene::Light light = tracked_light(RSE::LIGHT_OMNI, 0xFFFFFFFF);
			light.position[0] = float(lamps[i].x);
			light.position[1] = float(lamps[i].y);
			light.position[2] = float(lamps[i].z);
			DDGILightTracker::observe(RID::from_uint64(1000 + i), light, Vector3());
		}
		DDGILightTracker::end();
		const uint64_t after = DDGILightTracker::events_since(serial, events);
		const uint64_t fired = after - serial;
		run.events += fired;
		run.lost += fired - events.size();
		run.worst = MAX(run.worst, fired);
		run.per_frame.push_back(fired);
		serial = after;
	}
	run.region_key = state.slots[0].region_key;
	DDGILightTracker::begin(false, 0, 1.0f);
	return run;
}

TEST_CASE("[RayScene] Stable light region steps stay under the tracker ring, and their events match Camera mode") {
	TrackerOff tracker_off;
	const Vector3 camera_at(5.0f, 1.0f, -3.0f);
	CHECK(RayScene::tracker_region_for(true, camera_at, PICK_HALF) == AABB(camera_at - PICK_HALF, PICK_HALF * 2.0f));
	CHECK_FALSE(RayScene::tracker_region_for(false, camera_at, PICK_HALF).has_volume());
	CHECK_FALSE(RayScene::tracker_region_for(true, camera_at, Vector3()).has_volume());
	const AABB far_box(Vector3(5000.0f, 0.0f, 0.0f), Vector3(1.0f, 1.0f, 1.0f));
	const AABB near_region = RayScene::tracker_region_for(true, camera_at, PICK_HALF);
	CHECK(RayScene::tracker_observes(RSE::LIGHT_DIRECTIONAL, near_region, far_box));
	CHECK_FALSE(RayScene::tracker_observes(RSE::LIGHT_OMNI, near_region, far_box));
	CHECK(RayScene::tracker_observes(RSE::LIGHT_OMNI, AABB(), far_box));
	const AABB unit_region(Vector3(), Vector3(10.0f, 10.0f, 10.0f));
	CHECK_FALSE(RayScene::tracker_observes(RSE::LIGHT_OMNI, unit_region, AABB(Vector3(10.0f, 0.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f))));
	CHECK(RayScene::tracker_observes(RSE::LIGHT_OMNI, unit_region, AABB(Vector3(9.0f, 0.0f, 0.0f), Vector3(2.0f, 2.0f, 2.0f))));

	const StepRun seen = region_step_run(STEP_STABLE);
	const StepRun snapped = region_step_run(STEP_STABLE_SNAPPED);
	const StepRun stale = region_step_run(STEP_STABLE_STALE);
	const StepRun camera = region_step_run(STEP_CAMERA);

	CHECK(snapped.region_key == Vector3i(3, 0, 0));
	CHECK(seen.region_key == Vector3i(3, 0, 0));
	CHECK(snapped.worst == 160);
	CHECK(snapped.lost > 0);
	REQUIRE(stale.per_frame.size() == camera.per_frame.size());
	uint32_t stale_frames = 0;
	for (uint32_t f = 0; f < stale.per_frame.size(); f++) {
		stale_frames += stale.per_frame[f] != camera.per_frame[f];
	}
	CHECK(stale_frames > 0);

	CHECK(seen.lost == 0);
	CHECK(camera.lost == 0);
	CHECK(seen.events > 0);
	CHECK(seen.worst <= 36);
	CHECK(seen.worst < DDGILightTracker::MAX_EVENTS);
	CHECK(seen.events == camera.events);
	REQUIRE(seen.per_frame.size() == camera.per_frame.size());
	for (uint32_t f = 0; f < seen.per_frame.size(); f++) {
		INFO("frame ", f);
		CHECK(seen.per_frame[f] == camera.per_frame[f]);
	}
}

} // namespace TestRaySceneLights

#endif
