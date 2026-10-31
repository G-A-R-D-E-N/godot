/**************************************************************************/
/*  test_direct_lights.cpp                                                */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_direct_lights)

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/environment/direct_lights.h"

namespace TestDirectLights {

using RendererRD::DirectLightTable;

RID light_id(uint64_t p_id) {
	return RID::from_uint64(p_id);
}

DirectLightTable::Lamp lamp_at(const Vector3 &p_position, float p_range = 3.0f, float p_movement = 0.0f) {
	DirectLightTable::Lamp lamp;
	lamp.position = p_position;
	lamp.range = p_range;
	lamp.movement = p_movement;
	lamp.size = 0.1f;
	return lamp;
}

TEST_CASE("[DirectLights] Flags word layout") {
	CHECK(DirectLightTable::pack_flags(DirectLightTable::NO_SLOT, 1.0f) == 0u);
	CHECK(DirectLightTable::pack_flags(DirectLightTable::MAX_SLOT + 1, 1.0f) == 0u);

	const uint32_t full = DirectLightTable::pack_flags(DirectLightTable::MAX_SLOT, 1.0f);
	CHECK((full & 1u) == 1u);
	CHECK(((full >> 1) & 0xFFFFu) == DirectLightTable::MAX_SLOT);
	CHECK(((full >> 17) & 0xFFu) == 255u);
	CHECK((full >> 25) == 0u);

	const uint32_t half = DirectLightTable::pack_flags(7, 0.5f);
	CHECK(((half >> 1) & 0xFFFFu) == 7u);
	CHECK(((half >> 17) & 0xFFu) == 128u);

	CHECK(((DirectLightTable::pack_flags(7, -1.0f) >> 17) & 0xFFu) == 0u);
	CHECK(((DirectLightTable::pack_flags(7, 2.0f) >> 17) & 0xFFu) == 255u);
}

TEST_CASE("[DirectLights] Slots written for the GPU follow the table") {
	DirectLightTable table;
	LocalVector<DirectLightTable::GpuSlot> slots;
	table.write_slots(slots);
	CHECK(slots.size() == 1u);
	CHECK(slots[0].caster_mask == 0u);

	table.begin_frame(5);
	DirectLightTable::Lamp lamp = lamp_at(Vector3(1, 2, 3));
	lamp.caster_mask = 0x1234u;
	const uint32_t slot = table.observe(light_id(1), lamp);
	table.write_slots(slots);
	REQUIRE(slots.size() == slot + 1);
	CHECK(slots[slot].caster_mask == 0x1234u);
	CHECK(slots[slot].size == 0.1f);
	CHECK(slots[slot].reset_frame == 5u);
	CHECK(slots[slot].dynamic == 0u);
	CHECK(slots[0].caster_mask == 0u);

	table.begin_frame(6);
	lamp.position = Vector3(1.1f, 2, 3);
	table.observe(light_id(1), lamp);
	table.write_slots(slots);
	CHECK(slots[slot].reset_frame == 5u);
	CHECK(slots[slot].dynamic == 1u);

	table.begin_frame(6 + DirectLightTable::DYNAMIC_FRAMES);
	table.observe(light_id(1), lamp);
	table.write_slots(slots);
	CHECK(slots[slot].dynamic == 0u);

	table.begin_frame(7 + DirectLightTable::DYNAMIC_FRAMES);
	table.begin_frame(8 + DirectLightTable::DYNAMIC_FRAMES);
	table.write_slots(slots);
	CHECK(slots[slot].caster_mask == 0u);
}

TEST_CASE("[DirectLights] Lamp values written for a dump follow the slots") {
	DirectLightTable table;
	LocalVector<float> values;
	table.write_lamps(values);
	CHECK(values.size() == DirectLightTable::LAMP_VALUES);

	table.begin_frame(1);
	table.observe(light_id(1), lamp_at(Vector3(9, 9, 9)));
	const uint32_t slot = table.observe(light_id(2), lamp_at(Vector3(1, 2, 3)));
	table.write_lamps(values);
	REQUIRE(values.size() == (slot + 1) * DirectLightTable::LAMP_VALUES);
	CHECK(values[slot * 4 + 0] == 1.0f);
	CHECK(values[slot * 4 + 1] == 2.0f);
	CHECK(values[slot * 4 + 2] == 3.0f);
	CHECK(values[slot * 4 + 3] == 0.1f);
	CHECK(values[0] == 0.0f);
}

TEST_CASE("[DirectLights] Clearing a used table changes its generation once") {
	DirectLightTable table;
	const uint64_t empty = table.get_generation();
	table.clear();
	CHECK(table.get_generation() == empty);

	table.begin_frame(5);
	table.observe(light_id(1), lamp_at(Vector3()));
	CHECK(table.get_generation() == empty);
	table.clear();
	CHECK(table.get_generation() == empty + 1);
	table.clear();
	CHECK(table.get_generation() == empty + 1);
}

TEST_CASE("[DirectLights] A light with no setting is owned, unmoving and keeps its own size") {
	const DirectLightTable::Setting setting;
	CHECK(setting.owned);
	CHECK(setting.movement == 0.0f);
	CHECK(setting.resolved_size(0.25f) == 0.25f);

	DirectLightTable::Setting written;
	written.size = 0.0f;
	CHECK(written.resolved_size(0.25f) == 0.0f);
}

TEST_CASE("[DirectLights] Reset distance") {
	CHECK(DirectLightTable::reset_distance(3.0f, 0.0f) == 0.5f);
	CHECK(DirectLightTable::reset_distance(20.0f, 0.0f) == 2.0f);
	CHECK(DirectLightTable::reset_distance(3.0f, 0.3f) == doctest::Approx(0.5f + 2.0f * Math::sqrt(3.0f) * 0.3f));
}

TEST_CASE("[DirectLights] Slots are stable, unique, never 0 and reused only after 8 frames") {
	DirectLightTable table;
	table.begin_frame(1);
	const uint32_t a = table.observe(light_id(1), lamp_at(Vector3()));
	const uint32_t b = table.observe(light_id(2), lamp_at(Vector3(1, 0, 0)));
	CHECK(a != DirectLightTable::NO_SLOT);
	CHECK(b != DirectLightTable::NO_SLOT);
	CHECK(a != b);
	CHECK(table.observe(light_id(1), lamp_at(Vector3())) == a);

	table.begin_frame(2);
	CHECK(table.observe(light_id(1), lamp_at(Vector3())) == a);
	CHECK(table.observe(light_id(2), lamp_at(Vector3(1, 0, 0))) == b);

	table.begin_frame(3);
	CHECK(table.observe(light_id(1), lamp_at(Vector3())) == a);
	CHECK(table.slots_in_use() == 2);

	table.begin_frame(4);
	table.observe(light_id(1), lamp_at(Vector3()));
	CHECK(table.slots_in_use() == 1);
	CHECK(table.get_state(light_id(2)).slot == DirectLightTable::NO_SLOT);

	for (uint64_t frame = 5; frame < 12; frame++) {
		table.begin_frame(frame);
		CHECK(table.observe(light_id(1), lamp_at(Vector3())) == a);
		CHECK(table.observe(light_id(100 + frame), lamp_at(Vector3())) != b);
	}
	table.begin_frame(12);
	table.observe(light_id(1), lamp_at(Vector3()));
	CHECK(table.observe(light_id(99), lamp_at(Vector3())) == b);
}

TEST_CASE("[DirectLights] Frames with no render don't release a slot") {
	DirectLightTable table;
	table.begin_frame(1);
	const uint32_t a = table.observe(light_id(1), lamp_at(Vector3()));
	table.begin_frame(40);
	CHECK(table.observe(light_id(1), lamp_at(Vector3())) == a);
	CHECK_FALSE(table.get_state(light_id(1)).hard_reset);
}

TEST_CASE("[DirectLights] A long gap keeps the slot and still sees a change") {
	DirectLightTable table;
	table.begin_frame(1);
	const uint32_t a = table.observe(light_id(1), lamp_at(Vector3()));
	table.begin_frame(1000);
	CHECK(table.observe(light_id(1), lamp_at(Vector3(0.6f, 0, 0))) == a);
	CHECK(table.get_state(light_id(1)).hard_reset);
}

TEST_CASE("[DirectLights] Frame 0 is a frame like any other") {
	DirectLightTable table;
	table.begin_frame(0);
	const uint32_t a = table.observe(light_id(1), lamp_at(Vector3()));
	CHECK(a != DirectLightTable::NO_SLOT);
	CHECK(table.get_state(light_id(1)).hard_reset);
	table.begin_frame(1);
	CHECK(table.observe(light_id(1), lamp_at(Vector3())) == a);
	CHECK_FALSE(table.get_state(light_id(1)).hard_reset);
}

TEST_CASE("[DirectLights] Slots run out without handing out a used one") {
	DirectLightTable table;
	table.begin_frame(1);
	LocalVector<bool> seen;
	seen.resize(DirectLightTable::MAX_SLOT + 1);
	for (uint32_t i = 0; i <= DirectLightTable::MAX_SLOT; i++) {
		seen[i] = false;
	}
	for (uint64_t i = 1; i <= DirectLightTable::MAX_SLOT; i++) {
		const uint32_t slot = table.observe(light_id(i), lamp_at(Vector3()));
		REQUIRE(slot != DirectLightTable::NO_SLOT);
		REQUIRE(slot <= DirectLightTable::MAX_SLOT);
		REQUIRE_FALSE(seen[slot]);
		seen[slot] = true;
	}
	CHECK(table.observe(light_id(DirectLightTable::MAX_SLOT + 1), lamp_at(Vector3())) == DirectLightTable::NO_SLOT);
	CHECK(table.get_state(light_id(DirectLightTable::MAX_SLOT + 1)).slot == DirectLightTable::NO_SLOT);
}

TEST_CASE("[DirectLights] A lamp that comes on gets a hard reset, and one that goes off and on gets a new slot") {
	DirectLightTable table;
	table.begin_frame(1);
	const uint32_t a = table.observe(light_id(1), lamp_at(Vector3()));
	CHECK(table.get_state(light_id(1)).hard_reset);

	table.begin_frame(2);
	table.observe(light_id(1), lamp_at(Vector3()));
	CHECK_FALSE(table.get_state(light_id(1)).hard_reset);

	table.begin_frame(3);
	table.begin_frame(4);
	const uint32_t again = table.observe(light_id(1), lamp_at(Vector3()));
	CHECK(again != a);
	CHECK(table.get_state(light_id(1)).hard_reset);
}

TEST_CASE("[DirectLights] Counters count hard resets of a running lamp and frames it stays dynamic") {
	DirectLightTable table;
	table.begin_frame(1);
	table.observe(light_id(1), lamp_at(Vector3()));
	CHECK(table.get_hard_resets() == 0);
	CHECK(table.get_dynamic_lamp_frames() == 0);

	table.begin_frame(2);
	table.observe(light_id(1), lamp_at(Vector3(0.1f, 0, 0)));
	CHECK(table.get_hard_resets() == 0);

	table.begin_frame(3);
	CHECK(table.get_dynamic_lamp_frames() == 1);
	table.observe(light_id(1), lamp_at(Vector3(5.0f, 0, 0)));
	CHECK(table.get_hard_resets() == 1);
}

TEST_CASE("[DirectLights] 10,000 frames at the flicker cap give no hard reset") {
	DirectLightTable table;
	const float m = 0.3f;
	const Vector3 center(10, 2, -4);
	int resets = 0;
	for (uint64_t frame = 1; frame <= 10000; frame++) {
		table.begin_frame(frame);
		const Vector3 corner((frame & 1) ? m : -m, (frame & 2) ? m : -m, (frame & 4) ? m : -m);
		table.observe(light_id(1), lamp_at(center + corner, 3.0f, m));
		if (frame > 1 && table.get_state(light_id(1)).hard_reset) {
			resets++;
		}
	}
	CHECK(resets == 0);
	CHECK(table.get_state(light_id(1)).dynamic);
}

TEST_CASE("[DirectLights] A 0.6 m step gives exactly one hard reset") {
	DirectLightTable table;
	int resets = 0;
	for (uint64_t frame = 1; frame <= 100; frame++) {
		table.begin_frame(frame);
		table.observe(light_id(1), lamp_at(frame < 50 ? Vector3() : Vector3(0.6f, 0, 0)));
		if (frame > 1 && table.get_state(light_id(1)).hard_reset) {
			resets++;
		}
	}
	CHECK(resets == 1);
}

TEST_CASE("[DirectLights] Caster mask, size and type changes each give one hard reset") {
	DirectLightTable table;
	table.begin_frame(1);
	table.observe(light_id(1), lamp_at(Vector3()));

	DirectLightTable::Lamp lamp = lamp_at(Vector3());
	lamp.caster_mask = 1;
	table.begin_frame(2);
	table.observe(light_id(1), lamp);
	CHECK(table.get_state(light_id(1)).hard_reset);
	table.begin_frame(3);
	table.observe(light_id(1), lamp);
	CHECK_FALSE(table.get_state(light_id(1)).hard_reset);

	lamp.size = 0.2f;
	table.begin_frame(4);
	table.observe(light_id(1), lamp);
	CHECK(table.get_state(light_id(1)).hard_reset);
	table.begin_frame(5);
	table.observe(light_id(1), lamp);
	CHECK_FALSE(table.get_state(light_id(1)).hard_reset);

	lamp.type = 1;
	table.begin_frame(6);
	table.observe(light_id(1), lamp);
	CHECK(table.get_state(light_id(1)).hard_reset);
}

TEST_CASE("[DirectLights] Moving makes a lamp dynamic for 30 frames") {
	DirectLightTable table;
	table.begin_frame(1);
	table.observe(light_id(1), lamp_at(Vector3()));
	CHECK_FALSE(table.get_state(light_id(1)).dynamic);

	table.begin_frame(2);
	table.observe(light_id(1), lamp_at(Vector3(0.01f, 0, 0)));
	CHECK(table.get_state(light_id(1)).dynamic);
	CHECK_FALSE(table.get_state(light_id(1)).hard_reset);

	for (uint64_t frame = 3; frame <= 31; frame++) {
		table.begin_frame(frame);
		table.observe(light_id(1), lamp_at(Vector3(0.01f, 0, 0)));
		CHECK(table.get_state(light_id(1)).dynamic);
	}
	table.begin_frame(32);
	table.observe(light_id(1), lamp_at(Vector3(0.01f, 0, 0)));
	CHECK_FALSE(table.get_state(light_id(1)).dynamic);
}

TEST_CASE("[DirectLights] A second view in the same frame sees the same events") {
	DirectLightTable table;
	table.begin_frame(1);
	table.observe(light_id(1), lamp_at(Vector3()));

	table.begin_frame(2);
	table.observe(light_id(1), lamp_at(Vector3(0.6f, 0, 0)));
	CHECK(table.get_state(light_id(1)).hard_reset);
	table.begin_frame(2);
	table.observe(light_id(1), lamp_at(Vector3(0.6f, 0, 0)));
	CHECK(table.get_state(light_id(1)).hard_reset);
}

TEST_CASE("[DirectLights] A lamp only the first view saw keeps its slot") {
	DirectLightTable table;
	table.begin_frame(1);
	const uint32_t a = table.observe(light_id(1), lamp_at(Vector3()));
	const uint32_t b = table.observe(light_id(2), lamp_at(Vector3(1, 0, 0)));
	table.begin_frame(1);
	table.observe(light_id(1), lamp_at(Vector3()));

	table.begin_frame(2);
	CHECK(table.observe(light_id(1), lamp_at(Vector3())) == a);
	CHECK(table.observe(light_id(2), lamp_at(Vector3(1, 0, 0))) == b);
	CHECK_FALSE(table.get_state(light_id(2)).hard_reset);
}

TEST_CASE("[DirectLights] Which lights DI owns") {
	CHECK(DirectLightTable::owns(true, false, 1u, 1.0f, false));
	CHECK(DirectLightTable::owns(true, false, 1u, 0.0f, false));
	CHECK_FALSE(DirectLightTable::owns(false, false, 1u, 1.0f, false));
	CHECK_FALSE(DirectLightTable::owns(true, true, 1u, 1.0f, false));
	CHECK_FALSE(DirectLightTable::owns(true, false, 0u, 1.0f, false));
	CHECK_FALSE(DirectLightTable::owns(true, false, 1u, -0.1f, false));
	CHECK_FALSE(DirectLightTable::owns(true, false, 1u, 1.0f, true));
}

TEST_CASE("[DirectLights] A moved instance makes only the lamps it can reach dynamic") {
	DirectLightTable table;
	table.begin_frame(1);
	DirectLightTable::Lamp lamp = lamp_at(Vector3());
	lamp.range = 3.0f;
	DirectLightTable::Lamp far_lamp = lamp_at(Vector3(100, 0, 0));
	far_lamp.range = 3.0f;
	table.observe(light_id(1), lamp);
	table.observe(light_id(2), far_lamp);

	table.begin_frame(2);
	table.observe(light_id(1), lamp);
	table.observe(light_id(2), far_lamp);
	CHECK_FALSE(table.get_state(light_id(1)).dynamic);

	LocalVector<AABB> moved;
	table.mark_moved(moved);
	CHECK_FALSE(table.get_state(light_id(1)).dynamic);

	moved.push_back(AABB(Vector3(2, 0, 0), Vector3(1, 1, 1)));
	table.mark_moved(moved);
	CHECK(table.get_state(light_id(1)).dynamic);
	CHECK_FALSE(table.get_state(light_id(2)).dynamic);

	moved.clear();
	moved.push_back(AABB(Vector3(4, 0, 0), Vector3(1, 1, 1)));
	table.begin_frame(2 + DirectLightTable::DYNAMIC_FRAMES);
	table.observe(light_id(1), lamp);
	table.mark_moved(moved);
	CHECK_FALSE(table.get_state(light_id(1)).dynamic);
}

TEST_CASE("[DirectLights] Clear starts over") {
	DirectLightTable table;
	table.begin_frame(1);
	const uint32_t a = table.observe(light_id(1), lamp_at(Vector3()));
	table.clear();
	CHECK(table.slots_in_use() == 0);
	table.begin_frame(1);
	CHECK(table.observe(light_id(2), lamp_at(Vector3())) == a);
}

} // namespace TestDirectLights

#endif
