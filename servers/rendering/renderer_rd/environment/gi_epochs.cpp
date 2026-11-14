/**************************************************************************/
/*  gi_epochs.cpp                                                         */
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

#include "gi_epochs.h"

#include "core/templates/hashfuncs.h"

namespace RendererRD {

uint32_t GIEpochs::_wrap(uint64_t p_value) {
	const uint32_t value = uint32_t(p_value);
	return value == 0 ? 1 : value;
}

GIEpochs *GIEpochs::get_singleton() {
	static GIEpochs instance;
	return &instance;
}

uint32_t GIEpochs::next(uint32_t p_value) {
	const uint32_t value = p_value + 1;
	return value == 0 ? 1 : value;
}

uint64_t GISceneTracker::add_instance(uint64_t p_signature, uint32_t p_instance_hash) {
	return p_signature + (uint64_t(p_instance_hash) | (uint64_t(1) << 32));
}

uint64_t GISceneTracker::member_key(const void *p_instance, uint64_t p_mesh_id) {
	return (uint64_t(uintptr_t(p_instance)) * 0x9E3779B97F4A7C15ull) ^ p_mesh_id;
}

uint64_t GISceneTracker::member_signature_of(const LocalVector<uint64_t> &p_members) {
	uint64_t result = 0;
	for (uint64_t member : p_members) {
		result = add_instance(result, hash_murmur3_one_64(member));
	}
	return result;
}

GISceneTracker::Change GISceneTracker::update(uint64_t p_signature, const LocalVector<uint64_t> &p_members) {
	Change change;
	const uint64_t new_member_signature = member_signature_of(p_members);
	const bool same_members = have && new_member_signature == member_signature;
	if (have && p_signature != signature) {
		change.scene = true;
		if (!members.is_empty() && !same_members) {
			bool kept = false;
			for (uint64_t member : p_members) {
				if (members.has(member)) {
					kept = true;
					break;
				}
			}
			change.world = !kept;
		}
	} else if (!have) {
		change.scene = true;
	}
	if (!same_members) {
		members.clear();
		for (uint64_t member : p_members) {
			members.insert(member);
		}
		member_signature = new_member_signature;
	}
	signature = p_signature;
	have = true;
	return change;
}

void GIEpochs::sync_ray_scene(uint64_t p_feed_serial, uint32_t p_structure_generation) {
	feed_live = p_feed_serial != 0;
	feed_serial = feed_live ? _wrap(p_feed_serial) : 0;
	structure_generation = p_structure_generation;
}

void GIEpochs::bump(Source p_source) {
	if (p_source < SOURCE_COUNT) {
		epochs[p_source] = next(epochs[p_source]);
	}
}

void GIEpochs::apply(const GISceneTracker::Change &p_change) {
	if (p_change.scene) {
		bump(SOURCE_SCENE);
	}
	if (p_change.world) {
		bump(SOURCE_WORLD);
	}
}

uint32_t GIEpochs::get(Source p_source) const {
	return p_source < SOURCE_COUNT ? epochs[p_source] : 0;
}

GIEpochs::Snapshot GIEpochs::snapshot() const {
	Snapshot result;
	result.scene = epochs[SOURCE_SCENE];
	result.light = epochs[SOURCE_LIGHT];
	result.sky = epochs[SOURCE_SKY];
	result.origin = epochs[SOURCE_ORIGIN];
	result.world = epochs[SOURCE_WORLD];
	return result;
}

void GIEpochs::fill_record(GIProviderRecord &r_record) const {
	r_record.scene_epoch = epochs[SOURCE_SCENE];
	r_record.light_epoch = epochs[SOURCE_LIGHT];
	r_record.sky_epoch = epochs[SOURCE_SKY];
	r_record.origin_epoch = epochs[SOURCE_ORIGIN];
	if (r_record.scene_epoch != 0) {
		r_record.flags |= GI_PROVIDER_FLAG_SCENE_EPOCH;
	}
	if (r_record.light_epoch != 0) {
		r_record.flags |= GI_PROVIDER_FLAG_LIGHT_EPOCH;
	}
	if (r_record.sky_epoch != 0) {
		r_record.flags |= GI_PROVIDER_FLAG_SKY_EPOCH;
	}
	if (r_record.origin_epoch != 0) {
		r_record.flags |= GI_PROVIDER_FLAG_ORIGIN_EPOCH;
	}
}

void GIEpochs::merge_boxes(LocalVector<AABB> &r_boxes, real_t p_distance) {
	uint32_t count = r_boxes.size();
	if (count < 2) {
		return;
	}
	struct MinX {
		bool operator()(const AABB &p_a, const AABB &p_b) const { return p_a.position.x < p_b.position.x; }
	};
	r_boxes.sort_custom<MinX>();
	LocalVector<uint8_t> alive;
	alive.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		alive[i] = 1;
	}
	bool merged = true;
	while (merged) {
		merged = false;
		for (uint32_t i = 0; i < count; i++) {
			if (!alive[i]) {
				continue;
			}
			for (uint32_t j = i + 1; j < count; j++) {
				if (r_boxes[j].position.x > r_boxes[i].position.x + r_boxes[i].size.x + p_distance) {
					break;
				}
				if (alive[j] && r_boxes[i].grow(p_distance).intersects(r_boxes[j])) {
					r_boxes[i] = r_boxes[i].merge(r_boxes[j]);
					alive[j] = 0;
					merged = true;
				}
			}
		}
	}
	uint32_t out = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (alive[i]) {
			r_boxes[out++] = r_boxes[i];
		}
	}
	r_boxes.resize(out);
}

