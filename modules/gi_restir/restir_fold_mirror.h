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

#include "core/templates/hash_map.h"
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

static constexpr uint32_t RESTIR_FOLD_DUMP_BINDING = 32u;
static constexpr uint32_t RESTIR_FOLD_SLOT_HP_EQ_PX = 27u;
static constexpr uint32_t RESTIR_FOLD_SLOT_PHASE0 = 28u;
static constexpr uint32_t RESTIR_FOLD_SLOT_PHASE0_START = 29u;
static constexpr uint32_t RESTIR_FOLD_SLOT_REFUSED_OFF = 30u;
static constexpr uint32_t RESTIR_FOLD_START_CUT = 0u;
static constexpr uint32_t RESTIR_FOLD_START_FRAME = 1u;
static constexpr uint32_t RESTIR_FOLD_START_PLANE = 2u;
static constexpr uint32_t RESTIR_FOLD_START_NORMAL = 3u;
static constexpr uint32_t RESTIR_FOLD_START_CLASS = 4u;
static constexpr uint32_t RESTIR_FOLD_START_NO_SAMPLE = 5u;
static constexpr uint32_t RESTIR_FOLD_START_VIS = 6u;
static constexpr uint32_t RESTIR_FOLD_START_OTHER = 7u;
static constexpr uint32_t RESTIR_FOLD_START_PRE = 8u;
static constexpr uint32_t RESTIR_FOLD_START_CLASSES = 16u;
static constexpr uint32_t RESTIR_FOLD_START_S = 64u;
static constexpr uint32_t RESTIR_FOLD_START_U = 64u;
static constexpr uint32_t RESTIR_FOLD_START_COUNT = RESTIR_FOLD_START_CLASSES * RESTIR_FOLD_START_S * RESTIR_FOLD_START_U;
static constexpr uint32_t RESTIR_FOLD_DUMP_HEADER_WORDS = 4u;
static constexpr uint32_t RESTIR_FOLD_DUMP_BYTES = (RESTIR_FOLD_DUMP_HEADER_WORDS + RESTIR_FOLD_START_COUNT) * 4u;
static constexpr uint32_t RESTIR_FOLD_REJ_FRAME_CODE = 1u;
static constexpr uint32_t RESTIR_FOLD_REJ_PLANE_CODE = 2u;
static constexpr uint32_t RESTIR_FOLD_REJ_NORMAL_CODE = 3u;
static constexpr uint32_t RESTIR_FOLD_REJ_CLASS_CODE = 4u;
static constexpr uint32_t RESTIR_FOLD_REJ_CUT_CODE = 9u;
static constexpr uint32_t RESTIR_FOLD_REJ_NO_SAMPLE_CODE = 10u;
static constexpr uint32_t RESTIR_FOLD_REJ_VIS_CODE = 11u;
static constexpr uint32_t RESTIR_FOLD_REJ_CODES = 16u;
static constexpr uint32_t RESTIR_FOLD_INFO_VALID = 1u << 14u;

static constexpr uint32_t RESTIR_FOLD_LINEAGE_IN_BINDING = 33u;
static constexpr uint32_t RESTIR_FOLD_LINEAGE_OUT_BINDING = 34u;
static constexpr uint32_t RESTIR_FOLD_REFUSED_ID_BINDING = 35u;
static constexpr uint32_t RESTIR_FOLD_LINEAGE_PIXEL_MASK = 0x00FFFFFFu;
static constexpr uint32_t RESTIR_FOLD_LINEAGE_CLOCK_SHIFT = 24u;
static constexpr uint32_t RESTIR_FOLD_LINEAGE_CLOCK_MASK = 0xFFu;
static constexpr uint32_t RESTIR_FOLD_LINEAGE_UNKNOWN = 0xFF000000u;
static constexpr uint64_t RESTIR_FOLD_LINEAGE_MAX_TEXELS = RESTIR_FOLD_LINEAGE_PIXEL_MASK;

