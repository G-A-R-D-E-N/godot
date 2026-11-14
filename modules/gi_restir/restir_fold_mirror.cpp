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
