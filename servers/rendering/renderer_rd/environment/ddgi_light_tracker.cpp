/**************************************************************************/
/*  ddgi_light_tracker.cpp                                                */
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

#include "ddgi_light_tracker.h"

#include "core/templates/hash_map.h"
#include "servers/rendering/rendering_server_enums.h"

using namespace RendererRD;

namespace {

struct Record {
	RayScene::Light light;
	Vector3 world;
	uint64_t last_fire = 0;
	uint64_t seen_frame = 0;
};

constexpr float ENERGY_BAND_LOW = 0.8f;
constexpr float ENERGY_BAND_HIGH = 1.25f;
constexpr float COLOR_TOLERANCE = 0.1f;
constexpr float RANGE_TOLERANCE = 0.1f;
constexpr float MOVE_SPACING_FRACTION = 0.25f;
constexpr float SPOT_COS_TURN = 0.98480775f;
constexpr float SPOT_TOLERANCE = 0.1f;
constexpr float SAME_CENTER_SQUARED = 1e-6f;
constexpr float SAME_HALF = 1e-3f;

HashMap<RID, Record> records;
LocalVector<DDGILightTracker::Box> pending;
DDGILightTracker::Event events[DDGILightTracker::MAX_EVENTS];
uint64_t event_serial = 0;
DDGILightTracker::Stats window;
bool enabled = false;
bool reset_on = false;
bool list_on = false;
bool initialized = false;
uint64_t frame_now = 0;
float spacing = 1.0f;

float half_of(const RayScene::Light &p_light) {
	const float range = 1.0f / MAX(p_light.inv_radius, 1e-4f);
	if (p_light.type != uint32_t(RSE::LIGHT_AREA)) {
		return range;
	}
	const Vector3 a(p_light.half_a[0], p_light.half_a[1], p_light.half_a[2]);
	const Vector3 b(p_light.half_b[0], p_light.half_b[1], p_light.half_b[2]);
	return range + Math::sqrt(a.length_squared() + b.length_squared());
}

bool axis_changed(const float *p_reference, const float *p_axis) {
	const Vector3 reference(p_reference[0], p_reference[1], p_reference[2]);
	const Vector3 axis(p_axis[0], p_axis[1], p_axis[2]);
	return (axis - reference).length() > RANGE_TOLERANCE * MAX(reference.length(), 1e-4f);
}

bool differs(const Record &p_reference, const RayScene::Light &p_light, const Vector3 &p_world) {
	const RayScene::Light &ref = p_reference.light;
	if (ref.type != p_light.type || ref.caster_mask != p_light.caster_mask) {
		return true;
	}
	if (ref.energy == 0.0f || p_light.energy == 0.0f || (ref.energy < 0.0f) != (p_light.energy < 0.0f)) {
		if (ref.energy != p_light.energy) {
			return true;
		}
	} else {
		const float ratio = p_light.energy / ref.energy;
		if (ratio < ENERGY_BAND_LOW || ratio > ENERGY_BAND_HIGH) {
			return true;
		}
	}
	float reference_peak = 1e-4f;
	float color_delta = 0.0f;
	for (int k = 0; k < 3; k++) {
		reference_peak = MAX(reference_peak, ref.color[k]);
		color_delta = MAX(color_delta, Math::abs(p_light.color[k] - ref.color[k]));
	}
	if (color_delta > COLOR_TOLERANCE * reference_peak) {
		return true;
	}
	if (ref.inv_radius > 0.0f && Math::abs(p_light.inv_radius / ref.inv_radius - 1.0f) > RANGE_TOLERANCE) {
		return true;
	}
	if (p_reference.world.distance_to(p_world) > MOVE_SPACING_FRACTION * spacing) {
		return true;
	}
	if (p_light.type == uint32_t(RSE::LIGHT_SPOT)) {
		const Vector3 a(ref.direction[0], ref.direction[1], ref.direction[2]);
		const Vector3 b(p_light.direction[0], p_light.direction[1], p_light.direction[2]);
		if (a.dot(b) < SPOT_COS_TURN) {
			return true;
		}
		if (Math::abs(p_light.cos_spot_angle - ref.cos_spot_angle) > SPOT_TOLERANCE * MAX(Math::abs(ref.cos_spot_angle), 1e-3f)) {
			return true;
		}
		if (Math::abs(p_light.inv_spot_attenuation - ref.inv_spot_attenuation) > SPOT_TOLERANCE * MAX(Math::abs(ref.inv_spot_attenuation), 1e-3f)) {
			return true;
		}
	} else if (p_light.type == uint32_t(RSE::LIGHT_AREA)) {
		const Vector3 a(ref.direction[0], ref.direction[1], ref.direction[2]);
		const Vector3 b(p_light.direction[0], p_light.direction[1], p_light.direction[2]);
		if (a.dot(b) < SPOT_COS_TURN || axis_changed(ref.half_a, p_light.half_a) || axis_changed(ref.half_b, p_light.half_b)) {
			return true;
		}
	}
	return false;
}

void post(const Vector3 &p_center, float p_half) {
	for (const DDGILightTracker::Box &box : pending) {
		if (box.center.distance_squared_to(p_center) < SAME_CENTER_SQUARED && Math::abs(box.half - p_half) < SAME_HALF) {
			return;
		}
	}
	if (pending.size() >= DDGILightTracker::MAX_PENDING) {
		uint32_t nearest = 0;
		real_t best = Math::INF;
		for (uint32_t i = 0; i < pending.size(); i++) {
			const real_t d = pending[i].center.distance_squared_to(p_center);
			if (d < best) {
				best = d;
				nearest = i;
			}
		}
		DDGILightTracker::Box &box = pending[nearest];
		const Vector3 low = (box.center - Vector3(box.half, box.half, box.half)).min(p_center - Vector3(p_half, p_half, p_half));
		const Vector3 high = (box.center + Vector3(box.half, box.half, box.half)).max(p_center + Vector3(p_half, p_half, p_half));
		const Vector3 extent = high - low;
		box.center = (low + high) * 0.5;
		box.half = float(MAX(extent.x, MAX(extent.y, extent.z)) * 0.5);
		return;
	}
	DDGILightTracker::Box box;
	box.center = p_center;
	box.half = p_half;
	pending.push_back(box);
}

void fire(const Vector3 &p_center, float p_half, uint64_t p_since_last) {
	if (reset_on) {
		post(p_center, p_half);
	}
	if (list_on) {
		DDGILightTracker::Event &event = events[event_serial % DDGILightTracker::MAX_EVENTS];
		event.center = p_center;
		event.half = p_half;
		event.since_last = uint32_t(MIN(p_since_last, uint64_t(UINT32_MAX)));
		event_serial++;
	}
}

} // namespace

