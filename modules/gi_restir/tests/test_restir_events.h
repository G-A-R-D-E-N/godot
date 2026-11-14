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

}
