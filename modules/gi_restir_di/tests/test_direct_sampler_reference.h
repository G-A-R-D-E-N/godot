/**************************************************************************/
/*  test_direct_sampler_reference.h                                       */
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

#include "core/templates/local_vector.h"
#include "tests/test_macros.h"

#include "modules/gi_restir_di/di_sampler_reference.h"

#include <limits>

namespace TestDirectSamplerReference {

using namespace DiSamplerReference;

static int64_t rational_gcd(int64_t p_a, int64_t p_b) {
	p_a = p_a < 0 ? -p_a : p_a;
	p_b = p_b < 0 ? -p_b : p_b;
	while (p_b != 0) {
		const int64_t rest = p_a % p_b;
		p_a = p_b;
		p_b = rest;
	}
	return p_a;
}

struct Rational {
	int64_t num = 0;
	int64_t den = 1;

	Rational() {}
	Rational(int64_t p_num) :
			num(p_num) {}
	Rational(int64_t p_num, int64_t p_den) {
		if (p_den < 0) {
			p_num = -p_num;
			p_den = -p_den;
		}
		const int64_t divisor = rational_gcd(p_num, p_den);
		num = divisor > 0 ? p_num / divisor : p_num;
		den = divisor > 0 ? p_den / divisor : p_den;
		if (num == 0 && den != 0) {
			den = 1;
		}
	}

