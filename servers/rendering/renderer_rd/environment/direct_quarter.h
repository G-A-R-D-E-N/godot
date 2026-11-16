/**************************************************************************/
/*  direct_quarter.h                                                      */
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

#include "core/math/math_funcs.h"
#include "core/math/vector2.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"

#include <cstdint>

namespace RendererRD::DirectQuarter {

static constexpr int MAX_ENTRIES = 8;
static constexpr int MAX_USED = 16;
static constexpr int MAX_MERGED = 32;
static constexpr int MAX_CANDIDATES = 9;
static constexpr float SURFACE_MIN = 0.25f;

enum Tier : uint32_t {
	TIER_NONE = 0,
	TIER_NEAR = 1,
	TIER_WIDE = 2,
	TIER_EMPTY = 3,
};

struct Surface {
	float depth = 0.0f;
	Vector3 normal;
	Vector3 position;

	bool is_sky() const { return depth <= 0.0f; }
};

struct Rebuild {
	float inv00 = 1.0f;
	float inv11 = 1.0f;
	float inv23 = 1.0f;
	float inv30 = 0.0f;
	float inv31 = 0.0f;
	Vector2 pixel_size = Vector2(1.0f, 1.0f);

	Vector3 position(const Vector2i &p_pixel, float p_depth) const {
		const Vector2 uv = (Vector2(p_pixel) + Vector2(0.5f, 0.5f)) * pixel_size * 2.0f - Vector2(1.0f, 1.0f);
		const Vector2 at_unit = Vector2(inv00 * uv.x, inv11 * uv.y);
		if (inv23 == 0.0f) {
			return Vector3(at_unit.x + inv30, at_unit.y + inv31, -p_depth);
		}
		return Vector3(at_unit.x * p_depth, at_unit.y * p_depth, -p_depth);
	}
};

struct Block {
	uint32_t phase = 0;
	Surface guide;
	uint32_t slots[MAX_ENTRIES] = {};
	float factors[MAX_ENTRIES + 2] = {};

