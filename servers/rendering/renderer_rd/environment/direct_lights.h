/**************************************************************************/
/*  direct_lights.h                                                       */
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

#include "core/math/vector3.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"

namespace RendererRD {

class DirectLightTable {
public:
	static constexpr uint32_t NO_SLOT = 0;
	static constexpr uint32_t MAX_SLOT = 65535;
	static constexpr uint64_t SLOT_REUSE_FRAMES = 8;
	static constexpr uint64_t DYNAMIC_FRAMES = 30;
	static constexpr float MIN_RESET_DISTANCE = 0.5f;
	static constexpr float RESET_RANGE_FRACTION = 0.1f;
	static constexpr uint32_t LAMP_VALUES = 4;

	struct Setting {
		bool owned = true;
		float movement = 0.0f;
		float size = -1.0f;

		float resolved_size(float p_light_size) const { return size < 0.0f ? p_light_size : size; }
	};

	struct Lamp {
		Vector3 position;
		float range = 0.0f;
		float movement = 0.0f;
		float size = 0.0f;
		uint32_t caster_mask = 0xFFFFFFFF;
		uint32_t type = 0;
	};

	struct State {
		uint32_t slot = NO_SLOT;
		bool hard_reset = false;
		bool dynamic = false;
	};

	struct GpuSlot {
		uint32_t caster_mask = 0;
		float size = 0.0f;
		uint32_t reset_frame = 0;
		uint32_t dynamic = 0;
	};

	static uint32_t pack_flags(uint32_t p_slot, float p_strength);
	static float reset_distance(float p_range, float p_movement);
	static bool owns(bool p_owned, bool p_area, uint32_t p_cull_mask, float p_energy, bool p_projector);

	void begin_frame(uint64_t p_frame);
	uint32_t observe(RID p_light, const Lamp &p_lamp);
	State get_state(RID p_light) const;
	uint32_t slots_in_use() const;
	void write_slots(LocalVector<GpuSlot> &r_slots) const;
	void write_lamps(LocalVector<float> &r_values) const;
	uint64_t get_generation() const { return generation; }
	void clear();

private:
	struct Record {
		Lamp lamp;
		Vector3 reset_position;
		uint64_t last_seen = 0;
		uint64_t reset_frame = 0;
		uint64_t dynamic_until = 0;
		uint32_t slot = NO_SLOT;
	};

	struct RetiredSlot {
		uint32_t slot = NO_SLOT;
		uint64_t free_at = 0;
	};

	uint32_t _take_slot();

	HashMap<RID, Record> records;
	LocalVector<RetiredSlot> retired;
	LocalVector<uint32_t> free_slots;
	uint32_t next_slot = 1;
	uint64_t frame = 0;
	uint64_t generation = 0;
};

} // namespace RendererRD