void GIEpochs::publish_dirty_frame(const LocalVector<AABB> &p_boxes, bool p_overflow, real_t p_merge_distance) {
	if (!feed_live) {
		return;
	}
	merge_distance = p_merge_distance;
	DirtyFrame &frame = ring[feed_serial % DIRTY_RING_FRAMES];
	frame.serial = feed_serial;
	frame.scene_epoch = epochs[SOURCE_SCENE];
	frame.structure_generation = structure_generation;
	frame.overflow = p_overflow || p_boxes.size() > DIRTY_RING_BOXES;
	frame.boxes.clear();
	for (uint32_t i = 0; i < p_boxes.size() && i < DIRTY_RING_BOXES; i++) {
		frame.boxes.push_back(p_boxes[i]);
	}
}

void GIEpochs::clear_dirty_feed() {
	for (uint32_t i = 0; i < DIRTY_RING_FRAMES; i++) {
		ring[i].serial = 0;
		ring[i].overflow = false;
		ring[i].boxes.clear();
	}
}

GIEpochs::DirtyResult GIEpochs::_finish(LocalVector<AABB> &r_boxes, bool p_overflow) const {
	merge_boxes(r_boxes, merge_distance);
	if (r_boxes.size() > DIRTY_QUERY_BOXES) {
		r_boxes.resize(DIRTY_QUERY_BOXES);
		p_overflow = true;
	}
	if (p_overflow) {
		return DIRTY_OVERFLOW;
	}
	return r_boxes.is_empty() ? DIRTY_NONE : DIRTY_BOXES;
}

GIEpochs::DirtyResult GIEpochs::get_dirty_boxes_since_serial(uint32_t p_since_serial, LocalVector<AABB> &r_boxes) const {
	r_boxes.clear();
	if (!feed_live) {
		return DIRTY_OVERFLOW;
	}
	const uint32_t behind = feed_serial - p_since_serial;
	if (behind == 0) {
		return DIRTY_NONE;
	}
	if (behind > DIRTY_RING_FRAMES) {
		return DIRTY_OVERFLOW;
	}
	bool overflow = false;
	for (uint32_t k = 1; k <= behind; k++) {
		const uint32_t serial = p_since_serial + k;
		const DirtyFrame &frame = ring[serial % DIRTY_RING_FRAMES];
		if (frame.serial != serial) {
			r_boxes.clear();
			return DIRTY_OVERFLOW;
		}
		overflow = overflow || frame.overflow;
		for (const AABB &box : frame.boxes) {
			r_boxes.push_back(box);
		}
	}
	return _finish(r_boxes, overflow);
}

GIEpochs::DirtyResult GIEpochs::get_dirty_boxes(uint32_t p_since_scene_epoch, LocalVector<AABB> &r_boxes) const {
	r_boxes.clear();
	if (p_since_scene_epoch == epochs[SOURCE_SCENE]) {
		return DIRTY_NONE;
	}
	if (!feed_live) {
		return DIRTY_OVERFLOW;
	}
	bool overflow = false;
	bool covered = false;
	for (uint32_t k = 0; k < DIRTY_RING_FRAMES; k++) {
		const uint32_t serial = feed_serial - k;
		const DirtyFrame &frame = ring[serial % DIRTY_RING_FRAMES];
		if (frame.serial != serial) {
			break;
		}
		if (int32_t(frame.scene_epoch - p_since_scene_epoch) <= 0) {
			covered = true;
			break;
		}
		overflow = overflow || frame.overflow;
		for (const AABB &box : frame.boxes) {
			r_boxes.push_back(box);
		}
	}
	if (!covered) {
		r_boxes.clear();
		return DIRTY_OVERFLOW;
	}
	return _finish(r_boxes, overflow);
}

} // namespace RendererRD
