/**************************************************************************/
/*  restir_fold.cpp                                                       */
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

#include "restir_fold.h"

uint32_t restir_fold_k(int64_t p_setting) {
	if (p_setting <= 0) {
		return 0;
	}
	const int64_t wanted = MIN(p_setting, int64_t(RESTIR_FOLD_K_MAX));
	uint32_t k = RESTIR_FOLD_K_MIN;
	while (int64_t(k) * 2 <= wanted) {
		k *= 2;
	}
	return k;
}

uint32_t restir_fold_k_eff(uint32_t p_k, bool p_b_mode, bool p_feedback) {
	if (!p_b_mode || p_feedback) {
		return 0;
	}
	return restir_fold_k(int64_t(p_k));
}

bool restir_reval_frame(uint32_t p_frame_index, uint32_t p_period) {
	if (p_period < 2u) {
		return false;
	}
	return p_frame_index % p_period == p_period - 1u;
}

bool restir_fold_pre_frame(bool p_budget, bool p_rays, uint32_t p_frame_index, uint32_t p_period) {
	if (!p_budget || !p_rays) {
		return false;
	}
	return restir_reval_frame(p_frame_index + 1u, p_period);
}

bool restir_fold_cut(const RestirFoldFrame &p_previous, const RestirFoldFrame &p_current) {
	if (p_previous.k_eff != p_current.k_eff) {
		return true;
	}
	if (p_previous.k == 0u && p_current.k == 0u) {
		return false;
	}
	return p_previous.mode != p_current.mode || p_previous.feedback != p_current.feedback;
}

RestirFoldWords restir_fold_words(uint32_t p_flags, uint32_t p_k_eff, uint32_t p_clock, bool p_pre_frame, bool p_u1, bool p_feedback) {
	RestirFoldWords words;
	words.k = restir_fold_k_eff(p_k_eff, !p_u1, p_feedback);
	words.clock = words.k != 0u ? p_clock : 0u;
	words.flags = (p_flags & ~RESTIR_FOLD_F_PRE_FRAME) | ((words.k != 0u && p_pre_frame) ? RESTIR_FOLD_F_PRE_FRAME : 0u);
	return words;
}
