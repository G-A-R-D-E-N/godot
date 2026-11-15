/**************************************************************************/
/*  restir_probe.cpp                                                      */
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

#include "restir_probe.h"

#include "core/variant/variant.h"

#include "restir_epochs.h"
#include "restir_fold_mirror.h"
#include "restir_settings.h"
#include "restir_harness.h"
#include "restir_test.h"

#include "shaders/restir_shim_check.glsl.gen.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "servers/rendering/renderer_rd/environment/gi_module_hooks.h"
#include "core/object/callable_mp.h"
#include "servers/rendering/rendering_server.h"

void RestirProbe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_status"), &RestirProbe::get_status);
	ClassDB::bind_method(D_METHOD("configure", "options"), &RestirProbe::configure);
	ClassDB::bind_method(D_METHOD("get_records"), &RestirProbe::get_records);
	ClassDB::bind_method(D_METHOD("reset_records"), &RestirProbe::reset_records);
	ClassDB::bind_method(D_METHOD("plan_epochs", "case"), &RestirProbe::plan_epochs);
	ClassDB::bind_method(D_METHOD("get_pixel_sums"), &RestirProbe::get_pixel_sums);
	ClassDB::bind_method(D_METHOD("reset_pixel_sums"), &RestirProbe::reset_pixel_sums);
	ClassDB::bind_method(D_METHOD("request_capture"), &RestirProbe::request_capture);
	ClassDB::bind_method(D_METHOD("get_capture"), &RestirProbe::get_capture);
	ClassDB::bind_method(D_METHOD("request_final_capture"), &RestirProbe::request_final_capture);
	ClassDB::bind_method(D_METHOD("get_final_capture"), &RestirProbe::get_final_capture);
	ClassDB::bind_method(D_METHOD("run_case", "case"), &RestirProbe::run_case);
	ClassDB::bind_method(D_METHOD("case_ready"), &RestirProbe::case_ready);
	ClassDB::bind_method(D_METHOD("get_case_result"), &RestirProbe::get_case_result);
	ClassDB::bind_method(D_METHOD("read_case_file", "path"), &RestirProbe::read_case_file);
	ClassDB::bind_method(D_METHOD("write_case_file", "path", "case"), &RestirProbe::write_case_file);
	ClassDB::bind_method(D_METHOD("get_fold_rows"), &RestirProbe::get_fold_rows);
	ClassDB::bind_method(D_METHOD("get_fold_row_fields"), &RestirProbe::get_fold_row_fields);
	ClassDB::bind_method(D_METHOD("get_fold_seeds"), &RestirProbe::get_fold_seeds);
	ClassDB::bind_method(D_METHOD("get_fold_starts"), &RestirProbe::get_fold_starts);
	ClassDB::bind_method(D_METHOD("reset_fold_starts"), &RestirProbe::reset_fold_starts);
	ClassDB::bind_method(D_METHOD("get_fold_starts_frames"), &RestirProbe::get_fold_starts_frames);
	ClassDB::bind_method(D_METHOD("mirror_fold_starts", "info", "width", "k", "clock", "pre_frame"), &RestirProbe::mirror_fold_starts);
	ClassDB::bind_method(D_METHOD("run_fold_test", "args"), &RestirProbe::run_fold_test);
	ClassDB::bind_method(D_METHOD("fold_test_ready"), &RestirProbe::fold_test_ready);
	ClassDB::bind_method(D_METHOD("get_fold_test_result"), &RestirProbe::get_fold_test_result);
	ClassDB::bind_method(D_METHOD("mirror_fold_test", "args"), &RestirProbe::mirror_fold_test);
	ClassDB::bind_method(D_METHOD("get_fold_families", "tag"), &RestirProbe::get_fold_families);
	ClassDB::bind_method(D_METHOD("get_fold_family_totals", "tag"), &RestirProbe::get_fold_family_totals);
	ClassDB::bind_method(D_METHOD("reset_fold_families"), &RestirProbe::reset_fold_families);
}

static_assert(sizeof(RestirShimCheckShaderRD) > 0, "the shim check header must be part of every build");

