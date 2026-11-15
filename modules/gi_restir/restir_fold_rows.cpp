/**************************************************************************/
/*  restir_fold_rows.cpp                                                  */
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

#include "restir_test.h"

static constexpr uint32_t RESTIR_FOLD_REJECT_CODES = 16u;
static constexpr uint32_t RESTIR_FOLD_REJ_CUT = 9u;

const char *const *restir_fold_row_names() {
	static const char *const names[RESTIR_FOLD_ROW_FIELDS] = {
		"record",
		"frame",
		"tag",
		"serial",
		"clock",
		"k",
		"k_eff",
		"mode",
		"feedback",
		"cut",
		"cut_reason",
		"pre_frame",
		"reval",
		"history_used",
		"trace_w",
		"trace_h",
		"n_valid",
		"cut_rej",
		"had_history",
		"refused",
		"renewed_raw",
		"started",
		"carried",
		"refused_empty",
		"hp_eq_px",
		"expected_phase0",
		"s_max",
		"n_over",
		"hist_texels",
		"lineage_refusals",
		"lineage_unknown",
		"lineage_families",
	};
	return names;
}

int64_t restir_fold_n_valid(const uint32_t *p_temporal_reject) {
	int64_t sum = 0;
	for (uint32_t i = 0; i < RESTIR_FOLD_REJECT_CODES; i++) {
		sum += int64_t(p_temporal_reject[i]);
	}
	return sum;
}

int64_t restir_fold_cut_rej(const uint32_t *p_temporal_reject) {
	return int64_t(p_temporal_reject[RESTIR_FOLD_REJ_CUT]);
}

void restir_fold_pack_row(const RestirFrameRecord &p_record, int64_t p_index, int64_t *r_row) {
	const RestirFoldRecord &f = p_record.fold;
	const RestirFrameStats &s = p_record.stats;
	r_row[0] = p_index;
	r_row[1] = int64_t(p_record.frame);
	r_row[2] = f.tag;
	r_row[3] = f.serial;
	r_row[4] = f.clock;
	r_row[5] = f.k;
	r_row[6] = f.k_eff;
	r_row[7] = f.mode;
	r_row[8] = f.feedback;
	r_row[9] = f.cut;
	r_row[10] = f.cut_reason;
	r_row[11] = f.pre_frame;
	r_row[12] = f.reval;
	r_row[13] = f.history_used;
	r_row[14] = f.trace_w;
	r_row[15] = f.trace_h;
	r_row[16] = restir_fold_n_valid(s.temporal_reject);
	r_row[17] = restir_fold_cut_rej(s.temporal_reject);
	r_row[18] = int64_t(s.had_history);
	r_row[19] = int64_t(s.fold_refused);
	r_row[20] = int64_t(s.fold_renewed);
	r_row[21] = int64_t(s.fold_started);
	r_row[22] = int64_t(s.fold_carried);
	r_row[23] = int64_t(s.fold_refused_empty);
	r_row[24] = f.hp_eq_px;
	r_row[25] = f.expected_phase0;
	r_row[26] = f.s_max;
	r_row[27] = f.n_over;
	r_row[28] = f.hist_texels;
	r_row[29] = f.lineage_refusals;
	r_row[30] = f.lineage_unknown;
	r_row[31] = f.lineage_families;
}
