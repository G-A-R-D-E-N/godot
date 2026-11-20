/**************************************************************************/
/*  gi_light_query.cpp                                                    */
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

#include "gi_light_query.h"

#include "core/math/math_funcs.h"
#include "core/math/vector3.h"

#include <cstring>

GiLightQuery::Validation GiLightQuery::validate(const PackedFloat32Array &p_queries, const PackedInt32Array &p_slots) {
	if (p_queries.size() % int64_t(FLOATS_PER_QUERY) != 0) {
		return INVALID_FLOAT_COUNT;
	}
	const int64_t count = p_queries.size() / int64_t(FLOATS_PER_QUERY);
	if (count > int64_t(MAX_QUERIES)) {
		return INVALID_QUERY_COUNT;
	}
	if (p_slots.size() != count) {
		return INVALID_SLOT_COUNT;
	}
	uint32_t seen = 0;
	for (int64_t i = 0; i < count; i++) {
		const int32_t slot = p_slots[i];
		if (slot < 0 || slot >= int32_t(MAX_QUERIES)) {
			return INVALID_SLOT_RANGE;
		}
		if (seen & (1u << uint32_t(slot))) {
			return INVALID_SLOT_REPEATED;
		}
		seen |= 1u << uint32_t(slot);
		const float *query = p_queries.ptr() + i * FLOATS_PER_QUERY;
		for (uint32_t k = 0; k < FLOATS_PER_QUERY; k++) {
			if (!Math::is_finite(query[k])) {
				return INVALID_VALUE;
			}
		}
		if (double(query[3]) * query[3] + double(query[4]) * query[4] + double(query[5]) * query[5] < 1e-12) {
			return INVALID_FACING;
		}
	}
	return VALID;
}

const char *GiLightQuery::validation_text(Validation p_validation) {
	switch (p_validation) {
		case VALID:
			return "";
		case INVALID_FLOAT_COUNT:
			return "GI light queries must hold 7 floats per query: px, py, pz, fx, fy, fz, sun_factor.";
		case INVALID_QUERY_COUNT:
			return "At most 32 GI light queries can be set per viewport.";
		case INVALID_SLOT_COUNT:
			return "GI light queries need exactly one slot per query.";
		case INVALID_SLOT_RANGE:
			return "GI light query slots must be in the range 0 to 31.";
		case INVALID_SLOT_REPEATED:
			return "Each GI light query slot can appear only once.";
		case INVALID_VALUE:
			return "GI light query values must be finite.";
		case INVALID_FACING:
			return "A GI light query facing must not be zero.";
	}
	return "";
}

void GiLightQuery::pack(const PackedFloat32Array &p_queries, const PackedInt32Array &p_slots, LocalVector<Query> &r_queries) {
	const uint32_t count = MIN(uint32_t(p_slots.size()), uint32_t(p_queries.size() / int64_t(FLOATS_PER_QUERY)));
	r_queries.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const float *source = p_queries.ptr() + i * FLOATS_PER_QUERY;
		const Vector3 facing = Vector3(source[3], source[4], source[5]).normalized();
		Query &query = r_queries[i];
		for (int k = 0; k < 3; k++) {
			query.position[k] = source[k];
			query.facing[k] = float(facing[k]);
		}
		query.slot = uint32_t(p_slots[i]);
		query.sun_factor = CLAMP(source[6], 0.0f, 1.0f);
	}
}

String GiLightQuery::shader_defines() {
	String defines = "\n";
	defines += vformat("#define GI_LIGHT_QUERY_FLAG_TRIMMED %du\n", int64_t(FLAG_TRIMMED));
	defines += vformat("#define GI_LIGHT_QUERY_FLAG_PARTIAL_CLOUD %du\n", int64_t(FLAG_PARTIAL_CLOUD));
	defines += vformat("#define GI_LIGHT_QUERY_FLAG_SURFELS_ON %du\n", int64_t(FLAG_SURFELS_ON));
	defines += vformat("#define GI_LIGHT_QUERY_FLAG_NO_COVERAGE %du\n", int64_t(FLAG_NO_COVERAGE));
	defines += vformat("#define GI_LIGHT_QUERY_FLAG_HARDWARE_RAYS %du\n", int64_t(FLAG_HARDWARE_RAYS));
	defines += vformat("#define GI_LIGHT_QUERY_THREADS %d\n", int64_t(THREADS));
	defines += vformat("#define GI_LIGHT_QUERY_SUN_RAYS %du\n", int64_t(SUN_RAYS));
	defines += vformat("#define GI_LIGHT_QUERY_AREA_LONG %du\n", int64_t(AREA_SAMPLES_LONG));
	defines += vformat("#define GI_LIGHT_QUERY_AREA_SHORT %du\n", int64_t(AREA_SAMPLES_SHORT));
	defines += vformat("#define GI_LIGHT_QUERY_AREA_SAMPLES %du\n", int64_t(AREA_SAMPLES));
	defines += vformat("#define GI_LIGHT_QUERY_SKY_SAMPLES %du\n", int64_t(SKY_SAMPLES));
	defines += vformat("#define GI_LIGHT_QUERY_SUN_DISC %.4f\n", double(SUN_DISC_RADIUS));
	defines += vformat("#define GI_LIGHT_QUERY_POINT_FLOOR %.4f\n", double(POINT_DISTANCE_FLOOR));
	defines += vformat("#define GI_LIGHT_QUERY_BIAS %.4f\n", double(SHADOW_BIAS));
	return defines;
}

