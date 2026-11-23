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

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/templates/local_vector.h"
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

struct Row {
	LocalVector<Block> blocks;

	const Block *operator()(const Vector2i &p_block) const {
		if (p_block.y != 0 || p_block.x < 0 || p_block.x >= int(blocks.size())) {
			return nullptr;
		}
		return &blocks[p_block.x];
	}
};

struct Field {
	Vector2i size;
	LocalVector<Block> blocks;

	const Block *operator()(const Vector2i &p_block) const {
		if (p_block.x < 0 || p_block.y < 0 || p_block.x >= size.x || p_block.y >= size.y) {
			return nullptr;
		}
		return &blocks[p_block.y * size.x + p_block.x];
	}
};

struct Edge {
	float shift = 0.0f;
	float width = 0.0f;
};

float crossing(const LocalVector<float> &p_profile, float p_level) {
	LocalVector<int> found;
	for (uint32_t i = 0; i + 1 < p_profile.size(); i++) {
		if (p_profile[i] < p_level && p_profile[i + 1] >= p_level) {
			found.push_back(int(i));
		}
	}
	if (found.is_empty()) {
		return -1000.0f;
	}
	const int i = found[found.size() / 2];
	return float(i) + 0.5f + (p_level - p_profile[i]) / (p_profile[i + 1] - p_profile[i]);
}

template <typename Offset>
Edge history_edge(double p_speed, int p_frames, const Offset &p_offset) {
	const int n = 512;
	const int nb = n / 2;
	const float cap = 32.0f;
	const double edge_screen = double(n / 2) + 0.25;
	const double edge = edge_screen + p_speed * double(p_frames);
	const Vector2i grid(nb, 1);
	const Vector2i full(n, 2);
	const auto every_block = [](const Vector2i &, uint32_t &r_phase) {
		r_phase = 0;
		return true;
	};
	LocalVector<float> history;
	LocalVector<float> next;
	history.resize(nb);
	next.resize(nb);
	for (int k = 0; k < nb; k++) {
		history[k] = 2.0 * double(k) + 0.5 >= edge ? 1.0f : 0.0f;
	}
	for (int t = 1; t <= p_frames; t++) {
		for (int k = 0; k < nb; k++) {
			const double rep = 2.0 * double(k) + 0.5;
			const float sample = rep + p_speed * double(t) >= edge ? 1.0f : 0.0f;
			Candidate candidates[MAX_CANDIDATES];
			float total = 0.0f;
			const Vector2 x(real_t(p_offset(rep, t)), 0.5f);
			const int count = history_candidates(x, Vector2i(k, 0), Vector2i(2 * k, 0), grid, full, every_block, candidates, total);
			float previous = sample;
			if (count > 0) {
				previous = float(blend_history(candidates, count, total, [&history](const Vector2i &p_block, Vector2 &r_accum) {
					r_accum = Vector2(history[p_block.x], 1.0f);
					return true;
				}).x);
			}
			next[k] = previous + (sample - previous) / cap;
		}
		SWAP(history, next);
	}
	Rebuild rebuild = flat_rebuild();
	rebuild.pixel_size = Vector2(1.0f / float(n), 0.5f);
	Row row;
	row.blocks.resize(nb);
	for (int k = 0; k < nb; k++) {
		row.blocks[k] = lamp_block(0, 5.0f, 7, history[k]);
	}
	LocalVector<float> profile;
	profile.resize(n);
	for (int f = 0; f < n; f++) {
		profile[f] = resolve(Vector2i(f, 0), surface(5.0f), 4, rebuild, row).factors[0];
	}
	Edge out;
	out.shift = crossing(profile, 0.5f) - float(edge_screen);
	out.width = crossing(profile, 0.9f) - crossing(profile, 0.1f);
	return out;
}

TEST_CASE("[DirectQuarter] The history blend matches the 1D model of design 1.4 at every strafe speed") {
	const double speeds[7] = { 0.0, 0.5, 0.7, 1.0, 1.5, 2.0, 2.5 };
	const float widths[7] = { 1.60f, 10.77f, 11.84f, 12.40f, 10.81f, 1.60f, 10.77f };
	const float shifts[7] = { -0.75f, 0.05f, -0.07f, -0.35f, -0.29f, -0.75f, 0.05f };
	for (int i = 0; i < 7; i++) {
		const double speed = speeds[i];
		const Edge edge = history_edge(speed, 60, [speed](double p_rep, int) { return p_rep + speed; });
		CHECK(Math::abs(edge.width - widths[i]) < 0.02f);
		CHECK(Math::abs(edge.shift - shifts[i]) < 0.02f);
	}
}

float halton(uint32_t p_index, uint32_t p_base) {
	float f = 1.0f;
	float r = 0.0f;
	while (p_index > 0) {
		f /= float(p_base);
		r += f * float(p_index % p_base);
		p_index /= p_base;
	}
	return r;
}

float jitter_ndc(int p_frame, float p_size) {
	return (halton(uint32_t(p_frame % 16) + 1, 2) - 0.5f) * 2.0f / p_size;
}

float jitter_delta_px(int p_frame, float p_size) {
	return (jitter_ndc(p_frame - 1, p_size) - jitter_ndc(p_frame, p_size)) * p_size * 0.5f;
}

