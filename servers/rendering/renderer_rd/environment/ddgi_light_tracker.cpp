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
	uint32_t seen = 0;
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
DDGILightTracker::Stats window;
bool enabled = false;
bool initialized = false;
uint64_t frame_now = 0;
uint32_t generation = 0;
float spacing = 1.0f;

float half_of(const RayScene::Light &p_light) {
	return 1.0f / MAX(p_light.inv_radius, 1e-4f);
}

bool differs(const Record &p_reference, const RayScene::Light &p_light, const Vector3 &p_world) {
	const RayScene::Light &ref = p_reference.light;
	if (ref.type != p_light.type) {
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

} // namespace

void DDGILightTracker::begin(bool p_enabled, uint64_t p_frame, float p_spacing) {
	if (!p_enabled) {
		if (enabled) {
			records.clear();
			pending.clear();
			window = Stats();
			initialized = false;
		}
		enabled = false;
		return;
	}
	enabled = true;
	frame_now = p_frame;
	spacing = p_spacing;
	generation++;
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
		created.seen = generation;
		records.insert(p_light, created);
		if (initialized) {
			post(world, half_of(p_data));
			window.fired++;
		}
		return;
	}
	record->seen = generation;
	if (!differs(*record, p_data, world) || frame_now - record->last_fire < RATE_FRAMES) {
		return;
	}
	post(record->world, half_of(record->light));
	post(world, half_of(p_data));
	window.fired++;
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
		if (entry.value.seen != generation) {
			gone.push_back(entry.key);
		}
	}
	for (const RID &id : gone) {
		const Record &record = records[id];
		if (initialized) {
			post(record.world, half_of(record.light));
			window.fired++;
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

DDGILightTracker::Stats &DDGILightTracker::stats() {
	return window;
}
