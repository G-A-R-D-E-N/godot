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

#include "servers/rendering/rendering_server_enums.h"

#include <cstring>

using namespace RendererRD;

namespace {

struct Tracked {
	RayScene::Light light;
	Vector3 world;
};

LocalVector<Tracked> previous;
LocalVector<DDGILightTracker::Box> pending;
bool previous_valid = false;
bool pending_all = false;

constexpr double SAME_POSITION_SQUARED = 1e-6;

bool same(const Tracked &p_a, const Tracked &p_b) {
	return memcmp(&p_a.light, &p_b.light, sizeof(RayScene::Light)) == 0 && p_a.world.distance_squared_to(p_b.world) < SAME_POSITION_SQUARED;
}

void post(const Tracked &p_tracked) {
	if (pending.size() >= DDGILightTracker::MAX_BOXES) {
		pending_all = true;
		return;
	}
	DDGILightTracker::Box box;
	box.center = p_tracked.world;
	box.half = 1.0f / MAX(p_tracked.light.inv_radius, 1e-4f);
	pending.push_back(box);
}

} // namespace

void DDGILightTracker::update(const LocalVector<RayScene::Light> &p_lights, const Vector3 &p_anchor, bool p_enabled) {
	if (!p_enabled) {
		previous.clear();
		pending.clear();
		previous_valid = false;
		pending_all = false;
		return;
	}
	LocalVector<Tracked> current;
	for (const RayScene::Light &light : p_lights) {
		if (light.type == uint32_t(RSE::LIGHT_DIRECTIONAL)) {
			continue;
		}
		Tracked tracked;
		tracked.light = light;
		tracked.world = Vector3(light.position[0], light.position[1], light.position[2]) + p_anchor;
		for (int k = 0; k < 3; k++) {
			tracked.light.position[k] = 0.0f;
		}
		current.push_back(tracked);
	}
	if (previous_valid) {
		LocalVector<uint8_t> matched;
		matched.resize(previous.size());
		for (uint32_t i = 0; i < matched.size(); i++) {
			matched[i] = 0;
		}
		LocalVector<uint8_t> current_matched;
		current_matched.resize(current.size());
		for (uint32_t i = 0; i < current.size(); i++) {
			current_matched[i] = 0;
			if (i < previous.size() && same(current[i], previous[i])) {
				current_matched[i] = 1;
				matched[i] = 1;
			}
		}
		for (uint32_t i = 0; i < current.size(); i++) {
			if (current_matched[i]) {
				continue;
			}
			for (uint32_t j = 0; j < previous.size(); j++) {
				if (!matched[j] && same(current[i], previous[j])) {
					matched[j] = 1;
					current_matched[i] = 1;
					break;
				}
			}
			if (!current_matched[i]) {
				post(current[i]);
			}
		}
		for (uint32_t j = 0; j < previous.size(); j++) {
			if (!matched[j]) {
				post(previous[j]);
			}
		}
	}
	previous = current;
	previous_valid = true;
}

bool DDGILightTracker::take(LocalVector<Box> &r_boxes) {
	r_boxes = pending;
	const bool all = pending_all;
	pending.clear();
	pending_all = false;
	return all;
}