TEST_CASE("[DirectQuarter] A still camera under a 16-phase jitter keeps a hard edge 1.6 px wide once jitter_delta is removed") {
	const float size = 512.0f;
	const auto reprojected = [size](double p_rep, int p_frame) {
		const float ndc = (float(p_rep) / size) * 2.0f - 1.0f + (jitter_ndc(p_frame - 1, size) - jitter_ndc(p_frame, size));
		return double((ndc * 0.5f + 0.5f) * size);
	};
	const Edge removed = history_edge(0.0, 600, [&](double p_rep, int p_frame) {
		return reprojected(p_rep, p_frame) - double(jitter_delta_px(p_frame, size));
	});
	CHECK(Math::abs(removed.width - 1.6f) < 0.02f);
	const Edge kept = history_edge(0.0, 600, reprojected);

	CHECK(kept.width > 3.0f);
}

TEST_CASE("[DirectQuarter] Equal lamp lists merge to that list with weighted mean factors") {
	const Rebuild rebuild = flat_rebuild();
	Grid grid;
	grid.size = Vector2i(8, 8);
	Block a = lamp_block(0, 5.0f, 11, 0.2f);
	a.slots[1] = 12;
	a.factors[1] = 0.4f;
	a.slots[2] = 13;
	a.factors[2] = 0.0f;
	Block b = a;
	b.factors[0] = 0.6f;
	b.factors[1] = 0.8f;
	b.factors[2] = 1.0f;
	grid.at(2, 2) = a;
	grid.at(3, 2) = b;
	const Resolved out = resolve(Vector2i(5, 4), surface(5.0f), 4, rebuild, grid);
	CHECK_EQ(out.tier, uint32_t(TIER_NEAR));
	CHECK_EQ(out.slots[0], 11u);
	CHECK_EQ(out.slots[1], 12u);
	CHECK_EQ(out.slots[2], 13u);
	CHECK(out.factors[0] == doctest::Approx(0.4f));
	CHECK(out.factors[1] == doctest::Approx(0.6f));
	CHECK(out.factors[2] == doctest::Approx(0.5f));
	CHECK_FALSE(out.cut);
}

TEST_CASE("[DirectQuarter] Lists differing by one lamp keep the one with the higher score") {
	const Rebuild rebuild = flat_rebuild();
	for (int swap = 0; swap < 2; swap++) {
		Grid grid;
		grid.size = Vector2i(8, 8);
		Block heavy = lamp_block(1, 5.0f, 11, 0.5f);
		heavy.slots[1] = 12;
		heavy.slots[2] = swap == 0 ? 13 : 14;
		Block light = lamp_block(0, 5.0f, 11, 0.5f);
		light.slots[1] = 12;
		light.slots[2] = swap == 0 ? 14 : 13;
		grid.at(2, 2) = heavy;
		grid.at(3, 2) = light;
		const Resolved out = resolve(Vector2i(5, 4), surface(5.0f), 4, rebuild, grid);
		CHECK_EQ(out.tier, uint32_t(TIER_NEAR));
		CHECK_EQ(out.slots[2], swap == 0 ? 13u : 14u);
		CHECK(out.cut);
	}
}

TEST_CASE("[DirectQuarter] Across a depth step no pixel takes the other side's value") {
	const Rebuild rebuild = flat_rebuild();
	Grid grid;
	grid.size = Vector2i(8, 8);
	for (int y = 0; y < 8; y++) {
		for (int x = 0; x < 8; x++) {
			grid.at(x, y) = x < 4 ? lamp_block(0, 5.0f, 7, 1.0f) : lamp_block(0, 6.0f, 7, 0.0f);
		}
	}
	for (int y = 2; y < 14; y++) {
		for (int x = 2; x < 14; x++) {
			const bool is_near = x < 8;
			const Resolved out = resolve(Vector2i(x, y), surface(is_near ? 5.0f : 6.0f), 4, rebuild, grid);
			CHECK(out.tier != uint32_t(TIER_EMPTY));
			CHECK_EQ(out.factors[0], is_near ? 1.0f : 0.0f);
		}
	}
}

TEST_CASE("[DirectQuarter] Bumpy normals on a flat plane keep every block, weighted by their facing") {
	Rebuild rebuild = flat_rebuild();
	rebuild.pixel_size = Vector2(1.0f / 2560.0f, 1.0f / 1440.0f);
	const float tilt = Math::deg_to_rad(20.0f);
	const Vector3 left(Math::sin(tilt), 0.0f, Math::cos(tilt));
	const Vector3 right(-Math::sin(tilt), 0.0f, Math::cos(tilt));
	Grid grid;
	grid.size = Vector2i(8, 8);
	grid.at(2, 2) = lamp_block(0, 5.0f, 7, 0.0f);
	grid.at(2, 2).guide.normal = left;
	grid.at(3, 2) = lamp_block(0, 5.0f, 7, 1.0f);
	grid.at(3, 2).guide.normal = right;
	const Resolved out = resolve(Vector2i(5, 4), surface(5.0f, left), 4, rebuild, grid);
	const float facing = Math::cos(2.0f * tilt) * Math::cos(2.0f * tilt);
	CHECK(facing == doctest::Approx(0.587f).epsilon(0.002));
	CHECK_EQ(out.tier, uint32_t(TIER_NEAR));
	CHECK(out.factors[0] == doctest::Approx(facing / (1.0f + facing)).epsilon(1e-3));
}

