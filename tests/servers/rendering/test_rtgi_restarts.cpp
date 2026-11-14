/**************************************************************************/
/*  test_rtgi_restarts.cpp                                                */
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

TEST_FORCE_LINK(test_rtgi_restarts)

#ifdef RD_ENABLED

#include "core/math/basis.h"
#include "servers/rendering/renderer_rd/environment/rtgi_restarts.h"

namespace TestRtgiRestarts {

using namespace RendererRD;

constexpr uint32_t QUIET_NONE = 0;
constexpr uint32_t QUIET_30 = 30;

DDGILightTracker::Event make_event(const Vector3 &p_center, float p_half, uint32_t p_since_last = 100) {
	DDGILightTracker::Event event;
	event.center = p_center;
	event.half = p_half;
	event.since_last = p_since_last;
	return event;
}

RtgiRestarts::Selection run_select(const LocalVector<DDGILightTracker::Event> &p_events, uint32_t p_quiet = QUIET_NONE) {
	RtgiRestarts::Selection selection;
	RtgiRestarts::select(p_events, Transform3D(), Vector3(), Vector3(144, 80, 144), p_quiet, selection);
	return selection;
}

RtgiRestarts::DirectionalSample make_sample(uint64_t p_id, const Vector3 &p_direction, float p_energy) {
	RtgiRestarts::DirectionalSample sample;
	sample.light = RID::from_uint64(p_id);
	sample.direction = p_direction;
	sample.energy = p_energy;
	return sample;
}

Vector3 tilted(float p_degrees) {
	const float radians = Math::deg_to_rad(p_degrees);
	return Vector3(Math::sin(radians), -Math::cos(radians), 0.0f);
}

TEST_CASE("[RtgiRestarts] No events selects nothing") {
	const RtgiRestarts::Selection selection = run_select(LocalVector<DDGILightTracker::Event>());
	CHECK(selection.count == 0);
	CHECK(selection.dropped == 0);
	CHECK(selection.repeats == 0);
	CHECK(selection.crossings == 0);
}

TEST_CASE("[RtgiRestarts] A sphere lands in view space with its reach") {
	Transform3D camera;
	camera.origin = Vector3(10, 2, -3);
	camera.basis = Basis(Vector3(0, 1, 0), Math::deg_to_rad(90.0f));
	Transform3D world_to_view;
	world_to_view.basis = Basis(Vector3(0, 1, 0), Math::deg_to_rad(-90.0f));
	world_to_view.origin = world_to_view.basis.xform(-camera.origin);

	LocalVector<DDGILightTracker::Event> events;
	events.push_back(make_event(camera.origin + camera.basis.xform(Vector3(1, 2, -5)), 3.5f));
	RtgiRestarts::Selection selection;
	RtgiRestarts::select(events, world_to_view, camera.origin, Vector3(144, 80, 144), QUIET_NONE, selection);
	REQUIRE(selection.count == 1);
	CHECK(selection.spheres[0][0] == doctest::Approx(1.0f));
	CHECK(selection.spheres[0][1] == doctest::Approx(2.0f));
	CHECK(selection.spheres[0][2] == doctest::Approx(-5.0f));
	CHECK(selection.spheres[0][3] == doctest::Approx(3.5f));
}

TEST_CASE("[RtgiRestarts] Only a new or gone lamp outside the inner box is a crossing") {
	LocalVector<DDGILightTracker::Event> events;
	events.push_back(make_event(Vector3(127, 0, 0), 2.0f, UINT32_MAX));
	events.push_back(make_event(Vector3(0, -71, 0), 2.0f, UINT32_MAX));
	events.push_back(make_event(Vector3(0, 0, -127), 2.0f, UINT32_MAX));
	RtgiRestarts::Selection selection = run_select(events);
	CHECK(selection.crossings == 3);
	CHECK(selection.count == 0);

	events.clear();
	events.push_back(make_event(Vector3(125, 69, -125), 2.0f, UINT32_MAX));
	selection = run_select(events);
	CHECK(selection.crossings == 0);
	CHECK(selection.count == 1);

	events.clear();
	events.push_back(make_event(Vector3(127, 0, 0), 2.0f, 50));
	selection = run_select(events);
	CHECK(selection.crossings == 0);
	CHECK(selection.count == 1);
}

TEST_CASE("[RtgiRestarts] A crossing is measured from the camera") {
	LocalVector<DDGILightTracker::Event> events;
	events.push_back(make_event(Vector3(1000, 0, 0), 2.0f, UINT32_MAX));
	RtgiRestarts::Selection selection;
	Transform3D world_to_view;
	world_to_view.origin = Vector3(-990, 0, 0);
	RtgiRestarts::select(events, world_to_view, Vector3(990, 0, 0), Vector3(144, 80, 144), QUIET_NONE, selection);
	CHECK(selection.crossings == 0);
	REQUIRE(selection.count == 1);
	CHECK(selection.spheres[0][0] == doctest::Approx(10.0f));
}

TEST_CASE("[RtgiRestarts] Repeats keep their history only inside the quiet spell") {
	LocalVector<DDGILightTracker::Event> events;
	events.push_back(make_event(Vector3(1, 0, 0), 2.0f, 8));
	CHECK(run_select(events, QUIET_NONE).count == 1);
	RtgiRestarts::Selection selection = run_select(events, QUIET_30);
	CHECK(selection.count == 0);
	CHECK(selection.repeats == 1);

	events.clear();
	events.push_back(make_event(Vector3(1, 0, 0), 2.0f, 29));
	CHECK(run_select(events, QUIET_30).repeats == 1);
	events.clear();
	events.push_back(make_event(Vector3(1, 0, 0), 2.0f, 30));
	selection = run_select(events, QUIET_30);
	CHECK(selection.repeats == 0);
	CHECK(selection.count == 1);

	events.clear();
	events.push_back(make_event(Vector3(1, 0, 0), 2.0f, UINT32_MAX));
	selection = run_select(events, QUIET_30);
	CHECK(selection.repeats == 0);
	CHECK(selection.count == 1);
}

TEST_CASE("[RtgiRestarts] Keeps the sixteen nearest surfaces and counts the rest") {
	LocalVector<DDGILightTracker::Event> events;
	for (int i = 40; i > 0; i--) {
		events.push_back(make_event(Vector3(0, 0, -float(i)), 0.5f));
	}
	RtgiRestarts::Selection selection = run_select(events);
	CHECK(selection.count == RtgiRestarts::MAX_SPHERES);
	CHECK(selection.dropped == 24);
	for (uint32_t i = 0; i < RtgiRestarts::MAX_SPHERES; i++) {
		CHECK(selection.spheres[i][2] == doctest::Approx(-float(i + 1)));
	}

	events.clear();
	for (int i = 1; i <= 40; i++) {
		events.push_back(make_event(Vector3(0, 0, -float(i)), 0.5f));
	}
	selection = run_select(events);
	CHECK(selection.count == RtgiRestarts::MAX_SPHERES);
	CHECK(selection.dropped == 24);
	CHECK(selection.spheres[15][2] == doctest::Approx(-16.0f));

	events.clear();
	for (int i = 1; i <= 16; i++) {
		events.push_back(make_event(Vector3(0, 0, -float(i)), 0.5f));
	}
	events.push_back(make_event(Vector3(0, 0, -50), 100.0f));
	selection = run_select(events);
	CHECK(selection.count == RtgiRestarts::MAX_SPHERES);
	CHECK(selection.dropped == 1);
	CHECK(selection.spheres[0][3] == doctest::Approx(100.0f));
	CHECK(selection.spheres[15][2] == doctest::Approx(-15.0f));
}

TEST_CASE("[RtgiRestarts] Identical spheres count once") {
	LocalVector<DDGILightTracker::Event> events;
	events.push_back(make_event(Vector3(1, 2, -3), 2.0f));
	events.push_back(make_event(Vector3(1, 2, -3), 2.0f));
	CHECK(run_select(events).count == 1);
	events.push_back(make_event(Vector3(1, 2, -3), 2.5f));
	CHECK(run_select(events).count == 2);
	events.push_back(make_event(Vector3(1, 2, -3.01f), 2.0f));
	CHECK(run_select(events).count == 3);
	events.push_back(make_event(Vector3(1.01f, 2, -3), 2.0f));
	CHECK(run_select(events).count == 4);
	events.push_back(make_event(Vector3(1, 2.01f, -3), 2.0f));
	CHECK(run_select(events).count == 5);
}

TEST_CASE("[RtgiRestarts] A sphere wholly behind the camera takes no slot") {
	LocalVector<DDGILightTracker::Event> events;
	events.push_back(make_event(Vector3(0, 0, 5), 4.0f));
	RtgiRestarts::Selection selection = run_select(events);
	CHECK(selection.count == 0);
	CHECK(selection.behind == 1);
	CHECK(selection.dropped == 0);

	events.clear();
	events.push_back(make_event(Vector3(0, 0, 3), 4.0f));
	selection = run_select(events);
	CHECK(selection.count == 1);
	CHECK(selection.behind == 0);

	events.clear();
	for (int i = 1; i <= 16; i++) {
		events.push_back(make_event(Vector3(0, 0, float(i) + 1.0f), 0.5f));
	}
	events.push_back(make_event(Vector3(0, 0, -40), 0.5f));
	selection = run_select(events);
	CHECK(selection.count == 1);
	CHECK(selection.behind == 16);
	CHECK(selection.dropped == 0);
	CHECK(selection.spheres[0][2] == doctest::Approx(-40.0f));
}

TEST_CASE("[RtgiRestarts] A running sun never jumps") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 100000.0f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE);
	for (int frame = 1; frame <= 1000; frame++) {
		samples[0].direction = tilted(frame * 0.1f);
		const RtgiRestarts::DirectionalRead read = RtgiRestarts::read_directionals(samples, records, frame, 2.0f, 0.0f, QUIET_NONE);
		CHECK(read.jumps == 0);
		CHECK(read.repeats == 0);
	}
	CHECK(records.size() == 1);
}

