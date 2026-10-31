/**************************************************************************/
/*  direct_lights.cpp                                                     */
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

#include "direct_lights.h"

#include "core/math/math_funcs.h"

using namespace RendererRD;

uint32_t DirectLightTable::pack_flags(uint32_t p_slot, float p_strength) {
	if (p_slot == NO_SLOT || p_slot > MAX_SLOT) {
		return 0;
	}
	const uint32_t strength = uint32_t(Math::round(CLAMP(p_strength, 0.0f, 1.0f) * 255.0f));
	return 1u | (p_slot << 1) | (strength << 17);
}

float DirectLightTable::reset_distance(float p_range, float p_movement) {
	return MAX(MIN_RESET_DISTANCE, RESET_RANGE_FRACTION * p_range) + 2.0f * Math::sqrt(3.0f) * p_movement;
}

bool DirectLightTable::owns(bool p_owned, bool p_area, uint32_t p_cull_mask, float p_energy, bool p_projector) {
	return p_owned && !p_area && p_cull_mask != 0 && p_energy >= 0.0f && !p_projector;
}

void DirectLightTable::begin_frame(uint64_t p_frame) {
	if (p_frame == frame) {
		return;
	}
	LocalVector<RID> gone;
	for (const KeyValue<RID, Record> &entry : records) {
		if (entry.value.last_seen != frame) {
			gone.push_back(entry.key);
		} else if (frame < entry.value.dynamic_until) {
			dynamic_lamp_frames++;
		}
	}
	for (const RID &id : gone) {
		RetiredSlot slot;
		slot.slot = records[id].slot;
		slot.free_at = p_frame + SLOT_REUSE_FRAMES;
		retired.push_back(slot);
		records.erase(id);
	}
	for (uint32_t i = 0; i < retired.size();) {
		if (retired[i].free_at <= p_frame) {
			free_slots.push_back(retired[i].slot);
			retired.remove_at_unordered(i);
		} else {
			i++;
		}
	}
	frame = p_frame;
}

uint32_t DirectLightTable::_take_slot() {
	if (!free_slots.is_empty()) {
		const uint32_t slot = free_slots[free_slots.size() - 1];
		free_slots.resize(free_slots.size() - 1);
		return slot;
	}
	return next_slot <= MAX_SLOT ? next_slot++ : NO_SLOT;
}

uint32_t DirectLightTable::observe(RID p_light, const Lamp &p_lamp) {
	Record *record = records.getptr(p_light);
	if (!record) {
		const uint32_t slot = _take_slot();
		if (slot == NO_SLOT) {
			return NO_SLOT;
		}
		Record created;
		created.lamp = p_lamp;
		created.reset_position = p_lamp.position;
		created.last_seen = frame;
		created.reset_frame = frame;
		created.slot = slot;
		records.insert(p_light, created);
		return slot;
	}
	if (record->last_seen == frame) {
		return record->slot;
	}
	record->last_seen = frame;
	if (record->lamp.position != p_lamp.position) {
		record->dynamic_until = frame + DYNAMIC_FRAMES;
	}
	if (record->lamp.caster_mask != p_lamp.caster_mask || record->lamp.size != p_lamp.size || record->lamp.type != p_lamp.type || record->reset_position.distance_to(p_lamp.position) > reset_distance(p_lamp.range, p_lamp.movement)) {
		record->reset_frame = frame;
		record->reset_position = p_lamp.position;
		hard_resets++;
	}
	record->lamp = p_lamp;
	return record->slot;
}

DirectLightTable::State DirectLightTable::get_state(RID p_light) const {
	State state;
	const Record *record = records.getptr(p_light);
	if (record) {
		state.slot = record->slot;
		state.hard_reset = record->reset_frame == frame;
		state.dynamic = frame < record->dynamic_until;
	}
	return state;
}

uint32_t DirectLightTable::slots_in_use() const {
	return records.size();
}

void DirectLightTable::mark_moved(const LocalVector<AABB> &p_moved) {
	if (p_moved.is_empty()) {
		return;
	}
	for (KeyValue<RID, Record> &entry : records) {
		Record &record = entry.value;
		const real_t range_squared = record.lamp.range * record.lamp.range;
		for (const AABB &bounds : p_moved) {
			const Vector3 nearest = record.lamp.position.clamp(bounds.position, bounds.get_end());
			if (nearest.distance_squared_to(record.lamp.position) <= range_squared) {
				record.dynamic_until = frame + DYNAMIC_FRAMES;
				break;
			}
		}
	}
}

void DirectLightTable::write_slots(LocalVector<GpuSlot> &r_slots) const {
	r_slots.resize(next_slot);
	for (uint32_t i = 0; i < next_slot; i++) {
		r_slots[i] = GpuSlot();
	}
	for (const KeyValue<RID, Record> &entry : records) {
		const Record &record = entry.value;
		GpuSlot &slot = r_slots[record.slot];
		slot.caster_mask = record.lamp.caster_mask;
		slot.size = record.lamp.size;
		slot.reset_frame = uint32_t(record.reset_frame);
		slot.dynamic = frame < record.dynamic_until ? 1 : 0;
	}
}

void DirectLightTable::write_lamps(LocalVector<float> &r_values) const {
	r_values.resize(next_slot * LAMP_VALUES);
	for (uint32_t i = 0; i < r_values.size(); i++) {
		r_values[i] = 0.0f;
	}
	for (const KeyValue<RID, Record> &entry : records) {
		const Record &record = entry.value;
		const uint32_t at = record.slot * LAMP_VALUES;
		r_values[at + 0] = float(record.lamp.position.x);
		r_values[at + 1] = float(record.lamp.position.y);
		r_values[at + 2] = float(record.lamp.position.z);
		r_values[at + 3] = record.lamp.size;
	}
}

void DirectLightTable::clear() {
	if (next_slot > 1 || frame != 0) {
		generation++;
	}
	records.clear();
	retired.clear();
	free_slots.clear();
	next_slot = 1;
	frame = 0;
}
