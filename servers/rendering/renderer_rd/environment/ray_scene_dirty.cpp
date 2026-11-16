/**************************************************************************/
/*  ray_scene_dirty.cpp                                                   */
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

#include "ray_scene.h"

#include "core/config/project_settings.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "servers/rendering/renderer_rd/environment/gi_epochs.h"

#include <string.h>

using namespace RendererRD;

static uint32_t _history_bits(uint8_t p_history) {
	uint32_t count = 0;
	for (uint32_t i = 0; i < RayScene::FEED_HISTORY_FRAMES; i++) {
		count += (p_history >> i) & 1u;
	}
	return count;
}

static Array _box_array(const AABB &p_box) {
	Array a;
	a.push_back(double(p_box.position.x));
	a.push_back(double(p_box.position.y));
	a.push_back(double(p_box.position.z));
	a.push_back(double(p_box.size.x));
	a.push_back(double(p_box.size.y));
	a.push_back(double(p_box.size.z));
	return a;
}

static Array _boxes_array(const LocalVector<AABB> &p_boxes) {
	Array a;
	for (const AABB &box : p_boxes) {
		a.push_back(_box_array(box));
	}
	return a;
}

void RayScene::_feed_stop() {
	if (!feed_was_on) {
		return;
	}
	feed_was_on = false;
	feed_baseline = false;
	feed_records.clear();
	feed_active.clear();
	feed_raw.clear();
	feed = DirtyFeed();
	feed_log.unref();
	GIEpochs::get_singleton()->clear_dirty_feed();
}

void RayScene::_feed_begin() {
	if (feed_consumers.get() == 0) {
		_feed_stop();
		return;
	}
	feed_baseline = !feed_was_on;
	if (feed_baseline) {
		feed_was_on = true;
		feed_records.clear();
		feed_active.clear();
		feed_generation_before = structure_generation;
	}
	feed_epoch_before = GIEpochs::get_singleton()->get(GIEpochs::SOURCE_SCENE);
	feed_stamp++;
	feed_raw.clear();
	feed_raw_overflow = false;
	feed_seen = 0;
	feed_survivors = 0;
	feed_tracked_before = feed_records.size();
	feed.changes = 0;
	for (const void *key : feed_active) {
		FeedRecord *record = feed_records.getptr(key);
		if (record) {
			record->history = uint8_t(record->history << 1);
		}
	}
}

void RayScene::_feed_observe(const void *p_key, const Transform3D &p_world, RID p_mesh_rid, const MeshEntry &p_mesh, bool p_usable, uint32_t p_state) {
	feed_seen++;
	FeedRecord *record = feed_records.getptr(p_key);
	if (record) {
		feed_survivors++;
		record->stamp = feed_stamp;
		if (record->mesh == p_mesh_rid && record->generation == p_mesh.generation && record->usable == uint8_t(p_usable) && record->state == p_state && memcmp(&record->world, &p_world, sizeof(Transform3D)) == 0) {
			return;
		}
		feed.changes++;
		if (feed_raw.size() < FEED_RAW_LIMIT) {
			feed_raw.push_back(record->bounds);
		} else {
			feed_raw_overflow = true;
		}
		record->world = p_world;
		record->mesh = p_mesh_rid;
		record->generation = p_mesh.generation;
		record->usable = uint8_t(p_usable);
		record->state = p_state;
		record->bounds = p_world.xform(p_mesh.aabb);
		if (feed_raw.size() < FEED_RAW_LIMIT) {
			feed_raw.push_back(record->bounds);
		} else {
			feed_raw_overflow = true;
		}
		record->history |= 1;
		if (!record->listed) {
			record->listed = true;
			feed_active.push_back(p_key);
		}
		return;
	}
	FeedRecord entry;
	entry.world = p_world;
	entry.mesh = p_mesh_rid;
	entry.generation = p_mesh.generation;
	entry.usable = uint8_t(p_usable);
	entry.state = p_state;
	entry.stamp = feed_stamp;
	entry.bounds = p_world.xform(p_mesh.aabb);
	if (!feed_baseline) {
		feed.changes++;
		if (feed_raw.size() < FEED_RAW_LIMIT) {
			feed_raw.push_back(entry.bounds);
		} else {
			feed_raw_overflow = true;
		}
		entry.history = 1;
		entry.listed = true;
		feed_active.push_back(p_key);
	}
	feed_records.insert(p_key, entry);
}