TEST_CASE("[RtgiRestarts] A turn past the limit jumps once") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 1.0f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE);
	samples[0].direction = tilted(1.9f);
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
	samples[0].direction = tilted(1.9f + 2.1f);
	CHECK(RtgiRestarts::read_directionals(samples, records, 2, 2.0f, 0.0f, QUIET_NONE).jumps == 1);
	CHECK(RtgiRestarts::read_directionals(samples, records, 3, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
}

TEST_CASE("[RtgiRestarts] An energy step is measured against the strongest light") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 1.0f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE);
	samples[0].energy = 1.2f;
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
	samples[0].energy = 1.6f;
	CHECK(RtgiRestarts::read_directionals(samples, records, 2, 2.0f, 0.0f, QUIET_NONE).jumps == 1);
	samples[0].energy = 1.3f;
	CHECK(RtgiRestarts::read_directionals(samples, records, 3, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
	samples[0].energy = 1.0f;
	CHECK(RtgiRestarts::read_directionals(samples, records, 4, 2.0f, 0.0f, QUIET_NONE).jumps == 1);

	records.clear();
	samples.clear();
	samples.push_back(make_sample(1, tilted(0.0f), 100.0f));
	samples.push_back(make_sample(2, tilted(40.0f), 0.001f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE);
	samples[1].energy = 10.0f;
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
}

TEST_CASE("[RtgiRestarts] A faint light turning beside a strong one does not jump") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 100.0f));
	samples.push_back(make_sample(2, tilted(40.0f), 0.01f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE);
	samples[1].direction = tilted(90.0f);
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 0.0f, QUIET_NONE).jumps == 0);

	records.clear();
	samples.clear();
	samples.push_back(make_sample(2, tilted(40.0f), 0.01f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE);
	samples[0].direction = tilted(90.0f);
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 0.0f, QUIET_NONE).jumps == 1);
}