void DDGILightTracker::begin(bool p_enabled, uint64_t p_frame, float p_spacing, bool p_list) {
	reset_on = p_enabled;
	list_on = p_list;
	if (!p_enabled) {
		pending.clear();
		window = Stats();
	}
	if (!p_enabled && !p_list) {
		if (enabled) {
			records.clear();
			initialized = false;
		}
		enabled = false;
		return;
	}
	enabled = true;
	frame_now = p_frame;
	spacing = p_spacing;
}

void DDGILightTracker::observe(RID p_light, const RayScene::Light &p_data, const Vector3 &p_anchor) {
	if (!enabled || p_data.type == uint32_t(RSE::LIGHT_DIRECTIONAL)) {
		return;
	}
	const Vector3 world = Vector3(p_data.position[0], p_data.position[1], p_data.position[2]) + p_anchor;
	Record *record = records.getptr(p_light);
	if (!record) {
		Record created;
		created.light = p_data;
		created.world = world;
		created.last_fire = frame_now;
		created.seen_frame = frame_now;
		records.insert(p_light, created);
		if (initialized) {
			fire(world, half_of(p_data), UINT64_MAX);
			window.fired += reset_on;
		}
		return;
	}
	record->seen_frame = frame_now;
	if (!differs(*record, p_data, world) || frame_now - record->last_fire < RATE_FRAMES) {
		return;
	}
	const uint64_t since_last = frame_now - record->last_fire;
	fire(record->world, half_of(record->light), since_last);
	fire(world, half_of(p_data), since_last);
	window.fired += reset_on;
	record->light = p_data;
	record->world = world;
	record->last_fire = frame_now;
}

void DDGILightTracker::end() {
	if (!enabled) {
		return;
	}
	LocalVector<RID> gone;
	for (const KeyValue<RID, Record> &entry : records) {
		if (frame_now > entry.value.seen_frame + 1) {
			gone.push_back(entry.key);
		}
	}
	for (const RID &id : gone) {
		const Record &record = records[id];
		if (initialized) {
			fire(record.world, half_of(record.light), UINT64_MAX);
			window.fired += reset_on;
		}
		records.erase(id);
	}
	initialized = true;
}

void DDGILightTracker::take(LocalVector<Box> &r_boxes) {
	r_boxes.clear();
	const uint32_t count = MIN(uint32_t(pending.size()), MAX_BOXES);
	for (uint32_t i = 0; i < count; i++) {
		r_boxes.push_back(pending[i]);
	}
	LocalVector<Box> rest;
	for (uint32_t i = count; i < pending.size(); i++) {
		rest.push_back(pending[i]);
	}
	pending = rest;
}

uint32_t DDGILightTracker::pending_count() {
	return pending.size();
}

uint64_t DDGILightTracker::events_since(uint64_t p_serial, LocalVector<Event> &r_events) {
	r_events.clear();
	const uint64_t oldest = event_serial > MAX_EVENTS ? event_serial - MAX_EVENTS : 0;
	for (uint64_t serial = MAX(p_serial, oldest); serial < event_serial; serial++) {
		r_events.push_back(events[serial % MAX_EVENTS]);
	}
	return event_serial;
}

DDGILightTracker::Stats &DDGILightTracker::stats() {
	return window;
}