	Rational operator+(const Rational &p_other) const { return Rational(num * p_other.den + p_other.num * den, den * p_other.den); }
	Rational operator*(const Rational &p_other) const { return Rational(num * p_other.num, den * p_other.den); }
	Rational operator/(const Rational &p_other) const { return Rational(num * p_other.den, den * p_other.num); }
	Rational &operator+=(const Rational &p_other) {
		*this = *this + p_other;
		return *this;
	}
	bool operator==(const Rational &p_other) const { return num == p_other.num && den == p_other.den; }
	bool operator!=(const Rational &p_other) const { return !(*this == p_other); }
	bool is_zero() const { return num == 0; }
};

static void race_counts_double(uint32_t p_table, bool p_log_key, uint64_t *r_counts) {
	uint32_t ids[RACE_TABLE_LAMPS];
	uint32_t id_hashes[RACE_TABLE_LAMPS];
	for (uint32_t lamp = 0; lamp < RACE_TABLE_LAMPS; lamp++) {
		ids[lamp] = race_table_id(p_table, lamp);
		id_hashes[lamp] = hash(ids[lamp]);
		r_counts[lamp] = 0;
	}
	r_counts[RACE_TABLE_LAMPS] = 0;
	for (uint32_t seed = 0; seed < RACE_TABLE_SEEDS; seed++) {
		const uint32_t seed_hash = hash(seed);
		double best = Math::INF;
		uint32_t best_id = NO_PICK;
		uint32_t pick = RACE_TABLE_LAMPS;
		for (uint32_t lamp = 0; lamp < RACE_TABLE_LAMPS; lamp++) {
			const uint32_t k = hash(seed_hash ^ id_hashes[lamp]) >> 9u;
			const double target = double(RACE_TABLE_WEIGHTS[p_table][lamp]);
			const double key = p_log_key ? race_exponential_double(k) / target : race_u_double(k) / target;
			if (key < best || (key == best && ids[lamp] < best_id)) {
				best = key;
				best_id = ids[lamp];
				pick = lamp;
			}
		}
		r_counts[pick]++;
	}
}

static double race_largest_z(uint32_t p_table, const uint64_t *p_counts) {
	if (p_counts[RACE_TABLE_LAMPS] != 0) {
		return Math::INF;
	}
	double total = 0.0;
	for (uint32_t lamp = 0; lamp < RACE_TABLE_LAMPS; lamp++) {
		total += double(RACE_TABLE_WEIGHTS[p_table][lamp]);
	}
	const double seeds = double(RACE_TABLE_SEEDS);
	double largest = 0.0;
	for (uint32_t lamp = 0; lamp < RACE_TABLE_LAMPS; lamp++) {
		const double share = double(RACE_TABLE_WEIGHTS[p_table][lamp]) / total;
		const double standard_error = Math::sqrt(share * (1.0 - share) / seeds);
		largest = MAX(largest, Math::abs(double(p_counts[lamp]) / seeds - share) / standard_error);
	}
	return largest;
}

TEST_CASE("[DirectSampler] Hash mirror inverts and reaches both ends of the race's k") {
	CHECK(hash(0u) == 0u);
	for (uint32_t i = 0; i < 4096; i++) {
		const uint32_t value = i * 0x01000193u + 0x9e3779b9u;
		CHECK(hash_inverse(hash(value)) == value);
		CHECK(hash(hash_inverse(value)) == value);
	}
	const uint32_t ids[] = { 0u, 1u, race_table_id(0, 3), 0xFFFFFFFFu };
	for (const uint32_t id : ids) {
		const uint32_t seed_hashes[] = { hash(id), hash_inverse(0xFFFFFFFFu) ^ hash(id) };
		CHECK(race_k(seed_hashes[0], id) == 0u);
		CHECK(race_k(seed_hashes[1], id) == RACE_K_TOP);
		for (const uint32_t seed_hash : seed_hashes) {
			CHECK(hash(hash_inverse(seed_hash)) == seed_hash);
			const float exponential = race_exponential(seed_hash, id);
			CHECK(Math::is_finite(exponential));
			CHECK(exponential > 0.0f);
		}
	}
	CHECK(0.5f * (1.0f / 8388608.0f) > 0.0f);
	CHECK((float(RACE_K_TOP) + 0.5f) * (1.0f / 8388608.0f) < 1.0f);
	CHECK((float(16777215u) + 0.5f) * (1.0f / 16777216.0f) == 1.0f);
}

TEST_CASE("[DirectSampler] Race distribution in double precision over 2^22 seeds") {
	uint64_t counts[RACE_TABLE_LAMPS + 1];
	for (uint32_t table = 0; table < RACE_TABLE_COUNT; table++) {
		race_counts_double(table, true, counts);
		CHECK(race_largest_z(table, counts) <= RACE_TABLE_BAR_SE);
	}
	race_counts_double(0, false, counts);
	CHECK(race_largest_z(0, counts) > RACE_TABLE_BAR_SE);
}

static uint32_t full_set_pick(const float *p_targets, uint32_t p_count, float p_total, float p_u, float &r_weight) {
	Inversion inversion = inversion_begin(p_total, p_u);
	for (uint32_t i = 0; i < p_count; i++) {
		inversion_step(inversion, p_targets[i], i);
	}
	const uint32_t pick = inversion_end(inversion);
	r_weight = weight(p_total, inversion.pick_target);
	return pick;
}

static void check_full_set(const float *p_targets, uint32_t p_count, bool p_exact_weights) {
	const float grid = 1.0f / 16777216.0f;
	float total = 0.0f;
	for (uint32_t i = 0; i < p_count; i++) {
		if (candidate(p_targets[i])) {
			total += p_targets[i];
		}
	}
	int exponent = 0;
	REQUIRE(std::frexp(total, &exponent) == 0.5f);
	float running = 0.0f;
	uint64_t covered = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		if (!candidate(p_targets[i])) {
			continue;
		}
		const float low = running;
		running += p_targets[i];
		const float u_low = low / total;
		const float u_high = running / total - grid;
		REQUIRE(u_low * 16777216.0f == Math::floor(u_low * 16777216.0f));
		REQUIRE(u_high * 16777216.0f == Math::floor(u_high * 16777216.0f));
		float weight_low = 0.0f;
		float weight_high = 0.0f;
		CHECK(full_set_pick(p_targets, p_count, total, u_low, weight_low) == i);
		CHECK(full_set_pick(p_targets, p_count, total, u_high, weight_high) == i);
		CHECK(weight_low == weight_high);
		const uint64_t draws = uint64_t(double(u_high - u_low) * 16777216.0) + 1;
		CHECK(double(draws) == double(p_targets[i]) / double(total) * 16777216.0);
		covered += draws;
		const double expectation = double(draws) * double(grid) * double(weight_low);
		if (p_exact_weights) {
			CHECK(expectation == 1.0);
		} else {
			CHECK(Math::abs(expectation - 1.0) <= double(grid));
		}
	}
	CHECK(covered == 16777216u);
}