TEST_CASE("[RtgiRestarts] A light switched off has no direction to read") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 100.0f));
	samples.push_back(make_sample(2, tilted(40.0f), 0.0f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE);
	samples[1].direction = tilted(90.0f);
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
}

TEST_CASE("[RtgiRestarts] A new light and a removed light are steps from and to nothing") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 1.0f));
	CHECK(RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE).jumps == 1);
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 0.0f, QUIET_NONE).jumps == 0);

	samples.push_back(make_sample(2, tilted(30.0f), 0.1f));
	CHECK(RtgiRestarts::read_directionals(samples, records, 2, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
	CHECK(records.size() == 2);

	samples.remove_at(1);
	CHECK(RtgiRestarts::read_directionals(samples, records, 3, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
	CHECK(records.size() == 1);

	samples.clear();
	CHECK(RtgiRestarts::read_directionals(samples, records, 4, 2.0f, 0.0f, QUIET_NONE).jumps == 1);
	CHECK(records.size() == 0);
	CHECK(RtgiRestarts::read_directionals(samples, records, 5, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
}

TEST_CASE("[RtgiRestarts] A flash cuts once inside the quiet spell and again after it") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 1.0f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_30);

	samples[0].energy = 10.0f;
	RtgiRestarts::DirectionalRead read = RtgiRestarts::read_directionals(samples, records, 100, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 1);
	CHECK(read.repeats == 0);

	samples[0].energy = 1.0f;
	read = RtgiRestarts::read_directionals(samples, records, 105, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 0);
	CHECK(read.repeats == 1);

	samples[0].energy = 10.0f;
	read = RtgiRestarts::read_directionals(samples, records, 134, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 0);
	CHECK(read.repeats == 1);

	samples[0].energy = 1.0f;
	read = RtgiRestarts::read_directionals(samples, records, 164, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 1);
	CHECK(read.repeats == 0);
}

TEST_CASE("[RtgiRestarts] A light gone inside the quiet spell returns as a repeat") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 1.0f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_30);

	LocalVector<RtgiRestarts::DirectionalSample> none;
	RtgiRestarts::DirectionalRead read = RtgiRestarts::read_directionals(none, records, 100, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 1);
	CHECK(records.size() == 1);

	read = RtgiRestarts::read_directionals(samples, records, 110, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 0);
	CHECK(read.repeats == 1);

	read = RtgiRestarts::read_directionals(none, records, 200, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 1);
	CHECK(records.size() == 1);
	read = RtgiRestarts::read_directionals(none, records, 229, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 0);
	CHECK(records.size() == 1);
	read = RtgiRestarts::read_directionals(none, records, 230, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 0);
	CHECK(records.size() == 0);
}

TEST_CASE("[RtgiRestarts] A first jump is never a repeat, even in the first frames") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 1.0f));
	const RtgiRestarts::DirectionalRead read = RtgiRestarts::read_directionals(samples, records, 3, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 1);
	CHECK(read.repeats == 0);
}