Block traced(const Vector2i &p_block, const Surface p_pixels[4], uint32_t p_near_slot, float p_near_depth) {
	Block block;
	const int pick = representative(p_block, p_pixels);
	if (pick < 0) {
		return block;
	}
	block.phase = uint32_t(pick);
	block.guide = p_pixels[pick];
	const bool is_near = p_pixels[pick].depth == p_near_depth;
	block.slots[0] = is_near ? p_near_slot : p_near_slot + 100;
	for (uint32_t i = 0; i < 6; i++) {
		block.factors[i] = 1.0f;
	}
	block.factors[0] = is_near ? 0.0f : 1.0f;
	return block;
}

template <typename DepthAt>
void check_two_surfaces(const DepthAt &p_depth_at, const Vector2i &p_size) {
	const Rebuild rebuild = flat_rebuild();
	const float near_depth = 3.0f;
	Field field;
	field.size = block_grid(p_size);
	field.blocks.resize(field.size.x * field.size.y);
	for (int by = 0; by < field.size.y; by++) {
		for (int bx = 0; bx < field.size.x; bx++) {
			const Vector2i block(bx, by);
			Surface pixels[4];
			for (uint32_t k = 0; k < 4; k++) {
				const Vector2i at = block_pixel(block, k);
				pixels[k] = placed(rebuild, at, p_depth_at(at));
			}
			field.blocks[by * field.size.x + bx] = traced(block, pixels, 3, near_depth);
		}
	}
	int total[2] = { 0, 0 };
	int found[2] = { 0, 0 };
	for (int y = 4; y < p_size.y - 4; y++) {
		for (int x = 4; x < p_size.x - 4; x++) {
			const float depth = p_depth_at(Vector2i(x, y));
			const bool is_near = depth == near_depth;
			const Resolved out = resolve(Vector2i(x, y), surface(depth), 4, rebuild, field);
			CHECK((out.tier == uint32_t(TIER_EMPTY) || out.slots[0] == (is_near ? 3u : 103u)));
			CHECK_NE(out.slots[0], is_near ? 103u : 3u);
			total[is_near ? 1 : 0]++;
			found[is_near ? 1 : 0] += out.tier == uint32_t(TIER_EMPTY) ? 0 : 1;
		}
	}
	for (int side = 0; side < 2; side++) {
		CHECK(total[side] > 0);
		CHECK(found[side] * 2 >= total[side]);
	}
}

TEST_CASE("[DirectQuarter] A mixed region under both parities resolves every pixel from its own surface") {
	check_two_surfaces([](const Vector2i &p_pixel) { return (p_pixel.x & 1) == 0 ? 3.0f : 9.0f; }, Vector2i(32, 32));
}

TEST_CASE("[DirectQuarter] Wires at 0, 45 and 90 degrees never take the background's lamp") {
	check_two_surfaces([](const Vector2i &p_pixel) { return p_pixel.y == 13 ? 3.0f : 9.0f; }, Vector2i(32, 32));
	check_two_surfaces([](const Vector2i &p_pixel) { return p_pixel.x == 13 ? 3.0f : 9.0f; }, Vector2i(32, 32));
	check_two_surfaces([](const Vector2i &p_pixel) { return p_pixel.x == p_pixel.y ? 3.0f : 9.0f; }, Vector2i(32, 32));
}

TEST_CASE("[DirectQuarter] A sliver whose normal turns 80 degrees is not used") {
	const Rebuild rebuild = flat_rebuild();
	const float turn = Math::deg_to_rad(80.0f);
	Grid grid;
	grid.size = Vector2i(8, 8);
	grid.at(2, 2) = lamp_block(0, 5.0f, 7, 0.0f);
	grid.at(2, 2).guide.normal = Vector3(Math::sin(turn), 0.0f, Math::cos(turn));
	grid.at(3, 2) = lamp_block(0, 5.0f, 9, 1.0f);
	const Resolved out = resolve(Vector2i(5, 4), surface(5.0f), 4, rebuild, grid);
	CHECK(Math::cos(turn) * Math::cos(turn) == doctest::Approx(0.030f).epsilon(0.01));

	CHECK_EQ(out.tier, uint32_t(TIER_NEAR));
	CHECK_EQ(out.slots[0], 9u);
	CHECK_EQ(out.slots[1], 0u);
	CHECK_EQ(out.factors[0], 1.0f);
}

TEST_CASE("[DirectQuarter] A lit pixel beside lamp-free blocks never resolves with slots 0 at tier 1") {
	const Rebuild rebuild = flat_rebuild();
	Grid grid;
	grid.size = Vector2i(8, 8);
	SUBCASE("A lit block within 3 px gives tier 2") {
		grid.at(1, 2) = lamp_block(1, 5.0f, 9, 0.75f);
		const Resolved out = resolve(Vector2i(5, 5), surface(5.0f), 4, rebuild, grid);
		CHECK_EQ(out.tier, uint32_t(TIER_WIDE));
		CHECK_EQ(out.slots[0], 9u);
	}
	SUBCASE("No lit block within 3 px gives tier 3") {
		const Resolved out = resolve(Vector2i(5, 5), surface(5.0f), 4, rebuild, grid);
		CHECK_EQ(out.tier, uint32_t(TIER_EMPTY));
		CHECK_EQ(out.factors[0], 1.0f);
	}
}

