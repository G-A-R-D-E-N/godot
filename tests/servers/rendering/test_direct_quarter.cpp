/**************************************************************************/
/*  test_direct_quarter.cpp                                               */
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

TEST_FORCE_LINK(test_direct_quarter)

#include "servers/rendering/renderer_rd/environment/direct_quarter.h"

namespace TestDirectQuarter {

using namespace RendererRD::DirectQuarter;

Rebuild flat_rebuild() {
	Rebuild rebuild;
	rebuild.inv00 = 1.0f;
	rebuild.inv11 = 1.0f;
	rebuild.inv23 = 1.0f;
	rebuild.pixel_size = Vector2(1.0f / 64.0f, 1.0f / 64.0f);
	return rebuild;
}

Surface surface(float p_depth, const Vector3 &p_normal = Vector3(0, 0, 1)) {
	Surface s;
	s.depth = p_depth;
	s.normal = p_normal.normalized();
	return s;
}

Surface placed(const Rebuild &p_rebuild, const Vector2i &p_pixel, float p_depth, const Vector3 &p_normal = Vector3(0, 0, 1)) {
	Surface s = surface(p_depth, p_normal);
	s.position = p_rebuild.position(p_pixel, p_depth);
	return s;
}

struct Grid {
	Vector2i size;
	Block blocks[16 * 16];

