/**************************************************************************/
/*  test_restir_fold_lineage.h                                            */
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

#include "../restir_fold_mirror.h"
#include "../restir_test.h"

#include "tests/test_macros.h"

namespace TestRestirFoldLineage {

static int64_t family_count(const HashMap<int, int64_t> &p_sizes, int p_size) {
	const int64_t *count = p_sizes.getptr(p_size);
	return count ? *count : 0;
}

TEST_CASE("[RestirFoldLineage] Bindings and lineage ids") {
	CHECK(RESTIR_FOLD_LINEAGE_IN_BINDING == 33u);
	CHECK(RESTIR_FOLD_LINEAGE_OUT_BINDING == 34u);
	CHECK(RESTIR_FOLD_REFUSED_ID_BINDING == 35u);
	CHECK(restir_fold_lineage_pixel(0u, 0u, 640u) == 1u);
	CHECK(restir_fold_lineage_pixel(3u, 2u, 640u) == 1284u);
	CHECK(restir_fold_lineage_pixel(639u, 359u, 640u) == 230400u);
	CHECK(restir_fold_lineage_id(1u, 0u) == 1u);
	CHECK(restir_fold_lineage_id(5u, 0x1FFu) == 0xFF000005u);
	CHECK(restir_fold_lineage_id(230400u, 1007u) == (((1007u & 0xFFu) << 24u) | 230400u));
	CHECK(restir_fold_lineage_id(0u, 7u) == RESTIR_FOLD_LINEAGE_UNKNOWN);
	CHECK(restir_fold_lineage_id(RESTIR_FOLD_LINEAGE_PIXEL_MASK + 1u, 7u) == RESTIR_FOLD_LINEAGE_UNKNOWN);
	CHECK(restir_fold_lineage_id(RESTIR_FOLD_LINEAGE_PIXEL_MASK, 0u) == RESTIR_FOLD_LINEAGE_PIXEL_MASK);
	CHECK((RESTIR_FOLD_LINEAGE_UNKNOWN & RESTIR_FOLD_LINEAGE_PIXEL_MASK) == 0u);
	CHECK(uint64_t(640u) * uint64_t(360u) <= RESTIR_FOLD_LINEAGE_MAX_TEXELS);
	CHECK(uint64_t(4096u) * uint64_t(4096u) > RESTIR_FOLD_LINEAGE_MAX_TEXELS);
}

TEST_CASE("[RestirFoldLineage] Families of a known id list") {
	const uint32_t ids[] = { 5u, 5u, 5u, 7u, 0u, 7u, 9u };
	HashMap<int, int64_t> sizes;
	const RestirFoldFamilyCount count = restir_fold_families(ids, 7, sizes);
	CHECK(count.refusals == 6);
	CHECK(count.unknown == 0);
	CHECK(count.families == 3);
	CHECK(sizes.size() == 3u);
	CHECK(family_count(sizes, 3) == 1);
	CHECK(family_count(sizes, 2) == 1);
	CHECK(family_count(sizes, 1) == 1);
	const RestirFoldFamilyCount again = restir_fold_families(ids, 7, sizes);
	CHECK(again.families == 3);
	CHECK(family_count(sizes, 3) == 2);
	CHECK(family_count(sizes, 2) == 2);
	CHECK(family_count(sizes, 1) == 2);
	CHECK(family_count(sizes, 4) == 0);
}

TEST_CASE("[RestirFoldLineage] Unknown lineages and empty input") {
	const uint32_t ids[] = { RESTIR_FOLD_LINEAGE_UNKNOWN, 0x05000000u, 4u, 0u, 4u, 0u };
	HashMap<int, int64_t> sizes;
	const RestirFoldFamilyCount count = restir_fold_families(ids, 6, sizes);
	CHECK(count.refusals == 4);
	CHECK(count.unknown == 2);
	CHECK(count.families == 1);
	CHECK(sizes.size() == 1u);
	CHECK(family_count(sizes, 2) == 1);
	const RestirFoldFamilyCount none = restir_fold_families(nullptr, 6, sizes);
	CHECK(none.refusals == 0);
	CHECK(none.families == 0);
	const uint32_t zeros[] = { 0u, 0u, 0u };
	const RestirFoldFamilyCount quiet = restir_fold_families(zeros, 3, sizes);
	CHECK(quiet.refusals == 0);
	CHECK(quiet.families == 0);
	CHECK(sizes.size() == 1u);
}

TEST_CASE("[RestirFoldLineage] Ids are distinct over a refusal window") {
	constexpr uint32_t width = 64u;
	constexpr uint32_t height = 36u;
	constexpr uint32_t clocks = 66u;
	constexpr uint32_t first_clock = 250u;
	Vector<uint32_t> ids;
	ids.resize(int64_t(width * height * clocks));
	uint32_t *w = ids.ptrw();
	uint32_t i = 0u;
	for (uint32_t c = 0u; c < clocks; c++) {
		for (uint32_t y = 0u; y < height; y++) {
			for (uint32_t x = 0u; x < width; x++) {
				w[i++] = restir_fold_lineage_id(restir_fold_lineage_pixel(x, y, width), first_clock + c);
			}
		}
	}
	HashMap<int, int64_t> sizes;
	const RestirFoldFamilyCount count = restir_fold_families(ids.ptr(), ids.size(), sizes);
	CHECK(count.refusals == int64_t(width * height * clocks));
	CHECK(count.unknown == 0);
	CHECK(count.families == count.refusals);
	CHECK(sizes.size() == 1u);
	CHECK(family_count(sizes, 1) == count.refusals);
}

TEST_CASE("[RestirFoldLineage] Family counts accumulate across frames") {
	restir_fold_families_reset();
	const uint32_t frame[] = { 5u, 5u, 5u, 7u, 0u, 7u, 9u, RESTIR_FOLD_LINEAGE_UNKNOWN };
	const RestirFoldFamilyCount first = restir_fold_families_add(0, frame, 8);
	CHECK(first.refusals == 7);
	CHECK(first.unknown == 1);
	CHECK(first.families == 3);
	restir_fold_families_add(0, frame, 8);
	const RestirFoldFamilyCount none = restir_fold_families_add(0, nullptr, 0);
	CHECK(none.refusals == 0);
	CHECK(none.families == 0);
	const RestirFoldFamilyTotals totals = restir_fold_family_totals(0);
	CHECK(totals.frames == 3u);
	CHECK(totals.refusals == 14);
	CHECK(totals.unknown == 2);
	CHECK(totals.families == 6);
	const Vector<int64_t> pairs = restir_fold_family_pairs(0);
	const int64_t expected[] = { 1, 2, 2, 2, 3, 2 };
	REQUIRE(pairs.size() == 6);
	for (int i = 0; i < 6; i++) {
		CHECK(pairs[i] == expected[i]);
	}
	restir_fold_families_reset();
}

TEST_CASE("[RestirFoldLineage] Family counts are kept per fold tag") {
	restir_fold_families_reset();
	const uint32_t tag1[] = { 5u, 5u, 7u };
	const uint32_t tag2[] = { 9u, 9u, 9u };
	restir_fold_families_add(1, tag1, 3);
	restir_fold_families_add(2, tag2, 3);
	const RestirFoldFamilyTotals one = restir_fold_family_totals(1);
	CHECK(one.frames == 1u);
	CHECK(one.refusals == 3);
	CHECK(one.unknown == 0);
	CHECK(one.families == 2);
	const Vector<int64_t> one_pairs = restir_fold_family_pairs(1);
	REQUIRE(one_pairs.size() == 4);
	CHECK(one_pairs[0] == 1);
	CHECK(one_pairs[1] == 1);
	CHECK(one_pairs[2] == 2);
	CHECK(one_pairs[3] == 1);
	const RestirFoldFamilyTotals two = restir_fold_family_totals(2);
	CHECK(two.frames == 1u);
	CHECK(two.refusals == 3);
	CHECK(two.unknown == 0);
	CHECK(two.families == 1);
	const Vector<int64_t> two_pairs = restir_fold_family_pairs(2);
	REQUIRE(two_pairs.size() == 2);
	CHECK(two_pairs[0] == 3);
	CHECK(two_pairs[1] == 1);
	const RestirFoldFamilyTotals other = restir_fold_family_totals(3);
	CHECK(other.frames == 0u);
	CHECK(other.refusals == 0);
	CHECK(other.unknown == 0);
	CHECK(other.families == 0);
	CHECK(restir_fold_family_pairs(3).is_empty());
	restir_fold_families_reset();
	CHECK(restir_fold_family_totals(1).frames == 0u);
	CHECK(restir_fold_family_totals(2).frames == 0u);
	CHECK(restir_fold_family_pairs(1).is_empty());
	CHECK(restir_fold_family_pairs(2).is_empty());
}

}