void RayScene::_feed_end() {
	if (feed_records.size() > feed_seen) {
		LocalVector<const void *> stale;
		for (const KeyValue<const void *, FeedRecord> &entry : feed_records) {
			if (entry.value.stamp != feed_stamp) {
				stale.push_back(entry.key);
			}
		}
		for (const void *key : stale) {
			const FeedRecord *record = feed_records.getptr(key);
			feed.changes++;
			if (feed_raw.size() < FEED_RAW_LIMIT) {
				feed_raw.push_back(record->bounds);
			} else {
				feed_raw_overflow = true;
			}
			feed_records.erase(key);
		}
	}
	if (!feed_baseline && feed_tracked_before > 0 && feed_survivors == 0 && structure_generation == feed_generation_before) {
		structure_generation++;
	}

	const uint32_t mover_frames = CLAMP(uint32_t(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/mover_frames")), 1u, FEED_HISTORY_FRAMES);
	const uint32_t cap = MAX(1u, uint32_t(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/dirty_box_cap")));
	const real_t distance = MAX(real_t(0), real_t(GLOBAL_GET_CACHED(float, "rendering/global_illumination/ray_scene/dirty_merge_distance")));

	LocalVector<AABB> movers;
	uint32_t kept = 0;
	uint32_t mover_count = 0;
	for (uint32_t i = 0; i < feed_active.size(); i++) {
		const void *key = feed_active[i];
		FeedRecord *record = feed_records.getptr(key);
		if (!record) {
			continue;
		}
		if (record->history == 0) {
			record->listed = false;
			continue;
		}
		feed_active[kept++] = key;
		if (!feed_baseline && _history_bits(record->history) >= mover_frames) {
			mover_count++;
			if (movers.size() < FEED_RAW_LIMIT) {
				movers.push_back(record->bounds);
			}
		}
	}
	feed_active.resize(kept);

	bool overflow = feed_frame_overflow(feed_raw_overflow, feed_baseline);
	if (!overflow) {
		GIEpochs::merge_boxes(feed_raw, distance);
	}
	if (feed_raw.size() > cap) {
		overflow = true;
		feed_raw.resize(cap);
	}
	GIEpochs::merge_boxes(movers, distance);
	if (movers.size() > cap) {
		movers.resize(cap);
	}

	feed_serial++;
	if (feed_serial == 0) {
		feed_serial = 1;
	}
	feed.serial = feed_serial;
	feed.baseline = feed_baseline;
	feed.overflow = overflow;
	feed.structure_generation = structure_generation;
	feed.tracked = feed_records.size();
	feed.mover_count = mover_count;
	feed.boxes.clear();
	for (const AABB &box : feed_raw) {
		feed.boxes.push_back(box);
	}
	feed.mover_boxes.clear();
	for (const AABB &box : movers) {
		feed.mover_boxes.push_back(box);
	}
	feed_generation_before = structure_generation;
	if (feed_frame_needs_epoch(feed.changes, feed_raw_overflow, GIEpochs::get_singleton()->get(GIEpochs::SOURCE_SCENE) != feed_epoch_before)) {
		GIEpochs::get_singleton()->bump(GIEpochs::SOURCE_SCENE);
	}
	feed_epoch_history[feed_serial % FEED_EPOCH_HISTORY] = GIEpochs::get_singleton()->get(GIEpochs::SOURCE_SCENE);
}

void RayScene::_feed_log_line() {
	if (!feed_was_on || feed_log_mode < 2) {
		feed_log.unref();
		return;
	}
	if (feed_log.is_null()) {
		const String base = GLOBAL_GET("rendering/global_illumination/ray_scene/debug_dirty_feed_path");
		if (base.is_empty()) {
			return;
		}
		feed_log = FileAccess::open(base, FileAccess::WRITE);
		if (feed_log.is_null()) {
			return;
		}
	}
	GIEpochs *epochs = GIEpochs::get_singleton();
	Dictionary d;
	d["serial"] = int64_t(feed.serial);
	d["baseline"] = feed.baseline;
	d["overflow"] = feed.overflow;
	d["structure_generation"] = int64_t(feed.structure_generation);
	d["changes"] = int64_t(feed.changes);
	d["tracked"] = int64_t(feed.tracked);
	d["instances"] = int64_t(instances.size());
	d["scene_epoch"] = int64_t(epochs->get(GIEpochs::SOURCE_SCENE));
	d["geometry_version"] = int64_t(uint32_t(instance_signature) ^ uint32_t(instance_signature >> 32));
	d["anchor"] = _box_array(AABB(anchor, Vector3()));
	d["boxes"] = _boxes_array(feed.boxes);
	d["mover_count"] = int64_t(feed.mover_count);
	d["mover_boxes"] = _boxes_array(feed.mover_boxes);
	d["update_us"] = int64_t(OS::get_singleton()->get_ticks_usec() - feed_update_start);
	const uint32_t behind[] = { 1, 4, 16, 17 };
	for (const uint32_t k : behind) {
		LocalVector<AABB> boxes;
		const GIEpochs::DirtyResult result = epochs->get_dirty_boxes_since_serial(feed.serial - k, boxes);
		Dictionary q;
		q["result"] = int64_t(result);
		q["boxes"] = _boxes_array(boxes);
		d[vformat("q%d", int(k))] = q;
	}
	LocalVector<AABB> epoch_boxes;
	const GIEpochs::DirtyResult epoch_result = epochs->get_dirty_boxes(feed_epoch_history[(feed.serial - 4) % FEED_EPOCH_HISTORY], epoch_boxes);
	Dictionary e;
	e["result"] = int64_t(epoch_result);
	e["boxes"] = _boxes_array(epoch_boxes);
	d["e4"] = e;
	feed_log->store_line(JSON::stringify(d, "", false, true));
	feed_log->flush();
}