enum RestirFoldLineageRefusal : int32_t {
	RESTIR_FOLD_LINEAGE_REFUSAL_NONE = 0,
	RESTIR_FOLD_LINEAGE_REFUSAL_NO_DUMP = 1,
	RESTIR_FOLD_LINEAGE_REFUSAL_GRID = 2,
	RESTIR_FOLD_LINEAGE_REFUSAL_UNAVAILABLE = 3,
};

struct RestirFoldFamilyCount {
	int64_t refusals = 0;
	int64_t unknown = 0;
	int64_t families = 0;
};

enum RestirFoldTestSub : uint32_t {
	RESTIR_FOLD_TEST_DRAW = 0,
	RESTIR_FOLD_TEST_PHI = 1,
	RESTIR_FOLD_TEST_PSI = 2,
	RESTIR_FOLD_TEST_ISQRT = 3,
	RESTIR_FOLD_TEST_MAX = 4,
};

static constexpr uint32_t RESTIR_FOLD_TEST_GROUP = 64u;
static constexpr uint32_t RESTIR_FOLD_TEST_GROUPS_X = 1024u;
static constexpr uint64_t RESTIR_FOLD_TEST_MAX_THREADS = uint64_t(1) << 24u;
static constexpr uint64_t RESTIR_FOLD_TEST_MAX_WORDS = uint64_t(1) << 22u;

struct RestirFoldTestArgs {
	uint32_t sub = RESTIR_FOLD_TEST_DRAW;
	uint32_t k = 8u;
	uint32_t count = 0u;
	uint32_t v0 = 0u;
	uint32_t grid_w = 0u;
	uint32_t grid_h = 0u;
	uint32_t stride = 1u;
	uint32_t frames = 1u;
};

uint32_t restir_fold_pcg(uint32_t p_value);
uint32_t restir_fold_rng_state(uint32_t p_x, uint32_t p_y, uint32_t p_frame, uint32_t p_dimension, uint32_t p_seed);
uint32_t restir_fold_phi(uint32_t p_x, uint32_t p_y);
uint32_t restir_fold_isqrt_from(uint32_t p_seed, uint32_t p_x);
uint32_t restir_fold_isqrt(uint32_t p_x);
uint32_t restir_fold_draw(uint32_t p_psi, uint32_t p_k);
uint32_t restir_fold_rand_seed(uint32_t p_seed);
Vector<uint32_t> restir_fold_other_seeds(uint32_t p_rtgi_seed, uint32_t p_seed_offset);

uint32_t restir_fold_start_class(uint32_t p_reason, bool p_pre_frame);
uint32_t restir_fold_start_index(uint32_t p_class, uint32_t p_s, uint32_t p_u);
bool restir_fold_start_sums(const Vector<uint32_t> &p_starts, int64_t *r_sums);
void restir_fold_start_mirror(const uint32_t *p_info, int64_t p_count, int64_t p_width, uint32_t p_k, uint32_t p_clock, bool p_pre_frame, Vector<uint32_t> &r_starts);

const char *restir_fold_test_invalid(const RestirFoldTestArgs &p_args);
uint64_t restir_fold_test_threads(const RestirFoldTestArgs &p_args);
uint64_t restir_fold_test_words(const RestirFoldTestArgs &p_args);
void restir_fold_test_initial(const RestirFoldTestArgs &p_args, Vector<uint32_t> &r_words);
void restir_fold_test_mirror(const RestirFoldTestArgs &p_args, Vector<uint32_t> &r_words);
int64_t restir_fold_test_compare(const Vector<uint32_t> &p_a, const Vector<uint32_t> &p_b, int64_t &r_first);

uint32_t restir_fold_lineage_pixel(uint32_t p_x, uint32_t p_y, uint32_t p_width);
uint32_t restir_fold_lineage_id(uint32_t p_pixel_word, uint32_t p_clock_word);
RestirFoldFamilyCount restir_fold_families(const uint32_t *p_ids, int64_t p_count, HashMap<int, int64_t> &r_sizes);
const char *restir_fold_lineage_refusal_text(int32_t p_code);