TEST_CASE("[DirectSampler] Full-set weights against the exact sum over every selection") {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	const float powers[] = { 1.0f, 0.0f, 2.0f, -1.0f, 4.0f, 1e-30f, 8.0f, 1.0f };
	const float halves[] = { 0.5f, 0.25f, 0.125f, 0.125f };
	const float single[] = { 4.0f };
	const float odd[] = { 2.0f, nan, 2.0f, inf, 4.0f };
	const float mixed[] = { 3.0f, 5.0f, 0.0f, 7.0f, 1.0f };
	const float wide[] = { 6.0f, 10.0f, 3.0f, 2.0f, 9.0f, 1.0f, 1.0f };
	check_full_set(powers, 8, true);
	check_full_set(halves, 4, true);
	check_full_set(single, 1, true);
	check_full_set(odd, 5, true);
	check_full_set(mixed, 5, false);
	check_full_set(wide, 7, false);
}

enum StratumMutant {
	STRATUM_EXACT,
	STRATUM_COUNT_OVER_STRATA,
	STRATUM_NO_SIZE,
	STRATUM_OVER_FULL,
};

static int64_t stratum_target(uint32_t p_lamp) {
	return 1 + int64_t((p_lamp * 7u + 3u) % 9u);
}

static int64_t stratum_f(uint32_t p_lamp) {
	return 1 + int64_t((p_lamp * 5u + 2u) % 7u);
}

static bool strata_hold(uint32_t p_strata, uint32_t p_count) {
	if (stratum_begin(0, p_count, p_strata) != 0 || stratum_begin(p_strata, p_count, p_strata) != p_count) {
		return false;
	}
	uint32_t smallest = p_count;
	uint32_t largest = 0;
	uint32_t total = 0;
	for (uint32_t j = 0; j < p_strata; j++) {
		const uint32_t begin = stratum_begin(j, p_count, p_strata);
		const uint32_t end = stratum_begin(j + 1, p_count, p_strata);
		if (end < begin) {
			return false;
		}
		smallest = MIN(smallest, end - begin);
		largest = MAX(largest, end - begin);
		total += end - begin;
	}
	return largest - smallest <= 1 && total == p_count;
}

static Rational stratum_w(uint32_t p_lamp, uint32_t p_size, uint32_t p_strata, uint32_t p_count, StratumMutant p_mutant) {
	const Rational target(stratum_target(p_lamp));
	if (p_mutant == STRATUM_NO_SIZE) {
		return target;
	}
	if (p_mutant == STRATUM_COUNT_OVER_STRATA) {
		return target * Rational(p_count, p_strata);
	}
	return stratum_weight(target, p_size);
}

