/**************************************************************************/
/*  restir_fold_mirror.cpp                                                */
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

#include "restir_fold_mirror.h"

#include "core/math/math_funcs.h"

static_assert(RESTIR_FOLD_START_CLASSES == 2u * RESTIR_FOLD_START_PRE, "Every start class has one pre-frame twin");
static_assert(RESTIR_FOLD_START_OTHER + 1u == RESTIR_FOLD_START_PRE, "OTHER is the last plain start class");

uint32_t restir_fold_pcg(uint32_t p_value) {
	const uint32_t state = p_value * 747796405u + 2891336453u;
	const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint32_t restir_fold_rng_state(uint32_t p_x, uint32_t p_y, uint32_t p_frame, uint32_t p_dimension, uint32_t p_seed) {
	return restir_fold_pcg(p_x + restir_fold_pcg(p_y + restir_fold_pcg(p_frame + restir_fold_pcg(p_dimension + p_seed))));
}

uint32_t restir_fold_phi(uint32_t p_x, uint32_t p_y) {
	return restir_fold_pcg(p_x + restir_fold_pcg(p_y + RESTIR_FOLD_C1));
}

uint32_t restir_fold_isqrt_from(uint32_t p_seed, uint32_t p_x) {
	uint32_t s = p_seed;
	if ((s + 1u) * (s + 1u) <= p_x) {
		s++;
	}
	if (s * s > p_x) {
		s--;
	}
	return s;
}

uint32_t restir_fold_isqrt(uint32_t p_x) {
	return restir_fold_isqrt_from(uint32_t(Math::sqrt(float(p_x))), p_x);
}

uint32_t restir_fold_draw(uint32_t p_psi, uint32_t p_k) {
	const uint32_t quarter = (p_k * p_k) >> 2u;
	if (quarter == 0u) {
		return 0u;
	}
	const uint32_t x = p_psi % (3u * quarter);
	if (x < quarter) {
		return restir_fold_isqrt(x);
	}
	return (p_k >> 1u) + (x - quarter) / p_k;
}

uint32_t restir_fold_rand_seed(uint32_t p_seed) {
	return restir_fold_pcg(p_seed * 0x9E3779B1u + 0x7F4A7C15u);
}

Vector<uint32_t> restir_fold_other_seeds(uint32_t p_rtgi_seed, uint32_t p_seed_offset) {
	Vector<uint32_t> seeds;
	seeds.push_back(RESTIR_FOLD_C1);
	for (uint32_t d = 0u; d < RESTIR_FOLD_RAND2_SUMS; d++) {
		seeds.push_back(p_rtgi_seed + d);
	}
	const uint32_t module_seed = p_rtgi_seed + p_seed_offset;
	if (p_seed_offset != 0u) {
		for (uint32_t d = 0u; d < RESTIR_FOLD_RAND2_SUMS; d++) {
			seeds.push_back(module_seed + d);
		}
	}
	const uint32_t hashed[2] = { restir_fold_rand_seed(module_seed), restir_fold_rand_seed(RESTIR_FOLD_DETERMINISTIC_SEED) };
	for (const uint32_t seed : hashed) {
		for (uint32_t d = 0u; d < RESTIR_FOLD_RAND_DIMS; d++) {
			seeds.push_back(seed + RESTIR_FOLD_RAND_DIM_FIRST + d);
		}
	}
	return seeds;
}

uint32_t restir_fold_start_class(uint32_t p_reason, bool p_pre_frame) {
	uint32_t base = RESTIR_FOLD_START_OTHER;
	switch (p_reason) {
		case RESTIR_FOLD_REJ_CUT_CODE:
			base = RESTIR_FOLD_START_CUT;
			break;
		case RESTIR_FOLD_REJ_FRAME_CODE:
			base = RESTIR_FOLD_START_FRAME;
			break;
		case RESTIR_FOLD_REJ_PLANE_CODE:
			base = RESTIR_FOLD_START_PLANE;
			break;
		case RESTIR_FOLD_REJ_NORMAL_CODE:
			base = RESTIR_FOLD_START_NORMAL;
			break;
		case RESTIR_FOLD_REJ_CLASS_CODE:
			base = RESTIR_FOLD_START_CLASS;
			break;
		case RESTIR_FOLD_REJ_NO_SAMPLE_CODE:
			base = RESTIR_FOLD_START_NO_SAMPLE;
			break;
		case RESTIR_FOLD_REJ_VIS_CODE:
			base = RESTIR_FOLD_START_VIS;
			break;
		default:
			break;
	}
	return base + (p_pre_frame ? RESTIR_FOLD_START_PRE : 0u);
}

uint32_t restir_fold_start_index(uint32_t p_class, uint32_t p_s, uint32_t p_u) {
	return (p_class * RESTIR_FOLD_START_S + p_s) * RESTIR_FOLD_START_U + p_u;
}

bool restir_fold_start_sums(const Vector<uint32_t> &p_starts, int64_t *r_sums) {
	for (uint32_t c = 0u; c < RESTIR_FOLD_START_PRE; c++) {
		r_sums[c] = -1;
	}
	if (p_starts.size() != int64_t(RESTIR_FOLD_START_COUNT)) {
		return false;
	}
	for (uint32_t c = 0u; c < RESTIR_FOLD_START_PRE; c++) {
		r_sums[c] = 0;
	}
	const uint32_t *w = p_starts.ptr();
	constexpr uint32_t class_words = RESTIR_FOLD_START_S * RESTIR_FOLD_START_U;
	for (uint32_t c = 0u; c < RESTIR_FOLD_START_CLASSES; c++) {
		int64_t sum = 0;
		const uint32_t *row = w + restir_fold_start_index(c, 0u, 0u);
		for (uint32_t i = 0u; i < class_words; i++) {
			sum += int64_t(row[i]);
		}
		r_sums[c % RESTIR_FOLD_START_PRE] += sum;
	}
	return true;
}

void restir_fold_start_mirror(const uint32_t *p_info, int64_t p_count, int64_t p_width, uint32_t p_k, uint32_t p_clock, bool p_pre_frame, Vector<uint32_t> &r_starts) {
	r_starts.clear();
	r_starts.resize(int64_t(RESTIR_FOLD_START_COUNT));
	r_starts.fill(0u);
	if (!p_info || p_width <= 0 || p_k < 4u || p_k > 64u || (p_k & (p_k - 1u)) != 0u) {
		return;
	}
	uint32_t *w = r_starts.ptrw();
	const uint32_t start_class = restir_fold_start_class(RESTIR_FOLD_REJ_CUT_CODE, p_pre_frame);
	for (int64_t i = 0; i < p_count; i++) {
		if ((p_info[i] & RESTIR_FOLD_INFO_VALID) == 0u) {
			continue;
		}
		const uint32_t x = uint32_t(i % p_width);
		const uint32_t y = uint32_t(i / p_width);
		uint32_t s = restir_fold_draw(restir_fold_rng_state(x, y, p_clock, RESTIR_FOLD_DIM_START, RESTIR_FOLD_C2), p_k);
		if (p_pre_frame) {
			s = MIN(s, p_k - 2u);
		}
		const uint32_t u = (p_clock + restir_fold_phi(x, y)) % p_k;
		w[restir_fold_start_index(start_class, s, u)]++;
	}
}

const char *restir_fold_test_invalid(const RestirFoldTestArgs &p_args) {
	if (p_args.sub >= RESTIR_FOLD_TEST_MAX) {
		return "sub must be 0 to 3";
	}
	if (p_args.k < 4u || p_args.k > 64u || (p_args.k & (p_args.k - 1u)) != 0u) {
		return "k must be a power of two from 4 to 64";
	}
	const uint64_t grid = uint64_t(p_args.grid_w) * uint64_t(p_args.grid_h);
	if (p_args.sub != RESTIR_FOLD_TEST_PHI && p_args.count == 0u) {
		return "count must be above 0";
	}
	if ((p_args.sub == RESTIR_FOLD_TEST_PHI || p_args.sub == RESTIR_FOLD_TEST_PSI) && grid == 0u) {
		return "grid_w and grid_h must be above 0";
	}
	if (p_args.sub == RESTIR_FOLD_TEST_PSI) {
		if (p_args.frames == 0u || p_args.stride == 0u) {
			return "frames and stride must be above 0";
		}
		if (uint64_t(p_args.count - 1u) * uint64_t(p_args.stride) >= grid) {
			return "the last sampled pixel lies outside the grid";
		}
	}
	if (restir_fold_test_threads(p_args) > RESTIR_FOLD_TEST_MAX_THREADS) {
		return "too many threads";
	}
	if (restir_fold_test_words(p_args) > RESTIR_FOLD_TEST_MAX_WORDS) {
		return "too many output words";
	}
	return nullptr;
}

uint64_t restir_fold_test_threads(const RestirFoldTestArgs &p_args) {
	switch (p_args.sub) {
		case RESTIR_FOLD_TEST_PHI:
			return uint64_t(p_args.grid_w) * uint64_t(p_args.grid_h);
		case RESTIR_FOLD_TEST_PSI:
			return uint64_t(p_args.count) * uint64_t(p_args.frames);
		default:
			return uint64_t(p_args.count);
	}
}

uint64_t restir_fold_test_words(const RestirFoldTestArgs &p_args) {
	switch (p_args.sub) {
		case RESTIR_FOLD_TEST_PHI:
			return uint64_t(p_args.k);
		case RESTIR_FOLD_TEST_PSI:
			return uint64_t(p_args.count) * uint64_t(p_args.frames);
		case RESTIR_FOLD_TEST_ISQRT:
			return 2u;
		default:
			return uint64_t(p_args.count);
	}
}

void restir_fold_test_initial(const RestirFoldTestArgs &p_args, Vector<uint32_t> &r_words) {
	r_words.clear();
	if (restir_fold_test_invalid(p_args)) {
		return;
	}
	r_words.resize(int64_t(restir_fold_test_words(p_args)));
	r_words.fill(0u);
	if (p_args.sub == RESTIR_FOLD_TEST_ISQRT) {
		r_words.set(1, 0xFFFFFFFFu);
	}
}

void restir_fold_test_mirror(const RestirFoldTestArgs &p_args, Vector<uint32_t> &r_words) {
	restir_fold_test_initial(p_args, r_words);
	if (r_words.is_empty()) {
		return;
	}
	uint32_t *w = r_words.ptrw();
	const uint32_t k = p_args.k;
	const uint32_t threads = uint32_t(restir_fold_test_threads(p_args));
	for (uint32_t t = 0u; t < threads; t++) {
		switch (p_args.sub) {
			case RESTIR_FOLD_TEST_DRAW: {
				w[t] = restir_fold_draw(t, k);
			} break;
			case RESTIR_FOLD_TEST_PHI: {
				w[restir_fold_phi(t % p_args.grid_w, t / p_args.grid_w) % k]++;
			} break;
			case RESTIR_FOLD_TEST_PSI: {
				const uint32_t v = p_args.v0 + t % p_args.frames;
				const uint32_t linear = (t / p_args.frames) * p_args.stride;
				const uint32_t x = linear % p_args.grid_w;
				const uint32_t y = linear / p_args.grid_w;
				const uint32_t phi = restir_fold_phi(x, y) % k;
				const uint32_t psi_r = restir_fold_rng_state(x, y, v, RESTIR_FOLD_DIM_RENEW, RESTIR_FOLD_C2) % k;
				const uint32_t s = restir_fold_draw(restir_fold_rng_state(x, y, v, RESTIR_FOLD_DIM_START, RESTIR_FOLD_C2), k);
				w[t] = phi | (psi_r << 8u) | (s << 16u);
			} break;
			case RESTIR_FOLD_TEST_ISQRT: {
				const uint32_t s = restir_fold_isqrt(t);
				if (!(s * s <= t && t < (s + 1u) * (s + 1u))) {
					w[0]++;
					w[1] = MIN(w[1], t);
				}
			} break;
			default:
				break;
		}
	}
}

int64_t restir_fold_test_compare(const Vector<uint32_t> &p_a, const Vector<uint32_t> &p_b, int64_t &r_first) {
	r_first = -1;
	const int64_t common = MIN(p_a.size(), p_b.size());
	int64_t mismatches = MAX(p_a.size(), p_b.size()) - common;
	const uint32_t *a = p_a.ptr();
	const uint32_t *b = p_b.ptr();
	for (int64_t i = 0; i < common; i++) {
		if (a[i] != b[i]) {
			if (r_first < 0) {
				r_first = i;
			}
			mismatches++;
		}
	}
	if (r_first < 0 && mismatches > 0) {
		r_first = common;
	}
	return mismatches;
}

uint32_t restir_fold_lineage_pixel(uint32_t p_x, uint32_t p_y, uint32_t p_width) {
	return p_y * p_width + p_x + 1u;
}

uint32_t restir_fold_lineage_id(uint32_t p_pixel_word, uint32_t p_clock_word) {
	if (p_pixel_word == 0u || p_pixel_word > RESTIR_FOLD_LINEAGE_PIXEL_MASK) {
		return RESTIR_FOLD_LINEAGE_UNKNOWN;
	}
	return ((p_clock_word & RESTIR_FOLD_LINEAGE_CLOCK_MASK) << RESTIR_FOLD_LINEAGE_CLOCK_SHIFT) | p_pixel_word;
}

RestirFoldFamilyCount restir_fold_families(const uint32_t *p_ids, int64_t p_count, HashMap<int, int64_t> &r_sizes) {
	RestirFoldFamilyCount count;
	if (!p_ids || p_count <= 0) {
		return count;
	}
	HashMap<uint32_t, int> members;
	for (int64_t i = 0; i < p_count; i++) {
		const uint32_t id = p_ids[i];
		if (id == 0u) {
			continue;
		}
		count.refusals++;
		if ((id & RESTIR_FOLD_LINEAGE_PIXEL_MASK) == 0u) {
			count.unknown++;
			continue;
		}
		int *seen = members.getptr(id);
		if (seen) {
			(*seen)++;
		} else {
			members.insert(id, 1);
		}
	}
	for (const KeyValue<uint32_t, int> &entry : members) {
		int64_t *sized = r_sizes.getptr(entry.value);
		if (sized) {
			(*sized)++;
		} else {
			r_sizes.insert(entry.value, 1);
		}
		count.families++;
	}
	return count;
}

const char *restir_fold_lineage_refusal_text(int32_t p_code) {
	switch (p_code) {
		case RESTIR_FOLD_LINEAGE_REFUSAL_NONE:
			return "";
		case RESTIR_FOLD_LINEAGE_REFUSAL_NO_DUMP:
			return "fold_lineage needs fold_dump on";
		case RESTIR_FOLD_LINEAGE_REFUSAL_GRID:
			return "fold_lineage needs a trace grid of at most 16777215 texels";
		case RESTIR_FOLD_LINEAGE_REFUSAL_UNAVAILABLE:
			return "fold_lineage images or shaders are not available";
		default:
			return "fold_lineage refused for an unknown reason";
	}
}