Dictionary RestirProbe::get_status() const {
	RendererRD::GIModuleHooks *hooks = RendererRD::GIModuleHooks::get_singleton();
	RendererRD::RtgiResampleBackend *gi = hooks->get_rtgi_backend();

	Dictionary status;
	status["module"] = "gi_restir";
	status["rtgi_registered"] = gi != nullptr;
	status["rtgi_name"] = gi ? String(gi->get_name()) : String();
	status["rtgi_active"] = gi && gi->is_active();
	if (gi) {
		Dictionary gi_status;
		gi->get_status(gi_status);
		status["rtgi_status"] = gi_status;
	}
	status["settings"] = restir_settings_snapshot();
	return status;
}

void RestirProbe::configure(const Dictionary &p_options) {
	RestirTestOptions &options = restir_test_options();
	if (p_options.has("readback")) {
		options.readback = p_options["readback"];
	}
	if (p_options.has("spatial_feedback")) {
		options.spatial_feedback = int(p_options["spatial_feedback"]);
	}
	if (p_options.has("neighbor_selection")) {
		options.neighbor_selection = int(p_options["neighbor_selection"]);
	}
	if (p_options.has("compat_k")) {
		options.compat_k = int(p_options["compat_k"]);
	}
	if (p_options.has("compat_gate")) {
		options.compat_gate = int(p_options["compat_gate"]);
	}
	if (p_options.has("compat_early_stop")) {
		options.compat_early_stop = int(p_options["compat_early_stop"]);
	}
	if (p_options.has("pixel_detail")) {
		options.pixel_detail = p_options["pixel_detail"];
	}
	if (p_options.has("history")) {
		options.history = p_options["history"];
	}
	if (p_options.has("sky")) {
		options.sky = p_options["sky"];
		options.constant_sky = true;
	}
	if (p_options.has("real_sky")) {
		options.constant_sky = !bool(p_options["real_sky"]);
	}
	if (p_options.has("freeze")) {
		options.freeze = p_options["freeze"];
	}
	if (p_options.has("d_min")) {
		options.d_min = float(p_options["d_min"]);
	}
	if (p_options.has("cos_min")) {
		options.cos_min = float(p_options["cos_min"]);
	}
	if (p_options.has("jac_max")) {
		options.jac_max = float(p_options["jac_max"]);
	}
	if (p_options.has("w_clamp")) {
		options.w_clamp = float(p_options["w_clamp"]);
	}
	if (p_options.has("vis_fail_limit")) {
		options.vis_fail_limit = int(p_options["vis_fail_limit"]);
	}
	if (p_options.has("accumulate_from")) {
		options.accumulate_from = int(p_options["accumulate_from"]);
	}
	if (p_options.has("accumulate_to")) {
		options.accumulate_to = int(p_options["accumulate_to"]);
	}
	if (p_options.has("seed_offset")) {
		options.seed_offset = uint32_t(int64_t(p_options["seed_offset"]));
	}
	if (p_options.has("rearm_dump") && bool(p_options["rearm_dump"])) {
		options.dumped = false;
	}
	if (p_options.has("fold_tag")) {
		options.fold_tag = int(p_options["fold_tag"]);
	}
	if (p_options.has("release_once")) {
		options.release_once = p_options["release_once"];
	}
	if (p_options.has("fold_dump")) {
		options.fold_dump = p_options["fold_dump"];
		if (options.fold_dump) {
			options.readback = true;
		} else {
			options.fold_lineage = false;
		}
	}
	if (p_options.has("fold_lineage")) {
		const bool wanted = p_options["fold_lineage"];
		if (wanted && !options.fold_dump) {
			options.fold_lineage = false;
			options.fold_lineage_refusal = RESTIR_FOLD_LINEAGE_REFUSAL_NO_DUMP;
		} else {
			options.fold_lineage = wanted;
			options.fold_lineage_refusal = RESTIR_FOLD_LINEAGE_REFUSAL_NONE;
		}
	}
}

PackedInt64Array RestirProbe::get_fold_families(int p_tag) const {
	return restir_fold_family_pairs(p_tag);
}

Dictionary RestirProbe::get_fold_family_totals(int p_tag) const {
	const RestirFoldFamilyTotals totals = restir_fold_family_totals(p_tag);
	Dictionary d;
	d["frames"] = int64_t(totals.frames);
	d["refusals"] = totals.refusals;
	d["unknown"] = totals.unknown;
	d["families"] = totals.families;
	return d;
}

