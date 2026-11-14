/**************************************************************************/
/*  test_restir_events.h                                                  */
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

#include "../restir_events.h"

#include "core/templates/local_vector.h"
#include "tests/test_macros.h"

namespace TestRestirEvents {

constexpr float FOUR_PI = 12.566371f;

RestirEventSphere make_sphere(const Vector3 &p_center, float p_reach, uint32_t p_since_last = UINT32_MAX) {
	RestirEventSphere sphere;
	sphere.center = p_center;
	sphere.reach = p_reach;
	sphere.since_last = p_since_last;
	return sphere;
}

RestirEventPlan plan_for(const LocalVector<RestirEventSphere> &p_spheres, float p_reach_scale = 2.0f, float p_solid_angle = 0.05f, uint32_t p_quiet = 0, uint32_t p_jump_m_cap = 0, bool p_directional_jump = false, uint32_t p_lost = 0, bool p_history = true) {
	RestirEventInputs inputs;
	inputs.settings.events_on = true;
	inputs.settings.reach_scale = p_reach_scale;
	inputs.settings.solid_angle = p_solid_angle;
	inputs.settings.jump_m_cap = p_jump_m_cap;
	inputs.events = p_spheres.ptr();
	inputs.event_count = p_spheres.size();
	inputs.quiet_frames = p_quiet;
	inputs.directional_jump = p_directional_jump;
	inputs.events_lost = p_lost;
	inputs.trace_width = 64;
	inputs.trace_height = 32;
	inputs.history = p_history;
	RestirEventPlan plan;
	restir_plan_events(inputs, plan);
	return plan;
}

TEST_CASE("[RestirEvents] Budget is a floored percent of trace pixels") {
	CHECK(restir_event_budget(1920, 1080, 25) == 518400u);
	CHECK(restir_event_budget(3, 3, 25) == 2u);
	CHECK(restir_event_budget(64, 32, 0) == 0u);
	CHECK(restir_event_budget(64, 32, 100) == 2048u);
	CHECK(restir_event_budget(64, 32, 250) == 2048u);
}

TEST_CASE("[RestirEvents] First fire against the quiet window") {
	CHECK(restir_event_first_fire(UINT32_MAX, 0));
	CHECK(restir_event_first_fire(UINT32_MAX, 120));
	CHECK(restir_event_first_fire(0, 0));
	CHECK_FALSE(restir_event_first_fire(29, 30));
	CHECK(restir_event_first_fire(30, 30));
}

TEST_CASE("[RestirEvents] Solid angle of a sphere") {
	CHECK(restir_event_solid_angle(5.0f, 40.0f) == doctest::Approx(0.0492811f).epsilon(1e-4));
	CHECK(restir_event_solid_angle(5.0f, 39.0f) > 0.05f);
	CHECK(restir_event_solid_angle(5.0f, 41.0f) < 0.05f);
	CHECK(restir_event_solid_angle(5.0f, 5.0f) == doctest::Approx(FOUR_PI));
	CHECK(restir_event_solid_angle(5.0f, 2.0f) == doctest::Approx(FOUR_PI));
	CHECK(restir_event_solid_angle(0.0f, 0.0f) == doctest::Approx(FOUR_PI));
	CHECK(restir_event_solid_angle(1e-3f, 1000.0f) > 0.0f);
}

TEST_CASE("[RestirEvents] Receiver cap: reach scale and solid angle") {
	LocalVector<RestirEventSphere> spheres;
	spheres.push_back(make_sphere(Vector3(10, 0, 0), 1.0f));
	const RestirEventPlan reach_only = plan_for(spheres, 2.0f, FOUR_PI);
	CHECK(restir_event_caps(reach_only.ubo, Vector3(11.9, 0, 0)));
	CHECK_FALSE(restir_event_caps(reach_only.ubo, Vector3(12.1, 0, 0)));
	const RestirEventPlan angle_only = plan_for(spheres, 0.0f, 0.05f);
	CHECK(angle_only.ubo.params[0] == 0.0f);
	CHECK(restir_event_caps(angle_only.ubo, Vector3(10, 7.8, 0)));
	CHECK_FALSE(restir_event_caps(angle_only.ubo, Vector3(10, 8.2, 0)));
	CHECK(restir_event_caps(angle_only.ubo, Vector3(10, 0.5, 0)));
	LocalVector<RestirEventSphere> far;
	far.push_back(make_sphere(Vector3(0, 0, 0), 5.0f));
	const RestirEventPlan defaults = plan_for(far);
	CHECK(restir_event_caps(defaults.ubo, Vector3(0, 0, 39)));
	CHECK_FALSE(restir_event_caps(defaults.ubo, Vector3(0, 0, 41)));
}

TEST_CASE("[RestirEvents] Repeats re-shade but do not cap") {
	LocalVector<RestirEventSphere> spheres;
	spheres.push_back(make_sphere(Vector3(0, 0, 0), 1.0f, 5));
	const RestirEventPlan quiet = plan_for(spheres, 2.0f, 0.05f, 30);
	CHECK(quiet.capping == 0u);
	CHECK(restir_event_holds(quiet.ubo, Vector3(0.5, 0, 0)));
	CHECK_FALSE(restir_event_caps(quiet.ubo, Vector3(0.5, 0, 0)));
	const RestirEventPlan loud = plan_for(spheres, 2.0f, 0.05f, 0);
	CHECK(loud.capping == 1u);
	CHECK(restir_event_caps(loud.ubo, Vector3(0.5, 0, 0)));
}

TEST_CASE("[RestirEvents] Sample test uses the raw reach") {
	LocalVector<RestirEventSphere> spheres;
	spheres.push_back(make_sphere(Vector3(0, 0, 0), 1.0f));
	spheres.push_back(make_sphere(Vector3(5, 0, 0), 0.5f));
	const RestirEventPlan plan = plan_for(spheres, 8.0f);
	CHECK(restir_event_holds(plan.ubo, Vector3(0.9, 0, 0)));
	CHECK(restir_event_holds(plan.ubo, Vector3(1.0, 0, 0)));
	CHECK_FALSE(restir_event_holds(plan.ubo, Vector3(1.5, 0, 0)));
	CHECK(restir_event_holds(plan.ubo, Vector3(5.4, 0, 0)));
	CHECK_FALSE(restir_event_holds(plan.ubo, Vector3(3.0, 0, 0)));
}

TEST_CASE("[RestirEvents] Hash bins and the budget cut") {
	CHECK(restir_event_bin(0.0f) == 0u);
	CHECK(restir_event_bin(-1.0f) == 0u);
	CHECK(restir_event_bin(0.5f) == 128u);
	CHECK(restir_event_bin(0.9999999f) == 255u);
	CHECK(restir_event_bin(1.0f) == 255u);
	uint32_t bins[RESTIR_EVENT_BINS] = {};
	CHECK(restir_event_cut_bin(bins, 0) == RESTIR_EVENT_BINS);
	bins[3] = 4;
	bins[10] = 4;
	bins[200] = 4;
	CHECK(restir_event_cut_bin(bins, 12) == RESTIR_EVENT_BINS);
	CHECK(restir_event_cut_bin(bins, 11) == 200u);
	CHECK(restir_event_cut_bin(bins, 8) == 200u);
	CHECK(restir_event_cut_bin(bins, 7) == 10u);
	CHECK(restir_event_cut_bin(bins, 3) == 3u);
	CHECK(restir_event_cut_bin(bins, 0) == 3u);
	uint32_t taken = 0;
	const uint32_t cut = restir_event_cut_bin(bins, 9);
	for (uint32_t t = 0; t < cut; t++) {
		taken += bins[t];
	}
	CHECK(taken == 8u);
}

TEST_CASE("[RestirEvents] Off plans nothing") {
	LocalVector<RestirEventSphere> spheres;
	spheres.push_back(make_sphere(Vector3(1, 2, 3), 1.0f));
	RestirEventInputs inputs;
	inputs.events = spheres.ptr();
	inputs.event_count = spheres.size();
	inputs.directional_jump = true;
	inputs.events_lost = 3;
	inputs.history = true;
	inputs.trace_width = 8;
	inputs.trace_height = 8;
	RestirEventPlan plan;
	restir_plan_events(inputs, plan);
	CHECK_FALSE(plan.active);
	CHECK_FALSE(plan.jump_frame);
	CHECK(plan.validate == RESTIR_EVENT_VALIDATE_NONE);
	CHECK(plan.ubo.counts[0] == 0u);
	CHECK(plan.ubo.sphere[0][3] == 0.0f);
	CHECK(plan.ubo.gates[1] == 0u);
}

TEST_CASE("[RestirEvents] Packing and selection") {
	LocalVector<RestirEventSphere> spheres;
	spheres.push_back(make_sphere(Vector3(1, 2, 3), 0.75f, 7));
	spheres.push_back(make_sphere(Vector3(-4, 5, -6), 2.0f));
	const RestirEventPlan plan = plan_for(spheres, 3.0f, 0.1f, 10);
	CHECK(plan.active);
	CHECK_FALSE(plan.jump_frame);
	CHECK(plan.event_count == 2u);
	CHECK(plan.capping == 1u);
	CHECK(plan.validate == RESTIR_EVENT_VALIDATE_EVENT);
	CHECK(plan.budget == 512u);
	CHECK(plan.ubo.counts[0] == 2u);
	CHECK(plan.ubo.counts[1] == 2u);
	CHECK(plan.ubo.counts[2] == 0u);
	CHECK(plan.ubo.counts[3] == 0u);
	CHECK(plan.ubo.params[0] == 3.0f);
	CHECK(plan.ubo.params[1] == doctest::Approx(0.1f));
	CHECK(plan.ubo.gates[0] == 10u);
	CHECK(plan.ubo.gates[1] == 512u);
	CHECK(plan.ubo.gates[2] == 0u);
	CHECK(plan.ubo.gates[3] == uint32_t(RESTIR_EVENT_VALIDATE_EVENT));
	CHECK(plan.ubo.sphere[0][0] == 1.0f);
	CHECK(plan.ubo.sphere[0][1] == 2.0f);
	CHECK(plan.ubo.sphere[0][2] == 3.0f);
	CHECK(plan.ubo.sphere[0][3] == 0.75f);
	CHECK(plan.ubo.sphere[1][3] == 2.0f);
	CHECK(plan.ubo.since_last[0] == 7u);
	CHECK(plan.ubo.since_last[1] == UINT32_MAX);
	CHECK(plan.ubo.sphere[2][3] == 0.0f);
	CHECK(plan.ubo.since_last[2] == 0u);

	LocalVector<RestirEventSphere> many;
	for (uint32_t i = 0; i < 70; i++) {
		many.push_back(make_sphere(Vector3(float(i), 0, 0), 0.5f));
	}
	const RestirEventPlan full = plan_for(many);
	CHECK(full.event_count == RESTIR_EVENT_MAX);
	CHECK(full.ubo.counts[0] == RESTIR_EVENT_MAX);
	CHECK(full.ubo.sphere[63][0] == 63.0f);

	const RestirEventPlan none = plan_for(LocalVector<RestirEventSphere>());
	CHECK(none.active);
	CHECK(none.validate == RESTIR_EVENT_VALIDATE_NONE);
	CHECK(none.capping == 0u);
	CHECK_FALSE(restir_event_caps(none.ubo, Vector3()));

	const RestirEventPlan cut = plan_for(spheres, 2.0f, 0.05f, 0, 0, false, 0, false);
	CHECK(cut.validate == RESTIR_EVENT_VALIDATE_NONE);
}

TEST_CASE("[RestirEvents] Jump frames") {
	LocalVector<RestirEventSphere> spheres;
	spheres.push_back(make_sphere(Vector3(0, 0, 0), 1.0f));
	const RestirEventPlan sun = plan_for(spheres, 2.0f, 0.05f, 0, 0, true);
	CHECK(sun.jump_frame);
	CHECK(sun.ubo.gates[2] == 1u);
	CHECK(sun.validate == RESTIR_EVENT_VALIDATE_NONE);
	CHECK(sun.ubo.counts[0] == 1u);
	const RestirEventPlan sun_arm = plan_for(spheres, 2.0f, 0.05f, 0, 4, true);
	CHECK(sun_arm.validate == RESTIR_EVENT_VALIDATE_ALL);
	CHECK(sun_arm.ubo.counts[3] == 4u);
	const RestirEventPlan lost = plan_for(spheres, 2.0f, 0.05f, 0, 0, false, 2);
	CHECK(lost.jump_frame);
	CHECK(lost.validate == RESTIR_EVENT_VALIDATE_NONE);
	const RestirEventPlan lost_no_events = plan_for(LocalVector<RestirEventSphere>(), 2.0f, 0.05f, 0, 3, false, 1);
	CHECK(lost_no_events.jump_frame);
	CHECK(lost_no_events.validate == RESTIR_EVENT_VALIDATE_ALL);
	const RestirEventPlan quiet = plan_for(spheres);
	CHECK_FALSE(quiet.jump_frame);
	CHECK(quiet.ubo.gates[2] == 0u);
	const RestirEventPlan jump_no_history = plan_for(spheres, 2.0f, 0.05f, 0, 4, true, 0, false);
	CHECK(jump_no_history.validate == RESTIR_EVENT_VALIDATE_NONE);
}

RestirHoldUpdate hold_step(RestirHoldRing &r_ring, const LocalVector<RestirEventSphere> &p_spheres, uint32_t p_hold_frames, uint32_t p_quiet = 0, const Vector3 &p_anchor = Vector3()) {
	return restir_hold_update(r_ring, p_spheres.ptr(), p_spheres.size(), p_anchor, p_quiet, p_hold_frames);
}

TEST_CASE("[RestirEvents] Hold keeps a sphere for its frames from the event frame") {
	RestirHoldRing ring;
	LocalVector<RestirEventSphere> fire;
	fire.push_back(make_sphere(Vector3(1, 2, 3), 0.5f));
	const LocalVector<RestirEventSphere> none;
	RestirHoldUpdate first = hold_step(ring, fire, 40);
	CHECK(first.added == 1u);
	CHECK(ring.count == 1u);
	CHECK(ring.entries[0].age == 0u);
	for (uint32_t k = 1; k < 40; k++) {
		hold_step(ring, none, 40);
		CHECK(ring.count == 1u);
		CHECK(ring.entries[0].age == k);
	}
	RestirHoldUpdate gone = hold_step(ring, none, 40);
	CHECK(gone.expired == 1u);
	CHECK(ring.count == 0u);

	hold_step(ring, fire, 1);
	CHECK(ring.count == 1u);
	CHECK(hold_step(ring, none, 1).expired == 1u);
	CHECK(ring.count == 0u);

	hold_step(ring, fire, 40);
	RestirHoldUpdate zero = hold_step(ring, fire, 0);
	CHECK(zero.expired == 1u);
	CHECK(zero.added == 0u);
	CHECK(ring.count == 0u);
}

TEST_CASE("[RestirEvents] Hold refreshes a held sphere that holds the new one") {
	RestirHoldRing ring;
	LocalVector<RestirEventSphere> big;
	big.push_back(make_sphere(Vector3(0, 0, 0), 2.0f));
	const LocalVector<RestirEventSphere> none;
	hold_step(ring, big, 40);
	for (int k = 0; k < 10; k++) {
		hold_step(ring, none, 40);
	}
	CHECK(ring.entries[0].age == 10u);
	LocalVector<RestirEventSphere> inside;
	inside.push_back(make_sphere(Vector3(1.5, 0, 0), 1.0f));
	RestirHoldUpdate refresh = hold_step(ring, inside, 40);
	CHECK(refresh.refreshed == 1u);
	CHECK(refresh.added == 0u);
	CHECK(ring.count == 1u);
	CHECK(ring.entries[0].age == 0u);
	CHECK(ring.entries[0].reach == 2.0f);

	LocalVector<RestirEventSphere> larger;
	larger.push_back(make_sphere(Vector3(0.5, 0, 0), 3.0f));
	CHECK(hold_step(ring, larger, 40).added == 1u);
	CHECK(ring.count == 2u);

	LocalVector<RestirEventSphere> outside;
	outside.push_back(make_sphere(Vector3(2.5, 0, 0), 0.5f));
	RestirHoldRing lone;
	hold_step(lone, big, 40);
	CHECK(hold_step(lone, outside, 40).added == 1u);
	CHECK(lone.count == 2u);

	LocalVector<RestirEventSphere> pair;
	pair.push_back(make_sphere(Vector3(4, 5, 6), 1.0f, 12));
	pair.push_back(make_sphere(Vector3(4, 5, 6), 1.0f, 12));
	RestirHoldRing flicker;
	RestirHoldUpdate both = hold_step(flicker, pair, 40);
	CHECK(both.added == 1u);
	CHECK(both.refreshed == 1u);
	CHECK(flicker.count == 1u);
}

TEST_CASE("[RestirEvents] Hold takes first fires only") {
	RestirHoldRing ring;
	LocalVector<RestirEventSphere> repeat;
	repeat.push_back(make_sphere(Vector3(0, 0, 0), 1.0f, 10));
	CHECK(hold_step(ring, repeat, 40, 30).added == 0u);
	CHECK(ring.count == 0u);
	CHECK(hold_step(ring, repeat, 40, 10).added == 1u);
	CHECK(ring.count == 1u);
	RestirHoldRing empty;
	CHECK(restir_hold_update(empty, nullptr, 5, Vector3(), 0, 40).added == 0u);
	CHECK(empty.count == 0u);
}

TEST_CASE("[RestirEvents] Hold drops the oldest when the ring is full") {
	RestirHoldRing ring;
	for (uint32_t i = 0; i < RESTIR_HOLD_MAX; i++) {
		LocalVector<RestirEventSphere> one;
		one.push_back(make_sphere(Vector3(real_t(i) * 10.0, 0, 0), 1.0f));
		CHECK(hold_step(ring, one, 256).dropped == 0u);
	}
	CHECK(ring.count == RESTIR_HOLD_MAX);
	CHECK(ring.entries[0].age == RESTIR_HOLD_MAX - 1u);
	LocalVector<RestirEventSphere> extra;
	extra.push_back(make_sphere(Vector3(0, 100, 0), 1.0f));
	RestirHoldUpdate full = hold_step(ring, extra, 256);
	CHECK(full.dropped == 1u);
	CHECK(full.added == 1u);
	CHECK(ring.count == RESTIR_HOLD_MAX);
	CHECK(ring.entries[0].center.is_equal_approx(Vector3(0, 100, 0)));
	CHECK(ring.entries[0].age == 0u);
	CHECK(ring.entries[1].age == RESTIR_HOLD_MAX - 1u);
}

TEST_CASE("[RestirEvents] Hold clears, and breaks an age tie at the lowest index") {
	RestirHoldRing ring;
	LocalVector<RestirEventSphere> many;
	for (uint32_t i = 0; i < RESTIR_HOLD_MAX; i++) {
		many.push_back(make_sphere(Vector3(real_t(i) * 10.0, 0, 0), 1.0f));
	}
	CHECK(hold_step(ring, many, 40).added == RESTIR_HOLD_MAX);
	LocalVector<RestirEventSphere> extra;
	extra.push_back(make_sphere(Vector3(0, 100, 0), 1.0f));
	RestirHoldUpdate tie = hold_step(ring, extra, 40);
	CHECK(tie.dropped == 1u);
	CHECK(ring.entries[0].center.is_equal_approx(Vector3(0, 100, 0)));
	CHECK(ring.entries[1].center.is_equal_approx(Vector3(10, 0, 0)));
	restir_hold_clear(ring);
	CHECK(ring.count == 0u);
	const LocalVector<RestirEventSphere> none;
	CHECK(hold_step(ring, none, 40).expired == 0u);
	CHECK(ring.count == 0u);
}

TEST_CASE("[RestirEvents] Hold keeps world positions across an anchor move") {
	RestirHoldRing ring;
	LocalVector<RestirEventSphere> fire;
	fire.push_back(make_sphere(Vector3(1, 0, 0), 0.5f));
	hold_step(ring, fire, 40, 0, Vector3(100, 0, 0));
	CHECK(ring.entries[0].center.is_equal_approx(Vector3(101, 0, 0)));
	LocalVector<RestirEventSphere> moved;
	moved.push_back(make_sphere(Vector3(11.2, 0, 0), 0.25f));
	RestirHoldUpdate again = hold_step(ring, moved, 40, 0, Vector3(90, 0, 0));
	CHECK(again.refreshed == 1u);
	CHECK(ring.count == 1u);
	LocalVector<RestirEventSphere> stale;
	stale.push_back(make_sphere(Vector3(1, 0, 0), 0.25f));
	CHECK(hold_step(ring, stale, 40, 0, Vector3(90, 0, 0)).added == 1u);
	CHECK(ring.count == 2u);
	RestirEventsUBO ubo = {};
	restir_hold_pack(ring, Vector3(90, 0, 0), ubo);
	CHECK(ubo.hold[0] == 2u);
	CHECK(ubo.held[1][0] == doctest::Approx(1.0f));
	CHECK(ubo.held[0][0] == doctest::Approx(11.0f));
	CHECK(ubo.held[0][3] == 0.5f);
}

TEST_CASE("[RestirEvents] Hold caps by reach and solid angle like the event test") {
	RestirHoldRing ring;
	LocalVector<RestirEventSphere> fire;
	fire.push_back(make_sphere(Vector3(0, 0, 0), 5.0f));
	hold_step(ring, fire, 40);
	RestirEventsUBO ubo = {};
	ubo.params[0] = 2.0f;
	ubo.params[1] = 0.05f;
	CHECK_FALSE(restir_hold_caps(ubo, Vector3(1, 0, 0)));
	restir_hold_pack(ring, Vector3(), ubo);
	CHECK(restir_hold_caps(ubo, Vector3(9.9, 0, 0)));
	CHECK(restir_hold_caps(ubo, Vector3(39, 0, 0)));
	CHECK_FALSE(restir_hold_caps(ubo, Vector3(41, 0, 0)));
	RestirEventsUBO as_event = ubo;
	as_event.counts[0] = 1;
	as_event.counts[1] = 1;
	as_event.since_last[0] = UINT32_MAX;
	for (int i = 0; i < 4; i++) {
		as_event.sphere[0][i] = ubo.held[0][i];
	}
	for (real_t x = 0.0; x < 60.0; x += 0.5) {
		CHECK(restir_hold_caps(ubo, Vector3(x, 0, 0)) == restir_event_caps(as_event, Vector3(x, 0, 0)));
	}
}

RestirSkyEpoch sky_epoch_at(float p_luma) {
	RestirSkyEpoch epoch;
	restir_sky_epoch_step(epoch, p_luma, true);
	return epoch;
}

TEST_CASE("[RestirEvents] Sky epoch starts on the first frame without a jump") {
	RestirSkyEpoch epoch;
	CHECK_FALSE(restir_sky_epoch_step(epoch, 3.0f, true));
	CHECK(epoch.luma == 3.0f);
	CHECK(epoch.epochs == 1u);
	CHECK(epoch.jump == 0u);
	CHECK(epoch.jumps == 0u);
	RestirSkyEpoch dark;
	CHECK_FALSE(restir_sky_epoch_step(dark, 0.0f, false));
	CHECK(dark.epochs == 1u);
	CHECK(dark.luma == 0.0f);
}

TEST_CASE("[RestirEvents] Sky epoch keeps its luma inside the band, edges included") {
	const float inside[] = { 1.0f, 0.8f, 1.25f, 0.9f, 1.2f, 0.81f, 1.249f };
	for (float ratio : inside) {
		RestirSkyEpoch epoch = sky_epoch_at(1.0f);
		CHECK_FALSE(restir_sky_epoch_step(epoch, ratio, true));
		CHECK(epoch.luma == 1.0f);
		CHECK(epoch.epochs == 1u);
		CHECK(epoch.jump == 0u);
		CHECK(epoch.jumps == 0u);
	}
	RestirSkyEpoch scaled = sky_epoch_at(2.0f);
	CHECK_FALSE(restir_sky_epoch_step(scaled, 1.6f, true));
	CHECK_FALSE(restir_sky_epoch_step(scaled, 2.5f, true));
	CHECK(scaled.luma == 2.0f);
}

TEST_CASE("[RestirEvents] Sky epoch jumps outside the band and starts a new epoch") {
	RestirSkyEpoch low = sky_epoch_at(1.0f);
	CHECK(restir_sky_epoch_step(low, 0.79f, true));
	CHECK(low.jump == 1u);
	CHECK(low.luma == 0.79f);
	CHECK(low.epochs == 2u);
	CHECK(low.jumps == 1u);
	CHECK_FALSE(restir_sky_epoch_step(low, 0.79f, true));
	CHECK(low.jump == 0u);
	CHECK(low.epochs == 2u);
	RestirSkyEpoch high = sky_epoch_at(1.0f);
	CHECK(restir_sky_epoch_step(high, 1.26f, true));
	CHECK(high.jump == 1u);
	CHECK(high.luma == 1.26f);
	CHECK(high.jumps == 1u);
}

TEST_CASE("[RestirEvents] Sky epoch measures each jump from the new epoch, not the first") {
	RestirSkyEpoch epoch = sky_epoch_at(1.0f);
	CHECK_FALSE(restir_sky_epoch_step(epoch, 1.2f, true));
	CHECK_FALSE(restir_sky_epoch_step(epoch, 1.24f, true));
	CHECK(restir_sky_epoch_step(epoch, 1.3f, true));
	CHECK(epoch.luma == 1.3f);
	CHECK_FALSE(restir_sky_epoch_step(epoch, 1.6f, true));
	CHECK(restir_sky_epoch_step(epoch, 1.7f, true));
	CHECK(epoch.epochs == 3u);
	CHECK(epoch.jumps == 2u);
}

TEST_CASE("[RestirEvents] Sky epoch restarts on a cut without a jump") {
	RestirSkyEpoch epoch = sky_epoch_at(1.0f);
	CHECK_FALSE(restir_sky_epoch_step(epoch, 5.0f, false));
	CHECK(epoch.jump == 0u);
	CHECK(epoch.luma == 5.0f);
	CHECK(epoch.epochs == 2u);
	CHECK(epoch.jumps == 0u);
	CHECK_FALSE(restir_sky_epoch_step(epoch, 4.5f, true));
	CHECK(restir_sky_epoch_step(epoch, 1.0f, true));
	epoch.jump = 1;
	CHECK_FALSE(restir_sky_epoch_step(epoch, 1.0f, false));
	CHECK(epoch.jump == 0u);
}

TEST_CASE("[RestirEvents] Sky epoch from a black sky jumps on any light and skips a non-finite luma") {
	RestirSkyEpoch epoch = sky_epoch_at(0.0f);
	CHECK_FALSE(restir_sky_epoch_step(epoch, 0.0f, true));
	CHECK(restir_sky_epoch_step(epoch, 1e-6f, true));
	CHECK(epoch.luma == 1e-6f);
	epoch.jump = 1;
	CHECK_FALSE(restir_sky_epoch_step(epoch, float(Math::NaN), true));
	CHECK_FALSE(restir_sky_epoch_step(epoch, float(Math::INF), true));
	CHECK(epoch.jump == 0u);
	CHECK(epoch.luma == 1e-6f);
	CHECK(epoch.epochs == 2u);
	RestirSkyEpoch none;
	CHECK_FALSE(restir_sky_epoch_step(none, float(Math::NaN), true));
	CHECK(none.epochs == 0u);
}

TEST_CASE("[RestirEvents] Sky jump counts as capped only when the event cap is on, not already applied and below the temporal cap") {
	CHECK(restir_sky_jump_caps(true, 0u, 4u, 20u));
	CHECK(restir_sky_jump_caps(true, 0u, 0u, 20u));
	CHECK_FALSE(restir_sky_jump_caps(false, 0u, 4u, 20u));
	CHECK_FALSE(restir_sky_jump_caps(true, 1u, 4u, 20u));
	CHECK_FALSE(restir_sky_jump_caps(true, 0u, 20u, 20u));
	CHECK_FALSE(restir_sky_jump_caps(true, 0u, 32u, 20u));
	RestirSkyEpoch epoch = sky_epoch_at(1.0f);
	CHECK_FALSE(restir_sky_epoch_step(epoch, 1.1f, true, true));
	CHECK(epoch.capped == 0u);
	CHECK(restir_sky_epoch_step(epoch, 2.0f, true, false));
	CHECK(epoch.jumps == 1u);
	CHECK(epoch.capped == 0u);
	CHECK(restir_sky_epoch_step(epoch, 4.0f, true, true));
	CHECK(epoch.jumps == 2u);
	CHECK(epoch.capped == 1u);
	CHECK_FALSE(restir_sky_epoch_step(epoch, 9.0f, false, true));
	CHECK(epoch.capped == 1u);
}

TEST_CASE("[RestirEvents] Tile plan reads nothing while every tile is traced") {
	CHECK(restir_plan_tiles(true, true, 1920u, 1080u, 240u, 135u) == RESTIR_TILES_ALL);
	CHECK(restir_plan_tiles(true, false, 1920u, 1080u, 0u, 0u) == RESTIR_TILES_ALL);
	CHECK(restir_plan_tiles(true, true, 1920u, 1080u, 7u, 7u) == RESTIR_TILES_ALL);
	CHECK(strcmp(restir_tile_state_name(RESTIR_TILES_ALL), "all") == 0);
}

TEST_CASE("[RestirEvents] Tile plan skips only with a texture of one texel per tile") {
	CHECK(restir_plan_tiles(false, true, 1920u, 1080u, 240u, 135u) == RESTIR_TILES_SKIP);
	CHECK(restir_plan_tiles(false, true, 1921u, 1081u, 241u, 136u) == RESTIR_TILES_SKIP);
	CHECK(restir_plan_tiles(false, true, 1u, 1u, 1u, 1u) == RESTIR_TILES_SKIP);
	CHECK(restir_plan_tiles(false, true, 1921u, 1081u, 240u, 135u) == RESTIR_TILES_UNAVAILABLE);
	CHECK(restir_plan_tiles(false, true, 1920u, 1080u, 240u, 136u) == RESTIR_TILES_UNAVAILABLE);
	CHECK(restir_plan_tiles(false, false, 1920u, 1080u, 240u, 135u) == RESTIR_TILES_UNAVAILABLE);
	CHECK(restir_plan_tiles(false, true, 0u, 1080u, 0u, 135u) == RESTIR_TILES_UNAVAILABLE);
	CHECK(strcmp(restir_tile_state_name(RESTIR_TILES_SKIP), "skip") == 0);
	CHECK(strcmp(restir_tile_state_name(RESTIR_TILES_UNAVAILABLE), "unavailable") == 0);
}

uint32_t pack_pdf_bits(const RestirPackWord &p_word) {
	uint32_t bits = 0;
	memcpy(&bits, &p_word.pdf, sizeof(uint32_t));
	return bits;
}

RestirPackPixel pack_pixel(bool p_traced, bool p_geometry, bool p_blocked, bool p_covered, uint32_t p_m) {
	RestirPackPixel pixel;
	pixel.tile_traced = p_traced;
	pixel.geometry = p_geometry;
	pixel.blocked = p_blocked;
	pixel.covered = p_covered;
	pixel.m = p_m;
	return pixel;
}

TEST_CASE("[RestirEvents] Pack word keeps the old flags and a zero pdf word with the defaults") {
	for (const bool traced : { false, true }) {
		const RestirPackWord none = restir_pack_word(pack_pixel(traced, false, false, false, 5u), false, false);
		CHECK(none.flags == RESTIR_CANDIDATE_NO_CANDIDATE);
		CHECK_FALSE(none.radiance);
		const RestirPackWord blocked = restir_pack_word(pack_pixel(traced, true, true, true, 5u), false, false);
		CHECK(blocked.flags == 0u);
		CHECK_FALSE(blocked.radiance);
		const RestirPackWord covered = restir_pack_word(pack_pixel(traced, true, false, true, 5u), false, false);
		CHECK(covered.flags == 0u);
		CHECK(covered.radiance);
		const RestirPackWord thinned = restir_pack_word(pack_pixel(traced, true, false, false, 5u), false, false);
		CHECK(thinned.flags == RESTIR_CANDIDATE_THINNED);
		CHECK_FALSE(thinned.radiance);
		CHECK(pack_pdf_bits(none) == 0u);
		CHECK(pack_pdf_bits(blocked) == 0u);
		CHECK(pack_pdf_bits(covered) == 0u);
		CHECK(pack_pdf_bits(thinned) == 0u);
	}
}

TEST_CASE("[RestirEvents] Pack word marks untraced tiles not traced, with no radiance or count") {
	for (const bool geometry : { false, true }) {
		const RestirPackWord word = restir_pack_word(pack_pixel(false, geometry, false, true, 9u), true, true);
		CHECK(word.flags == (RESTIR_CANDIDATE_NO_CANDIDATE | RESTIR_CANDIDATE_NOT_TRACED));
		CHECK_FALSE(word.radiance);
		CHECK(pack_pdf_bits(word) == 0u);
	}
	const RestirPackWord traced = restir_pack_word(pack_pixel(true, true, false, true, 9u), true, false);
	CHECK(traced.flags == 0u);
	CHECK(traced.radiance);
	CHECK(pack_pdf_bits(traced) == 0u);
}

TEST_CASE("[RestirEvents] Pack word carries M only where it packs a candidate") {
	CHECK(restir_pack_word(pack_pixel(true, true, false, true, 7u), true, true).pdf == 7.0f);
	CHECK(restir_pack_word(pack_pixel(true, true, true, false, 3u), false, true).pdf == 3.0f);
	CHECK(restir_pack_word(pack_pixel(true, true, false, true, 0x1FFFFu), false, true).pdf == 65535.0f);
	CHECK(pack_pdf_bits(restir_pack_word(pack_pixel(true, true, false, true, 0u), false, true)) == 0u);
	CHECK(pack_pdf_bits(restir_pack_word(pack_pixel(true, true, false, false, 7u), false, true)) == 0u);
	CHECK(pack_pdf_bits(restir_pack_word(pack_pixel(true, false, false, true, 7u), false, true)) == 0u);
}

}