TEST_CASE("[RtgiRestarts] A light that never jumped is freed when it goes") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 1.0f));
	samples.push_back(make_sample(2, tilted(40.0f), 0.1f));
	RtgiRestarts::DirectionalRead read = RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 1);
	CHECK(records.size() == 2);

	samples.resize(1);
	read = RtgiRestarts::read_directionals(samples, records, 3, 2.0f, 0.0f, QUIET_30);
	CHECK(read.jumps == 0);
	CHECK(read.repeats == 0);
	CHECK(records.size() == 1);
}

TEST_CASE("[RtgiRestarts] Lights are matched by RID, not by their order") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), 1.0f));
	samples.push_back(make_sample(2, tilted(40.0f), 0.5f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE);

	LocalVector<RtgiRestarts::DirectionalSample> swapped;
	swapped.push_back(samples[1]);
	swapped.push_back(samples[0]);
	const RtgiRestarts::DirectionalRead read = RtgiRestarts::read_directionals(swapped, records, 1, 2.0f, 0.0f, QUIET_NONE);
	CHECK(read.jumps == 0);
	CHECK(records.size() == 2);
}

TEST_CASE("[RtgiRestarts] Negative energy counts by its size, and a sign flip jumps") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(0.0f), -10.0f));
	samples.push_back(make_sample(2, tilted(40.0f), 1.0f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE);

	samples[1].energy = 1.5f;
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 0.0f, QUIET_NONE).jumps == 0);

	samples[0].energy = 10.0f;
	CHECK(RtgiRestarts::read_directionals(samples, records, 2, 2.0f, 0.0f, QUIET_NONE).jumps == 1);
}

TEST_CASE("[RtgiRestarts] No lights and dark lights read nothing") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	CHECK(RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
	samples.push_back(make_sample(1, tilted(0.0f), 0.0f));
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
	CHECK(RtgiRestarts::read_directionals(samples, records, 2, 2.0f, 0.0f, QUIET_NONE).jumps == 0);
}

TEST_CASE("[RtgiRestarts] A floor of zero keeps every step of a fading sun") {
	const float energies[] = { 30.2123f, 23.9492f, 18.5f, 14.0f, 10.2f, 7.1f, 6.5f };
	const uint32_t expected[] = { 1, 1, 1, 1, 1, 0 };
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(89.0f), energies[0]));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE);
	for (int frame = 1; frame < 7; frame++) {
		samples[0].energy = energies[frame];
		const RtgiRestarts::DirectionalRead read = RtgiRestarts::read_directionals(samples, records, frame, 2.0f, 0.0f, QUIET_NONE);
		CHECK(read.jumps == expected[frame - 1]);
		CHECK(read.repeats == 0);
	}
}

