/**************************************************************************/
/*  test_restir_fold.h                                                    */
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

#include "../restir_epochs.h"
#include "../restir_fold.h"
#include "../restir_fold_mirror.h"
#include "../restir_harness.h"

#include "core/math/math_funcs.h"
#include "tests/test_macros.h"

namespace TestRestirFold {

constexpr uint32_t FOLD_C1 = 0x68E31DA4u;
constexpr uint32_t FOLD_C2 = 0x2C1B3C6Du;
constexpr uint32_t FOLD_BITS = 0xFF00u;
constexpr uint32_t FOLD_S_SHIFT = 8u;
constexpr uint32_t FOLD_S_MASK = 0x3Fu;
constexpr uint32_t FOLD_MV = 1u << 14u;
constexpr uint32_t FOLD_KS[] = { 4u, 8u, 16u, 32u, 64u };

bool mirror_refuses(uint32_t p_h_s, uint32_t p_k, bool p_pre_frame) {
	const uint32_t bound = p_pre_frame ? p_k - 1u : p_k;
	return p_h_s + 1u >= bound;
}

struct FoldPixel {
	bool had = false;
	bool cleared = false;
	uint32_t h_s = 0;
	bool h_mv = false;
	bool still = true;
	uint32_t v = 0;
	uint32_t phi = 0;
	uint32_t psi_r = 0;
	uint32_t psi_s = 0;
};

uint32_t mirror_fold_bits(const FoldPixel &p_pixel, uint32_t p_k, bool p_pre_frame) {
	const uint32_t half_k = p_k >> 1u;
	uint32_t s = 0;
	bool mv = false;
	if (p_pixel.had && !p_pixel.cleared) {
		s = p_pixel.h_s + 1u;
		mv = p_pixel.h_mv || !p_pixel.still;
	} else {
		if (p_pixel.cleared) {
			const uint32_t u = (!p_pixel.h_mv && p_pixel.still) ? (p_pixel.v + p_pixel.phi) % p_k : p_pixel.psi_r % p_k;
			s = u <= half_k ? u : u - half_k;
			mv = false;
		} else {
			s = restir_fold_draw(p_pixel.psi_s, p_k);
			mv = true;
		}
		if (p_pre_frame) {
			s = MIN(s, p_k - 2u);
		}
	}
	return ((s & FOLD_S_MASK) << FOLD_S_SHIFT) | (mv ? FOLD_MV : 0u);
}

uint32_t stored_s(uint32_t p_flags) {
	return (p_flags >> FOLD_S_SHIFT) & FOLD_S_MASK;
}

bool stored_mv(uint32_t p_flags) {
	return (p_flags & FOLD_MV) != 0u;
}

uint32_t draw_weight(uint32_t p_s, uint32_t p_k) {
	if (p_s == 0u) {
		return 1u;
	}
	if (p_s < p_k / 2u) {
		return 2u * p_s + 1u;
	}
	return p_k;
}

uint32_t ring_distance(uint32_t p_a, uint32_t p_b) {
	return MIN(p_a - p_b, p_b - p_a);
}

TEST_CASE("[RestirFold] K reads as a power of two from 4 to 64, rounded down, and 0 or less is off") {
	CHECK(restir_fold_k(0) == 0u);
	CHECK(restir_fold_k(-1) == 0u);
	CHECK(restir_fold_k(-64) == 0u);
	CHECK(restir_fold_k(1) == 4u);
	CHECK(restir_fold_k(3) == 4u);
	CHECK(restir_fold_k(4) == 4u);
	CHECK(restir_fold_k(7) == 4u);
	CHECK(restir_fold_k(8) == 8u);
	CHECK(restir_fold_k(15) == 8u);
	CHECK(restir_fold_k(16) == 16u);
	CHECK(restir_fold_k(31) == 16u);
	CHECK(restir_fold_k(32) == 32u);
	CHECK(restir_fold_k(63) == 32u);
	CHECK(restir_fold_k(64) == 64u);
	CHECK(restir_fold_k(65) == 64u);
	CHECK(restir_fold_k(1000000) == 64u);
	for (int64_t value = 1; value <= 200; value++) {
		const uint32_t k = restir_fold_k(value);
		CHECK(k >= RESTIR_FOLD_K_MIN);
		CHECK(k <= RESTIR_FOLD_K_MAX);
		CHECK((k & (k - 1u)) == 0u);
		CHECK(restir_fold_k(int64_t(k)) == k);
	}
}

TEST_CASE("[RestirFold] K_eff is 0 with spatial feedback on, outside the B modes or with K 0") {
	for (const uint32_t k : FOLD_KS) {
		CHECK(restir_fold_k_eff(k, true, false) == k);
		CHECK(restir_fold_k_eff(k, true, true) == 0u);
		CHECK(restir_fold_k_eff(k, false, false) == 0u);
		CHECK(restir_fold_k_eff(k, false, true) == 0u);
	}
	CHECK(restir_fold_k_eff(0u, true, false) == 0u);
	CHECK(restir_fold_k_eff(5u, true, false) == 4u);
}

TEST_CASE("[RestirFold] Pre-frame is the frame before each revalidation frame, with budget and rays, whatever the history") {
	bool matches = true;
	for (uint32_t period = 2u; period <= 16u; period++) {
		for (uint32_t frame = 0u; frame < 400u; frame++) {
			const bool reval = restir_reval_frame(frame, period);
			const bool reval_next = restir_reval_frame(frame + 1u, period);
			const bool pre_frame = restir_fold_pre_frame(true, true, frame, period);
			matches = matches && reval == (frame % period == period - 1u);
			matches = matches && reval_next == ((frame + 1u) % period == period - 1u);
			matches = matches && pre_frame == reval_next;
			matches = matches && pre_frame == (frame % period == period - 2u);
			matches = matches && !(pre_frame && reval);
			matches = matches && !restir_fold_pre_frame(false, true, frame, period);
			matches = matches && !restir_fold_pre_frame(true, false, frame, period);
		}
		const uint32_t last = 0xFFFFFFFFu;
		matches = matches && restir_fold_pre_frame(true, true, last, period) == (last % period == period - 2u);
		matches = matches && !restir_fold_pre_frame(true, true, last, period);
		matches = matches && restir_reval_frame(last, period) == (last % period == period - 1u);
	}
	CHECK(matches);
	CHECK_FALSE(restir_reval_frame(0u, 0u));
	CHECK_FALSE(restir_reval_frame(0u, 1u));
	CHECK_FALSE(restir_reval_frame(1u, 1u));
	CHECK(restir_reval_frame(2u, 3u));
	CHECK(restir_reval_frame(0xFFFFFFFFu, 16u));
	CHECK_FALSE(restir_fold_pre_frame(true, true, 0u, 1u));
	CHECK_FALSE(restir_fold_pre_frame(true, true, 0u, 0u));
	CHECK(restir_fold_pre_frame(true, true, 1u, 3u));
	CHECK(restir_fold_pre_frame(true, true, 4u, 3u));
	CHECK_FALSE(restir_fold_pre_frame(true, true, 2u, 3u));
	CHECK(restir_fold_pre_frame(true, true, 0xFFFFFFFEu, 16u));
	CHECK_FALSE(restir_fold_pre_frame(true, true, 0xFFFFFFFFu, 16u));
}

TEST_CASE("[RestirFold] The upload words: bit 19 only with K above 0 and a pre-frame, clock and K 0 at K 0, U1 and feedback force 0") {
	const uint32_t others = 0xFFFFFFFFu & ~RESTIR_FOLD_F_PRE_FRAME;
	bool masked = true;
	for (const uint32_t flags : { 0u, RESTIR_FOLD_F_PRE_FRAME, 0xFFFFFFFFu, 0x12345678u }) {
		for (const uint32_t k : { 0u, 1u, 4u, 7u, 32u, 64u, 1000u }) {
			for (uint32_t gate = 0u; gate < 8u; gate++) {
				const bool pre_frame = (gate & 1u) != 0u;
				const bool u1 = (gate & 2u) != 0u;
				const bool feedback = (gate & 4u) != 0u;
				const RestirFoldWords words = restir_fold_words(flags, k, 0xCAFEF00Du, pre_frame, u1, feedback);
				const uint32_t gated = (u1 || feedback) ? 0u : restir_fold_k(int64_t(k));
				masked = masked && words.k == gated;
				masked = masked && (words.flags & others) == (flags & others);
				masked = masked && ((words.flags & RESTIR_FOLD_F_PRE_FRAME) != 0u) == (gated != 0u && pre_frame);
				masked = masked && words.clock == (gated != 0u ? 0xCAFEF00Du : 0u);
			}
		}
	}
	CHECK(masked);

	const RestirFoldWords off = restir_fold_words(0xFFFFFFFFu, 0u, 1234u, true, false, false);
	CHECK(off.flags == others);
	CHECK(off.clock == 0u);
	CHECK(off.k == 0u);
	const RestirFoldWords seven = restir_fold_words(RESTIR_FOLD_F_PRE_FRAME, 7u, 1234u, false, false, false);
	CHECK(seven.k == 4u);
	CHECK(seven.clock == 1234u);
	CHECK(seven.flags == 0u);
	const RestirFoldWords pre = restir_fold_words(0u, 32u, 1234u, true, false, false);
	CHECK(pre.k == 32u);
	CHECK(pre.flags == RESTIR_FOLD_F_PRE_FRAME);
	const RestirFoldWords u1 = restir_fold_words(0u, 32u, 1234u, true, true, false);
	CHECK(u1.k == 0u);
	CHECK(u1.clock == 0u);
	CHECK(u1.flags == 0u);
	const RestirFoldWords feedback = restir_fold_words(0u, 32u, 1234u, true, false, true);
	CHECK(feedback.k == 0u);
	CHECK(feedback.clock == 0u);
	CHECK(feedback.flags == 0u);
}

TEST_CASE("[RestirFold] The harness dict carries clock, k_eff and pre_frame, and a dict without them reads 0, 0 and false") {
	RestirFrameParams params;
	params.fold_clock = 0xFFFFFFF0u;
	params.fold_k_eff = 32u;
	params.fold_pre_frame = true;
	const RestirFrameParams back = RestirHarness::params_from_dict(RestirHarness::params_to_dict(params));
	CHECK(back.fold_clock == 0xFFFFFFF0u);
	CHECK(back.fold_k_eff == 32u);
	CHECK(back.fold_pre_frame);
	const RestirFrameParams bare = RestirHarness::params_from_dict(Dictionary());
	CHECK(bare.fold_clock == 0u);
	CHECK(bare.fold_k_eff == 0u);
	CHECK_FALSE(bare.fold_pre_frame);
}

TEST_CASE("[RestirFold] A change of K_eff, or of mode or feedback while K is set, is a cut") {
	RestirFoldFrame off;
	CHECK_FALSE(restir_fold_cut(off, off));
	RestirFoldFrame mode_changed = off;
	mode_changed.mode = 2u;
	CHECK_FALSE(restir_fold_cut(off, mode_changed));
	RestirFoldFrame feedback_changed = off;
	feedback_changed.feedback = true;
	CHECK_FALSE(restir_fold_cut(off, feedback_changed));

	RestirFoldFrame on;
	on.k = 32u;
	on.k_eff = 32u;
	on.mode = 1u;
	CHECK_FALSE(restir_fold_cut(on, on));
	CHECK(restir_fold_cut(off, on));
	CHECK(restir_fold_cut(on, off));
	RestirFoldFrame k_changed = on;
	k_changed.k = 8u;
	k_changed.k_eff = 8u;
	CHECK(restir_fold_cut(on, k_changed));
	RestirFoldFrame b2k = on;
	b2k.mode = 3u;
	CHECK(restir_fold_cut(on, b2k));
	RestirFoldFrame feedback_on = on;
	feedback_on.feedback = true;
	feedback_on.k_eff = 0u;
	CHECK(restir_fold_cut(on, feedback_on));
	CHECK(restir_fold_cut(feedback_on, on));
	RestirFoldFrame feedback_k = feedback_on;
	feedback_k.k = 8u;
	CHECK_FALSE(restir_fold_cut(feedback_on, feedback_k));
	RestirFoldFrame feedback_mode = feedback_on;
	feedback_mode.mode = 0u;
	CHECK(restir_fold_cut(feedback_on, feedback_mode));
}

TEST_CASE("[RestirFold] The epoch planner files a Fold change as its own cut, after the request and backend cuts and before the origin and scene cuts") {
	RestirEpochInputs inputs;
	inputs.have_history = true;
	RestirEpochPlan plan;
	restir_plan_epochs(inputs, plan);
	CHECK_FALSE(plan.cut);
	inputs.fold_changed = true;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.cut);
	CHECK(plan.reason == RESTIR_CUT_FOLD);
	inputs.hardware_changed = true;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.reason == RESTIR_CUT_BACKEND);
	inputs.history_cut = true;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.reason == RESTIR_CUT_REQUEST);
	inputs.have_history = false;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.reason == RESTIR_CUT_FIRST);

	RestirEpochInputs moved;
	moved.have_history = true;
	moved.have_epochs = true;
	moved.current.origin = 1u;
	moved.current.scene = 1u;
	moved.fold_changed = true;
	restir_plan_epochs(moved, plan);
	CHECK(plan.cut);
	CHECK(plan.origin_changed);
	CHECK(plan.scene_changed);
	CHECK(plan.reason == RESTIR_CUT_FOLD);
	moved.fold_changed = false;
	restir_plan_epochs(moved, plan);
	CHECK(plan.reason == RESTIR_CUT_ORIGIN);
	moved.current.origin = 0u;
	moved.fold_changed = true;
	restir_plan_epochs(moved, plan);
	CHECK(plan.cut);
	CHECK_FALSE(plan.origin_changed);
	CHECK(plan.scene_changed);
	CHECK(plan.reason == RESTIR_CUT_FOLD);
	moved.fold_changed = false;
	restir_plan_epochs(moved, plan);
	CHECK(plan.reason == RESTIR_CUT_SCENE_NO_FEED);
}

