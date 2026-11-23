/**************************************************************************/
/*  test_restir_fold_rows.h                                               */
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

#include "../restir_fold_mirror.h"
#include "../restir_test.h"

#include "core/string/ustring.h"
#include "tests/test_macros.h"

namespace TestRestirFoldRows {

uint32_t ring_gap(uint32_t p_a, uint32_t p_b) {
	return MIN(p_a - p_b, p_b - p_a);
}

TEST_CASE("[RestirFoldRows] The engine Fold constants are the shader's") {
	CHECK(RESTIR_FOLD_C1 == 0x68E31DA4u);
	CHECK(RESTIR_FOLD_C2 == 0x2C1B3C6Du);
	CHECK(RESTIR_FOLD_DIM_RENEW == 0u);
	CHECK(RESTIR_FOLD_DIM_START == 1u);
	CHECK(RESTIR_FOLD_DETERMINISTIC_SEED == 0x1234567u);
}

TEST_CASE("[RestirFoldRows] The rs_rand seed hash gives the default seeds") {
	CHECK(restir_fold_rand_seed(1u) == 0xF5F17964u);
	CHECK(restir_fold_rand_seed(0x1234567u) == 0x56F88133u);
	CHECK(restir_fold_rand_seed(1u) == restir_fold_pcg(1u * 0x9E3779B1u + 0x7F4A7C15u));
}

TEST_CASE("[RestirFoldRows] Other seeds at the defaults: C1, the eight rand2 sums, then two blocks of 512 hashed sums") {
	const Vector<uint32_t> seeds = restir_fold_other_seeds(1u, 0u);
	REQUIRE(seeds.size() == 1033);
	CHECK(seeds[0] == 0x68E31DA4u);
	for (int d = 0; d < 8; d++) {
		CHECK(seeds[1 + d] == uint32_t(1 + d));
	}
	CHECK(seeds[9] == 0xF5F17964u + 64u);
	CHECK(seeds[9 + 511] == 0xF5F17964u + 575u);
	CHECK(seeds[9 + 512] == 0x56F88133u + 64u);
	CHECK(seeds[1032] == 0x56F88133u + 575u);
	bool spaced = true;
	for (int i = 0; i < seeds.size(); i++) {
		spaced = spaced && ring_gap(RESTIR_FOLD_C2, seeds[i]) >= 5u;
		spaced = spaced && ring_gap(RESTIR_FOLD_C2 + 1u, seeds[i]) >= 5u;
	}
	CHECK(spaced);
}

TEST_CASE("[RestirFoldRows] Other seeds with a seed offset add the module's own eight rand2 sums and hash the offset seed") {
	const Vector<uint32_t> seeds = restir_fold_other_seeds(1u, 3u);
	REQUIRE(seeds.size() == 1041);
	CHECK(seeds[0] == RESTIR_FOLD_C1);
	for (int d = 0; d < 8; d++) {
		CHECK(seeds[1 + d] == uint32_t(1 + d));
		CHECK(seeds[9 + d] == uint32_t(4 + d));
	}
	CHECK(seeds[17] == restir_fold_rand_seed(4u) + 64u);
	CHECK(seeds[17 + 511] == restir_fold_rand_seed(4u) + 575u);
	CHECK(seeds[17 + 512] == 0x56F88133u + 64u);
	CHECK(seeds[1040] == 0x56F88133u + 575u);
}

TEST_CASE("[RestirFoldRows] The draw returns 0 for K below 2") {
	CHECK(restir_fold_draw(12345u, 0u) == 0u);
	CHECK(restir_fold_draw(12345u, 1u) == 0u);
}

TEST_CASE("[RestirFoldRows] n_valid sums the temporal reject histogram and cut_rej reads code 9") {
	uint32_t reject[16] = {};
	CHECK(restir_fold_n_valid(reject) == 0);
	CHECK(restir_fold_cut_rej(reject) == 0);
	for (uint32_t i = 0; i < 16u; i++) {
		reject[i] = i + 1u;
	}
	CHECK(restir_fold_n_valid(reject) == 136);
	CHECK(restir_fold_cut_rej(reject) == 10);
	reject[9] = 0xFFFFFFFFu;
	CHECK(restir_fold_n_valid(reject) == int64_t(136 - 10) + int64_t(0xFFFFFFFFu));
	CHECK(restir_fold_cut_rej(reject) == int64_t(0xFFFFFFFFu));
}

TEST_CASE("[RestirFoldRows] The row names are the 42 fields in order") {
	CHECK(RESTIR_FOLD_ROW_FIELDS == 42);
	const char *const *names = restir_fold_row_names();
	const char *const expected[RESTIR_FOLD_ROW_FIELDS] = { "record", "frame", "tag", "serial", "clock", "k", "k_eff", "mode", "feedback", "cut", "cut_reason", "pre_frame", "reval", "history_used", "trace_w", "trace_h", "n_valid", "cut_rej", "had_history", "refused", "renewed_raw", "started", "carried", "refused_empty", "hp_eq_px", "expected_phase0", "s_max", "n_over", "hist_texels", "lineage_refusals", "lineage_unknown", "lineage_families", "phase0_start", "refused_off", "start_cut", "start_frame", "start_plane", "start_normal", "start_class", "start_no_sample", "start_vis", "start_other" };
	for (int i = 0; i < RESTIR_FOLD_ROW_FIELDS; i++) {
		CHECK(String(names[i]) == String(expected[i]));
	}
}

TEST_CASE("[RestirFoldRows] A record packs to the known row, renewed exported raw") {
	RestirFrameRecord record;
	record.frame = 77u;
	record.fold.tag = 3;
	record.fold.serial = 2;
	record.fold.clock = 100;
	record.fold.k = 8;
	record.fold.k_eff = 8;
	record.fold.mode = 1;
	record.fold.feedback = 0;
	record.fold.cut = 1;
	record.fold.cut_reason = 0;
	record.fold.pre_frame = 0;
	record.fold.reval = 1;
	record.fold.history_used = 0;
	record.fold.trace_w = 640;
	record.fold.trace_h = 360;
	record.stats.temporal_reject[0] = 1000u;
	record.stats.temporal_reject[9] = 230000u;
	record.stats.temporal_reject[10] = 5u;
	record.stats.temporal_reject[13] = 12u;
	record.stats.had_history = 999u;
	record.stats.fold_refused = 12.0;
	record.stats.fold_renewed = 12.0;
	record.stats.fold_started = 230005.0;
	record.stats.fold_carried = 1000.0;
	record.stats.fold_refused_empty = 3.0;
	int64_t row[RESTIR_FOLD_ROW_FIELDS] = {};
	restir_fold_pack_row(record, 4, row);
	const int64_t expected[RESTIR_FOLD_ROW_FIELDS] = { 4, 77, 3, 2, 100, 8, 8, 1, 0, 1, 0, 0, 1, 0, 640, 360, 231017, 230000, 999, 12, 12, 230005, 1000, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
	for (int i = 0; i < RESTIR_FOLD_ROW_FIELDS; i++) {
		CHECK(row[i] == expected[i]);
	}
	CHECK(row[19] + (row[20] - row[19]) + row[21] + row[22] == row[16]);
}

TEST_CASE("[RestirFoldRows] A record without Fold fields packs -1 there and zeros for the stats") {
	RestirFrameRecord record;
	int64_t row[RESTIR_FOLD_ROW_FIELDS] = {};
	restir_fold_pack_row(record, 0, row);
	CHECK(row[0] == 0);
	CHECK(row[1] == 0);
	for (int i = 2; i <= 15; i++) {
		CHECK(row[i] == -1);
	}
	for (int i = 16; i <= 23; i++) {
		CHECK(row[i] == 0);
	}
	for (int i = 24; i < RESTIR_FOLD_ROW_FIELDS; i++) {
		CHECK(row[i] == -1);
	}
}

TEST_CASE("[RestirFoldRows] The split and start-class fields pack after lineage_families, the older fields unmoved") {
	RestirFrameRecord record;
	record.fold.hp_eq_px = 230400;
	record.fold.expected_phase0 = 28000;
	record.fold.lineage_families = 9;
	record.fold.phase0_start = 790;
	record.fold.refused_off = 70;
	record.fold.start_cut = 1;
	record.fold.start_frame = 2;
	record.fold.start_plane = 3;
	record.fold.start_normal = 4;
	record.fold.start_class = 5;
	record.fold.start_no_sample = 6;
	record.fold.start_vis = 7;
	record.fold.start_other = 8;
	int64_t row[RESTIR_FOLD_ROW_FIELDS] = {};
	restir_fold_pack_row(record, 0, row);
	CHECK(row[24] == 230400);
	CHECK(row[25] == 28000);
	CHECK(row[31] == 9);
	const int64_t tail[10] = { 790, 70, 1, 2, 3, 4, 5, 6, 7, 8 };
	for (int i = 0; i < 10; i++) {
		CHECK(row[32 + i] == tail[i]);
	}
}

} //namespace TestRestirFoldRows