TEST_CASE("[DirectQuarter] 1921x1081 gives a 961x541 grid whose last block holds one pixel") {
	const Rebuild rebuild = flat_rebuild();
	const Vector2i size(1921, 1081);
	const Vector2i grid = block_grid(size);
	CHECK(grid == Vector2i(961, 541));
	const Vector2i last = grid - Vector2i(1, 1);
	Surface pixels[4];
	int inside = 0;
	for (uint32_t k = 0; k < 4; k++) {
		const Vector2i at = block_pixel(last, k);
		if (at.x < size.x && at.y < size.y) {
			pixels[k] = placed(rebuild, at, 5.0f);
			inside++;
		}
	}
	CHECK_EQ(inside, 1);
	CHECK_EQ(representative(last, pixels), 0);
}

struct Dump {
	int width = 0;
	int height = 0;
	int block_width = 0;
	int block_height = 0;
	uint32_t entries = 0;
	Rebuild rebuild;
	LocalVector<uint16_t> slots;
	LocalVector<uint16_t> factors;
	LocalVector<uint8_t> tier;
	LocalVector<uint16_t> slots_block;
	LocalVector<uint16_t> factors_block;
	LocalVector<uint32_t> guide_block;
	LocalVector<float> depth_full;
	LocalVector<uint32_t> normal_full;
	LocalVector<Block> blocks;

	const Block *operator()(const Vector2i &p_block) const {
		if (p_block.x < 0 || p_block.y < 0 || p_block.x >= block_width || p_block.y >= block_height) {
			return nullptr;
		}
		return &blocks[p_block.y * block_width + p_block.x];
	}
};

Vector3 oct_decode(uint32_t p_word) {
	const float x = CLAMP(float(int16_t(p_word & 0xffffu)) / 32767.0f, -1.0f, 1.0f);
	const float y = CLAMP(float(int16_t(p_word >> 16u)) / 32767.0f, -1.0f, 1.0f);
	Vector3 n(x, y, 1.0f - Math::abs(x) - Math::abs(y));
	const float fold = MAX(float(-n.z), 0.0f);
	n.x += n.x >= 0.0f ? -fold : fold;
	n.y += n.y >= 0.0f ? -fold : fold;
	return n.normalized();
}

uint32_t oct_encode(const Vector3 &p_normal) {
	const float sum = Math::abs(p_normal.x) + Math::abs(p_normal.y) + Math::abs(p_normal.z);
	Vector2 folded(p_normal.x / sum, p_normal.y / sum);
	if (p_normal.z < 0.0f) {
		folded = Vector2((1.0f - Math::abs(folded.y)) * (folded.x >= 0.0f ? 1.0f : -1.0f), (1.0f - Math::abs(folded.x)) * (folded.y >= 0.0f ? 1.0f : -1.0f));
	}
	const uint32_t lo = uint32_t(uint16_t(int16_t(Math::round(CLAMP(float(folded.x), -1.0f, 1.0f) * 32767.0f))));
	const uint32_t hi = uint32_t(uint16_t(int16_t(Math::round(CLAMP(float(folded.y), -1.0f, 1.0f) * 32767.0f))));
	return lo | (hi << 16u);
}

uint32_t float_bits(float p_value) {
	uint32_t bits;
	memcpy(&bits, &p_value, sizeof(bits));
	return bits;
}

float bits_float(uint32_t p_bits) {
	float value;
	memcpy(&value, &p_bits, sizeof(value));
	return value;
}

void build_blocks(Dump &r_dump) {
	const int count = r_dump.block_width * r_dump.block_height;
	r_dump.blocks.resize(count);
	for (int i = 0; i < count; i++) {
		Block block;
		const uint32_t word = r_dump.guide_block[i * 2];
		if ((word & ~3u) != 0u) {
			block.phase = word & 3u;
			block.guide.depth = bits_float(word & ~3u);
			block.guide.normal = oct_decode(r_dump.guide_block[i * 2 + 1]);
			for (uint32_t e = 0; e < r_dump.entries; e++) {
				block.slots[e] = r_dump.slots_block[e * count + i];
			}
			for (uint32_t e = 0; e < r_dump.entries + 2; e++) {
				block.factors[e] = float(r_dump.factors_block[e * count + i]) / 65535.0f;
			}
		}
		r_dump.blocks[i] = block;
	}
}

Surface own_surface(const Dump &p_dump, int p_index) {
	Surface s;
	const float depth = p_dump.depth_full[p_index];
	if (depth <= 0.0f) {
		return s;
	}
	const uint32_t packed = p_dump.normal_full[p_index];
	const Vector3 unit(float(packed & 255u) / 255.0f, float((packed >> 8u) & 255u) / 255.0f, float((packed >> 16u) & 255u) / 255.0f);
	s.depth = depth;
	s.normal = (unit * 2.0f - Vector3(1.0f, 1.0f, 1.0f)).normalized();
	return s;
}

