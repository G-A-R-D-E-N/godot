/**************************************************************************/
/*  rtgi_restarts.cpp                                                     */
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

#include "rtgi_restarts.h"

#include "core/math/math_funcs.h"

namespace RendererRD {

namespace {

constexpr float SAME_SPHERE = 1e-3f;

void note_jump(RtgiRestarts::DirectionalRecord &r_record, uint64_t p_frame, uint32_t p_quiet_frames, RtgiRestarts::DirectionalRead &r_read) {
	if (r_record.last_jump != UINT64_MAX && p_frame - r_record.last_jump < p_quiet_frames) {
		r_read.repeats++;
	} else {
		r_read.jumps++;
	}
	r_record.last_jump = p_frame;
}

} // namespace

void RtgiRestarts::select(const LocalVector<DDGILightTracker::Event> &p_events, const Transform3D &p_world_to_view, const Vector3 &p_camera_origin, const Vector3 &p_region_half, uint32_t p_quiet_frames, Selection &r_selection) {
	r_selection = Selection();
	const Vector3 inner = p_region_half * INNER_BOX_FRACTION;
	float nearness[MAX_SPHERES] = {};
	for (const DDGILightTracker::Event &event : p_events) {
		const bool new_or_gone = event.since_last == UINT32_MAX;
		if (!new_or_gone && event.since_last < p_quiet_frames) {
			r_selection.repeats++;
			continue;
		}
		const Vector3 offset = event.center - p_camera_origin;
		if (new_or_gone && (Math::abs(offset.x) > inner.x || Math::abs(offset.y) > inner.y || Math::abs(offset.z) > inner.z)) {
			r_selection.crossings++;
			continue;
		}
		const Vector3 view = p_world_to_view.xform(event.center);
		if (float(view.z) - event.half > 0.0f) {
			r_selection.behind++;
			continue;
		}
		bool same = false;
		for (uint32_t i = 0; i < r_selection.count && !same; i++) {
			const float *sphere = r_selection.spheres[i];
			same = Math::abs(sphere[0] - float(view.x)) < SAME_SPHERE && Math::abs(sphere[1] - float(view.y)) < SAME_SPHERE && Math::abs(sphere[2] - float(view.z)) < SAME_SPHERE && Math::abs(sphere[3] - event.half) < SAME_SPHERE;
		}
		if (same) {
			continue;
		}
		const float surface = float(view.length()) - event.half;
		uint32_t at = r_selection.count;
		while (at > 0 && nearness[at - 1] > surface) {
			at--;
		}
		if (at == MAX_SPHERES) {
			r_selection.dropped++;
			continue;
		}
		if (r_selection.count == MAX_SPHERES) {
			r_selection.dropped++;
		} else {
			r_selection.count++;
		}
		for (uint32_t i = r_selection.count - 1; i > at; i--) {
			nearness[i] = nearness[i - 1];
			for (int k = 0; k < 4; k++) {
				r_selection.spheres[i][k] = r_selection.spheres[i - 1][k];
			}
		}
		nearness[at] = surface;
		r_selection.spheres[at][0] = float(view.x);
		r_selection.spheres[at][1] = float(view.y);
		r_selection.spheres[at][2] = float(view.z);
		r_selection.spheres[at][3] = event.half;
	}
}

RtgiRestarts::DirectionalRead RtgiRestarts::read_directionals(const LocalVector<DirectionalSample> &p_samples, LocalVector<DirectionalRecord> &r_records, uint64_t p_frame, float p_limit_degrees, float p_floor, uint32_t p_quiet_frames) {
	DirectionalRead read;
	float strongest = 0.0f;
	for (const DirectionalSample &sample : p_samples) {
		strongest = MAX(strongest, Math::abs(sample.energy));
	}
	for (const DirectionalRecord &record : r_records) {
		strongest = MAX(strongest, Math::abs(record.energy));
	}
	const float energy_limit = MAX(p_floor, ENERGY_SHARE * strongest);
	const float cos_limit = Math::cos(Math::deg_to_rad(p_limit_degrees));

	for (uint32_t i = r_records.size(); i > 0; i--) {
		DirectionalRecord &record = r_records[i - 1];
		bool seen = false;
		for (const DirectionalSample &sample : p_samples) {
			seen = sample.light == record.light;
			if (!seen) {
				continue;
			}
			const bool energy_jump = Math::abs(sample.energy - record.energy) > energy_limit;
			const bool direction_jump = MAX(Math::abs(sample.energy), Math::abs(record.energy)) > energy_limit && sample.direction.dot(record.direction) < cos_limit;
			if (energy_jump || direction_jump) {
				note_jump(record, p_frame, p_quiet_frames, read);
			}
			record.energy = sample.energy;
			record.direction = sample.direction;
			break;
		}
		if (seen) {
			continue;
		}
		if (Math::abs(record.energy) > energy_limit) {
			note_jump(record, p_frame, p_quiet_frames, read);
		}
		record.energy = 0.0f;
		if (record.last_jump == UINT64_MAX || p_frame - record.last_jump >= p_quiet_frames) {
			r_records.remove_at_unordered(i - 1);
		}
	}

	for (const DirectionalSample &sample : p_samples) {
		bool known = false;
		for (const DirectionalRecord &record : r_records) {
			known = known || record.light == sample.light;
		}
		if (known) {
			continue;
		}
		DirectionalRecord record;
		record.light = sample.light;
		record.direction = sample.direction;
		record.energy = sample.energy;
		if (Math::abs(sample.energy) > energy_limit) {
			note_jump(record, p_frame, p_quiet_frames, read);
		}
		r_records.push_back(record);
	}
	return read;
}

} // namespace RendererRD