void RestirProbe::reset_fold_families() {
	restir_fold_families_reset();
}

static PackedInt64Array _fold_test_words(const Vector<uint32_t> &p_words) {
	PackedInt64Array words;
	words.resize(p_words.size());
	int64_t *w = words.ptrw();
	for (int64_t i = 0; i < p_words.size(); i++) {
		w[i] = int64_t(p_words[i]);
	}
	return words;
}

PackedInt64Array RestirProbe::get_fold_starts() const {
	PackedInt64Array counts;
	counts.resize(int64_t(RESTIR_FOLD_START_COUNT));
	int64_t *w = counts.ptrw();
	MutexLock lock(restir_fold_starts_mutex());
	const LocalVector<uint64_t> &starts = restir_fold_starts();
	for (uint32_t i = 0; i < RESTIR_FOLD_START_COUNT; i++) {
		w[i] = starts.size() == RESTIR_FOLD_START_COUNT ? int64_t(starts[i]) : 0;
	}
	return counts;
}

void RestirProbe::reset_fold_starts() {
	MutexLock lock(restir_fold_starts_mutex());
	restir_fold_starts().clear();
	restir_fold_starts_frames() = 0;
}

int64_t RestirProbe::get_fold_starts_frames() const {
	MutexLock lock(restir_fold_starts_mutex());
	return int64_t(restir_fold_starts_frames());
}

PackedInt64Array RestirProbe::mirror_fold_starts(const PackedByteArray &p_info, int p_width, int p_k, int p_clock, bool p_pre_frame) const {
	PackedInt64Array counts;
	const int64_t texels = p_info.size() / 4;
	if (p_info.size() % 4 != 0 || p_width <= 0 || texels == 0 || texels % p_width != 0 || p_k < 4 || p_k > 64 || (p_k & (p_k - 1)) != 0 || p_clock < 0) {
		return counts;
	}
	Vector<uint32_t> words;
	words.resize(texels);
	memcpy(words.ptrw(), p_info.ptr(), size_t(texels) * sizeof(uint32_t));
	Vector<uint32_t> starts;
	restir_fold_start_mirror(words.ptr(), texels, p_width, uint32_t(p_k), uint32_t(p_clock), p_pre_frame, starts);
	return _fold_test_words(starts);
}

static bool _fold_test_arg(const Dictionary &p_args, const char *p_key, uint32_t &r_value) {
	if (!p_args.has(p_key)) {
		return true;
	}
	const int64_t value = p_args[p_key];
	if (value < 0 || value > int64_t(UINT32_MAX)) {
		return false;
	}
	r_value = uint32_t(value);
	return true;
}

static bool _fold_test_args(const Dictionary &p_args, RestirFoldTestArgs &r_args) {
	bool ok = _fold_test_arg(p_args, "sub", r_args.sub);
	ok = _fold_test_arg(p_args, "k", r_args.k) && ok;
	ok = _fold_test_arg(p_args, "count", r_args.count) && ok;
	ok = _fold_test_arg(p_args, "v0", r_args.v0) && ok;
	ok = _fold_test_arg(p_args, "grid_w", r_args.grid_w) && ok;
	ok = _fold_test_arg(p_args, "grid_h", r_args.grid_h) && ok;
	ok = _fold_test_arg(p_args, "stride", r_args.stride) && ok;
	ok = _fold_test_arg(p_args, "frames", r_args.frames) && ok;
	return ok;
}

static Dictionary _fold_test_result(const RestirFoldTestArgs &p_args, const String &p_source) {
	Dictionary result;
	result["source"] = p_source;
	result["ok"] = false;
	result["error"] = String();
	result["sub"] = int64_t(p_args.sub);
	result["k"] = int64_t(p_args.k);
	result["count"] = int64_t(p_args.count);
	result["v0"] = int64_t(p_args.v0);
	result["grid_w"] = int64_t(p_args.grid_w);
	result["grid_h"] = int64_t(p_args.grid_h);
	result["stride"] = int64_t(p_args.stride);
	result["frames"] = int64_t(p_args.frames);
	result["word_count"] = int64_t(0);
	result["words"] = PackedInt64Array();
	result["mirror_mismatches"] = int64_t(-1);
	result["first_mismatch"] = int64_t(-1);
	return result;
}