struct DumpTally {
	int64_t compared = 0;
	int64_t past_region = 0;
	int64_t threshold = 0;
	int64_t outside = 0;
	int64_t factor_off = 0;
	float worst_factor = 0.0f;
	int64_t class_g = 0;
	int64_t class_cut[CUT_KIND_COUNT] = {};
	int64_t differ = 0;
	int64_t differ_g = 0;
	int64_t differ_cut[CUT_KIND_COUNT] = {};
	int64_t differ_margin_zero = 0;
	int64_t differ_margin_bucket[5] = {};
	int64_t differ_tier = 0;
	int64_t differ_order_only = 0;
	int64_t differ_slot_set = 0;
	int64_t differ_factor_only = 0;
};

struct MirrorRecord {
	int32_t x = 0;
	int32_t y = 0;
	uint8_t gpu_tier = 0;
	uint8_t mirror_tier = 0;
	uint8_t flags = 0;
	uint8_t entries = 0;
	uint16_t gpu_slots[MAX_ENTRIES] = {};
	uint16_t mirror_slots[MAX_ENTRIES] = {};
	float gpu_factors[MAX_ENTRIES + 2] = {};
	float mirror_factors[MAX_ENTRIES + 2] = {};
	float margins[CUT_KIND_COUNT] = {};
};

static_assert(sizeof(MirrorRecord) == 140);

static constexpr uint8_t MIRROR_BY_CUT = 1;
static constexpr uint8_t MIRROR_BY_G = 2;

bool same_slot_set(const Dump &p_dump, int p_index, const Resolved &p_out) {
	const int64_t pixels = int64_t(p_dump.width) * p_dump.height;
	const uint32_t own_limit = p_dump.entries - 1;
	if (p_dump.slots[own_limit * pixels + p_index] != p_out.slots[own_limit]) {
		return false;
	}
	for (uint32_t e = 0; e < own_limit; e++) {
		bool gpu_in_mirror = false;
		bool mirror_in_gpu = false;
		for (uint32_t f = 0; f < own_limit; f++) {
			gpu_in_mirror = gpu_in_mirror || p_out.slots[f] == p_dump.slots[e * pixels + p_index];
			mirror_in_gpu = mirror_in_gpu || p_dump.slots[f * pixels + p_index] == p_out.slots[e];
		}
		if (!gpu_in_mirror || !mirror_in_gpu) {
			return false;
		}
	}
	return true;
}

int margin_bucket(float p_margin) {
	if (p_margin <= 1e-7f) {
		return 0;
	}
	if (p_margin <= 1e-6f) {
		return 1;
	}
	if (p_margin <= 1e-5f) {
		return 2;
	}
	if (p_margin < 1e-4f) {
		return 3;
	}
	return 4;
}

bool near_threshold(const Dump &p_dump, const Vector2i &p_pixel, const Surface &p_own) {
	Surface own = p_own;
	own.position = p_dump.rebuild.position(p_pixel, own.depth);
	const Vector2i near_block = bracket(p_pixel);
	for (int dy = -1; dy <= 2; dy++) {
		for (int dx = -1; dx <= 2; dx++) {
			const Block *block = p_dump(near_block + Vector2i(dx, dy));
			if (block == nullptr || block->is_empty()) {
				continue;
			}
			Surface other = block->guide;
			other.position = p_dump.rebuild.position(block_pixel(near_block + Vector2i(dx, dy), block->phase), other.depth);
			if (Math::abs(surface_weight(own, other) - SURFACE_MIN) < 1e-4f) {
				return true;
			}
		}
	}
	return false;
}

