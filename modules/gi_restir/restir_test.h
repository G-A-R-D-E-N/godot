/**************************************************************************/
/*  restir_test.h                                                         */
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

#include "restir_gi_core.h"

#include "core/math/color.h"
#include "core/os/mutex.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"

struct RestirTestOptions {
	bool readback = false;
	bool history = true;
	bool force_software = false;
	Color sky = Color(1, 1, 1, 1);
	bool constant_sky = false;
	uint32_t seed_offset = 0;
	bool capture = false;
	bool capture_final = false;
	bool freeze = false;
	bool dumped = false;
	uint32_t dump_after = 24;
	float d_min = -1.0f;
	float cos_min = -1.0f;
	float jac_max = -1.0f;
	float w_clamp = -1.0f;
	int vis_fail_limit = -1;
	int accumulate_from = -1;
	int accumulate_to = -1;
	bool pixel_detail = false;
	int spatial_feedback = -1;
	int neighbor_selection = -1;
	int compat_k = -1;
	int compat_gate = -1;
	int compat_early_stop = -1;
	int fold_tag = 0;
	bool release_once = false;
	bool fold_dump = false;
};

enum RestirDetail : uint32_t {
	RESTIR_DETAIL_CAND,
	RESTIR_DETAIL_CAND_N,
	RESTIR_DETAIL_OUT_TEMPORAL,
	RESTIR_DETAIL_OUT_SPATIAL,
	RESTIR_DETAIL_N_TEMPORAL,
	RESTIR_DETAIL_N_SPATIAL,
	RESTIR_DETAIL_BLOCKED,
	RESTIR_DETAIL_COUNT,
};

struct RestirPixelSums {
	Size2i size;
	LocalVector<double> output_luma;
	LocalVector<double> covered;
	LocalVector<double> valid;
	LocalVector<double> detail[RESTIR_DETAIL_COUNT];
	LocalVector<double> reject_temporal;
	LocalVector<double> reject_spatial;
	uint32_t frames = 0;
};

struct RestirFoldRecord {
	int64_t tag = -1;
	int64_t serial = -1;
	int64_t clock = -1;
	int64_t k = -1;
	int64_t k_eff = -1;
	int64_t mode = -1;
	int64_t feedback = -1;
	int64_t cut = -1;
	int64_t cut_reason = -1;
	int64_t pre_frame = -1;
	int64_t reval = -1;
	int64_t history_used = -1;
	int64_t trace_w = -1;
	int64_t trace_h = -1;
	int64_t hp_eq_px = -1;
	int64_t expected_phase0 = -1;
	int64_t s_max = -1;
	int64_t n_over = -1;
	int64_t hist_texels = -1;
};

struct RestirFrameRecord {
	uint32_t frame = 0;
	RestirFrameStats stats;
	RestirFoldRecord fold;
};

static constexpr int RESTIR_FOLD_ROW_FIELDS = 29;

const char *const *restir_fold_row_names();
int64_t restir_fold_n_valid(const uint32_t *p_temporal_reject);
int64_t restir_fold_cut_rej(const uint32_t *p_temporal_reject);
void restir_fold_pack_row(const RestirFrameRecord &p_record, int64_t p_index, int64_t *r_row);

RestirTestOptions &restir_test_options();
LocalVector<RestirFrameRecord> &restir_frame_records();
Mutex &restir_frame_records_mutex();
LocalVector<uint64_t> &restir_fold_starts();
Mutex &restir_fold_starts_mutex();
uint64_t &restir_fold_starts_frames();
void restir_fold_starts_add(const Vector<uint32_t> &p_starts);
Dictionary &restir_captured_case();
Dictionary &restir_final_capture();
RestirPixelSums &restir_pixel_sums();
