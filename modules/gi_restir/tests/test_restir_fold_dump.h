/**************************************************************************/
/*  test_restir_fold_dump.h                                               */
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

namespace TestRestirFoldDump {

constexpr uint32_t DUMP_KS[] = { 4u, 8u, 16u, 32u, 64u };

TEST_CASE("[RestirFoldDump] Start classes and histogram index") {
	CHECK(RESTIR_FOLD_START_CUT == 0u);
	CHECK(RESTIR_FOLD_START_FRAME == 1u);
	CHECK(RESTIR_FOLD_START_PLANE == 2u);
	CHECK(RESTIR_FOLD_START_NORMAL == 3u);
	CHECK(RESTIR_FOLD_START_CLASS == 4u);
	CHECK(RESTIR_FOLD_START_NO_SAMPLE == 5u);
	CHECK(RESTIR_FOLD_START_VIS == 6u);
	CHECK(RESTIR_FOLD_START_OTHER == 7u);
	CHECK(RESTIR_FOLD_START_PRE == 8u);
	CHECK(RESTIR_FOLD_START_CLASSES == 16u);
	CHECK(RESTIR_FOLD_REJ_CODES == 16u);
	const uint32_t expected[16] = { 7u, 1u, 2u, 3u, 4u, 7u, 7u, 7u, 7u, 0u, 5u, 6u, 7u, 7u, 7u, 7u };
	for (uint32_t reason = 0u; reason < RESTIR_FOLD_REJ_CODES; reason++) {
		CHECK(restir_fold_start_class(reason, false) == expected[reason]);
		CHECK(restir_fold_start_class(reason, true) == expected[reason] + 8u);
	}
	CHECK(restir_fold_start_class(RESTIR_FOLD_REJ_CUT_CODE, false) == 0u);
	CHECK(restir_fold_start_class(RESTIR_FOLD_REJ_FRAME_CODE, false) == 1u);
	CHECK(restir_fold_start_class(RESTIR_FOLD_REJ_PLANE_CODE, false) == 2u);
	CHECK(restir_fold_start_class(RESTIR_FOLD_REJ_NORMAL_CODE, false) == 3u);
	CHECK(restir_fold_start_class(RESTIR_FOLD_REJ_CLASS_CODE, false) == 4u);
	CHECK(restir_fold_start_class(RESTIR_FOLD_REJ_NO_SAMPLE_CODE, false) == 5u);
	CHECK(restir_fold_start_class(RESTIR_FOLD_REJ_VIS_CODE, false) == 6u);
	CHECK(restir_fold_start_class(RESTIR_FOLD_REJ_CUT_CODE, true) == 8u);
	CHECK(restir_fold_start_class(RESTIR_FOLD_REJ_VIS_CODE, true) == 14u);
	CHECK(restir_fold_start_class(15u, true) == 15u);
	CHECK(restir_fold_start_class(99u, false) == 7u);
	CHECK(restir_fold_start_index(0u, 0u, 0u) == 0u);
	CHECK(restir_fold_start_index(0u, 0u, 1u) == 1u);
	CHECK(restir_fold_start_index(0u, 1u, 0u) == 64u);
	CHECK(restir_fold_start_index(1u, 0u, 0u) == 4096u);
	CHECK(restir_fold_start_index(15u, 63u, 63u) == 65535u);
	CHECK(RESTIR_FOLD_START_COUNT == 65536u);
	CHECK(RESTIR_FOLD_DUMP_BYTES == 262160u);
	CHECK(RESTIR_FOLD_DUMP_BINDING == 32u);
	CHECK(RESTIR_FOLD_SLOT_HP_EQ_PX == 27u);
	CHECK(RESTIR_FOLD_SLOT_PHASE0 == 28u);
	CHECK(RESTIR_FOLD_SLOT_PHASE0_START == 29u);
	CHECK(RESTIR_FOLD_SLOT_REFUSED_OFF == 30u);
}

TEST_CASE("[RestirFoldDump] Start sums fold each pre-frame class into its base class") {
	Vector<uint32_t> frame;
	frame.resize(int64_t(RESTIR_FOLD_START_COUNT));
	frame.fill(0u);
	frame.set(int64_t(restir_fold_start_index(RESTIR_FOLD_START_CUT, 0u, 0u)), 3u);
	frame.set(int64_t(restir_fold_start_index(RESTIR_FOLD_START_CUT, 63u, 63u)), 4u);
	frame.set(int64_t(restir_fold_start_index(RESTIR_FOLD_START_CUT + RESTIR_FOLD_START_PRE, 5u, 2u)), 10u);
	frame.set(int64_t(restir_fold_start_index(RESTIR_FOLD_START_CLASS, 1u, 1u)), 7u);
	frame.set(int64_t(restir_fold_start_index(RESTIR_FOLD_START_VIS + RESTIR_FOLD_START_PRE, 0u, 9u)), 2u);
	frame.set(int64_t(restir_fold_start_index(RESTIR_FOLD_START_OTHER, 0u, 0u)), 1u);
	frame.set(int64_t(restir_fold_start_index(RESTIR_FOLD_START_OTHER + RESTIR_FOLD_START_PRE, 63u, 63u)), 6u);
	int64_t sums[RESTIR_FOLD_START_PRE];
	REQUIRE(restir_fold_start_sums(frame, sums));
	const int64_t expected[8] = { 17, 0, 0, 0, 7, 0, 2, 7 };
	int64_t total = 0;
	for (uint32_t c = 0u; c < RESTIR_FOLD_START_PRE; c++) {
		CHECK(sums[c] == expected[c]);
		total += sums[c];
	}
	CHECK(total == 33);
	Vector<uint32_t> short_frame;
	short_frame.resize(int64_t(RESTIR_FOLD_START_COUNT) - 1);
	short_frame.fill(1u);
	CHECK_FALSE(restir_fold_start_sums(short_frame, sums));
	for (uint32_t c = 0u; c < RESTIR_FOLD_START_PRE; c++) {
		CHECK(sums[c] == -1);
	}
}

TEST_CASE("[RestirFoldDump] Self-test compare finds a planted difference") {
	RestirFoldTestArgs args;
	args.sub = RESTIR_FOLD_TEST_DRAW;
	args.k = 16u;
	args.count = 192u;
	Vector<uint32_t> a;
	Vector<uint32_t> b;
	restir_fold_test_mirror(args, a);
	restir_fold_test_mirror(args, b);
	REQUIRE(a.size() == 192);
	int64_t first = -2;
	CHECK(restir_fold_test_compare(a, b, first) == 0);
	CHECK(first == -1);
	b.set(77, b[77] + 1u);
	CHECK(restir_fold_test_compare(a, b, first) == 1);
	CHECK(first == 77);
	b.set(150, b[150] ^ 0x10000u);
	CHECK(restir_fold_test_compare(a, b, first) == 2);
	CHECK(first == 77);
	Vector<uint32_t> c = a;
	c.resize(190);
	CHECK(restir_fold_test_compare(a, c, first) == 2);
	CHECK(first == 190);
	Vector<uint32_t> empty;
	CHECK(restir_fold_test_compare(empty, empty, first) == 0);
	CHECK(first == -1);
}

TEST_CASE("[RestirFoldDump] Self-test draw weights over one period") {
	for (const uint32_t k : DUMP_KS) {
		RestirFoldTestArgs args;
		args.sub = RESTIR_FOLD_TEST_DRAW;
		args.k = k;
		args.count = 3u * ((k * k) >> 2u);
		Vector<uint32_t> words;
		restir_fold_test_mirror(args, words);
		REQUIRE(words.size() == int64_t(args.count));
		Vector<uint32_t> weights;
		weights.resize(k);
		weights.fill(0u);
		bool in_range = true;
		for (int64_t i = 0; i < words.size(); i++) {
			if (words[i] >= k) {
				in_range = false;
				continue;
			}
			weights.set(words[i], weights[words[i]] + 1u);
		}
		CHECK(in_range);
		CHECK(weights[0] == 1u);
		for (uint32_t s = 0u; s < k; s++) {
			const uint32_t expected = s < (k >> 1u) ? 2u * s + 1u : k;
			CHECK(weights[s] == expected);
		}
	}
}

TEST_CASE("[RestirFoldDump] Self-test isqrt sub is exact over the whole range") {
	RestirFoldTestArgs args;
	args.sub = RESTIR_FOLD_TEST_ISQRT;
	args.k = 4u;
	args.count = uint32_t(RESTIR_FOLD_TEST_MAX_THREADS);
	CHECK(restir_fold_test_invalid(args) == nullptr);
	Vector<uint32_t> words;
	restir_fold_test_mirror(args, words);
	REQUIRE(words.size() == 2);
	CHECK(words[0] == 0u);
	CHECK(words[1] == 0xFFFFFFFFu);
	Vector<uint32_t> initial;
	restir_fold_test_initial(args, initial);
	int64_t first = -2;
	CHECK(restir_fold_test_compare(initial, words, first) == 0);
}

TEST_CASE("[RestirFoldDump] Self-test phi counts and psi packing") {
	RestirFoldTestArgs phi;
	phi.sub = RESTIR_FOLD_TEST_PHI;
	phi.k = 8u;
	phi.grid_w = 37u;
	phi.grid_h = 11u;
	Vector<uint32_t> counts;
	restir_fold_test_mirror(phi, counts);
	REQUIRE(counts.size() == 8);
	uint32_t total = 0u;
	for (int64_t i = 0; i < counts.size(); i++) {
		total += counts[i];
	}
	CHECK(total == 37u * 11u);
	CHECK(counts[restir_fold_phi(0u, 0u) % 8u] > 0u);

	RestirFoldTestArgs psi;
	psi.sub = RESTIR_FOLD_TEST_PSI;
	psi.k = 32u;
	psi.count = 5u;
	psi.v0 = 1000u;
	psi.grid_w = 40u;
	psi.grid_h = 3u;
	psi.stride = 29u;
	psi.frames = 3u;
	Vector<uint32_t> packed;
	restir_fold_test_mirror(psi, packed);
	REQUIRE(packed.size() == 15);
	const uint32_t t = 3u * 4u + 2u;
	const uint32_t x = (4u * 29u) % 40u;
	const uint32_t y = (4u * 29u) / 40u;
	const uint32_t v = 1002u;
	const uint32_t expected = (restir_fold_phi(x, y) % 32u) | ((restir_fold_rng_state(x, y, v, RESTIR_FOLD_DIM_RENEW, RESTIR_FOLD_C2) % 32u) << 8u) | (restir_fold_draw(restir_fold_rng_state(x, y, v, RESTIR_FOLD_DIM_START, RESTIR_FOLD_C2), 32u) << 16u);
	CHECK(packed[t] == expected);
	for (int64_t i = 0; i < packed.size(); i++) {
		CHECK((packed[i] & 0xFFu) < 32u);
		CHECK(((packed[i] >> 8u) & 0xFFu) < 32u);
		CHECK((packed[i] >> 16u) < 32u);
	}
}

TEST_CASE("[RestirFoldDump] Self-test arguments are checked") {
	RestirFoldTestArgs args;
	args.count = 16u;
	CHECK(restir_fold_test_invalid(args) == nullptr);
	args.k = 12u;
	CHECK(restir_fold_test_invalid(args) != nullptr);
	args.k = 2u;
	CHECK(restir_fold_test_invalid(args) != nullptr);
	args.k = 128u;
	CHECK(restir_fold_test_invalid(args) != nullptr);
	args.k = 8u;
	args.sub = RESTIR_FOLD_TEST_MAX;
	CHECK(restir_fold_test_invalid(args) != nullptr);
	args.sub = RESTIR_FOLD_TEST_DRAW;
	args.count = 0u;
	CHECK(restir_fold_test_invalid(args) != nullptr);
	Vector<uint32_t> words;
	words.push_back(5u);
	restir_fold_test_mirror(args, words);
	CHECK(words.is_empty());

	RestirFoldTestArgs phi;
	phi.sub = RESTIR_FOLD_TEST_PHI;
	CHECK(restir_fold_test_invalid(phi) != nullptr);
	phi.grid_w = 4096u;
	phi.grid_h = 4096u;
	CHECK(restir_fold_test_invalid(phi) == nullptr);
	phi.grid_h = 4097u;
	CHECK(restir_fold_test_invalid(phi) != nullptr);

	RestirFoldTestArgs psi;
	psi.sub = RESTIR_FOLD_TEST_PSI;
	psi.count = 4u;
	psi.grid_w = 10u;
	psi.grid_h = 1u;
	psi.stride = 3u;
	CHECK(restir_fold_test_invalid(psi) == nullptr);
	psi.stride = 4u;
	CHECK(restir_fold_test_invalid(psi) != nullptr);
	psi.stride = 3u;
	psi.frames = 0u;
	CHECK(restir_fold_test_invalid(psi) != nullptr);
	psi.frames = 1u;
	psi.count = 1u;
	psi.stride = 0u;
	CHECK(restir_fold_test_invalid(psi) != nullptr);

	RestirFoldTestArgs big;
	big.sub = RESTIR_FOLD_TEST_DRAW;
	big.count = uint32_t(RESTIR_FOLD_TEST_MAX_WORDS) + 1u;
	CHECK(restir_fold_test_invalid(big) != nullptr);
}

TEST_CASE("[RestirFoldDump] Cut-frame start mirror") {
	constexpr int64_t width = 5;
	constexpr int64_t height = 4;
	constexpr uint32_t clock = 1007u;
	Vector<uint32_t> info;
	info.resize(width * height);
	info.fill(RESTIR_FOLD_INFO_VALID | RESTIR_FOLD_REJ_CUT_CODE);
	info.set(3, 0u);
	info.set(11, RESTIR_FOLD_REJ_CUT_CODE);
	for (uint32_t k : DUMP_KS) {
		for (int pre = 0; pre < 2; pre++) {
			Vector<uint32_t> starts;
			restir_fold_start_mirror(info.ptr(), info.size(), width, k, clock, pre == 1, starts);
			REQUIRE(starts.size() == int64_t(RESTIR_FOLD_START_COUNT));
			const uint32_t start_class = pre == 1 ? 8u : 0u;
			uint64_t total = 0u;
			uint64_t in_class = 0u;
			uint64_t past_cap = 0u;
			for (uint32_t c = 0u; c < RESTIR_FOLD_START_CLASSES; c++) {
				for (uint32_t s = 0u; s < RESTIR_FOLD_START_S; s++) {
					for (uint32_t u = 0u; u < RESTIR_FOLD_START_U; u++) {
						const uint32_t n = starts[restir_fold_start_index(c, s, u)];
						total += n;
						in_class += c == start_class ? n : 0u;
						past_cap += (s > k - 1u || u > k - 1u || (pre == 1 && s > k - 2u)) ? n : 0u;
					}
				}
			}
			CHECK(total == uint64_t(width * height - 2));
			CHECK(in_class == total);
			CHECK(past_cap == 0u);
			uint32_t s0 = restir_fold_draw(restir_fold_rng_state(2u, 1u, clock, RESTIR_FOLD_DIM_START, RESTIR_FOLD_C2), k);
			if (pre == 1) {
				s0 = MIN(s0, k - 2u);
			}
			const uint32_t u0 = (clock + restir_fold_phi(2u, 1u)) % k;
			CHECK(starts[restir_fold_start_index(start_class, s0, u0)] >= 1u);
		}
	}
	Vector<uint32_t> none;
	restir_fold_start_mirror(info.ptr(), info.size(), width, 6u, clock, false, none);
	uint64_t none_total = 0u;
	for (int64_t i = 0; i < none.size(); i++) {
		none_total += none[i];
	}
	CHECK(none_total == 0u);
}

TEST_CASE("[RestirFoldDump] Start counts accumulate across frames") {
	{
		MutexLock lock(restir_fold_starts_mutex());
		restir_fold_starts().clear();
		restir_fold_starts_frames() = 0;
	}
	Vector<uint32_t> frame;
	frame.resize(int64_t(RESTIR_FOLD_START_COUNT));
	frame.fill(0u);
	frame.set(int64_t(restir_fold_start_index(4u, 3u, 7u)), 5u);
	frame.set(int64_t(restir_fold_start_index(0u, 0u, 0u)), 2u);
	restir_fold_starts_add(frame);
	restir_fold_starts_add(frame);
	Vector<uint32_t> short_frame;
	short_frame.resize(10);
	short_frame.fill(9u);
	restir_fold_starts_add(short_frame);
	{
		MutexLock lock(restir_fold_starts_mutex());
		const LocalVector<uint64_t> &starts = restir_fold_starts();
		REQUIRE(starts.size() == RESTIR_FOLD_START_COUNT);
		uint64_t total = 0u;
		for (uint32_t i = 0; i < starts.size(); i++) {
			total += starts[i];
		}
		CHECK(total == 14u);
		CHECK(restir_fold_starts_frames() == 2u);
		CHECK(starts[restir_fold_start_index(4u, 3u, 7u)] == 10u);
		CHECK(starts[0] == 4u);
		restir_fold_starts().clear();
	}
}

}