void RestirProbe::run_fold_test(const Dictionary &p_args) {
	{
		MutexLock lock(result_mutex);
		fold_test_done = false;
		fold_test_result = Dictionary();
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &RestirProbe::_run_fold_test_render).bind(p_args));
}

void RestirProbe::_run_fold_test_render(const Dictionary &p_args) {
	if (!harness_core) {
		harness_core = memnew(RestirGiCore);
	}
	RestirFoldTestArgs args;
	const bool in_range = _fold_test_args(p_args, args);
	Dictionary result = _fold_test_result(args, "gpu");
	Vector<uint32_t> gpu;
	String error = "argument out of range";
	if (in_range && harness_core->run_fold_test(args, gpu, error)) {
		Vector<uint32_t> cpu;
		restir_fold_test_mirror(args, cpu);
		int64_t first = -1;
		result["mirror_mismatches"] = restir_fold_test_compare(gpu, cpu, first);
		result["first_mismatch"] = first;
		result["word_count"] = int64_t(gpu.size());
		result["words"] = _fold_test_words(gpu);
		result["ok"] = true;
	} else {
		result["error"] = error;
	}
	MutexLock lock(result_mutex);
	fold_test_result = result;
	fold_test_done = true;
}

bool RestirProbe::fold_test_ready() const {
	MutexLock lock(result_mutex);
	return fold_test_done;
}

Dictionary RestirProbe::get_fold_test_result() const {
	MutexLock lock(result_mutex);
	return fold_test_result;
}

Dictionary RestirProbe::mirror_fold_test(const Dictionary &p_args) const {
	RestirFoldTestArgs args;
	const bool in_range = _fold_test_args(p_args, args);
	Dictionary result = _fold_test_result(args, "mirror");
	const char *invalid = in_range ? restir_fold_test_invalid(args) : "argument out of range";
	if (invalid) {
		result["error"] = String(invalid);
		return result;
	}
	Vector<uint32_t> cpu;
	restir_fold_test_mirror(args, cpu);
	result["mirror_mismatches"] = int64_t(0);
	result["word_count"] = int64_t(cpu.size());
	result["words"] = _fold_test_words(cpu);
	result["ok"] = true;
	return result;
}

void RestirProbe::reset_records() {
	MutexLock lock(restir_frame_records_mutex());
	restir_frame_records().clear();
}

PackedInt64Array RestirProbe::get_fold_rows() const {
	LocalVector<RestirFrameRecord> records;
	{
		MutexLock lock(restir_frame_records_mutex());
		records = restir_frame_records();
	}
	PackedInt64Array rows;
	rows.resize(int(records.size()) * RESTIR_FOLD_ROW_FIELDS);
	int64_t *w = rows.ptrw();
	for (uint32_t i = 0; i < records.size(); i++) {
		restir_fold_pack_row(records[i], int64_t(i), w + int64_t(i) * RESTIR_FOLD_ROW_FIELDS);
	}
	return rows;
}

PackedStringArray RestirProbe::get_fold_row_fields() const {
	PackedStringArray fields;
	const char *const *names = restir_fold_row_names();
	for (int i = 0; i < RESTIR_FOLD_ROW_FIELDS; i++) {
		fields.push_back(String(names[i]));
	}
	return fields;
}

Dictionary RestirProbe::get_fold_seeds() const {
	const uint32_t rtgi_seed = uint32_t(int(GLOBAL_GET("rendering/global_illumination/rtgi/seed")));
	const uint32_t seed_offset = restir_test_options().seed_offset;
	const bool deterministic = GLOBAL_GET("rendering/global_illumination/restir/gi/deterministic");
	const Vector<uint32_t> others = restir_fold_other_seeds(rtgi_seed, seed_offset);
	PackedInt64Array other_seeds;
	other_seeds.resize(others.size());
	for (int i = 0; i < others.size(); i++) {
		other_seeds.set(i, int64_t(others[i]));
	}
	Dictionary result;
	result["c1_seed"] = int64_t(RESTIR_FOLD_C1);
	result["c2_seed"] = int64_t(RESTIR_FOLD_C2);
	result["other_seeds"] = other_seeds;
	result["rtgi_seed"] = int64_t(rtgi_seed);
	result["seed_offset"] = int64_t(seed_offset);
	result["deterministic"] = deterministic;
	result["rand_seed_live"] = int64_t(restir_fold_rand_seed(deterministic ? RESTIR_FOLD_DETERMINISTIC_SEED : rtgi_seed + seed_offset));
	return result;
}