TEST_CASE("[RtgiRestarts] A lone sun's dusk step jumps only above the floor") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(89.0f), 30.2123f));
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 6.0f, QUIET_NONE);
	samples[0].energy = 23.9492f;
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 6.0f, QUIET_NONE).jumps == 1);

	records.clear();
	samples[0].energy = 30.2123f;
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 6.2f, QUIET_NONE);
	samples[0].energy = 23.9492f;
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 6.2f, QUIET_NONE).jumps == 1);

	records.clear();
	samples[0].energy = 30.2123f;
	RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 6.3f, QUIET_NONE);
	samples[0].energy = 23.9492f;
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 6.3f, QUIET_NONE).jumps == 0);

	const float tail[] = { 18.5f, 14.0f, 10.2f, 7.1f, 6.5f };
	for (int frame = 2; frame < 7; frame++) {
		samples[0].energy = tail[frame - 2];
		CHECK(RtgiRestarts::read_directionals(samples, records, frame, 2.0f, 6.0f, QUIET_NONE).jumps == 0);
	}
}

TEST_CASE("[RtgiRestarts] The floor also holds back a turn, a gone light and a new light") {
	LocalVector<RtgiRestarts::DirectionalRecord> records;
	LocalVector<RtgiRestarts::DirectionalSample> samples;
	samples.push_back(make_sample(1, tilted(80.0f), 5.0f));
	CHECK(RtgiRestarts::read_directionals(samples, records, 0, 2.0f, 0.0f, QUIET_NONE).jumps == 1);
	samples[0].direction = tilted(90.0f);
	CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, 0.0f, QUIET_NONE).jumps == 1);
	samples[0].direction = tilted(80.0f);
	CHECK(RtgiRestarts::read_directionals(samples, records, 2, 2.0f, 6.0f, QUIET_NONE).jumps == 0);

	LocalVector<RtgiRestarts::DirectionalSample> none;
	CHECK(RtgiRestarts::read_directionals(none, records, 3, 2.0f, 6.0f, QUIET_NONE).jumps == 0);
	CHECK(records.size() == 0);

	CHECK(RtgiRestarts::read_directionals(samples, records, 4, 2.0f, 6.0f, QUIET_NONE).jumps == 0);
	CHECK(records.size() == 1);
	CHECK(RtgiRestarts::read_directionals(none, records, 5, 2.0f, 0.0f, QUIET_NONE).jumps == 1);
	CHECK(records.size() == 0);

	CHECK(RtgiRestarts::read_directionals(samples, records, 6, 2.0f, 4.9f, QUIET_NONE).jumps == 1);
	samples[0].direction = tilted(90.0f);
	CHECK(RtgiRestarts::read_directionals(samples, records, 7, 2.0f, 4.9f, QUIET_NONE).jumps == 1);
	CHECK(RtgiRestarts::read_directionals(none, records, 8, 2.0f, 4.9f, QUIET_NONE).jumps == 1);
}

TEST_CASE("[RtgiRestarts] A strong light's share limit above the floor is unchanged") {
	const float floors[] = { 0.0f, 6.0f };
	for (const float jump_floor : floors) {
		LocalVector<RtgiRestarts::DirectionalRecord> records;
		LocalVector<RtgiRestarts::DirectionalSample> samples;
		samples.push_back(make_sample(1, tilted(0.0f), 100.0f));
		samples.push_back(make_sample(2, tilted(40.0f), 1.0f));
		RtgiRestarts::read_directionals(samples, records, 0, 2.0f, jump_floor, QUIET_NONE);
		samples[0].energy = 81.0f;
		CHECK(RtgiRestarts::read_directionals(samples, records, 1, 2.0f, jump_floor, QUIET_NONE).jumps == 0);
		samples[0].energy = 100.0f;
		CHECK(RtgiRestarts::read_directionals(samples, records, 2, 2.0f, jump_floor, QUIET_NONE).jumps == 0);
		samples[0].energy = 79.0f;
		CHECK(RtgiRestarts::read_directionals(samples, records, 3, 2.0f, jump_floor, QUIET_NONE).jumps == 1);
		samples[1].energy = 15.0f;
		CHECK(RtgiRestarts::read_directionals(samples, records, 4, 2.0f, jump_floor, QUIET_NONE).jumps == 0);
		samples[1].energy = 1.0f;
		samples[1].direction = tilted(60.0f);
		CHECK(RtgiRestarts::read_directionals(samples, records, 5, 2.0f, jump_floor, QUIET_NONE).jumps == 0);
	}
}

} // namespace TestRtgiRestarts

#endif
