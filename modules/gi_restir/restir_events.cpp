/**************************************************************************/
/*  restir_events.cpp                                                     */
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

#include "restir_events.h"

#include "core/math/math_funcs.h"

uint32_t restir_event_budget(uint32_t p_width, uint32_t p_height, uint32_t p_percent) {
	const uint64_t pixels = uint64_t(p_width) * uint64_t(p_height);
	return uint32_t(pixels * uint64_t(MIN(p_percent, 100u)) / 100u);
}

bool restir_event_first_fire(uint32_t p_since_last, uint32_t p_quiet_frames) {
	return p_since_last >= p_quiet_frames;
}

float restir_event_solid_angle(float p_reach, float p_distance) {
	if (p_distance <= p_reach || p_distance < 1e-6f) {
		return 4.0f * float(Math::PI);
	}
	const float k = p_reach / p_distance;
	const float k2 = k * k;
	return 2.0f * float(Math::PI) * k2 / (1.0f + Math::sqrt(1.0f - k2));
}

bool restir_event_caps(const RestirEventsUBO &p_ubo, const Vector3 &p_position) {
	const uint32_t count = MIN(p_ubo.counts[0], RESTIR_EVENT_MAX);
	for (uint32_t i = 0; i < count; i++) {
		if (i < p_ubo.counts[1] && !restir_event_first_fire(p_ubo.since_last[i], p_ubo.gates[0])) {
			continue;
		}
		const float dx = float(p_position.x) - p_ubo.sphere[i][0];
		const float dy = float(p_position.y) - p_ubo.sphere[i][1];
		const float dz = float(p_position.z) - p_ubo.sphere[i][2];
		const float distance = Math::sqrt(dx * dx + dy * dy + dz * dz);
		const float reach = p_ubo.sphere[i][3];
		if (distance < p_ubo.params[0] * reach) {
			return true;
		}
		if (restir_event_solid_angle(reach, distance) > p_ubo.params[1]) {
			return true;
		}
	}
	return false;
}

bool restir_event_holds(const RestirEventsUBO &p_ubo, const Vector3 &p_sample) {
	const uint32_t count = MIN(p_ubo.counts[1], RESTIR_EVENT_MAX);
	for (uint32_t i = 0; i < count; i++) {
		const float dx = float(p_sample.x) - p_ubo.sphere[i][0];
		const float dy = float(p_sample.y) - p_ubo.sphere[i][1];
		const float dz = float(p_sample.z) - p_ubo.sphere[i][2];
		const float reach = p_ubo.sphere[i][3];
		if (dx * dx + dy * dy + dz * dz <= reach * reach) {
			return true;
		}
	}
	return false;
}

uint32_t restir_event_bin(float p_key) {
	if (!(p_key > 0.0f)) {
		return 0;
	}
	return MIN(uint32_t(p_key * float(RESTIR_EVENT_BINS)), RESTIR_EVENT_BINS - 1u);
}

uint32_t restir_event_cut_bin(const uint32_t *p_bins, uint32_t p_budget) {
	uint32_t total = 0;
	for (uint32_t t = 0; t < RESTIR_EVENT_BINS; t++) {
		if (total + p_bins[t] > p_budget) {
			return t;
		}
		total += p_bins[t];
	}
	return RESTIR_EVENT_BINS;
}

void restir_plan_events(const RestirEventInputs &p_inputs, RestirEventPlan &r_plan) {
	r_plan = RestirEventPlan();
	if (!p_inputs.settings.events_on) {
		return;
	}
	r_plan.active = true;
	r_plan.jump_frame = p_inputs.directional_jump || p_inputs.events_lost > 0u;
	const uint32_t count = p_inputs.events ? MIN(p_inputs.event_count, RESTIR_EVENT_MAX) : 0u;
	r_plan.event_count = count;
	RestirEventsUBO &u = r_plan.ubo;
	for (uint32_t i = 0; i < count; i++) {
		const RestirEventSphere &event = p_inputs.events[i];
		u.sphere[i][0] = float(event.center.x);
		u.sphere[i][1] = float(event.center.y);
		u.sphere[i][2] = float(event.center.z);
		u.sphere[i][3] = MAX(event.reach, 0.0f);
		u.since_last[i] = event.since_last;
		if (restir_event_first_fire(event.since_last, p_inputs.quiet_frames)) {
			r_plan.capping++;
		}
	}
	r_plan.budget = restir_event_budget(p_inputs.trace_width, p_inputs.trace_height, p_inputs.settings.reshade_budget);
	if (p_inputs.history) {
		if (r_plan.jump_frame) {
			r_plan.validate = p_inputs.settings.jump_m_cap > 0u ? RESTIR_EVENT_VALIDATE_ALL : RESTIR_EVENT_VALIDATE_NONE;
		} else if (count > 0u) {
			r_plan.validate = RESTIR_EVENT_VALIDATE_EVENT;
		}
	}
	u.counts[0] = count;
	u.counts[1] = count;
	u.counts[2] = p_inputs.settings.event_m_cap;
	u.counts[3] = p_inputs.settings.jump_m_cap;
	u.params[0] = MAX(p_inputs.settings.reach_scale, 0.0f);
	u.params[1] = MAX(p_inputs.settings.solid_angle, 0.0f);
	u.gates[0] = p_inputs.quiet_frames;
	u.gates[1] = r_plan.budget;
	u.gates[2] = r_plan.jump_frame ? 1u : 0u;
	u.gates[3] = uint32_t(r_plan.validate);
}