Dictionary RestirProbe::get_records() const {
	LocalVector<RestirFrameRecord> records;
	{
		MutexLock lock(restir_frame_records_mutex());
		records = restir_frame_records();
	}
	const int stride = 35;
	PackedFloat64Array data;
	data.resize(int(records.size()) * stride);
	double *w = data.ptrw();
	for (uint32_t i = 0; i < records.size(); i++) {
		const RestirFrameStats &s = records[i].stats;
		double *r = w + int(i) * stride;
		r[0] = records[i].frame;
		r[1] = s.geometry;
		r[2] = s.output_pixels;
		r[3] = s.output_sum[0];
		r[4] = s.output_sum[1];
		r[5] = s.output_sum[2];
		r[6] = s.candidate_pixels;
		r[7] = s.candidate_sum[0];
		r[8] = s.candidate_sum[1];
		r[9] = s.candidate_sum[2];
		r[10] = s.m_sum;
		r[11] = s.clamped;
		r[12] = s.blocked;
		r[13] = s.temporal_selected;
		r[14] = s.spatial_selected;
		r[15] = s.violations;
		r[16] = s.has_sample;
		r[17] = s.shift_tested;
		r[18] = s.rej_distance;
		r[19] = s.rej_cos;
		r[20] = s.rej_jacobian;
		r[21] = s.energy_all;
		r[22] = s.energy_truncated;
		r[23] = s.pdf_refused;
		r[24] = s.techniques_present;
		r[25] = s.techniques_sample_unusable;
		r[26] = s.spatial_combines;
		r[27] = s.lo_refused;
		r[28] = s.no_canonical;
		r[29] = s.temporal_shift_tested;
		r[30] = s.temporal_shift_rejected;
		r[31] = s.compat_taps;
		r[32] = s.compat_early_stops;
		r[33] = s.compat_empty;
		r[34] = s.compat_h_sum;
	}
	Dictionary result;
	result["stride"] = stride;
	result["count"] = int64_t(records.size());
	result["data"] = data;
	return result;
}

static RendererRD::GIEpochs::Snapshot _snapshot_from(const Dictionary &p_dict) {
	RendererRD::GIEpochs::Snapshot s = {};
	s.scene = uint32_t(int64_t(p_dict.get("scene", 1)));
	s.light = uint32_t(int64_t(p_dict.get("light", 1)));
	s.sky = uint32_t(int64_t(p_dict.get("sky", 1)));
	s.origin = uint32_t(int64_t(p_dict.get("origin", 1)));
	return s;
}

Dictionary RestirProbe::plan_epochs(const Dictionary &p_case) const {
	const RendererRD::GIEpochs::Snapshot current = _snapshot_from(p_case.get("current", Dictionary()));
	RestirEpochInputs inputs;
	inputs.have_history = bool(p_case.get("have_history", true));
	inputs.have_epochs = bool(p_case.get("have_epochs", true));
	inputs.history_cut = bool(p_case.get("history_cut", false));
	inputs.hardware_changed = bool(p_case.get("hardware_changed", false));
	inputs.fold_changed = bool(p_case.get("fold_changed", false));
	inputs.previous = _snapshot_from(p_case.get("previous", Dictionary()));
	inputs.current = current;
	inputs.anchor = p_case.get("anchor", Vector3());
	inputs.light_soft_frames = uint32_t(int64_t(p_case.get("light_soft_frames", 0)));
	inputs.m_light_frames = uint32_t(int64_t(p_case.get("m_light_frames", 4)));
	RestirEpochPlan plan;
	restir_plan_epochs(inputs, plan);
	PackedFloat64Array boxes;
	for (uint32_t i = 0; i < plan.dirty_count; i++) {
		boxes.push_back(plan.dirty_min[i].x);
		boxes.push_back(plan.dirty_min[i].y);
		boxes.push_back(plan.dirty_min[i].z);
		boxes.push_back(plan.dirty_max[i].x);
		boxes.push_back(plan.dirty_max[i].y);
		boxes.push_back(plan.dirty_max[i].z);
	}
	Dictionary result;
	result["cut"] = plan.cut;
	result["reason"] = int64_t(plan.reason);
	result["dirty_count"] = int64_t(plan.dirty_count);
	result["dirty_boxes"] = boxes;
	result["scene_changed"] = plan.scene_changed;
	result["light_changed"] = plan.light_changed;
	result["sky_changed"] = plan.sky_changed;
	result["origin_changed"] = plan.origin_changed;
	result["light_soft_frames"] = int64_t(plan.light_soft_frames);
	return result;
}