DumpTally compare_dump(const Dump &p_dump, int p_print, LocalVector<MirrorRecord> *r_records = nullptr) {
	DumpTally tally;
	int print_outside = p_print;
	int print_inside = p_print;
	const int64_t pixels = int64_t(p_dump.width) * p_dump.height;
	for (int y = 0; y < p_dump.height; y++) {
		for (int x = 0; x < p_dump.width; x++) {
			const int index = y * p_dump.width + x;
			const uint32_t gpu_tier = p_dump.tier[index];
			if (gpu_tier == TIER_NONE && p_dump.depth_full[index] > 0.0f) {
				tally.past_region++;
				continue;
			}
			tally.compared++;
			const Surface own = own_surface(p_dump, index);
			float margin = 1e30f;
			uint32_t kind = CUT_NONE;
			float kind_margins[CUT_KIND_COUNT];
			const Resolved out = resolve(Vector2i(x, y), own, p_dump.entries, p_dump.rebuild, p_dump, &margin, &kind, kind_margins);
			const bool by_cut = margin < 1e-4f;
			const bool by_g = near_threshold(p_dump, Vector2i(x, y), own);
			const bool in_class = by_cut || by_g;
			if (in_class) {
				tally.threshold++;
			}
			if (by_g) {
				tally.class_g++;
			}
			if (by_cut) {
				tally.class_cut[kind]++;
			}
			bool same = out.tier == gpu_tier;
			for (uint32_t e = 0; e < p_dump.entries; e++) {
				same = same && out.slots[e] == p_dump.slots[e * pixels + index];
			}
			float worst = 0.0f;
			for (uint32_t e = 0; e < p_dump.entries + 2; e++) {
				worst = MAX(worst, Math::abs(out.factors[e] - float(p_dump.factors[e * pixels + index]) / 65535.0f));
			}
			const bool factors_ok = worst <= 1e-3f;
			if (same && factors_ok) {
				tally.worst_factor = MAX(tally.worst_factor, worst);
				continue;
			}
			if (r_records != nullptr) {
				MirrorRecord record;
				record.x = x;
				record.y = y;
				record.gpu_tier = uint8_t(gpu_tier);
				record.mirror_tier = uint8_t(out.tier);
				record.flags = (by_cut ? MIRROR_BY_CUT : 0) | (by_g ? MIRROR_BY_G : 0);
				record.entries = uint8_t(p_dump.entries);
				for (uint32_t e = 0; e < p_dump.entries; e++) {
					record.gpu_slots[e] = p_dump.slots[e * pixels + index];
					record.mirror_slots[e] = uint16_t(out.slots[e]);
				}
				for (uint32_t e = 0; e < p_dump.entries + 2; e++) {
					record.gpu_factors[e] = float(p_dump.factors[e * pixels + index]) / 65535.0f;
					record.mirror_factors[e] = out.factors[e];
				}
				for (uint32_t k = 0; k < CUT_KIND_COUNT; k++) {
					record.margins[k] = kind_margins[k];
				}
				r_records->push_back(record);
			}
			if (in_class) {
				tally.differ++;
				if (by_g) {
					tally.differ_g++;
				}
				if (by_cut) {
					tally.differ_cut[kind]++;
					tally.differ_margin_zero += margin == 0.0f ? 1 : 0;
					tally.differ_margin_bucket[margin_bucket(margin)]++;
				}
				if (out.tier != gpu_tier) {
					tally.differ_tier++;
				} else if (!same && same_slot_set(p_dump, index, out)) {
					tally.differ_order_only++;
				} else if (!same) {
					tally.differ_slot_set++;
				} else {
					tally.differ_factor_only++;
				}
			} else if (!same) {
				tally.outside++;
			} else {
				tally.factor_off++;
				tally.worst_factor = MAX(tally.worst_factor, worst);
			}
			int &budget = in_class ? print_inside : print_outside;
			if (budget > 0) {
				budget--;
				MESSAGE(doctest::String(vformat("DI1Q_DUMP_PIXEL (%d, %d) %s: GPU tier %d slots %d %d %d factor0 %f; mirror tier %d slots %d %d %d factor0 %f; cut margin %.9f kind %d; g band %s", x, y, in_class ? "in the class" : "OUTSIDE the class", gpu_tier, p_dump.slots[index], p_dump.slots[pixels + index], p_dump.entries > 2 ? p_dump.slots[2 * pixels + index] : 0u, float(p_dump.factors[index]) / 65535.0f, out.tier, out.slots[0], out.slots[1], out.slots[2], out.factors[0], margin, kind, by_g ? "yes" : "no").utf8().get_data()));
			}
		}
	}
	return tally;
}

bool write_mirror(const String &p_path, const LocalVector<MirrorRecord> &p_records) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	const char magic[8] = { 'D', 'I', '1', 'Q', 'M', 'I', 'R', '1' };
	bool ok = file->store_buffer(reinterpret_cast<const uint8_t *>(magic), 8);
	ok = file->store_32(uint32_t(sizeof(MirrorRecord))) && ok;
	ok = file->store_32(p_records.size()) && ok;
	if (!p_records.is_empty()) {
		ok = file->store_buffer(reinterpret_cast<const uint8_t *>(p_records.ptr()), uint64_t(p_records.size()) * sizeof(MirrorRecord)) && ok;
	}
	return ok;
}

template <typename T>
bool read_layers(const String &p_path, LocalVector<T> &r_out, int64_t p_values) {
	const Vector<uint8_t> bytes = FileAccess::get_file_as_bytes(p_path);
	if (int64_t(bytes.size()) != p_values * int64_t(sizeof(T))) {
		MESSAGE(doctest::String(vformat("%s holds %d bytes, not %d", p_path, bytes.size(), p_values * int64_t(sizeof(T))).utf8().get_data()));
		return false;
	}
	r_out.resize(p_values);
	memcpy(r_out.ptr(), bytes.ptr(), bytes.size());
	return true;
}