TEST_CASE("[RestirFold] The epoch planner cuts only inside the dirty boxes when the feed localises a scene change") {
	using RendererRD::GIEpochs;
	RestirEpochInputs inputs;
	inputs.have_history = true;
	inputs.have_epochs = true;
	inputs.previous.scene = 4u;
	inputs.current.scene = 5u;
	inputs.anchor = Vector3(100, 0, -50);
	RestirEpochPlan plan;

	restir_plan_epochs(inputs, plan);
	CHECK(plan.cut);
	CHECK(plan.reason == RESTIR_CUT_SCENE_NO_FEED);
	CHECK(plan.dirty_count == 0u);

	const AABB two[2] = { AABB(Vector3(101, 2, -49), Vector3(1, 2, 3)), AABB(Vector3(90, 0, -60), Vector3(4, 4, 4)) };
	inputs.have_feed = true;
	inputs.dirty_result = GIEpochs::DIRTY_BOXES;
	inputs.dirty_boxes = two;
	inputs.dirty_box_count = 2u;
	restir_plan_epochs(inputs, plan);
	CHECK_FALSE(plan.cut);
	CHECK(plan.reason == RESTIR_CUT_NONE);
	CHECK(plan.scene_changed);
	REQUIRE(plan.dirty_count == 2u);
	CHECK(plan.dirty_min[0].is_equal_approx(Vector3(1, 2, 1)));
	CHECK(plan.dirty_max[0].is_equal_approx(Vector3(2, 4, 4)));
	CHECK(plan.dirty_min[1].is_equal_approx(Vector3(-10, 0, -10)));
	CHECK(plan.dirty_max[1].is_equal_approx(Vector3(-6, 4, -6)));

	inputs.dirty_result = GIEpochs::DIRTY_NONE;
	inputs.dirty_boxes = nullptr;
	inputs.dirty_box_count = 0u;
	restir_plan_epochs(inputs, plan);
	CHECK_FALSE(plan.cut);
	CHECK(plan.scene_changed);
	CHECK(plan.dirty_count == 0u);

	inputs.dirty_result = GIEpochs::DIRTY_OVERFLOW;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.cut);
	CHECK(plan.reason == RESTIR_CUT_SCENE_OVERFLOW);
	CHECK(plan.dirty_count == 0u);

	LocalVector<AABB> many;
	for (uint32_t i = 0; i < RESTIR_DIRTY_MAX + 1u; i++) {
		many.push_back(AABB(Vector3(real_t(i) * 10, 0, 0), Vector3(1, 1, 1)));
	}
	inputs.dirty_result = GIEpochs::DIRTY_BOXES;
	inputs.dirty_boxes = many.ptr();
	inputs.dirty_box_count = RESTIR_DIRTY_MAX;
	restir_plan_epochs(inputs, plan);
	CHECK_FALSE(plan.cut);
	CHECK(plan.dirty_count == RESTIR_DIRTY_MAX);
	CHECK(plan.dirty_min[RESTIR_DIRTY_MAX - 1u].is_equal_approx(Vector3(real_t(RESTIR_DIRTY_MAX - 1u) * 10 - 100, 0, 50)));
	inputs.dirty_box_count = RESTIR_DIRTY_MAX + 1u;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.cut);
	CHECK(plan.reason == RESTIR_CUT_SCENE_OVERFLOW);
	CHECK(plan.dirty_count == 0u);

	inputs.dirty_box_count = 0u;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.cut);
	CHECK(plan.reason == RESTIR_CUT_SCENE_UNLOCALISED);

	const AABB not_a_number[1] = { AABB(Vector3(real_t(Math::NaN), 0, 0), Vector3(1, 1, 1)) };
	inputs.dirty_boxes = not_a_number;
	inputs.dirty_box_count = 1u;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.cut);
	CHECK(plan.reason == RESTIR_CUT_SCENE_UNLOCALISED);
	CHECK(plan.dirty_count == 0u);

	const AABB inside_out[1] = { AABB(Vector3(0, 0, 0), Vector3(1, -1, 1)) };
	inputs.dirty_boxes = inside_out;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.cut);
	CHECK(plan.reason == RESTIR_CUT_SCENE_UNLOCALISED);

	inputs.current.scene = inputs.previous.scene;
	inputs.dirty_boxes = two;
	inputs.dirty_box_count = 2u;
	restir_plan_epochs(inputs, plan);
	CHECK_FALSE(plan.cut);
	CHECK_FALSE(plan.scene_changed);
	CHECK(plan.dirty_count == 0u);
}