	bool is_empty() const { return guide.is_sky(); }
};

struct Resolved {
	uint32_t tier = TIER_NONE;
	uint32_t slots[MAX_ENTRIES] = {};
	float factors[MAX_ENTRIES + 2] = {};
	bool cut = false;
};

struct Candidate {
	Vector2i block;
	float weight = 0.0f;
};

inline Vector2i block_grid(const Vector2i &p_size) {
	return Vector2i((p_size.x + 1) / 2, (p_size.y + 1) / 2);
}

inline Vector2i block_pixel(const Vector2i &p_block, uint32_t p_phase) {
	return p_block * 2 + Vector2i(int(p_phase & 1u), int(p_phase >> 1u));
}

inline uint32_t parity(const Vector2i &p_block) {
	return uint32_t(p_block.x + p_block.y + (p_block.y >> 1)) & 1u;
}

inline float surface_weight(const Surface &p_center, const Surface &p_other) {
	const float plane = Math::abs(p_center.normal.dot(p_other.position - p_center.position));
	const float facing = MAX(p_center.normal.dot(p_other.normal), 0.0f);
	return Math::exp(-plane / MAX(0.01f * p_center.depth, 1e-4f)) * facing * facing;
}

inline int representative(const Vector2i &p_block, const Surface p_pixels[4]) {
	int nearest = -1;
	for (int k = 0; k < 4; k++) {
		if (!p_pixels[k].is_sky() && (nearest < 0 || p_pixels[k].depth < p_pixels[nearest].depth)) {
			nearest = k;
		}
	}
	if (nearest < 0) {
		return -1;
	}
	bool on_surface[4];
	bool smooth_block = true;
	for (int k = 0; k < 4; k++) {
		on_surface[k] = !p_pixels[k].is_sky() && surface_weight(p_pixels[nearest], p_pixels[k]) >= SURFACE_MIN;
		smooth_block = smooth_block && (p_pixels[k].is_sky() || on_surface[k]);
	}
	const bool want_surface = parity(p_block) == 0;
	for (int k = 0; k < 4; k++) {
		if (!p_pixels[k].is_sky() && (smooth_block || on_surface[k] == want_surface)) {
			return k;
		}
	}
	return nearest;
}

inline float tent(const Vector2 &p_gap) {
	return MAX(0.0f, 1.0f - 0.5f * Math::abs(p_gap.x)) * MAX(0.0f, 1.0f - 0.5f * Math::abs(p_gap.y));
}

inline Vector2i bracket(const Vector2i &p_pixel) {
	return Vector2i((p_pixel.x - 1) >> 1, (p_pixel.y - 1) >> 1);
}

template <typename BlockAt>
Resolved resolve(const Vector2i &p_pixel, const Surface &p_own, uint32_t p_entries, const Rebuild &p_rebuild, const BlockAt &p_block_at, float *r_cut_margin = nullptr) {
	Resolved out;
	const uint32_t own_limit = p_entries - 1;
	float margin = 1e30f;
	if (r_cut_margin != nullptr) {
		*r_cut_margin = margin;
	}
	for (uint32_t i = 0; i < own_limit + 3; i++) {
		out.factors[i] = 1.0f;
	}
	if (p_own.is_sky()) {
		out.tier = TIER_NONE;
		return out;
	}
	Surface own = p_own;
	own.position = p_rebuild.position(p_pixel, own.depth);
	const Vector2 center = Vector2(p_pixel) + Vector2(0.5f, 0.5f);
	const Block *used[MAX_USED];
	float used_weight[MAX_USED];
	int used_count = 0;
	const Vector2i near = bracket(p_pixel);
	for (int wide = 0; wide < 2 && used_count == 0; wide++) {
		const int from = wide == 0 ? 0 : -1;
		const int reach = wide == 0 ? 1 : 2;
		for (int dy = from; dy <= reach; dy++) {
			for (int dx = from; dx <= reach; dx++) {
				const Vector2i k = near + Vector2i(dx, dy);
				const Block *block = p_block_at(k);
				if (block == nullptr || block->is_empty()) {
					continue;
				}
				const Vector2i represented = block_pixel(k, block->phase);
				Surface other = block->guide;
				other.position = p_rebuild.position(represented, other.depth);
				const float g = surface_weight(own, other);
				if (g < SURFACE_MIN) {
					continue;
				}
				const Vector2 gap = Vector2(represented) + Vector2(0.5f, 0.5f) - center;
				float weight;
				if (wide == 0) {
					weight = tent(gap) * g;
				} else {
					const float d2 = gap.dot(gap);
					weight = d2 <= 9.0f ? g / (1.0f + d2) : 0.0f;
				}
				if (weight <= 0.0f) {
					continue;
				}
				used[used_count] = block;
				used_weight[used_count] = weight;
				used_count++;
			}
		}
		if (used_count > 0) {
			out.tier = wide == 0 ? TIER_NEAR : TIER_WIDE;
		}
	}
	if (used_count == 0) {
		out.tier = TIER_EMPTY;
		return out;
	}

	uint32_t merged_slot[MAX_MERGED];
	float merged_score[MAX_MERGED];
	uint32_t merged_count = 0;
	bool overflow = false;
	for (int u = 0; u < used_count; u++) {
		for (uint32_t r = 0; r < own_limit; r++) {
			const uint32_t slot = used[u]->slots[r];
			if (slot == 0) {
				break;
			}
			uint32_t found = merged_count;
			for (uint32_t m = 0; m < merged_count; m++) {
				if (merged_slot[m] == slot) {
					found = m;
					break;
				}
			}
			if (found == merged_count) {
				if (merged_count == uint32_t(MAX_MERGED)) {
					overflow = true;
					continue;
				}
				merged_slot[merged_count] = slot;
				merged_score[merged_count] = 0.0f;
				merged_count++;
			}
			merged_score[found] += used_weight[u] * float(own_limit - r);
		}
	}
	const uint32_t kept = MIN(merged_count, own_limit);
	for (uint32_t i = 0; i < kept; i++) {
		uint32_t best = i;
		for (uint32_t m = i + 1; m < merged_count; m++) {
			if (merged_score[m] > merged_score[best] || (merged_score[m] == merged_score[best] && merged_slot[m] < merged_slot[best])) {
				best = m;
			}
		}
		SWAP(merged_slot[i], merged_slot[best]);
		SWAP(merged_score[i], merged_score[best]);
	}
	for (uint32_t i = 0; i + 1 < kept; i++) {
		margin = MIN(margin, (merged_score[i] - merged_score[i + 1]) / MAX(merged_score[i], 1e-20f));
	}
	for (uint32_t m = kept; m < merged_count && kept > 0; m++) {
		margin = MIN(margin, (merged_score[kept - 1] - merged_score[m]) / MAX(merged_score[kept - 1], 1e-20f));
	}
	for (uint32_t i = 0; i < own_limit; i++) {
		const uint32_t slot = i < kept ? merged_slot[i] : 0;
		out.slots[i] = slot;
		if (slot == 0) {
			continue;
		}
		float sum = 0.0f;
		float total = 0.0f;
		for (int u = 0; u < used_count; u++) {
			for (uint32_t r = 0; r < own_limit && used[u]->slots[r] != 0; r++) {
				if (used[u]->slots[r] == slot) {
					sum += used_weight[u] * used[u]->factors[r];
					total += used_weight[u];
					break;
				}
			}
		}
		out.factors[i] = total > 0.0f ? sum / total : 1.0f;
	}

	uint32_t rest_key = 0;
	float rest_weight = 0.0f;
	float key_sums[MAX_USED];
	for (int u = 0; u < used_count; u++) {
		key_sums[u] = 0.0f;
		const uint32_t key = used[u]->slots[own_limit];
		if (key == 0) {
			continue;
		}
		float summed = 0.0f;
		for (int v = 0; v < used_count; v++) {
			if (used[v]->slots[own_limit] == key) {
				summed += used_weight[v];
			}
		}
		key_sums[u] = summed;
		if (summed > rest_weight || (summed == rest_weight && key < rest_key)) {
			rest_key = key;
			rest_weight = summed;
		}
	}
	for (int u = 0; u < used_count; u++) {
		if (used[u]->slots[own_limit] != 0 && used[u]->slots[own_limit] != rest_key) {
			margin = MIN(margin, (rest_weight - key_sums[u]) / MAX(rest_weight, 1e-20f));
		}
	}
	if (r_cut_margin != nullptr) {
		*r_cut_margin = margin;
	}

	out.slots[own_limit] = rest_key;
	if (rest_key != 0) {
		float rest_sum[3] = { 0.0f, 0.0f, 0.0f };
		float rest_total = 0.0f;
		for (int u = 0; u < used_count; u++) {
			if (used[u]->slots[own_limit] != rest_key) {
				continue;
			}
			for (int c = 0; c < 3; c++) {
				rest_sum[c] += used_weight[u] * used[u]->factors[own_limit + c];
			}
			rest_total += used_weight[u];
		}
		for (int c = 0; c < 3; c++) {
			out.factors[own_limit + c] = rest_total > 0.0f ? rest_sum[c] / rest_total : 1.0f;
		}
	}
	out.cut = merged_count > own_limit || overflow;
	return out;
}

inline Vector2i candidate_low(const Vector2 &p_x) {
	return Vector2i(int(Math::floor((p_x.x - 3.5f) * 0.5f)) + 1, int(Math::floor((p_x.y - 3.5f) * 0.5f)) + 1);
}

inline Vector2i candidate_high(const Vector2 &p_x) {
	return Vector2i(int(Math::ceil((p_x.x + 1.5f) * 0.5f)) - 1, int(Math::ceil((p_x.y + 1.5f) * 0.5f)) - 1);
}

template <typename History>
int history_candidates(const Vector2 &p_x, const Vector2i &p_own_block, const Vector2i &p_own_pixel, const Vector2i &p_grid, const Vector2i &p_full_size, const History &p_history, Candidate r_candidates[MAX_CANDIDATES], float &r_total) {
	r_total = 0.0f;
	if (!(p_x.x >= -4.0f && p_x.y >= -4.0f && p_x.x <= float(p_full_size.x) + 4.0f && p_x.y <= float(p_full_size.y) + 4.0f)) {
		return 0;
	}
	uint32_t own_phase = 0;
	const Vector2 own_center = Vector2(p_own_pixel) + Vector2(0.5f, 0.5f);
	if (Math::abs(p_x.x - own_center.x) <= 0.01f && Math::abs(p_x.y - own_center.y) <= 0.01f && p_history(p_own_block, own_phase)) {
		r_candidates[0].block = p_own_block;
		r_candidates[0].weight = 1.0f;
		r_total = 1.0f;
		return 1;
	}
	int count = 0;
	const Vector2i low = candidate_low(p_x);
	const Vector2i high = candidate_high(p_x);
	for (int ky = low.y; ky <= high.y; ky++) {
		for (int kx = low.x; kx <= high.x; kx++) {
			const Vector2i k(kx, ky);
			if (count >= MAX_CANDIDATES || k.x < 0 || k.y < 0 || k.x >= p_grid.x || k.y >= p_grid.y) {
				continue;
			}
			uint32_t phase = 0;
			if (!p_history(k, phase)) {
				continue;
			}
			const float weight = tent(Vector2(block_pixel(k, phase)) + Vector2(0.5f, 0.5f) - p_x);
			if (weight <= 0.0f) {
				continue;
			}
			r_candidates[count].block = k;
			r_candidates[count].weight = weight;
			r_total += weight;
			count++;
		}
	}
	return count;
}

template <typename AccumOf>
Vector2 blend_history(const Candidate p_candidates[MAX_CANDIDATES], int p_count, float p_total, const AccumOf &p_accum_of) {
	Vector2 sum;
	if (p_count == 0 || p_total <= 0.0f) {
		return sum;
	}
	for (int c = 0; c < p_count; c++) {
		Vector2 accum;
		if (p_accum_of(p_candidates[c].block, accum)) {
			sum += accum * p_candidates[c].weight;
		}
	}
	return sum / p_total;
}

} // namespace RendererRD::DirectQuarter