void RestirProbe::reset_pixel_sums() {
	restir_pixel_sums() = RestirPixelSums();
}

Dictionary RestirProbe::get_pixel_sums() const {
	const RestirPixelSums &sums = restir_pixel_sums();
	PackedFloat64Array output_luma;
	PackedFloat64Array covered;
	PackedFloat64Array valid;
	output_luma.resize(int(sums.output_luma.size()));
	covered.resize(int(sums.covered.size()));
	valid.resize(int(sums.valid.size()));
	for (uint32_t i = 0; i < sums.output_luma.size(); i++) {
		output_luma.set(int(i), sums.output_luma[i]);
		covered.set(int(i), sums.covered[i]);
		valid.set(int(i), sums.valid[i]);
	}
	Dictionary result;
	result["width"] = sums.size.x;
	result["height"] = sums.size.y;
	result["frames"] = int64_t(sums.frames);
	result["output_luma"] = output_luma;
	result["covered"] = covered;
	result["valid"] = valid;
	static const char *const detail_names[RESTIR_DETAIL_COUNT] = { "cand_luma", "cand_n", "out_temporal", "out_spatial", "n_temporal", "n_spatial", "blocked" };
	for (uint32_t d = 0; d < RESTIR_DETAIL_COUNT; d++) {
		PackedFloat64Array values;
		values.resize(int(sums.detail[d].size()));
		for (uint32_t i = 0; i < sums.detail[d].size(); i++) {
			values.set(int(i), sums.detail[d][i]);
		}
		result[detail_names[d]] = values;
	}
	PackedFloat64Array reject_temporal;
	PackedFloat64Array reject_spatial;
	reject_temporal.resize(int(sums.reject_temporal.size()));
	reject_spatial.resize(int(sums.reject_spatial.size()));
	for (uint32_t i = 0; i < sums.reject_temporal.size(); i++) {
		reject_temporal.set(int(i), sums.reject_temporal[i]);
		reject_spatial.set(int(i), sums.reject_spatial[i]);
	}
	result["reject_temporal"] = reject_temporal;
	result["reject_spatial"] = reject_spatial;
	return result;
}

void RestirProbe::request_capture() {
	restir_captured_case() = Dictionary();
	restir_test_options().capture = true;
}

void RestirProbe::request_final_capture() {
	restir_final_capture() = Dictionary();
	restir_test_options().capture_final = true;
}

Dictionary RestirProbe::get_final_capture() const {
	return restir_final_capture();
}

Dictionary RestirProbe::get_capture() const {
	return restir_captured_case();
}

void RestirProbe::run_case(const Dictionary &p_case) {
	{
		MutexLock lock(result_mutex);
		case_done = false;
		case_result = Dictionary();
	}
	RenderingServer::get_singleton()->call_on_render_thread(callable_mp(this, &RestirProbe::_run_case_render).bind(p_case));
}

void RestirProbe::_run_case_render(const Dictionary &p_case) {
	if (!harness_core) {
		harness_core = memnew(RestirGiCore);
	}
	Dictionary result = RestirHarness::run_case(*harness_core, p_case);
	MutexLock lock(result_mutex);
	case_result = result;
	case_done = true;
}

bool RestirProbe::case_ready() const {
	MutexLock lock(result_mutex);
	return case_done;
}

Dictionary RestirProbe::get_case_result() const {
	MutexLock lock(result_mutex);
	return case_result;
}

Dictionary RestirProbe::read_case_file(const String &p_path) const {
	return RestirHarness::read_case(p_path);
}

void RestirProbe::write_case_file(const String &p_path, const Dictionary &p_case) const {
	RestirHarness::write_case(p_path, p_case);
}

RestirProbe::~RestirProbe() {
	if (harness_core) {
		memdelete(harness_core);
		harness_core = nullptr;
	}
}
