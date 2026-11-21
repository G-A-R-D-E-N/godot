/**************************************************************************/
/*  restir_harness.cpp                                                    */
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

#include "restir_harness.h"

#include "restir_test.h"

#include "core/crypto/crypto_core.h"
#include "core/io/file_access.h"
#include "core/variant/typed_array.h"

namespace RestirHarness {

static String _hash(const Vector<uint8_t> &p_data) {
	unsigned char digest[32];
	CryptoCore::sha256(p_data.ptr(), p_data.size(), digest);
	String hex;
	for (int i = 0; i < 32; i++) {
		hex += String::num_int64(digest[i], 16).lpad(2, "0");
	}
	return hex;
}

Dictionary params_to_dict(const RestirFrameParams &p) {
	Dictionary d;
	d["size"] = Vector2i(p.trace_size);
	d["frame_index"] = int64_t(p.frame_index);
	d["seed"] = int64_t(p.seed);
	d["flags"] = int64_t(p.flags);
	d["mode"] = int64_t(p.mode);
	d["spatial_count"] = int64_t(p.spatial_count);
	d["spatial_radius"] = int64_t(p.spatial_radius);
	d["spatial_passes"] = int64_t(p.spatial_passes);
	d["m_temporal_cap"] = int64_t(p.m_temporal_cap);
	d["m_max"] = int64_t(p.m_max);
	d["n_target"] = int64_t(p.n_target);
	d["epoch_tag"] = int64_t(p.epoch_tag);
	d["debug_view"] = int64_t(p.debug_view);
	d["vis_fail_limit"] = int64_t(p.vis_fail_limit);
	d["spatial_feedback"] = p.spatial_feedback;
	d["compat"] = p.compat;
	d["compat_k"] = int64_t(p.compat_k);
	d["compat_gate"] = int64_t(p.compat_gate);
	d["compat_early_stop"] = p.compat_early_stop;
	d["camera_basis"] = p.camera_basis;
	d["previous_camera_basis"] = p.previous_camera_basis;
	d["camera_to_anchor"] = p.camera_to_anchor;
	d["previous_camera_to_anchor"] = p.previous_camera_to_anchor;
	d["view_to_previous_clip"] = p.view_to_previous_clip;
	d["jitter_shift"] = PackedFloat32Array({ p.jitter_shift[0], p.jitter_shift[1], p.jitter_shift[2], p.jitter_shift[3] });
	d["previous_jitter_shift"] = PackedFloat32Array({ p.previous_jitter_shift[0], p.previous_jitter_shift[1], p.previous_jitter_shift[2], p.previous_jitter_shift[3] });
	d["test_sky"] = p.test_sky;
	d["tol_plane"] = p.tol_plane;
	d["cos_normal"] = p.cos_normal;
	d["jac_max"] = p.jac_max;
	d["d_min"] = p.d_min;
	d["cos_min"] = p.cos_min;
	d["w_clamp"] = p.w_clamp;
	d["origin_scale"] = p.origin_scale;
	d["origin_floor"] = p.origin_floor;
	d["sky_distance"] = p.sky_distance;
	d["class_tol"] = p.class_tol;
	d["emission_exposure"] = p.emission_exposure;
	d["prev_epoch_tag"] = int64_t(p.prev_epoch_tag);
	d["dirty_count"] = int64_t(p.dirty_count);
	d["dirty_margin"] = p.dirty_margin;
	PackedFloat64Array boxes;
	for (uint32_t i = 0; i < MIN(p.dirty_count, 64u); i++) {
		boxes.push_back(p.dirty_min[i].x);
		boxes.push_back(p.dirty_min[i].y);
		boxes.push_back(p.dirty_min[i].z);
		boxes.push_back(p.dirty_max[i].x);
		boxes.push_back(p.dirty_max[i].y);
		boxes.push_back(p.dirty_max[i].z);
	}
	d["dirty_boxes"] = boxes;
	d["clock"] = int64_t(p.fold_clock);
	d["k_eff"] = int64_t(p.fold_k_eff);
	d["pre_frame"] = p.fold_pre_frame;
	if ((p.flags & RESTIR_F_EVENTS) != 0u) {
		PackedByteArray events;
		events.resize(sizeof(RestirEventsUBO));
		memcpy(events.ptrw(), &p.events, sizeof(RestirEventsUBO));
		d["events"] = events;
	}
	return d;
}

#define RESTIR_GET(m_key, m_field, m_type)    \
	if (p_dict.has(m_key)) {                  \
		p.m_field = (m_type)p_dict[m_key];    \
	}

#define RESTIR_GET_V(m_key, m_field)          \
	if (p_dict.has(m_key)) {                  \
		p.m_field = p_dict[m_key];            \
	}

RestirFrameParams params_from_dict(const Dictionary &p_dict) {
	RestirFrameParams p;
	if (p_dict.has("size")) {
		p.trace_size = Vector2i(p_dict["size"]);
	}
	RESTIR_GET("frame_index", frame_index, uint32_t)
	RESTIR_GET("seed", seed, uint32_t)
	RESTIR_GET("flags", flags, uint32_t)
	RESTIR_GET("mode", mode, uint32_t)
	RESTIR_GET("spatial_count", spatial_count, uint32_t)
	RESTIR_GET("spatial_radius", spatial_radius, uint32_t)
	RESTIR_GET("spatial_passes", spatial_passes, uint32_t)
	RESTIR_GET("m_temporal_cap", m_temporal_cap, uint32_t)
	RESTIR_GET("m_max", m_max, uint32_t)
	RESTIR_GET("n_target", n_target, uint32_t)
	RESTIR_GET("epoch_tag", epoch_tag, uint32_t)
	RESTIR_GET("debug_view", debug_view, uint32_t)
	RESTIR_GET("vis_fail_limit", vis_fail_limit, uint32_t)
	RESTIR_GET("spatial_feedback", spatial_feedback, bool)
	RESTIR_GET("compat", compat, bool)
	RESTIR_GET("compat_early_stop", compat_early_stop, bool)
	RESTIR_GET("compat_k", compat_k, uint32_t)
	RESTIR_GET("compat_gate", compat_gate, uint32_t)
	RESTIR_GET_V("camera_basis", camera_basis)
	RESTIR_GET_V("previous_camera_basis", previous_camera_basis)
	RESTIR_GET_V("camera_to_anchor", camera_to_anchor)
	RESTIR_GET_V("previous_camera_to_anchor", previous_camera_to_anchor)
	RESTIR_GET_V("view_to_previous_clip", view_to_previous_clip)
	if (p_dict.has("jitter_shift")) {
		const PackedFloat32Array shift = p_dict["jitter_shift"];
		for (int i = 0; i < 4 && i < shift.size(); i++) {
			p.jitter_shift[i] = shift[i];
		}
	}
	if (p_dict.has("previous_jitter_shift")) {
		const PackedFloat32Array shift = p_dict["previous_jitter_shift"];
		for (int i = 0; i < 4 && i < shift.size(); i++) {
			p.previous_jitter_shift[i] = shift[i];
		}
	}
	RESTIR_GET_V("test_sky", test_sky)
	RESTIR_GET("tol_plane", tol_plane, float)
	RESTIR_GET("cos_normal", cos_normal, float)
	RESTIR_GET("jac_max", jac_max, float)
	RESTIR_GET("d_min", d_min, float)
	RESTIR_GET("cos_min", cos_min, float)
	RESTIR_GET("w_clamp", w_clamp, float)
	RESTIR_GET("origin_scale", origin_scale, float)
	RESTIR_GET("origin_floor", origin_floor, float)
	RESTIR_GET("sky_distance", sky_distance, float)
	RESTIR_GET("class_tol", class_tol, float)
	RESTIR_GET("emission_exposure", emission_exposure, float)
	RESTIR_GET("prev_epoch_tag", prev_epoch_tag, uint32_t)
	RESTIR_GET("dirty_count", dirty_count, uint32_t)
	RESTIR_GET("dirty_margin", dirty_margin, float)
	RESTIR_GET("clock", fold_clock, uint32_t)
	RESTIR_GET("k_eff", fold_k_eff, uint32_t)
	RESTIR_GET("pre_frame", fold_pre_frame, bool)
	p.dirty_count = MIN(p.dirty_count, 64u);
	if (p_dict.has("dirty_boxes")) {
		const PackedFloat64Array boxes = p_dict["dirty_boxes"];
		for (uint32_t i = 0; i < p.dirty_count && int(i * 6 + 5) < boxes.size(); i++) {
			p.dirty_min[i] = Vector3(boxes[i * 6], boxes[i * 6 + 1], boxes[i * 6 + 2]);
			p.dirty_max[i] = Vector3(boxes[i * 6 + 3], boxes[i * 6 + 4], boxes[i * 6 + 5]);
		}
	}
	if (p_dict.has("events")) {
		const PackedByteArray events = p_dict["events"];
		if (events.size() == int64_t(sizeof(RestirEventsUBO))) {
			memcpy(&p.events, events.ptr(), sizeof(RestirEventsUBO));
		}
	}
	return p;
}

static void _store_texture(Dictionary &r_case, const String &p_key, RID p_rid, uint32_t p_layers = 1) {
	RD *rd = RD::get_singleton();
	RD::TextureFormat format = rd->texture_get_format(p_rid);
	r_case[p_key + "_format"] = int64_t(format.format);
	for (uint32_t l = 0; l < p_layers; l++) {
		r_case[p_key + "_l" + itos(l)] = rd->texture_get_data(p_rid, l);
	}
}

static void _store_bytes(Dictionary &r_case, const String &p_key, RD::DataFormat p_format, const PackedByteArray &p_data) {
	r_case[p_key + "_format"] = int64_t(p_format);
	r_case[p_key + "_l0"] = p_data;
}

static PackedByteArray _floats_to_unorm8(const Vector<uint8_t> &p_data) {
	const int count = p_data.size() / 4;
	PackedByteArray out;
	out.resize(count);
	const float *src = reinterpret_cast<const float *>(p_data.ptr());
	uint8_t *dst = out.ptrw();
	for (int i = 0; i < count; i++) {
		dst[i] = uint8_t(CLAMP(Math::round(src[i] * 255.0f), 0.0f, 255.0f));
	}
	return out;
}

Dictionary capture_case(RestirGiCore &p_core, const RestirFrameParams &p_params, const RestirGiInputs &p_inputs, const RestirGiResources &p_resources, uint32_t p_history_index) {
	Dictionary c;
	c["params"] = params_to_dict(p_params);
	c["size"] = Vector2i(p_params.trace_size);
	RID copies[6];
	if (!p_core.run_capture(p_inputs, p_params.trace_size, copies)) {
		return Dictionary();
	}
	RD *rd = RD::get_singleton();
	_store_bytes(c, "geo_pos", RD::DATA_FORMAT_R32G32B32A32_SFLOAT, rd->texture_get_data(copies[0], 0));
	_store_bytes(c, "geo_nrm", RD::DATA_FORMAT_R8G8B8A8_UNORM, _floats_to_unorm8(rd->texture_get_data(copies[1], 0)));
	_store_bytes(c, "prev_pos", RD::DATA_FORMAT_R32G32B32A32_SFLOAT, rd->texture_get_data(copies[2], 0));
	_store_bytes(c, "prev_nrm", RD::DATA_FORMAT_R8G8B8A8_UNORM, _floats_to_unorm8(rd->texture_get_data(copies[3], 0)));
	_store_bytes(c, "cand_first", RD::DATA_FORMAT_R32G32B32A32_UINT, rd->texture_get_data(copies[4], 0));
	_store_bytes(c, "cand_second", RD::DATA_FORMAT_R32G32B32A32_UINT, rd->texture_get_data(copies[5], 0));
	for (int i = 0; i < 6; i++) {
		rd->free_rid(copies[i]);
	}
	_store_texture(c, "history", p_resources.reservoir[p_history_index], 3);
	_store_texture(c, "vis", p_resources.vis_state);
	return c;
}

Dictionary capture_final(RestirGiCore &p_core, const RestirFrameParams &p_params, const RestirGiInputs &p_inputs, const RestirGiResources &p_resources) {
	Dictionary c;
	c["params"] = params_to_dict(p_params);
	c["size"] = Vector2i(p_params.trace_size);
	RID copies[6];
	if (!p_core.run_capture(p_inputs, p_params.trace_size, copies)) {
		return Dictionary();
	}
	RD *rd = RD::get_singleton();
	_store_bytes(c, "geo_pos", RD::DATA_FORMAT_R32G32B32A32_SFLOAT, rd->texture_get_data(copies[0], 0));
	_store_bytes(c, "geo_nrm", RD::DATA_FORMAT_R8G8B8A8_UNORM, _floats_to_unorm8(rd->texture_get_data(copies[1], 0)));
	for (int i = 0; i < 6; i++) {
		rd->free_rid(copies[i]);
	}
	_store_texture(c, "final", p_resources.reservoir[p_resources.final_index], 3);
	_store_texture(c, "radiance", p_resources.radiance);
	_store_texture(c, "coverage", p_resources.coverage);
	_store_texture(c, "debug", p_resources.debug);
	_store_texture(c, "info", p_resources.info);
	return c;
}

static RID _upload(const Dictionary &p_case, const String &p_key, const Size2i &p_size, uint32_t p_layers = 1) {
	RD::TextureFormat format;
	format.format = RD::DataFormat(int64_t(p_case[p_key + "_format"]));
	format.width = p_size.x;
	format.height = p_size.y;
	format.depth = 1;
	format.array_layers = p_layers;
	format.mipmaps = 1;
	format.texture_type = p_layers > 1 ? RD::TEXTURE_TYPE_2D_ARRAY : RD::TEXTURE_TYPE_2D;
	format.samples = RD::TEXTURE_SAMPLES_1;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	Vector<Vector<uint8_t>> data;
	for (uint32_t l = 0; l < p_layers; l++) {
		data.push_back(p_case[p_key + "_l" + itos(l)]);
	}
	return RD::get_singleton()->texture_create(format, RD::TextureView(), data);
}

Dictionary run_case(RestirGiCore &p_core, const Dictionary &p_case) {
	Dictionary result;
	RD *rd = RD::get_singleton();
	const Size2i size = Vector2i(p_case["size"]);
	RestirFrameParams params = params_from_dict(p_case["params"]);
	params.trace_size = size;
	const RestirTestOptions &overrides = restir_test_options();
	if (overrides.neighbor_selection >= 0) {
		params.compat = overrides.neighbor_selection > 0;
	}
	if (overrides.compat_k > 0) {
		params.compat_k = uint32_t(CLAMP(overrides.compat_k, 1, 64));
	}
	if (overrides.compat_gate >= 0) {
		params.compat_gate = uint32_t(CLAMP(overrides.compat_gate, 0, 2));
	}
	if (overrides.compat_early_stop >= 0) {
		params.compat_early_stop = overrides.compat_early_stop > 0;
	}
	if (params.compat) {
		params.flags |= RESTIR_F_PROBE_STATS;
	}
	const bool fold_dump = overrides.fold_dump;
	if (fold_dump) {
		params.flags |= RESTIR_F_PROBE_STATS;
	}

	RestirGiInputs inputs;
	inputs.geo_position = _upload(p_case, "geo_pos", size);
	inputs.geo_normal = _upload(p_case, "geo_nrm", size);
	inputs.previous_geo_position = _upload(p_case, "prev_pos", size);
	inputs.previous_geo_normal = _upload(p_case, "prev_nrm", size);
	inputs.candidate_first = _upload(p_case, "cand_first", size);
	inputs.candidate_second = _upload(p_case, "cand_second", size);
	inputs.use_rays = false;

	RestirGiResources res;
	if (!res.allocate(size)) {
		result["error"] = "allocation failed";
		return result;
	}
	RestirGiInputs clear_inputs;
	p_core.clear_history(res, clear_inputs);
	for (uint32_t l = 0; l < 3; l++) {
		rd->texture_update(res.reservoir[0], l, p_case["history_l" + itos(l)]);
	}
	rd->texture_update(res.vis_state, 0, p_case["vis_l0"]);
	res.history_index = 0;
	if (fold_dump) {
		res.ensure_fold_dump(true);
	}

	p_core.run_frame(res, params, inputs);

	Vector<uint8_t> radiance = rd->texture_get_data(res.radiance, 0);
	Vector<uint8_t> coverage = rd->texture_get_data(res.coverage, 0);
	Vector<uint8_t> debug = rd->texture_get_data(res.debug, 0);
	Vector<uint8_t> info = rd->texture_get_data(res.info, 0);
	Vector<uint8_t> reservoir[3];
	Vector<uint8_t> reservoir_all;
	for (uint32_t l = 0; l < 3; l++) {
		reservoir[l] = rd->texture_get_data(res.reservoir[res.final_index], l);
		reservoir_all.append_array(reservoir[l]);
	}
	result["radiance"] = radiance;
	result["coverage"] = coverage;
	result["debug"] = debug;
	result["info"] = info;
	for (uint32_t l = 0; l < 3; l++) {
		result["reservoir_l" + itos(l)] = reservoir[l];
	}
	result["hash_radiance"] = _hash(radiance);
	result["hash_coverage"] = _hash(coverage);
	result["hash_reservoir"] = _hash(reservoir_all);
	result["hash_info"] = _hash(info);
	const RestirFrameStats stats = RestirGiCore::read_stats(res);
	Dictionary s;
	s["geometry"] = stats.geometry;
	s["output_pixels"] = stats.output_pixels;
	s["temporal_selected"] = stats.temporal_selected;
	s["spatial_selected"] = stats.spatial_selected;
	s["clamped"] = stats.clamped;
	s["violations"] = stats.violations;
	s["m_sum"] = stats.m_sum;
	s["compat_taps"] = stats.compat_taps;
	s["compat_early_stops"] = stats.compat_early_stops;
	s["compat_empty"] = stats.compat_empty;
	s["compat_h_sum"] = stats.compat_h_sum;
	s["spatial_combines"] = stats.spatial_combines;
	s["techniques_present"] = stats.techniques_present;
	s["fold_refused"] = stats.fold_refused;
	s["fold_carried"] = stats.fold_carried;
	s["fold_renewed"] = stats.fold_renewed;
	s["fold_started"] = stats.fold_started;
	s["fold_refused_empty"] = stats.fold_refused_empty;
	result["stats"] = s;
	if (params.compat) {
		result["selection"] = RestirGiCore::read_selection(res);
	}
	if (fold_dump) {
		Dictionary d;
		d["active"] = res.fold_dump_ran;
		d["k_eff"] = int64_t(params.fold_k_eff);
		d["clock"] = int64_t(params.fold_clock);
		d["n_valid"] = restir_fold_n_valid(stats.temporal_reject);
		d["cut_rej"] = restir_fold_cut_rej(stats.temporal_reject);
		d["had_history"] = int64_t(stats.had_history);
		d["hp_eq_px"] = int64_t(stats.fold_hp_eq_px);
		d["expected_phase0"] = int64_t(stats.fold_phase0);
		d["phase0_start"] = int64_t(stats.fold_phase0_start);
		d["refused_off"] = int64_t(stats.fold_refused_off);
		RestirFoldDump dump;
		if (res.fold_dump_ran && RestirGiCore::read_fold_dump(res, dump)) {
			d["s_max"] = int64_t(dump.s_max);
			d["n_over"] = int64_t(dump.n_over);
			d["texels"] = int64_t(dump.texels);
			PackedInt64Array starts;
			starts.resize(int64_t(RESTIR_FOLD_START_COUNT));
			for (uint32_t i = 0; i < RESTIR_FOLD_START_COUNT; i++) {
				starts.set(int64_t(i), int64_t(dump.starts[i]));
			}
			d["starts"] = starts;
		} else {
			d["s_max"] = int64_t(-1);
			d["n_over"] = int64_t(-1);
			d["texels"] = int64_t(-1);
			d["starts"] = PackedInt64Array();
		}
		result["fold_dump"] = d;
	}
	result["size"] = Vector2i(size);

	res.free_all();
	RID rids[] = { inputs.geo_position, inputs.geo_normal, inputs.previous_geo_position, inputs.previous_geo_normal, inputs.candidate_first, inputs.candidate_second };
	for (RID rid : rids) {
		rd->free_rid(rid);
	}
	return result;
}

void write_case(const String &p_path, const Dictionary &p_case) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_var(p_case, false);
	}
}

Dictionary read_case(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return Dictionary();
	}
	return file->get_var(false);
}

} //namespace RestirHarness
