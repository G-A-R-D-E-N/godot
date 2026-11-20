/**************************************************************************/
/*  test_gi_light_query.cpp                                               */
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

TEST_FORCE_LINK(test_gi_light_query)

#include "servers/rendering/gi_light_query.h"

#include <cstddef>

namespace TestGiLightQuery {

static PackedFloat32Array make_queries(uint32_t p_count) {
	PackedFloat32Array queries;
	for (uint32_t i = 0; i < p_count; i++) {
		queries.push_back(float(i));
		queries.push_back(1.5f);
		queries.push_back(-2.0f);
		queries.push_back(0.0f);
		queries.push_back(0.0f);
		queries.push_back(2.0f);
		queries.push_back(1.0f);
	}
	return queries;
}

static PackedInt32Array make_slots(uint32_t p_count) {
	PackedInt32Array slots;
	for (uint32_t i = 0; i < p_count; i++) {
		slots.push_back(int32_t(i));
	}
	return slots;
}

static LocalVector<GiLightQuery::Query> requests_for(const LocalVector<uint32_t> &p_slots) {
	LocalVector<GiLightQuery::Query> queries;
	for (uint32_t slot : p_slots) {
		GiLightQuery::Query query = {};
		query.slot = slot;
		query.facing[1] = 1.0f;
		query.sun_factor = 1.0f;
		queries.push_back(query);
	}
	return queries;
}

static GiLightQuery::Result result_for(uint32_t p_slot, uint32_t p_frame, float p_value) {
	GiLightQuery::Result result = {};
	result.rgb[0] = p_value;
	result.rgb[1] = p_value * 2.0f;
	result.rgb[2] = p_value * 3.0f;
	result.y = p_value * 4.0f;
	result.coverage = 0.5f;
	result.body_y = p_value * 0.25f;
	result.slot = p_slot;
	result.frame = p_frame;
	result.flags = GiLightQuery::FLAG_SURFELS_ON;
	return result;
}

static void land(GiLightQuery &p_table, RID p_viewport, uint64_t p_generation, uint64_t p_usec, const LocalVector<GiLightQuery::Result> &p_results) {
	p_table.store_results(p_viewport, p_generation, p_usec, reinterpret_cast<const uint8_t *>(p_results.ptr()), uint64_t(p_results.size()) * sizeof(GiLightQuery::Result));
}

static Array row_for_slot(const Array &p_rows, int64_t p_slot) {
	for (int i = 0; i < p_rows.size(); i++) {
		const Array row = p_rows[i];
		if (int64_t(row[GiLightQuery::FIELD_SLOT]) == p_slot) {
			return row;
		}
	}
	return Array();
}

TEST_CASE("[GiLightQuery] The buffer entries have the std430 layout the shader reads") {
	CHECK(sizeof(GiLightQuery::Result) == 48);
	CHECK(offsetof(GiLightQuery::Result, rgb) == 0);
	CHECK(offsetof(GiLightQuery::Result, y) == 12);
	CHECK(offsetof(GiLightQuery::Result, coverage) == 16);
	CHECK(offsetof(GiLightQuery::Result, body_y) == 20);
	CHECK(offsetof(GiLightQuery::Result, slot) == 24);
	CHECK(offsetof(GiLightQuery::Result, frame) == 28);
	CHECK(offsetof(GiLightQuery::Result, flags) == 32);
	CHECK(sizeof(GiLightQuery::Query) == 32);
	CHECK(offsetof(GiLightQuery::Query, slot) == 12);
	CHECK(offsetof(GiLightQuery::Query, facing) == 16);
	CHECK(offsetof(GiLightQuery::Query, sun_factor) == 28);
	CHECK(sizeof(GiLightQuery::QueryHeader) == 32);
	CHECK(offsetof(GiLightQuery::QueryHeader, count) == 16);
	CHECK(offsetof(GiLightQuery::QueryHeader, frame) == 20);
	CHECK(offsetof(GiLightQuery::QueryHeader, flags) == 24);
	CHECK(offsetof(GiLightQuery::QueryHeader, sun_mask) == 28);
}

TEST_CASE("[GiLightQuery] Validation takes 7 floats and one unique slot per query, at most 32") {
	CHECK(GiLightQuery::validate(make_queries(1), make_slots(1)) == GiLightQuery::VALID);
	CHECK(GiLightQuery::validate(make_queries(32), make_slots(32)) == GiLightQuery::VALID);
	CHECK(GiLightQuery::validate(PackedFloat32Array(), PackedInt32Array()) == GiLightQuery::VALID);

	PackedFloat32Array short_queries = make_queries(2);
	short_queries.resize(13);
	CHECK(GiLightQuery::validate(short_queries, make_slots(2)) == GiLightQuery::INVALID_FLOAT_COUNT);
	CHECK(GiLightQuery::validate(make_queries(33), make_slots(33)) == GiLightQuery::INVALID_QUERY_COUNT);
	CHECK(GiLightQuery::validate(make_queries(3), make_slots(2)) == GiLightQuery::INVALID_SLOT_COUNT);

	PackedInt32Array slots = make_slots(2);
	slots.set(1, 32);
	CHECK(GiLightQuery::validate(make_queries(2), slots) == GiLightQuery::INVALID_SLOT_RANGE);
	slots.set(1, -1);
	CHECK(GiLightQuery::validate(make_queries(2), slots) == GiLightQuery::INVALID_SLOT_RANGE);
	slots.set(1, 0);
	CHECK(GiLightQuery::validate(make_queries(2), slots) == GiLightQuery::INVALID_SLOT_REPEATED);
	slots.set(1, 31);
	CHECK(GiLightQuery::validate(make_queries(2), slots) == GiLightQuery::VALID);

	PackedFloat32Array bad = make_queries(1);
	bad.set(2, Math::NaN);
	CHECK(GiLightQuery::validate(bad, make_slots(1)) == GiLightQuery::INVALID_VALUE);
	bad = make_queries(1);
	bad.set(6, Math::INF);
	CHECK(GiLightQuery::validate(bad, make_slots(1)) == GiLightQuery::INVALID_VALUE);
	bad = make_queries(1);
	bad.set(5, 0.0f);
	CHECK(GiLightQuery::validate(bad, make_slots(1)) == GiLightQuery::INVALID_FACING);

	for (int v = GiLightQuery::INVALID_FLOAT_COUNT; v <= GiLightQuery::INVALID_FACING; v++) {
		CHECK(String(GiLightQuery::validation_text(GiLightQuery::Validation(v))).length() > 0);
	}
}

TEST_CASE("[GiLightQuery] Packing keeps the slot as an integer, normalizes the facing and clamps the sun factor") {
	PackedFloat32Array queries = make_queries(3);
	queries.set(6, 1.5f);
	queries.set(13, -0.25f);
	queries.set(20, 0.4f);
	PackedInt32Array slots;
	slots.push_back(31);
	slots.push_back(0);
	slots.push_back(17);
	LocalVector<GiLightQuery::Query> packed;
	GiLightQuery::pack(queries, slots, packed);
	REQUIRE(packed.size() == 3);
	CHECK(packed[0].slot == 31);
	CHECK(packed[1].slot == 0);
	CHECK(packed[2].slot == 17);
	CHECK(packed[2].position[0] == 2.0f);
	CHECK(packed[2].position[1] == 1.5f);
	CHECK(packed[2].position[2] == -2.0f);
	CHECK(packed[0].facing[0] == 0.0f);
	CHECK(packed[0].facing[1] == 0.0f);
	CHECK(packed[0].facing[2] == doctest::Approx(1.0f));
	CHECK(packed[0].sun_factor == 1.0f);
	CHECK(packed[1].sun_factor == 0.0f);
	CHECK(packed[2].sun_factor == doctest::Approx(0.4f));
}

TEST_CASE("[GiLightQuery] Requests are found by the viewport's render target and an empty list stops them") {
	GiLightQuery table;
	const RID viewport = RID::from_uint64(11);
	const RID target = RID::from_uint64(12);
	CHECK_FALSE(table.is_active());
	table.set_requests(viewport, target, requests_for({ 3, 5 }));
	CHECK(table.is_active());

	RID found;
	uint64_t generation = 0;
	LocalVector<GiLightQuery::Query> queries;
	CHECK_FALSE(table.take_requests(RID::from_uint64(99), 1, found, generation, queries));
	REQUIRE(table.take_requests(target, 1, found, generation, queries));
	CHECK(found == viewport);
	CHECK(generation > 0);
	CHECK(queries.size() == 2);
	CHECK(queries[1].slot == 5);

	table.set_requests(viewport, target, LocalVector<GiLightQuery::Query>());
	CHECK_FALSE(table.is_active());
	CHECK_FALSE(table.take_requests(target, 2, found, generation, queries));

	table.set_requests(viewport, target, requests_for({ 1 }));
	CHECK(table.is_active());
	table.forget(viewport);
	CHECK_FALSE(table.is_active());
	CHECK(table.get_results(viewport, 0, 0).is_empty());
}

TEST_CASE("[GiLightQuery] Each slot keeps its newest result, also across a dropped batch and a late one") {
	GiLightQuery table;
	const RID viewport = RID::from_uint64(21);
	const RID target = RID::from_uint64(22);
	table.set_requests(viewport, target, requests_for({ 0, 7 }));
	RID found;
	uint64_t generation = 0;
	LocalVector<GiLightQuery::Query> queries;
	REQUIRE(table.take_requests(target, 10, found, generation, queries));
	CHECK(table.get_results(viewport, 0, 0).is_empty());

	land(table, viewport, generation, 1000, { result_for(0, 10, 1.0f), result_for(7, 10, 2.0f) });
	REQUIRE(table.take_requests(target, 11, found, generation, queries));
	REQUIRE(table.take_requests(target, 12, found, generation, queries));
	land(table, viewport, generation, 3000, { result_for(7, 12, 20.0f), result_for(0, 12, 10.0f) });
	land(table, viewport, generation, 2000, { result_for(0, 11, 5.0f), result_for(7, 11, 6.0f) });

	const Array rows = table.get_results(viewport, 23000, 12);
	REQUIRE(rows.size() == 2);
	const Array zero = row_for_slot(rows, 0);
	const Array seven = row_for_slot(rows, 7);
	REQUIRE(zero.size() == int(GiLightQuery::RESULT_FIELDS));
	REQUIRE(seven.size() == int(GiLightQuery::RESULT_FIELDS));
	CHECK(double(zero[GiLightQuery::FIELD_R]) == doctest::Approx(10.0));
	CHECK(double(zero[GiLightQuery::FIELD_G]) == doctest::Approx(20.0));
	CHECK(double(zero[GiLightQuery::FIELD_B]) == doctest::Approx(30.0));
	CHECK(double(zero[GiLightQuery::FIELD_Y]) == doctest::Approx(40.0));
	CHECK(double(zero[GiLightQuery::FIELD_COVERAGE]) == doctest::Approx(0.5));
	CHECK(double(zero[GiLightQuery::FIELD_BODY_Y]) == doctest::Approx(2.5));
	CHECK(int64_t(zero[GiLightQuery::FIELD_FRAME]) == 12);
	CHECK(int64_t(zero[GiLightQuery::FIELD_FLAGS]) == GiLightQuery::FLAG_SURFELS_ON);
	CHECK(double(zero[GiLightQuery::FIELD_AGE_MSEC]) == doctest::Approx(20.0));
	CHECK(int64_t(zero[GiLightQuery::FIELD_AGE_FRAMES]) == 0);
	CHECK(double(seven[GiLightQuery::FIELD_R]) == doctest::Approx(20.0));
	CHECK(int64_t(seven[GiLightQuery::FIELD_FRAME]) == 12);

	REQUIRE(table.take_requests(target, 13, found, generation, queries));
	REQUIRE(table.take_requests(target, 14, found, generation, queries));
	const Array later = table.get_results(viewport, 3000, 14);
	CHECK(int64_t(row_for_slot(later, 7)[GiLightQuery::FIELD_AGE_FRAMES]) == 2);
	CHECK(double(row_for_slot(later, 7)[GiLightQuery::FIELD_AGE_MSEC]) == doctest::Approx(0.0));
}

TEST_CASE("[GiLightQuery] Frame age follows the live frame also while nothing is dispatched") {
	GiLightQuery table;
	const RID viewport = RID::from_uint64(23);
	const RID target = RID::from_uint64(24);
	table.set_requests(viewport, target, requests_for({ 4 }));
	RID found;
	uint64_t generation = 0;
	LocalVector<GiLightQuery::Query> queries;
	REQUIRE(table.take_requests(target, 12, found, generation, queries));
	land(table, viewport, generation, 1000, { result_for(4, 12, 1.0f) });

	CHECK(int64_t(row_for_slot(table.get_results(viewport, 1000, 12), 4)[GiLightQuery::FIELD_AGE_FRAMES]) == 0);
	CHECK(int64_t(row_for_slot(table.get_results(viewport, 1000, 20), 4)[GiLightQuery::FIELD_AGE_FRAMES]) == 8);
	CHECK(int64_t(row_for_slot(table.get_results(viewport, 1000, 25), 4)[GiLightQuery::FIELD_AGE_FRAMES]) == 13);
	CHECK(int64_t(row_for_slot(table.get_results(viewport, 1000, 11), 4)[GiLightQuery::FIELD_AGE_FRAMES]) == 0);

	GiLightQuery wrapped;
	wrapped.set_requests(viewport, target, requests_for({ 4 }));
	REQUIRE(wrapped.take_requests(target, 0xFFFFFFFFu, found, generation, queries));
	land(wrapped, viewport, generation, 1000, { result_for(4, 0xFFFFFFFFu, 2.0f) });
	CHECK(int64_t(row_for_slot(wrapped.get_results(viewport, 1000, 1u), 4)[GiLightQuery::FIELD_AGE_FRAMES]) == 2);
}

TEST_CASE("[GiLightQuery] Thirty-two slots in shuffled order each read their own value") {
	GiLightQuery table;
	const RID viewport = RID::from_uint64(31);
	const RID target = RID::from_uint64(32);
	LocalVector<uint32_t> slots;
	for (uint32_t s = 0; s < GiLightQuery::MAX_QUERIES; s++) {
		slots.push_back((s * 13u + 5u) % GiLightQuery::MAX_QUERIES);
	}
	table.set_requests(viewport, target, requests_for(slots));
	RID found;
	uint64_t generation = 0;
	LocalVector<GiLightQuery::Query> queries;
	REQUIRE(table.take_requests(target, 40, found, generation, queries));
	CHECK(queries.size() == GiLightQuery::MAX_QUERIES);

	LocalVector<GiLightQuery::Result> results;
	for (uint32_t i = 0; i < GiLightQuery::MAX_QUERIES; i++) {
		const uint32_t slot = (i * 7u + 3u) % GiLightQuery::MAX_QUERIES;
		results.push_back(result_for(slot, 40, float(slot) + 0.5f));
	}
	land(table, viewport, generation, 0, results);

	const Array rows = table.get_results(viewport, 0, 0);
	REQUIRE(rows.size() == int(GiLightQuery::MAX_QUERIES));
	for (int i = 0; i < rows.size(); i++) {
		const Array row = rows[i];
		const int64_t slot = row[GiLightQuery::FIELD_SLOT];
		CHECK(slot == i);
		CHECK(double(row[GiLightQuery::FIELD_R]) == doctest::Approx(double(slot) + 0.5));
	}
}

TEST_CASE("[GiLightQuery] A slot that was dropped and asked again ignores batches from before") {
	GiLightQuery table;
	const RID viewport = RID::from_uint64(41);
	const RID target = RID::from_uint64(42);
	table.set_requests(viewport, target, requests_for({ 2, 4 }));
	RID found;
	uint64_t old_generation = 0;
	LocalVector<GiLightQuery::Query> queries;
	REQUIRE(table.take_requests(target, 50, found, old_generation, queries));

	table.set_requests(viewport, target, requests_for({ 4 }));
	land(table, viewport, old_generation, 0, { result_for(2, 50, 1.0f), result_for(4, 50, 2.0f) });
	Array rows = table.get_results(viewport, 0, 0);
	REQUIRE(rows.size() == 1);
	CHECK(int64_t(Array(rows[0])[GiLightQuery::FIELD_SLOT]) == 4);

	table.set_requests(viewport, target, requests_for({ 2, 4 }));
	land(table, viewport, old_generation, 0, { result_for(2, 51, 3.0f) });
	CHECK(row_for_slot(table.get_results(viewport, 0, 0), 2).is_empty());

	uint64_t new_generation = 0;
	REQUIRE(table.take_requests(target, 52, found, new_generation, queries));
	CHECK(new_generation > old_generation);
	land(table, viewport, new_generation, 0, { result_for(2, 52, 7.0f) });
	const Array two = row_for_slot(table.get_results(viewport, 0, 0), 2);
	REQUIRE_FALSE(two.is_empty());
	CHECK(double(two[GiLightQuery::FIELD_R]) == doctest::Approx(7.0));
}

TEST_CASE("[GiLightQuery] Readback data is read only in whole entries with slots in range") {
	GiLightQuery table;
	const RID viewport = RID::from_uint64(51);
	const RID target = RID::from_uint64(52);
	table.set_requests(viewport, target, requests_for({ 0, 1 }));
	RID found;
	uint64_t generation = 0;
	LocalVector<GiLightQuery::Query> queries;
	REQUIRE(table.take_requests(target, 60, found, generation, queries));

	LocalVector<GiLightQuery::Result> results;
	results.push_back(result_for(0, 60, 1.0f));
	results.push_back(result_for(1, 60, 2.0f));
	results.push_back(result_for(40, 60, 3.0f));
	table.store_results(viewport, generation, 0, reinterpret_cast<const uint8_t *>(results.ptr()), sizeof(GiLightQuery::Result) + 20);
	Array rows = table.get_results(viewport, 0, 0);
	REQUIRE(rows.size() == 1);
	CHECK(int64_t(Array(rows[0])[GiLightQuery::FIELD_SLOT]) == 0);

	land(table, viewport, generation, 0, results);
	rows = table.get_results(viewport, 0, 0);
	CHECK(rows.size() == 2);
	table.store_results(RID::from_uint64(999), generation, 0, reinterpret_cast<const uint8_t *>(results.ptr()), sizeof(GiLightQuery::Result));
	table.store_results(viewport, generation, 0, nullptr, 0);
	CHECK(table.get_results(viewport, 0, 0).size() == 2);
}

TEST_CASE("[GiLightQuery] Frames compare across the 32-bit wrap") {
	GiLightQuery table;
	const RID viewport = RID::from_uint64(61);
	const RID target = RID::from_uint64(62);
	table.set_requests(viewport, target, requests_for({ 9 }));
	RID found;
	uint64_t generation = 0;
	LocalVector<GiLightQuery::Query> queries;
	REQUIRE(table.take_requests(target, 1, found, generation, queries));
	land(table, viewport, generation, 0, { result_for(9, 0xFFFFFFFFu, 1.0f) });
	land(table, viewport, generation, 0, { result_for(9, 1u, 2.0f) });
	land(table, viewport, generation, 0, { result_for(9, 0xFFFFFFFEu, 3.0f) });
	const Array nine = row_for_slot(table.get_results(viewport, 0, 0), 9);
	REQUIRE_FALSE(nine.is_empty());
	CHECK(double(nine[GiLightQuery::FIELD_R]) == doctest::Approx(2.0));
}

TEST_CASE("[GiLightQuery] A frame without a dispatch drops the results and the batches still in flight") {
	GiLightQuery table;
	const RID viewport = RID::from_uint64(71);
	const RID target = RID::from_uint64(72);
	table.set_requests(viewport, target, requests_for({ 3 }));
	RID found;
	uint64_t generation = 0;
	LocalVector<GiLightQuery::Query> queries;

	REQUIRE(table.take_requests(target, 100, found, generation, queries));
	table.finish_frame(target, 100);
	land(table, viewport, generation, 0, { result_for(3, 100, 1.0f) });
	REQUIRE(table.get_results(viewport, 0, 0).size() == 1);

	REQUIRE(table.take_requests(target, 101, found, generation, queries));
	table.finish_frame(target, 101);
	table.finish_frame(RID::from_uint64(999), 102);
	CHECK(table.get_results(viewport, 0, 0).size() == 1);
	table.finish_frame(target, 102);
	CHECK(table.get_results(viewport, 0, 0).is_empty());
	land(table, viewport, generation, 0, { result_for(3, 101, 2.0f) });
	CHECK(table.get_results(viewport, 0, 0).is_empty());

	REQUIRE(table.take_requests(target, 103, found, generation, queries));
	land(table, viewport, generation, 0, { result_for(3, 103, 3.0f) });
	const Array three = row_for_slot(table.get_results(viewport, 0, 0), 3);
	REQUIRE_FALSE(three.is_empty());
	CHECK(double(three[GiLightQuery::FIELD_R]) == doctest::Approx(3.0));

	REQUIRE(table.take_requests(target, 104, found, generation, queries));
	table.skip(viewport, 104);
	CHECK(table.get_results(viewport, 0, 0).is_empty());
	land(table, viewport, generation, 0, { result_for(3, 103, 4.0f) });
	CHECK(table.get_results(viewport, 0, 0).is_empty());
	land(table, viewport, generation, 0, { result_for(3, 105, 5.0f) });
	CHECK(table.get_results(viewport, 0, 0).size() == 1);
}

TEST_CASE("[GiLightQuery] The shader gets the same flag values as the C++ side") {
	const String defines = GiLightQuery::shader_defines();
	CHECK(defines.contains(vformat("#define GI_LIGHT_QUERY_FLAG_TRIMMED %du\n", int64_t(GiLightQuery::FLAG_TRIMMED))));
	CHECK(defines.contains(vformat("#define GI_LIGHT_QUERY_FLAG_PARTIAL_CLOUD %du\n", int64_t(GiLightQuery::FLAG_PARTIAL_CLOUD))));
	CHECK(defines.contains(vformat("#define GI_LIGHT_QUERY_FLAG_SURFELS_ON %du\n", int64_t(GiLightQuery::FLAG_SURFELS_ON))));
	CHECK(defines.contains(vformat("#define GI_LIGHT_QUERY_FLAG_NO_COVERAGE %du\n", int64_t(GiLightQuery::FLAG_NO_COVERAGE))));
	CHECK(defines.contains("#define GI_LIGHT_QUERY_THREADS 64\n"));
	CHECK(defines.contains("#define GI_LIGHT_QUERY_AREA_SAMPLES 8u\n"));
	CHECK(defines.contains("#define GI_LIGHT_QUERY_POINT_FLOOR 0.1000\n"));
}

} // namespace TestGiLightQuery
