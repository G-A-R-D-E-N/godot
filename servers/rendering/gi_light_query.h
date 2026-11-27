/**************************************************************************/
/*  gi_light_query.h                                                      */
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

#pragma once

#include "core/os/mutex.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/array.h"
#include "core/variant/variant.h"

#include <stdint.h>

class GiLightQuery {
public:
	static constexpr uint32_t MAX_QUERIES = 32;
	static constexpr uint32_t FLOATS_PER_QUERY = 7;
	static constexpr uint32_t RESULT_FIELDS = 11;
	static constexpr uint32_t THREADS = 64;
	static constexpr uint32_t SUN_RAYS = 4;
	static constexpr uint32_t AREA_SAMPLES_LONG = 4;
	static constexpr uint32_t AREA_SAMPLES_SHORT = 2;
	static constexpr uint32_t AREA_SAMPLES = AREA_SAMPLES_LONG * AREA_SAMPLES_SHORT;
	static constexpr uint32_t SKY_SAMPLES = 8;
	static constexpr float SUN_DISC_RADIUS = 0.05f;
	static constexpr float POINT_DISTANCE_FLOOR = 0.1f;
	static constexpr float SHADOW_BIAS = 0.01f;

	enum Flag : uint32_t {
		FLAG_TRIMMED = 1,
		FLAG_PARTIAL_CLOUD = 2,
		FLAG_SURFELS_ON = 4,
		FLAG_NO_COVERAGE = 8,
		FLAG_HARDWARE_RAYS = 16,
	};

	enum Validation {
		VALID,
		INVALID_FLOAT_COUNT,
		INVALID_QUERY_COUNT,
		INVALID_SLOT_COUNT,
		INVALID_SLOT_RANGE,
		INVALID_SLOT_REPEATED,
		INVALID_VALUE,
		INVALID_FACING,
	};

	enum ResultField {
		FIELD_SLOT,
		FIELD_R,
		FIELD_G,
		FIELD_B,
		FIELD_Y,
		FIELD_COVERAGE,
		FIELD_BODY_Y,
		FIELD_FRAME,
		FIELD_FLAGS,
		FIELD_AGE_MSEC,
		FIELD_AGE_FRAMES,
	};

	struct Query {
		float position[3];
		uint32_t slot;
		float facing[3];
		float sun_factor;
	};

	struct QueryHeader {
		float ambient_color[3];
		float ambient_sky_mix;
		uint32_t count;
		uint32_t frame;
		uint32_t flags;
		uint32_t sun_mask;
	};

	struct Result {
		float rgb[3];
		float y;
		float coverage;
		float body_y;
		uint32_t slot;
		uint32_t frame;
		uint32_t flags;
		uint32_t pad[3];
	};

	static_assert(sizeof(Query) == 32);
	static_assert(sizeof(QueryHeader) == 32);
	static_assert(sizeof(Result) == 48);
	static_assert(MAX_QUERIES <= 32, "Slots are kept in a 32-bit mask.");
	static_assert(RESULT_FIELDS == FIELD_AGE_FRAMES + 1);

	static Validation validate(const PackedFloat32Array &p_queries, const PackedInt32Array &p_slots);
	static const char *validation_text(Validation p_validation);
	static void pack(const PackedFloat32Array &p_queries, const PackedInt32Array &p_slots, LocalVector<Query> &r_queries);
	static String shader_defines();

	void set_requests(RID p_viewport, RID p_render_target, const LocalVector<Query> &p_queries);
	bool take_requests(RID p_render_target, uint32_t p_frame, RID &r_viewport, uint64_t &r_generation, LocalVector<Query> &r_queries);
	void store_results(RID p_viewport, uint64_t p_generation, uint64_t p_dispatch_usec, const uint8_t *p_data, uint64_t p_size);
	Array get_results(RID p_viewport, uint64_t p_now_usec, uint32_t p_now_frame) const;
	void skip(RID p_viewport, uint32_t p_frame);
	void finish_frame(RID p_render_target, uint32_t p_frame);
	void forget(RID p_viewport);
	bool is_active() const { return active.get() > 0; }

	static GiLightQuery *get_singleton();

private:
	struct Slot {
		Result result = {};
		uint64_t dispatch_usec = 0;
		uint64_t added_generation = 0;
		bool requested = false;
		bool valid = false;
	};

	struct ViewportTable {
		RID render_target;
		LocalVector<Query> queries;
		Slot slots[MAX_QUERIES];
		uint64_t generation = 0;
		uint32_t last_frame = 0;
		uint32_t skip_frame = 0;
		bool taken = false;
		bool skipped = false;
	};

	mutable Mutex mutex;
	HashMap<RID, ViewportTable> viewports;
	SafeNumeric<uint32_t> active;
	uint64_t generation_counter = 0;

	void _update_active();
	static void _skip(ViewportTable &r_table, uint32_t p_frame);
};