TEST_CASE("[RestirFold] The epoch planner cuts everything on a world change, after the origin cut and before the scene check") {
	using RendererRD::GIEpochs;
	const AABB one[1] = { AABB(Vector3(0, 0, 0), Vector3(1, 1, 1)) };
	RestirEpochInputs inputs;
	inputs.have_history = true;
	inputs.have_epochs = true;
	inputs.previous.scene = 4u;
	inputs.current.scene = 5u;
	inputs.previous.world = 1u;
	inputs.current.world = 2u;
	inputs.have_feed = true;
	inputs.dirty_result = GIEpochs::DIRTY_BOXES;
	inputs.dirty_boxes = one;
	inputs.dirty_box_count = 1u;
	RestirEpochPlan plan;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.cut);
	CHECK(plan.world_changed);
	CHECK(plan.reason == RESTIR_CUT_WORLD);
	CHECK(plan.dirty_count == 0u);

	inputs.current.origin = 1u;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.reason == RESTIR_CUT_ORIGIN);
	inputs.current.origin = 0u;

	inputs.current.scene = inputs.previous.scene;
	restir_plan_epochs(inputs, plan);
	CHECK(plan.cut);
	CHECK(plan.reason == RESTIR_CUT_WORLD);

	inputs.current.world = inputs.previous.world;
	inputs.current.scene = 5u;
	restir_plan_epochs(inputs, plan);
	CHECK_FALSE(plan.cut);
	CHECK_FALSE(plan.world_changed);
	CHECK(plan.dirty_count == 1u);

	inputs.current.world = 2u;
	inputs.have_epochs = false;
	restir_plan_epochs(inputs, plan);
	CHECK_FALSE(plan.cut);
	CHECK_FALSE(plan.world_changed);
	CHECK_FALSE(plan.scene_changed);
	CHECK(plan.dirty_count == 0u);
}

