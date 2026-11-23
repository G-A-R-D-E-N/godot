/**************************************************************************/
/*  restir_fold.h                                                         */
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

#include "core/typedefs.h"

static constexpr uint32_t RESTIR_FOLD_K_MIN = 4;
static constexpr uint32_t RESTIR_FOLD_K_MAX = 64;
static constexpr uint32_t RESTIR_FOLD_F_PRE_FRAME = 524288;

struct RestirFoldFrame {
	uint32_t k = 0;
	uint32_t k_eff = 0;
	uint32_t mode = 0;
	bool feedback = false;
};

struct RestirFoldWords {
	uint32_t flags = 0;
	uint32_t clock = 0;
	uint32_t k = 0;
};

uint32_t restir_fold_k(int64_t p_setting);
uint32_t restir_fold_k_eff(uint32_t p_k, bool p_b_mode, bool p_feedback);
bool restir_reval_frame(uint32_t p_frame_index, uint32_t p_period);
bool restir_fold_pre_frame(bool p_budget, bool p_rays, uint32_t p_frame_index, uint32_t p_period);
bool restir_fold_cut(const RestirFoldFrame &p_previous, const RestirFoldFrame &p_current);
RestirFoldWords restir_fold_words(uint32_t p_flags, uint32_t p_k_eff, uint32_t p_clock, bool p_pre_frame, bool p_u1, bool p_feedback);