void GiLightQuery::_update_active() {
	uint32_t count = 0;
	for (const KeyValue<RID, ViewportTable> &E : viewports) {
		count += E.value.queries.is_empty() ? 0 : 1;
	}
	active.set(count);
}

void GiLightQuery::set_requests(RID p_viewport, RID p_render_target, const LocalVector<Query> &p_queries) {
	MutexLock lock(mutex);
	ViewportTable *table = viewports.getptr(p_viewport);
	if (!table) {
		if (p_queries.is_empty()) {
			return;
		}
		table = &viewports.insert(p_viewport, ViewportTable())->value;
	}
	table->render_target = p_render_target;
	table->generation = ++generation_counter;
	bool requested[MAX_QUERIES] = {};
	for (const Query &query : p_queries) {
		if (query.slot < MAX_QUERIES) {
			requested[query.slot] = true;
		}
	}
	for (uint32_t s = 0; s < MAX_QUERIES; s++) {
		Slot &slot = table->slots[s];
		if (requested[s] && !slot.requested) {
			slot.added_generation = table->generation;
			slot.valid = false;
		} else if (!requested[s]) {
			slot.valid = false;
		}
		slot.requested = requested[s];
	}
	table->queries = p_queries;
	_update_active();
}

bool GiLightQuery::take_requests(RID p_render_target, uint32_t p_frame, RID &r_viewport, uint64_t &r_generation, LocalVector<Query> &r_queries) {
	MutexLock lock(mutex);
	for (KeyValue<RID, ViewportTable> &E : viewports) {
		if (E.value.render_target != p_render_target || E.value.queries.is_empty()) {
			continue;
		}
		E.value.last_frame = p_frame;
		E.value.taken = true;
		r_viewport = E.key;
		r_generation = E.value.generation;
		r_queries = E.value.queries;
		return true;
	}
	return false;
}

void GiLightQuery::store_results(RID p_viewport, uint64_t p_generation, uint64_t p_dispatch_usec, const uint8_t *p_data, uint64_t p_size) {
	if (!p_data) {
		return;
	}
	MutexLock lock(mutex);
	ViewportTable *table = viewports.getptr(p_viewport);
	if (!table) {
		return;
	}
	const uint64_t count = MIN(p_size / uint64_t(sizeof(Result)), uint64_t(MAX_QUERIES));
	for (uint64_t i = 0; i < count; i++) {
		Result result;
		memcpy(&result, p_data + i * sizeof(Result), sizeof(Result));
		if (result.slot >= MAX_QUERIES) {
			continue;
		}
		Slot &slot = table->slots[result.slot];
		if (!slot.requested || p_generation < slot.added_generation) {
			continue;
		}
		if (table->skipped && int32_t(result.frame - table->skip_frame) <= 0) {
			continue;
		}
		if (slot.valid && int32_t(result.frame - slot.result.frame) < 0) {
			continue;
		}
		slot.result = result;
		slot.dispatch_usec = p_dispatch_usec;
		slot.valid = true;
	}
}

Array GiLightQuery::get_results(RID p_viewport, uint64_t p_now_usec, uint32_t p_now_frame) const {
	Array rows;
	MutexLock lock(mutex);
	const ViewportTable *table = viewports.getptr(p_viewport);
	if (!table) {
		return rows;
	}
	for (uint32_t s = 0; s < MAX_QUERIES; s++) {
		const Slot &slot = table->slots[s];
		if (!slot.requested || !slot.valid) {
			continue;
		}
		Array row;
		row.resize(RESULT_FIELDS);
		row[FIELD_SLOT] = int64_t(s);
		row[FIELD_R] = slot.result.rgb[0];
		row[FIELD_G] = slot.result.rgb[1];
		row[FIELD_B] = slot.result.rgb[2];
		row[FIELD_Y] = slot.result.y;
		row[FIELD_COVERAGE] = slot.result.coverage;
		row[FIELD_BODY_Y] = slot.result.body_y;
		row[FIELD_FRAME] = int64_t(slot.result.frame);
		row[FIELD_FLAGS] = int64_t(slot.result.flags);
		row[FIELD_AGE_MSEC] = p_now_usec > slot.dispatch_usec ? double(p_now_usec - slot.dispatch_usec) * 0.001 : 0.0;
		row[FIELD_AGE_FRAMES] = int64_t(MAX(int32_t(p_now_frame - slot.result.frame), int32_t(0)));
		rows.push_back(row);
	}
	return rows;
}

void GiLightQuery::_skip(ViewportTable &r_table, uint32_t p_frame) {
	for (Slot &slot : r_table.slots) {
		slot.valid = false;
	}
	r_table.skipped = true;
	r_table.skip_frame = p_frame;
}

void GiLightQuery::skip(RID p_viewport, uint32_t p_frame) {
	MutexLock lock(mutex);
	ViewportTable *table = viewports.getptr(p_viewport);
	if (table) {
		_skip(*table, p_frame);
	}
}

void GiLightQuery::finish_frame(RID p_render_target, uint32_t p_frame) {
	MutexLock lock(mutex);
	for (KeyValue<RID, ViewportTable> &E : viewports) {
		if (E.value.render_target != p_render_target || E.value.queries.is_empty()) {
			continue;
		}
		if (!E.value.taken || E.value.last_frame != p_frame) {
			_skip(E.value, p_frame);
		}
	}
}

void GiLightQuery::forget(RID p_viewport) {
	MutexLock lock(mutex);
	viewports.erase(p_viewport);
	_update_active();
}

GiLightQuery *GiLightQuery::get_singleton() {
	static GiLightQuery singleton;
	return &singleton;
}