TEST_CASE("[DirectQuarter][Dump] The mirror resolves a GPU dump as the GPU did") {
	const String path = OS::get_singleton()->get_environment("DI1Q_DUMP");
	if (path.is_empty()) {
		MESSAGE("DI1Q_DUMP is not set; skipped.");
		return;
	}
	const Dictionary report = JSON::parse_string(FileAccess::get_file_as_string(path));
	REQUIRE(bool(report.get("ok", false)));
	REQUIRE(String(report.get("mode", "")) == "quarter");
	Dump dump;
	dump.width = report["width"];
	dump.height = report["height"];
	dump.block_width = report["block_width"];
	dump.block_height = report["block_height"];
	dump.entries = uint32_t(int(report["entries"]));
	const Array inverse = report["inv_projection"];
	REQUIRE(inverse.size() == 16);
	dump.rebuild.inv00 = float(inverse[0]);
	dump.rebuild.inv11 = float(inverse[5]);
	dump.rebuild.inv23 = float(inverse[11]);
	dump.rebuild.inv30 = float(inverse[12]);
	dump.rebuild.inv31 = float(inverse[13]);
	dump.rebuild.pixel_size = Vector2(1.0f / float(dump.width), 1.0f / float(dump.height));
	REQUIRE(dump.entries >= 2);
	REQUIRE(dump.entries <= uint32_t(MAX_ENTRIES));
	const Dictionary files = report["files"];
	REQUIRE(files.has("tier"));
	REQUIRE(files.has("depth_full"));
	REQUIRE(files.has("normal_full"));
	const String dir = path.get_base_dir();
	const int64_t pixels = int64_t(dump.width) * dump.height;
	const int64_t blocks = int64_t(dump.block_width) * dump.block_height;
	REQUIRE(read_layers(dir.path_join(files["slots"]), dump.slots, pixels * dump.entries));
	REQUIRE(read_layers(dir.path_join(files["factors"]), dump.factors, pixels * (dump.entries + 2)));
	REQUIRE(read_layers(dir.path_join(files["tier"]), dump.tier, pixels));
	REQUIRE(read_layers(dir.path_join(files["slots_block"]), dump.slots_block, blocks * dump.entries));
	REQUIRE(read_layers(dir.path_join(files["factors_block"]), dump.factors_block, blocks * (dump.entries + 2)));
	REQUIRE(read_layers(dir.path_join(files["guide_block"]), dump.guide_block, blocks * 2));
	REQUIRE(read_layers(dir.path_join(files["depth_full"]), dump.depth_full, pixels));
	REQUIRE(read_layers(dir.path_join(files["normal_full"]), dump.normal_full, pixels));
	build_blocks(dump);
	LocalVector<MirrorRecord> records;
	const DumpTally tally = compare_dump(dump, 20, &records);
	const String mirror_path = path + ".mirror.bin";
	const bool mirror_ok = write_mirror(mirror_path, records);
	MESSAGE(doctest::String(vformat("DI1Q_DUMP_MIRROR %s: %d records of %d bytes, written %s", mirror_path, int64_t(records.size()), int64_t(sizeof(MirrorRecord)), mirror_ok ? "yes" : "no").utf8().get_data()));
	CHECK(mirror_ok);
	CHECK_EQ(int64_t(records.size()), tally.differ + tally.outside + tally.factor_off);
	MESSAGE(doctest::String(vformat("DI1Q_DUMP %s: compared %d, past the region %d, threshold class %d (limit %d), different outside the class %d, factors off by more than 1e-3 %d, worst factor difference %f", path, tally.compared, tally.past_region, tally.threshold, tally.compared / 10000, tally.outside, tally.factor_off, tally.worst_factor).utf8().get_data()));
	MESSAGE(doctest::String(vformat("DI1Q_DUMP_CLASS: class %d (g band %d; cut kept order %d, kept against dropped %d, rest key %d); differing in the class %d (g band %d; cut kept order %d, kept against dropped %d, rest key %d)", tally.threshold, tally.class_g, tally.class_cut[CUT_KEPT_ORDER], tally.class_cut[CUT_KEPT_DROPPED], tally.class_cut[CUT_REST_KEY], tally.differ, tally.differ_g, tally.differ_cut[CUT_KEPT_ORDER], tally.differ_cut[CUT_KEPT_DROPPED], tally.differ_cut[CUT_REST_KEY]).utf8().get_data()));
	MESSAGE(doctest::String(vformat("DI1Q_DUMP_DIFFER: tier %d, slot order only %d, slot set %d, factor only %d; of those flagged by a cut margin: exactly 0 %d; up to 1e-7, 0 included %d; 1e-7 to 1e-6 %d; 1e-6 to 1e-5 %d; 1e-5 to 1e-4 %d", tally.differ_tier, tally.differ_order_only, tally.differ_slot_set, tally.differ_factor_only, tally.differ_margin_zero, tally.differ_margin_bucket[0], tally.differ_margin_bucket[1], tally.differ_margin_bucket[2], tally.differ_margin_bucket[3]).utf8().get_data()));

	CHECK_EQ(tally.outside, 0);
	CHECK_EQ(tally.factor_off, 0);
	CHECK(tally.threshold * 10000 <= tally.compared);
}

Dump synthetic_dump() {
	Dump dump;
	dump.width = 33;
	dump.height = 17;
	dump.block_width = 17;
	dump.block_height = 9;
	dump.entries = 4;
	dump.rebuild = flat_rebuild();
	dump.rebuild.pixel_size = Vector2(1.0f / 33.0f, 1.0f / 17.0f);
	const int64_t pixels = int64_t(dump.width) * dump.height;
	const int64_t blocks = int64_t(dump.block_width) * dump.block_height;
	dump.guide_block.resize(blocks * 2);
	dump.slots_block.resize(blocks * dump.entries);
	dump.factors_block.resize(blocks * (dump.entries + 2));
	for (int64_t i = 0; i < blocks; i++) {
		const int bx = int(i % dump.block_width);
		const int by = int(i / dump.block_width);
		const bool lamp_free = (bx * 7 + by * 3) % 11 == 0;
		const float depth = bx < 9 ? 5.0f : 6.0f;
		const uint32_t phase = uint32_t(bx + by) & 3u;
		dump.guide_block[i * 2] = lamp_free ? 0u : ((float_bits(depth) & ~3u) | phase);
		dump.guide_block[i * 2 + 1] = oct_encode(Vector3(0.0f, 0.0f, 1.0f));
		for (uint32_t e = 0; e < dump.entries; e++) {
			dump.slots_block[e * blocks + i] = lamp_free ? 0 : uint16_t(e < 3 ? 10 + (bx + e) % 5 : 500 + by % 2);
		}
		for (uint32_t e = 0; e < dump.entries + 2; e++) {
			dump.factors_block[e * blocks + i] = uint16_t(((bx * 13 + by * 7 + e * 5) % 17) * 65535 / 16);
		}
	}
	dump.depth_full.resize(pixels);
	dump.normal_full.resize(pixels);
	for (int64_t i = 0; i < pixels; i++) {
		const int x = int(i % dump.width);
		dump.depth_full[i] = x < 18 ? 5.0f : 6.0f;
		dump.normal_full[i] = 128u | (128u << 8u) | (255u << 16u);
	}
	build_blocks(dump);
	dump.slots.resize(pixels * dump.entries);
	dump.factors.resize(pixels * (dump.entries + 2));
	dump.tier.resize(pixels);
	for (int64_t i = 0; i < pixels; i++) {
		const Resolved out = resolve(Vector2i(int(i % dump.width), int(i / dump.width)), own_surface(dump, int(i)), dump.entries, dump.rebuild, dump);
		dump.tier[i] = uint8_t(out.tier);
		for (uint32_t e = 0; e < dump.entries; e++) {
			dump.slots[e * pixels + i] = uint16_t(out.slots[e]);
		}
		for (uint32_t e = 0; e < dump.entries + 2; e++) {
			dump.factors[e * pixels + i] = uint16_t(Math::round(CLAMP(out.factors[e], 0.0f, 1.0f) * 65535.0f));
		}
	}
	return dump;
}