TEST_CASE("[RestirFold] Mirror: the integer square root is exact from a float seed off by one either way") {
	bool exact_all = true;
	for (uint32_t x = 0u; x < 3u * 64u * 64u / 4u; x++) {
		uint32_t exact = 0u;
		while ((exact + 1u) * (exact + 1u) <= x) {
			exact++;
		}
		exact_all = exact_all && restir_fold_isqrt(x) == exact;
		exact_all = exact_all && restir_fold_isqrt_from(exact, x) == exact;
		exact_all = exact_all && restir_fold_isqrt_from(exact + 1u, x) == exact;
		if (exact > 0u) {
			exact_all = exact_all && restir_fold_isqrt_from(exact - 1u, x) == exact;
		}
	}
	CHECK(exact_all);
}

TEST_CASE("[RestirFold] Mirror: KB9, the draw gives each s exactly its weight at every allowed K") {
	for (const uint32_t k : FOLD_KS) {
		const uint32_t total = 3u * k * k / 4u;
		uint32_t counts[64] = {};
		bool below_k = true;
		bool wraps = true;
		for (uint32_t x = 0u; x < total; x++) {
			const uint32_t s = restir_fold_draw(x, k);
			below_k = below_k && s < k;
			wraps = wraps && restir_fold_draw(x + total * 7u, k) == s;
			counts[MIN(s, 63u)]++;
		}
		CHECK(below_k);
		CHECK(wraps);
		uint32_t sum = 0u;
		for (uint32_t s = 0u; s < k; s++) {
			CHECK(counts[s] == draw_weight(s, k));
			sum += counts[s];
		}
		CHECK(sum == total);
	}
}