static bool stratum_expectations_hold(uint32_t p_strata, uint32_t p_count, StratumMutant p_mutant) {
	LocalVector<uint32_t> begins;
	LocalVector<uint32_t> sizes;
	LocalVector<uint32_t> choice;
	uint64_t draws = 1;
	for (uint32_t j = 0; j < p_strata; j++) {
		begins.push_back(stratum_begin(j, p_count, p_strata));
		sizes.push_back(stratum_begin(j + 1, p_count, p_strata) - begins[j]);
		choice.push_back(0);
		draws *= sizes[j];
	}
	LocalVector<Rational> expectation;
	expectation.resize(p_count);
	const Rational draw_probability(1, int64_t(draws));
	LocalVector<Rational> w;
	w.resize(p_strata);
	for (uint64_t draw = 0; draw < draws; draw++) {
		Rational sum;
		for (uint32_t j = 0; j < p_strata; j++) {
			w[j] = stratum_w(begins[j] + choice[j], sizes[j], p_strata, p_count, p_mutant);
			sum += w[j];
		}
		for (uint32_t j = 0; j < p_strata; j++) {
			const uint32_t lamp = begins[j] + choice[j];
			Rational contribution = contribution_weight(sum, Rational(stratum_target(lamp)));
			if (p_mutant == STRATUM_OVER_FULL) {
				contribution = contribution / Rational(int64_t(p_strata));
			}
			expectation[lamp] += draw_probability * (w[j] / sum) * contribution;
		}
		for (uint32_t j = 0; j < p_strata; j++) {
			if (++choice[j] < sizes[j]) {
				break;
			}
			choice[j] = 0;
		}
	}
	for (uint32_t lamp = 0; lamp < p_count; lamp++) {
		if (expectation[lamp] != Rational(1)) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[DirectSampler] Stratum weights over every stratified draw at C_full + 1 and the added cases") {
	const uint32_t cases[][2] = { { 32, 33 }, { 1, 2 }, { 1, 5 }, { 2, 3 }, { 2, 7 }, { 3, 4 }, { 3, 9 }, { 4, 5 }, { 4, 11 }, { 8, 9 }, { 8, 19 } };
	const StratumMutant mutants[] = { STRATUM_COUNT_OVER_STRATA, STRATUM_NO_SIZE, STRATUM_OVER_FULL };
	uint32_t mutant_failures[3] = {};
	for (const auto &stratum_case : cases) {
		CHECK(strata_hold(stratum_case[0], stratum_case[1]));
		CHECK(stratum_expectations_hold(stratum_case[0], stratum_case[1], STRATUM_EXACT));
		for (uint32_t m = 0; m < 3; m++) {
			if (!stratum_expectations_hold(stratum_case[0], stratum_case[1], mutants[m])) {
				mutant_failures[m]++;
			}
		}
	}
	CHECK(stratum_begin(31, 33, 32) == 31);
	CHECK(stratum_begin(1, 3, 2) == 1);
	for (uint32_t m = 0; m < 3; m++) {
		CHECK(mutant_failures[m] > 0);
	}
}

TEST_CASE("[DirectSampler] Stratum weights at 2 C_full + 3, the separable identity over the derived draws") {
	const uint32_t strata = 32;
	const uint32_t count = 67;
	REQUIRE(strata_hold(strata, count));
	LocalVector<uint32_t> begins;
	LocalVector<uint32_t> sizes;
	for (uint32_t j = 0; j < strata; j++) {
		begins.push_back(stratum_begin(j, count, strata));
		sizes.push_back(stratum_begin(j + 1, count, strata) - begins[j]);
	}
	LocalVector<uint32_t> lamps;
	lamps.resize(strata);
	uint32_t derived = 0;
	for (uint32_t side = 0; side < 2; side++) {
		for (uint32_t j = 0; j < strata; j++) {
			for (uint32_t position = 0; position < sizes[j]; position++) {
				for (uint32_t i = 0; i < strata; i++) {
					lamps[i] = begins[i] + (i == j ? position : (side == 0 ? 0 : sizes[i] - 1));
				}
				Rational sum;
				for (uint32_t i = 0; i < strata; i++) {
					sum += stratum_weight(Rational(stratum_target(lamps[i])), sizes[i]);
				}
				Rational resampled;
				Rational separable;
				for (uint32_t i = 0; i < strata; i++) {
					const Rational target(stratum_target(lamps[i]));
					const Rational w = stratum_weight(target, sizes[i]);
					resampled += (w / sum) * contribution_weight(sum, target) * Rational(stratum_f(lamps[i]));
					separable += Rational(int64_t(sizes[i])) * Rational(stratum_f(lamps[i]));
				}
				CHECK(resampled == separable);
				derived++;
			}
		}
	}
	CHECK(derived == 134);
}

TEST_CASE("[DirectSampler] Emitter reservoir keeps visibility with the selected color") {
	const Rational floor_target(1, 1000000);
	for (int blocked = 0; blocked < 2; blocked++) {
		const Rational targets[2] = { Rational(1) + floor_target, floor_target };
		const Rational contributions[2] = { blocked ? Rational(0) : Rational(4), Rational(6) };
		const Rational total = targets[0] + targets[1];
		Rational mean;
		for (int i = 0; i < 2; i++) {
			const Rational probability = targets[i] / total;
			const Rational weight = total / (Rational(2) * targets[i]);
			mean += probability * weight * contributions[i];
		}
		CHECK(mean == (contributions[0] + contributions[1]) / Rational(2));
	}
}

TEST_CASE("[DirectSampler] Emitter donor masses retain normalized contribution") {
	const Rational masses[2] = { Rational(2), Rational(3) };
	const Rational donor_weights[2] = { Rational(3, 2), Rational(2, 3) };
	const Rational targets[2] = { Rational(7), Rational(1, 1000) };
	const Rational values[2] = { Rational(0), Rational(5) };
	const Rational candidate[2] = { masses[0] * donor_weights[0] * targets[0], masses[1] * donor_weights[1] * targets[1] };
	const Rational sum = candidate[0] + candidate[1];
	const Rational mass = masses[0] + masses[1];
	Rational selected;
	Rational expected;
	for (int i = 0; i < 2; i++) {
		selected += (candidate[i] / sum) * (sum / (mass * targets[i])) * values[i];
		expected += masses[i] * donor_weights[i] * values[i] / mass;
	}
	CHECK(selected == expected);
}

static void check_source_pdf(const int64_t *p_targets, const int64_t *p_sources, uint32_t p_count, uint32_t p_draws) {
	int64_t source_total = 0;
	for (uint32_t i = 0; i < p_count; i++) {
		source_total += p_sources[i];
	}
	LocalVector<Rational> expectation;
	expectation.resize(p_count);
	LocalVector<uint32_t> drawn;
	drawn.resize(p_draws);
	uint64_t outcomes = 1;
	for (uint32_t d = 0; d < p_draws; d++) {
		outcomes *= p_count;
		drawn[d] = 0;
	}
	for (uint64_t outcome = 0; outcome < outcomes; outcome++) {
		Rational probability(1);
		Rational sum;
		LocalVector<Rational> w;
		for (uint32_t d = 0; d < p_draws; d++) {
			const Rational source(p_sources[drawn[d]], source_total);
			probability = probability * source;
			w.push_back(source_pdf_weight(Rational(p_targets[drawn[d]]), p_draws, source));
			sum += w[d];
		}
		if (!sum.is_zero()) {
			for (uint32_t d = 0; d < p_draws; d++) {
				if (!w[d].is_zero()) {
					expectation[drawn[d]] += probability * (w[d] / sum) * contribution_weight(sum, Rational(p_targets[drawn[d]]));
				}
			}
		}
		for (uint32_t d = 0; d < p_draws; d++) {
			if (++drawn[d] < p_count) {
				break;
			}
			drawn[d] = 0;
		}
	}
	for (uint32_t i = 0; i < p_count; i++) {
		CHECK(expectation[i] == Rational(p_targets[i] > 0 ? 1 : 0));
	}
}

TEST_CASE("[DirectSampler] Source-pdf weights for i.i.d. draws from a non-uniform table") {
	const int64_t targets_a[] = { 3, 1, 0, 2, 5 };
	const int64_t sources_a[] = { 1, 4, 2, 2, 7 };
	const int64_t targets_b[] = { 7, 2, 9, 1, 4, 6 };
	const int64_t sources_b[] = { 5, 1, 3, 1, 2, 4 };
	for (uint32_t draws = 1; draws <= 2; draws++) {
		check_source_pdf(targets_a, sources_a, 5, draws);
		check_source_pdf(targets_b, sources_b, 6, draws);
	}
}

} // namespace TestDirectSamplerReference