int clear_pixel(const Dump &p_dump) {
	for (int i = 0; i < p_dump.width * p_dump.height; i++) {
		const Vector2i pixel(i % p_dump.width, i / p_dump.width);
		const Surface own = own_surface(p_dump, i);
		float margin = 0.0f;
		const Resolved out = resolve(pixel, own, p_dump.entries, p_dump.rebuild, p_dump, &margin);
		if (out.tier == TIER_NEAR && out.slots[0] != 0 && margin > 1e-2f && !near_threshold(p_dump, pixel, own)) {
			return i;
		}
	}
	return -1;
}

TEST_CASE("[DirectQuarter] The dump comparison reads a mirror-made dump as equal and finds one changed pixel") {
	const Dump dump = synthetic_dump();
	LocalVector<MirrorRecord> same_records;
	const DumpTally same = compare_dump(dump, 0, &same_records);
	CHECK_EQ(same_records.size(), 0u);
	CHECK_EQ(same.compared, int64_t(dump.width) * dump.height);
	CHECK_EQ(same.outside, 0);
	CHECK_EQ(same.factor_off, 0);
	CHECK_EQ(same.differ, 0);
	int tier_two = 0;
	for (uint8_t t : dump.tier) {
		tier_two += t == TIER_WIDE ? 1 : 0;
	}
	CHECK(tier_two > 0);
	const int pick = clear_pixel(dump);
	REQUIRE(pick >= 0);
	Dump tier_changed = synthetic_dump();
	tier_changed.tier[pick] = tier_changed.tier[pick] == TIER_NEAR ? TIER_WIDE : TIER_NEAR;
	CHECK_EQ(compare_dump(tier_changed, 0).outside, 1);
	Dump slot_changed = synthetic_dump();
	slot_changed.slots[pick] = uint16_t(slot_changed.slots[pick] + 1);
	LocalVector<MirrorRecord> changed_records;
	CHECK_EQ(compare_dump(slot_changed, 0, &changed_records).outside, 1);
	REQUIRE_EQ(changed_records.size(), 1u);
	CHECK_EQ(changed_records[0].x, pick % dump.width);
	CHECK_EQ(changed_records[0].y, pick / dump.width);
	CHECK_EQ(int(changed_records[0].flags), 0);
	CHECK_EQ(int(changed_records[0].gpu_slots[0]), int(changed_records[0].mirror_slots[0]) + 1);
	CHECK_EQ(changed_records[0].gpu_slots[1], changed_records[0].mirror_slots[1]);
	Dump factor_changed = synthetic_dump();
	factor_changed.factors[pick] = uint16_t(factor_changed.factors[pick] > 30000 ? factor_changed.factors[pick] - 1000 : factor_changed.factors[pick] + 1000);
	CHECK_EQ(compare_dump(factor_changed, 0).factor_off, 1);
}

TEST_CASE("[DirectQuarter] Each cut kind's margin is the smallest of its own kind, and the least of them is the cut margin") {
	const Dump dump = synthetic_dump();
	int with_cut = 0;
	for (int i = 0; i < dump.width * dump.height; i++) {
		float margin = 1e30f;
		uint32_t kind = CUT_NONE;
		float kind_margins[CUT_KIND_COUNT];
		resolve(Vector2i(i % dump.width, i / dump.width), own_surface(dump, i), dump.entries, dump.rebuild, dump, &margin, &kind, kind_margins);
		CHECK_EQ(kind_margins[CUT_NONE], 1e30f);
		float least = 1e30f;
		for (uint32_t k = CUT_KEPT_ORDER; k < CUT_KIND_COUNT; k++) {
			least = MIN(least, kind_margins[k]);
		}
		CHECK_EQ(least, margin);
		if (kind != CUT_NONE) {
			with_cut++;
			CHECK_EQ(kind_margins[kind], margin);
		}
	}
	CHECK(with_cut > 0);
}

} // namespace TestDirectQuarter