TEST_CASE("[RestirFold] Mirror: rule 1 refuses at h_s + 1 >= K, or >= K - 1 on a pre-frame") {
	bool matches = true;
	for (const uint32_t k : FOLD_KS) {
		for (uint32_t h_s = 0u; h_s <= FOLD_S_MASK; h_s++) {
			matches = matches && mirror_refuses(h_s, k, false) == (h_s >= k - 1u);
			matches = matches && mirror_refuses(h_s, k, true) == (h_s >= k - 2u);
		}
	}
	CHECK(matches);
}

TEST_CASE("[RestirFold] Mirror: every stored s is at most K - 1, at most K - 2 on a pre-frame, and a renewal at most K / 2") {
	for (const uint32_t k : FOLD_KS) {
		bool bounded = true;
		bool renewal_bounded = true;
		for (const bool pre_frame : { false, true }) {
			for (uint32_t h_s = 0u; h_s <= FOLD_S_MASK; h_s++) {
				for (uint32_t kind = 0u; kind < 2u; kind++) {
					for (uint32_t probe = 0u; probe < 3u * k * k / 4u + k; probe++) {
						FoldPixel pixel;
						pixel.had = kind == 1u;
						pixel.cleared = pixel.had && mirror_refuses(h_s, k, pre_frame);
						pixel.h_s = h_s;
						pixel.h_mv = (probe & 1u) != 0u;
						pixel.still = (probe & 2u) != 0u;
						pixel.v = probe * 2654435761u;
						pixel.phi = probe * 40503u;
						pixel.psi_r = probe;
						pixel.psi_s = probe;
						const uint32_t s = stored_s(mirror_fold_bits(pixel, k, pre_frame));
						bounded = bounded && s <= (pre_frame ? k - 2u : k - 1u);
						if (pixel.cleared) {
							renewal_bounded = renewal_bounded && s <= k / 2u;
						}
					}
				}
			}
		}
		CHECK(bounded);
		CHECK(renewal_bounded);
	}
}

