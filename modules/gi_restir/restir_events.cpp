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

static bool _sphere_caps(const RestirEventsUBO &p_ubo, const float *p_sphere, const Vector3 &p_position) {
	const float dx = float(p_position.x) - p_sphere[0];
	const float dy = float(p_position.y) - p_sphere[1];
	const float dz = float(p_position.z) - p_sphere[2];
	const float distance = Math::sqrt(dx * dx + dy * dy + dz * dz);
	const float reach = p_sphere[3];
	if (distance < p_ubo.params[0] * reach) {
		return true;
	}
	return restir_event_solid_angle(reach, distance) > p_ubo.params[1];
}

bool restir_event_caps(const RestirEventsUBO &p_ubo, const Vector3 &p_position) {
	const uint32_t count = MIN(p_ubo.counts[0], RESTIR_EVENT_MAX);
	for (uint32_t i = 0; i < count; i++) {
		if (i < p_ubo.counts[1] && !restir_event_first_fire(p_ubo.since_last[i], p_ubo.gates[0])) {
			continue;
		}
		if (_sphere_caps(p_ubo, p_ubo.sphere[i], p_position)) {
			return true;
		}
	}
	return false;
}

bool restir_hold_caps(const RestirEventsUBO &p_ubo, const Vector3 &p_position) {
	const uint32_t count = MIN(p_ubo.hold[0], RESTIR_HOLD_MAX);
	for (uint32_t i = 0; i < count; i++) {
		if (_sphere_caps(p_ubo, p_ubo.held[i], p_position)) {
			return true;
		}
	}
	return false;
}

void restir_hold_clear(RestirHoldRing &r_ring) {
	r_ring.count = 0;
}

RestirHoldUpdate restir_hold_update(RestirHoldRing &r_ring, const RestirEventSphere *p_spheres, uint32_t p_count, const Vector3 &p_anchor, uint32_t p_quiet_frames, uint32_t p_hold_frames) {
	RestirHoldUpdate update;
	if (p_hold_frames == 0u) {
		update.expired = r_ring.count;
		r_ring.count = 0;
		return update;
	}
	uint32_t kept = 0;
	for (uint32_t i = 0; i < r_ring.count; i++) {
		RestirHoldEntry entry = r_ring.entries[i];
		entry.age++;
		if (entry.age >= p_hold_frames) {
			update.expired++;
			continue;
		}
		r_ring.entries[kept++] = entry;
	}
	r_ring.count = kept;
	const uint32_t count = p_spheres ? MIN(p_count, RESTIR_EVENT_MAX) : 0u;
	for (uint32_t i = 0; i < count; i++) {
		const RestirEventSphere &sphere = p_spheres[i];
		if (!restir_event_first_fire(sphere.since_last, p_quiet_frames)) {
			continue;
		}
		const Vector3 center = sphere.center + p_anchor;
		const float reach = MAX(sphere.reach, 0.0f);
		bool refreshed = false;
		for (uint32_t j = 0; j < r_ring.count; j++) {
			RestirHoldEntry &entry = r_ring.entries[j];
			if (entry.reach >= reach && (center - entry.center).length_squared() <= real_t(entry.reach) * real_t(entry.reach)) {
				entry.age = 0;
				refreshed = true;
				break;
			}
		}
		if (refreshed) {
			update.refreshed++;
			continue;
		}
		uint32_t slot = r_ring.count;
		if (slot == RESTIR_HOLD_MAX) {
			slot = 0;
			for (uint32_t j = 1; j < RESTIR_HOLD_MAX; j++) {
				if (r_ring.entries[j].age > r_ring.entries[slot].age) {
					slot = j;
				}
			}
			update.dropped++;
		} else {
			r_ring.count++;
		}
		r_ring.entries[slot].center = center;
		r_ring.entries[slot].reach = reach;
		r_ring.entries[slot].age = 0;
		update.added++;
	}
	return update;
}

void restir_hold_pack(const RestirHoldRing &p_ring, const Vector3 &p_anchor, RestirEventsUBO &r_ubo) {
	const uint32_t count = MIN(p_ring.count, RESTIR_HOLD_MAX);
	for (uint32_t i = 0; i < count; i++) {
		const Vector3 local = p_ring.entries[i].center - p_anchor;
		r_ubo.held[i][0] = float(local.x);
		r_ubo.held[i][1] = float(local.y);
		r_ubo.held[i][2] = float(local.z);
		r_ubo.held[i][3] = p_ring.entries[i].reach;
	}
	r_ubo.hold[0] = count;
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
