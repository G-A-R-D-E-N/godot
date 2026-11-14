/**************************************************************************/
/*  restir_fold_mirror.h                                                  */
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

#include "core/templates/vector.h"
#include "core/typedefs.h"

static constexpr uint32_t RESTIR_FOLD_C1 = 0x68E31DA4u;
static constexpr uint32_t RESTIR_FOLD_C2 = 0x2C1B3C6Du;
static constexpr uint32_t RESTIR_FOLD_DIM_RENEW = 0u;
static constexpr uint32_t RESTIR_FOLD_DIM_START = 1u;
static constexpr uint32_t RESTIR_FOLD_DETERMINISTIC_SEED = 0x1234567u;
static constexpr uint32_t RESTIR_FOLD_RAND2_SUMS = 8u;
static constexpr uint32_t RESTIR_FOLD_RAND_DIM_FIRST = 64u;
static constexpr uint32_t RESTIR_FOLD_RAND_DIMS = 512u;

uint32_t restir_fold_pcg(uint32_t p_value);
uint32_t restir_fold_rng_state(uint32_t p_x, uint32_t p_y, uint32_t p_frame, uint32_t p_dimension, uint32_t p_seed);
uint32_t restir_fold_phi(uint32_t p_x, uint32_t p_y);
uint32_t restir_fold_isqrt_from(uint32_t p_seed, uint32_t p_x);
uint32_t restir_fold_isqrt(uint32_t p_x);
uint32_t restir_fold_draw(uint32_t p_psi, uint32_t p_k);
uint32_t restir_fold_rand_seed(uint32_t p_seed);
Vector<uint32_t> restir_fold_other_seeds(uint32_t p_rtgi_seed, uint32_t p_seed_offset);