TEST_CASE("[RestirFold] Mirror: the write replaces bits 8 to 15 and keeps bits 0 to 7") {
	FoldPixel pixel;
	pixel.had = true;
	pixel.h_s = 4u;
	pixel.h_mv = true;
	const uint32_t bits = mirror_fold_bits(pixel, 32u, false);
	CHECK((bits & ~FOLD_BITS) == 0u);
	CHECK((bits & 0x8000u) == 0u);
	const uint32_t stored = (0xFFFFu & ~FOLD_BITS) | bits;
	CHECK((stored & 0xFFu) == 0xFFu);
	CHECK(stored_s(stored) == 5u);
	CHECK(stored_mv(stored));
	const uint32_t over = (0xFFFFu & ~FOLD_BITS) | mirror_fold_bits(FoldPixel(), 32u, false);
	CHECK((over & 0x8000u) == 0u);
	CHECK((over & 0xFFu) == 0xFFu);
}

TEST_CASE("[RestirFold] Mirror: carry adds one and keeps moved, renewal takes the pixel phase only after a still life, start is moved") {
	FoldPixel carry;
	carry.had = true;
	carry.h_s = 6u;
	carry.still = true;
	CHECK(stored_s(mirror_fold_bits(carry, 8u, false)) == 7u);
	CHECK_FALSE(stored_mv(mirror_fold_bits(carry, 8u, false)));
	carry.still = false;
	CHECK(stored_mv(mirror_fold_bits(carry, 8u, false)));
	carry.still = true;
	carry.h_mv = true;
	CHECK(stored_mv(mirror_fold_bits(carry, 8u, false)));

	FoldPixel renewal;
	renewal.had = true;
	renewal.cleared = true;
	renewal.h_s = 7u;
	renewal.v = 13u;
	renewal.phi = 2u;
	renewal.psi_r = 3u;
	CHECK(stored_s(mirror_fold_bits(renewal, 8u, false)) == 3u);
	renewal.phi = 0u;
	CHECK(stored_s(mirror_fold_bits(renewal, 8u, false)) == 1u);
	CHECK_FALSE(stored_mv(mirror_fold_bits(renewal, 8u, false)));
	renewal.h_mv = true;
	CHECK(stored_s(mirror_fold_bits(renewal, 8u, false)) == 3u);
	renewal.h_mv = false;
	renewal.still = false;
	CHECK(stored_s(mirror_fold_bits(renewal, 8u, false)) == 3u);
	renewal.psi_r = 6u;
	CHECK(stored_s(mirror_fold_bits(renewal, 8u, false)) == 2u);
	CHECK_FALSE(stored_mv(mirror_fold_bits(renewal, 8u, false)));

	FoldPixel start;
	start.psi_s = 47u;
	CHECK(stored_s(mirror_fold_bits(start, 8u, false)) == 7u);
	CHECK(stored_s(mirror_fold_bits(start, 8u, true)) == 6u);
	CHECK(stored_mv(mirror_fold_bits(start, 8u, false)));
	start.psi_s = 0u;
	CHECK(stored_s(mirror_fold_bits(start, 8u, false)) == 0u);
}

