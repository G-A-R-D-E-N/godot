/**************************************************************************/
/*  di_sampler_reference.h                                                */
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
#include "core/typedefs.h"

#include <cmath>

namespace DiSamplerReference {

inline constexpr uint32_t NO_PICK = 0xFFFFFFFFu;
inline constexpr float MIN_TARGET = 5.421011e-20f;
inline constexpr uint32_t RACE_K_TOP = 8388607u;

constexpr uint32_t hash(uint32_t p_value) {
	p_value ^= p_value >> 16u;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15u;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16u;
	return p_value;
}

constexpr uint32_t odd_inverse(uint32_t p_value) {
	uint32_t inverse = p_value;
	for (int i = 0; i < 5; i++) {
		inverse *= 2u - p_value * inverse;
	}
	return inverse;
}

constexpr uint32_t hash_inverse(uint32_t p_value) {
	p_value ^= p_value >> 16u;
	p_value *= odd_inverse(0x846ca68bu);
	p_value ^= (p_value >> 15u) ^ (p_value >> 30u);
	p_value *= odd_inverse(0x7feb352du);
	p_value ^= p_value >> 16u;
	return p_value;
}

constexpr bool candidate(float p_target) {
	return p_target >= MIN_TARGET && !Math::is_inf(p_target);
}

inline float weight(float p_total, float p_pick_target) {
	return p_pick_target > 0.0f ? p_total / p_pick_target : 0.0f;
}

inline float tail_u(uint32_t p_hash) {
	return float(p_hash >> 8u) / 16777216.0f;
}

struct Inversion {
	float goal = 0.0f;
	float running = 0.0f;
	uint32_t pick = NO_PICK;
	float pick_target = 0.0f;
	uint32_t last = NO_PICK;
	float last_target = 0.0f;
};

inline Inversion inversion_begin(float p_total, float p_u) {
	Inversion inversion;
	inversion.goal = p_u * p_total;
	inversion.running = 0.0f;
	inversion.pick = NO_PICK;
	inversion.pick_target = 0.0f;
	inversion.last = NO_PICK;
	inversion.last_target = 0.0f;
	return inversion;
}

inline void inversion_step(Inversion &r_inversion, float p_target, uint32_t p_index) {
	if (!candidate(p_target)) {
		return;
	}
	r_inversion.running += p_target;
	r_inversion.last = p_index;
	r_inversion.last_target = p_target;
	if (r_inversion.pick == NO_PICK && r_inversion.running > r_inversion.goal) {
		r_inversion.pick = p_index;
		r_inversion.pick_target = p_target;
	}
}

inline uint32_t inversion_end(Inversion &r_inversion) {
	if (r_inversion.pick == NO_PICK && r_inversion.last != NO_PICK) {
		r_inversion.pick = r_inversion.last;
		r_inversion.pick_target = r_inversion.last_target;
	}
	return r_inversion.pick;
}

constexpr uint32_t race_k(uint32_t p_seed_hash, uint32_t p_id) {
	return hash(p_seed_hash ^ hash(p_id)) >> 9u;
}

inline float precise_divide(float p_a, float p_b) {
	float quotient = p_a / p_b;
	float residual = std::fma(-quotient, p_b, p_a);
	float corrected = std::fma(residual, 1.0f / p_b, quotient);
	return corrected;
}

inline float race_exponential(uint32_t p_seed_hash, uint32_t p_id) {
	uint32_t k = hash(p_seed_hash ^ hash(p_id)) >> 9u;
	if (k < 4194304u) {
		return -Math::log((float(k) + 0.5f) * (1.0f / 8388608.0f));
	}
	float v = (8388607.5f - float(k)) * (1.0f / 8388608.0f);
	float t = v / (2.0f - v);
	float t2 = t * t;
	float series = 1.0f / 13.0f;
	series = std::fma(series, t2, 1.0f / 11.0f);
	series = std::fma(series, t2, 1.0f / 9.0f);
	series = std::fma(series, t2, 1.0f / 7.0f);
	series = std::fma(series, t2, 1.0f / 5.0f);
	series = std::fma(series, t2, 1.0f / 3.0f);
	series = std::fma(series, t2, 1.0f);
	return 2.0f * t * series;
}

inline double race_u_double(uint32_t p_k) {
	return (double(p_k) + 0.5) * (1.0 / 8388608.0);
}

inline double race_exponential_double(uint32_t p_k) {
	return -std::log(race_u_double(p_k));
}

constexpr uint32_t stratum_begin(uint32_t p_stratum, uint32_t p_count, uint32_t p_strata) {
	return uint32_t((uint64_t(p_stratum) * p_count) / p_strata);
}

template <typename T>
T stratum_weight(const T &p_target, uint32_t p_size) {
	return p_target * T(int64_t(p_size));
}

template <typename T>
T contribution_weight(const T &p_sum, const T &p_target) {
	return p_sum / p_target;
}

template <typename T>
T source_pdf_weight(const T &p_target, uint32_t p_draws, const T &p_source_pdf) {
	return p_target / (T(int64_t(p_draws)) * p_source_pdf);
}

inline constexpr uint32_t RACE_TABLE_COUNT = 2;
inline constexpr uint32_t RACE_TABLE_LAMPS = 8;
inline constexpr uint32_t RACE_TABLE_SEEDS = 1u << 22;
inline constexpr double RACE_TABLE_BAR_SE = 4.0;
inline constexpr float RACE_TABLE_WEIGHTS[RACE_TABLE_COUNT][RACE_TABLE_LAMPS] = {
	{ 0.0001f, 0.00037275937f, 0.0013894955f, 0.0051794747f, 0.019306977f, 0.071968567f, 0.26826958f, 1.0f },
	{ 0.5f, 0.0002f, 1.0f, 0.03f, 0.0001f, 0.25f, 0.007f, 0.6f },
};

constexpr uint32_t race_table_id(uint32_t p_table, uint32_t p_lamp) {
	return hash(0x4b413800u + p_table * 16u + p_lamp);
}

} // namespace DiSamplerReference