	const Block *operator()(const Vector2i &p_block) const {
		if (p_block.x < 0 || p_block.y < 0 || p_block.x >= size.x || p_block.y >= size.y) {
			return nullptr;
		}
		return &blocks[p_block.y * 16 + p_block.x];
	}
	Block &at(int p_x, int p_y) { return blocks[p_y * 16 + p_x]; }
};

Block lamp_block(uint32_t p_phase, float p_depth, uint32_t p_slot, float p_factor, uint32_t p_entries = 4) {
	Block block;
	block.phase = p_phase;
	block.guide = surface(p_depth);
	block.slots[0] = p_slot;
	for (uint32_t i = 0; i < p_entries + 2; i++) {
		block.factors[i] = 1.0f;
	}
	block.factors[0] = p_factor;
	return block;
}

TEST_CASE("[DirectQuarter] The block grid rounds odd sizes up") {
	CHECK(block_grid(Vector2i(2560, 1440)) == Vector2i(1280, 720));
	CHECK(block_grid(Vector2i(5, 3)) == Vector2i(3, 2));
	CHECK(block_grid(Vector2i(1, 1)) == Vector2i(1, 1));
	CHECK(block_pixel(Vector2i(2, 3), 3) == Vector2i(5, 7));
}

TEST_CASE("[DirectQuarter] Every 2x2 window of blocks holds both parities twice") {
	for (int y = 0; y < 63; y++) {
		for (int x = 0; x < 63; x++) {
			const uint32_t sum = parity(Vector2i(x, y)) + parity(Vector2i(x + 1, y)) + parity(Vector2i(x, y + 1)) + parity(Vector2i(x + 1, y + 1));
			CHECK_EQ(sum, 2u);
		}
	}
}

TEST_CASE("[DirectQuarter] No diagonal run of one parity is longer than two blocks") {
	for (int y = 0; y < 60; y++) {
		for (int x = 0; x < 60; x++) {
			const uint32_t p = parity(Vector2i(x, y));
			CHECK_FALSE((parity(Vector2i(x + 1, y + 1)) == p && parity(Vector2i(x + 2, y + 2)) == p));
			CHECK_FALSE((parity(Vector2i(x + 2, y)) == p && parity(Vector2i(x + 1, y + 1)) == p && parity(Vector2i(x, y + 2)) == p));
			CHECK_NE(parity(Vector2i(x + 1, y)), p);
		}
	}
}

TEST_CASE("[DirectQuarter] Representative rule") {
	const Rebuild rebuild = flat_rebuild();
	SUBCASE("An all-sky block is empty") {
		Surface pixels[4];
		CHECK_EQ(representative(Vector2i(0, 0), pixels), -1);
	}
	SUBCASE("A smooth block takes its first pixel that is not sky") {
		Surface pixels[4];
		for (uint32_t k = 1; k < 4; k++) {
			pixels[k] = placed(rebuild, block_pixel(Vector2i(4, 4), k), 5.0f);
		}
		CHECK_EQ(representative(Vector2i(4, 4), pixels), 1);
	}
	SUBCASE("A mixed block takes the near surface on parity 0 and the far one on parity 1") {
		for (const Vector2i &block : { Vector2i(0, 0), Vector2i(1, 0) }) {
			Surface pixels[4];
			pixels[0] = placed(rebuild, block_pixel(block, 0), 9.0f);
			pixels[1] = placed(rebuild, block_pixel(block, 1), 3.0f);
			pixels[2] = placed(rebuild, block_pixel(block, 2), 9.0f);
			pixels[3] = placed(rebuild, block_pixel(block, 3), 3.0f);
			CHECK_EQ(representative(block, pixels), parity(block) == 0 ? 1 : 0);
		}
	}
	SUBCASE("A sliver whose normal turns away counts as another surface") {
		const Vector2i block(0, 0);
		Surface pixels[4];
		for (uint32_t k = 0; k < 4; k++) {
			pixels[k] = placed(rebuild, block_pixel(block, k), 5.0f);
		}
		pixels[2].normal = Vector3(1, 0, 0);
		CHECK_EQ(parity(block), 0u);
		CHECK_EQ(representative(block, pixels), 0);
		CHECK_EQ(representative(Vector2i(1, 0), pixels), 2);
	}
	SUBCASE("Nearest ties keep the first pixel") {
		Surface pixels[4];
		pixels[2] = placed(rebuild, block_pixel(Vector2i(0, 0), 2), 4.0f);
		pixels[3] = placed(rebuild, block_pixel(Vector2i(0, 0), 3), 4.0f);
		CHECK_EQ(representative(Vector2i(0, 0), pixels), 2);
	}
}

TEST_CASE("[DirectQuarter] Tier 1 looks at exactly the blocks whose representative can lie within 2 px") {
	for (int x = 0; x < 40; x++) {
		const int low = bracket(Vector2i(x, x)).x;
		for (int k = -2; k < 24; k++) {
			bool reachable = false;
			for (uint32_t phase = 0; phase < 2; phase++) {
				const float gap = float(block_pixel(Vector2i(k, 0), phase).x) + 0.5f - (float(x) + 0.5f);
				reachable = reachable || Math::abs(gap) < 2.0f;
			}
			CHECK_EQ(reachable, k == low || k == low + 1);
		}
	}
}

TEST_CASE("[DirectQuarter] Resolve tiers") {
	const Rebuild rebuild = flat_rebuild();
	Grid grid;
	grid.size = Vector2i(8, 8);
	SUBCASE("Sky gives no entries and factors of 1") {
		const Resolved out = resolve(Vector2i(4, 4), Surface(), 4, rebuild, grid);
		CHECK_EQ(out.tier, uint32_t(TIER_NONE));
		CHECK_EQ(out.slots[0], 0u);
		CHECK_EQ(out.factors[0], 1.0f);
	}
	SUBCASE("A pixel with only empty blocks around is tier 3") {
		const Resolved out = resolve(Vector2i(4, 4), surface(5.0f), 4, rebuild, grid);
		CHECK_EQ(out.tier, uint32_t(TIER_EMPTY));
		CHECK_EQ(out.factors[0], 1.0f);
	}
	SUBCASE("A pixel on its own traced block copies it") {
		grid.at(2, 2) = lamp_block(0, 5.0f, 7, 0.25f);
		const Resolved out = resolve(Vector2i(4, 4), surface(5.0f), 4, rebuild, grid);
		CHECK_EQ(out.tier, uint32_t(TIER_NEAR));
		CHECK_EQ(out.slots[0], 7u);
		CHECK(out.factors[0] == doctest::Approx(0.25f));
		CHECK_FALSE(out.cut);
	}
	SUBCASE("Two blocks of one lamp blend by tent weight") {
		grid.at(2, 2) = lamp_block(0, 5.0f, 7, 0.0f);
		grid.at(3, 2) = lamp_block(0, 5.0f, 7, 1.0f);
		const Resolved out = resolve(Vector2i(5, 4), surface(5.0f), 4, rebuild, grid);
		CHECK_EQ(out.tier, uint32_t(TIER_NEAR));
		CHECK(out.factors[0] == doctest::Approx(0.5f));
	}
	SUBCASE("Blocks on another surface are not used, and a wider ring is") {
		grid.at(2, 2) = lamp_block(0, 50.0f, 7, 0.0f);
		grid.at(3, 2) = lamp_block(0, 50.0f, 7, 0.0f);
		grid.at(2, 3) = lamp_block(0, 50.0f, 7, 0.0f);
		grid.at(3, 3) = lamp_block(0, 50.0f, 7, 0.0f);
		grid.at(1, 2) = lamp_block(1, 5.0f, 9, 0.75f);
		const Resolved out = resolve(Vector2i(5, 5), surface(5.0f), 4, rebuild, grid);
		CHECK_EQ(out.tier, uint32_t(TIER_WIDE));
		CHECK_EQ(out.slots[0], 9u);
		CHECK(out.factors[0] == doctest::Approx(0.75f));
	}
	SUBCASE("A block in the wider ring but more than 3 px away is not used") {
		grid.at(4, 4) = lamp_block(0, 5.0f, 9, 0.75f);
		const Resolved out = resolve(Vector2i(5, 5), surface(5.0f), 4, rebuild, grid);
		CHECK_EQ(out.tier, uint32_t(TIER_EMPTY));
	}
}

TEST_CASE("[DirectQuarter] Lamp list merge, factors and rest record") {
	const Rebuild rebuild = flat_rebuild();
	Grid grid;
	grid.size = Vector2i(8, 8);
	Block a = lamp_block(0, 5.0f, 11, 0.2f);
	a.slots[1] = 12;
	a.factors[1] = 0.4f;
	a.slots[2] = 13;
	a.factors[2] = 0.6f;
	a.slots[3] = 501;
	a.factors[3] = 0.1f;
	a.factors[4] = 0.2f;
	a.factors[5] = 0.3f;
	Block b = lamp_block(0, 5.0f, 14, 0.9f);
	b.slots[1] = 11;
	b.factors[1] = 0.6f;
	b.slots[3] = 502;
	b.factors[3] = 0.7f;
	b.factors[4] = 0.8f;
	b.factors[5] = 0.9f;
	grid.at(2, 2) = a;
	grid.at(3, 2) = b;
	const Resolved out = resolve(Vector2i(5, 4), surface(5.0f), 4, rebuild, grid);
	CHECK_EQ(out.tier, uint32_t(TIER_NEAR));
	CHECK_EQ(out.slots[0], 11u);
	CHECK(out.factors[0] == doctest::Approx(0.4f));
	CHECK_EQ(out.slots[1], 14u);
	CHECK_EQ(out.slots[2], 12u);
	CHECK(out.cut);
	CHECK_EQ(out.slots[3], 501u);
	CHECK(out.factors[3] == doctest::Approx(0.1f));
	CHECK(out.factors[5] == doctest::Approx(0.3f));
}

TEST_CASE("[DirectQuarter] History candidates") {
	const Vector2i grid(32, 32);
	const auto every_block = [](const Vector2i &, uint32_t &r_phase) {
		r_phase = 0;
		return true;
	};
	SUBCASE("At most nine, and exactly the blocks with tent weight above 0") {
		for (int step = 0; step < 200; step++) {
			const Vector2 x(10.0f + 0.037f * float(step), 12.0f + 0.053f * float(step));
			Candidate candidates[MAX_CANDIDATES];
			float total = 0.0f;
			const int count = history_candidates(x, Vector2i(0, 0), Vector2i(-100, -100), grid, grid * 2, every_block, candidates, total);
			int brute = 0;
			for (int ky = 0; ky < grid.y; ky++) {
				for (int kx = 0; kx < grid.x; kx++) {
					brute += tent(Vector2(block_pixel(Vector2i(kx, ky), 0)) + Vector2(0.5f, 0.5f) - x) > 0.0f ? 1 : 0;
				}
			}
			CHECK(count <= MAX_CANDIDATES);
			CHECK_EQ(count, brute);
			CHECK(total > 0.0f);
		}
	}
	SUBCASE("At rest the own block is used alone") {
		Candidate candidates[MAX_CANDIDATES];
		float total = 0.0f;
		const int count = history_candidates(Vector2(9.5f, 7.5f), Vector2i(4, 3), Vector2i(9, 7), grid, grid * 2, every_block, candidates, total);
		CHECK_EQ(count, 1);
		CHECK(candidates[0].block == Vector2i(4, 3));
		CHECK_EQ(total, 1.0f);
	}
	SUBCASE("A lamp only a low-weight candidate holds enters with that share of its count") {
		Candidate candidates[MAX_CANDIDATES];
		candidates[0] = { Vector2i(0, 0), 0.75f };
		candidates[1] = { Vector2i(1, 0), 0.25f };
		const Vector2 blended = blend_history(candidates, 2, 1.0f, [](const Vector2i &p_block, Vector2 &r_accum) {
			if (p_block.x != 1) {
				return false;
			}
			r_accum = Vector2(16.0f, 32.0f);
			return true;
		});
		CHECK(blended.x == doctest::Approx(4.0f));
		CHECK(blended.y == doctest::Approx(8.0f));
	}
	SUBCASE("Outside the grid no block is a candidate") {
		Candidate candidates[MAX_CANDIDATES];
		float total = 0.0f;
		CHECK_EQ(history_candidates(Vector2(-20.0f, -20.0f), Vector2i(0, 0), Vector2i(-100, -100), grid, grid * 2, every_block, candidates, total), 0);
		CHECK_EQ(total, 0.0f);
	}
}

TEST_CASE("[DirectQuarter] The last block of an odd-sized image counts its missing pixels as sky") {
	const Rebuild rebuild = flat_rebuild();
	const Vector2i size(5, 3);
	const Vector2i last = block_grid(size) - Vector2i(1, 1);
	Surface pixels[4];
	for (uint32_t k = 0; k < 4; k++) {
		const Vector2i at = block_pixel(last, k);
		if (at.x < size.x && at.y < size.y) {
			pixels[k] = placed(rebuild, at, 5.0f);
		}
	}
	CHECK(last == Vector2i(2, 1));
	CHECK_EQ(representative(last, pixels), 0);
	const Vector2i middle(1, 0);
	Surface edge[4];
	for (uint32_t k = 0; k < 4; k++) {
		edge[k] = placed(rebuild, block_pixel(middle, k), 5.0f);
	}
	edge[0] = Surface();
	edge[1] = Surface();
	CHECK_EQ(representative(middle, edge), 2);
}

TEST_CASE("[DirectQuarter] Equal lamp scores keep the lower slot") {
	const Rebuild rebuild = flat_rebuild();
	Grid grid;
	grid.size = Vector2i(8, 8);
	grid.at(2, 2) = lamp_block(0, 5.0f, 40, 0.5f);
	grid.at(3, 2) = lamp_block(0, 5.0f, 30, 0.5f);
	const Resolved out = resolve(Vector2i(5, 4), surface(5.0f), 4, rebuild, grid);
	CHECK_EQ(out.slots[0], 30u);
	CHECK_EQ(out.slots[1], 40u);
	CHECK_EQ(out.slots[2], 0u);
	CHECK_FALSE(out.cut);
}

TEST_CASE("[DirectQuarter] A lamp held only by a candidate of weight 0.1 of 1.0 comes back with a tenth of its count") {
	Candidate candidates[MAX_CANDIDATES];
	candidates[0] = { Vector2i(0, 0), 0.9f };
	candidates[1] = { Vector2i(1, 0), 0.1f };
	const Vector2 blended = blend_history(candidates, 2, 1.0f, [](const Vector2i &p_block, Vector2 &r_accum) {
		if (p_block.x != 1) {
			return false;
		}
		r_accum = Vector2(8.0f, 32.0f);
		return true;
	});
	CHECK(blended.y == doctest::Approx(3.2f));
	CHECK(blended.x / blended.y == doctest::Approx(0.25f));
}

} // namespace TestDirectQuarter