TEST_CASE("[RestirFold] Mirror: a static pixel relocks to (v + phi) mod K within 3K frames of a start") {
	for (const uint32_t k : { 8u, 32u }) {
		const uint32_t v0 = 1000u;
		bool locked = true;
		for (uint32_t s0 = 0u; s0 < k; s0++) {
			for (uint32_t phi = 0u; phi < k; phi++) {
				for (uint32_t u_r = 0u; u_r < k; u_r++) {
					uint32_t s = s0;
					bool mv = true;
					for (uint32_t v = v0 + 1u; v < v0 + 4u * k; v++) {
						FoldPixel pixel;
						pixel.had = true;
						pixel.cleared = mirror_refuses(s, k, false);
						pixel.h_s = s;
						pixel.h_mv = mv;
						pixel.still = true;
						pixel.v = v;
						pixel.phi = phi;
						pixel.psi_r = u_r;
						const uint32_t bits = mirror_fold_bits(pixel, k, false);
						s = stored_s(bits);
						mv = stored_mv(bits);
						if (v >= v0 + 3u * k) {
							locked = locked && s == (v + phi) % k && pixel.cleared == ((v + phi) % k == 0u);
						}
					}
				}
			}
		}
		CHECK(locked);
	}
}

TEST_CASE("[RestirFold] Mirror: the hashes give the known answers") {
	CHECK(restir_fold_pcg(0u) == 0x07BB2FE2u);
	CHECK(restir_fold_pcg(1u) == 0xA8BEEA3Cu);
	CHECK(restir_fold_phi(3u, 5u) == 0x2D397518u);
	const uint32_t psi_r = restir_fold_rng_state(3u, 5u, 7u, 0u, FOLD_C2);
	const uint32_t psi_s = restir_fold_rng_state(3u, 5u, 7u, 1u, FOLD_C2);
	CHECK(psi_r == 0x5072A4E0u);
	CHECK(psi_s == 0xB99D54B9u);
	CHECK(psi_r % 8u == 0u);
	CHECK(psi_r % 32u == 0u);
	CHECK(restir_fold_draw(psi_s, 8u) == 7u);
	CHECK(restir_fold_draw(psi_s, 32u) == 13u);
}

TEST_CASE("[RestirFold] C2 sits at least 5 from C1 and from the default seed sums of every other rtgi_rng_state caller") {
	CHECK(ring_distance(FOLD_C2, FOLD_C1) >= 5u);
	const uint32_t default_seed = 1u;
	const uint32_t hashed[] = { restir_fold_pcg(default_seed * 0x9E3779B1u + 0x7F4A7C15u), restir_fold_pcg(0x1234567u * 0x9E3779B1u + 0x7F4A7C15u) };
	bool spaced = true;
	for (uint32_t own = 0u; own < 2u; own++) {
		for (uint32_t dim = 0u; dim < 8u; dim++) {
			spaced = spaced && ring_distance(FOLD_C2 + own, default_seed + dim) >= 5u;
		}
		for (const uint32_t seed : hashed) {
			for (uint32_t dim = 64u; dim < 64u + 512u; dim++) {
				spaced = spaced && ring_distance(FOLD_C2 + own, seed + dim) >= 5u;
			}
		}
	}
	CHECK(spaced);
}

}
