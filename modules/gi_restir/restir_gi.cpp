/**************************************************************************/
/*  restir_gi.cpp                                                         */
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

#include "restir_gi.h"

#include "restir_harness.h"
#include "restir_settings.h"

#include "core/config/project_settings.h"
#include "core/variant/variant.h"
#include "servers/rendering/renderer_rd/environment/gi_provider.h"
#include "servers/rendering/renderer_rd/environment/ray_scene.h"
#include "servers/rendering/renderer_rd/environment/rtgi.h"

using namespace RendererRD;

static_assert(RESTIR_EVENT_MAX >= DDGILightTracker::MAX_EVENTS, "The event UBO must hold every event R2 delivers in one frame");
static_assert(RESTIR_TILE_SIZE == RtgiPass::TILE_SIZE, "The module's tile planner must use R2's tile size");

RestirTestOptions &restir_test_options() {
	static RestirTestOptions options;
	return options;
}

LocalVector<RestirFrameRecord> &restir_frame_records() {
	static LocalVector<RestirFrameRecord> records;
	return records;
}

Mutex &restir_frame_records_mutex() {
	static Mutex mutex;
	return mutex;
}

LocalVector<uint64_t> &restir_fold_starts() {
	static LocalVector<uint64_t> starts;
	return starts;
}

Mutex &restir_fold_starts_mutex() {
	static Mutex mutex;
	return mutex;
}

uint64_t &restir_fold_starts_frames() {
	static uint64_t frames = 0;
	return frames;
}

void restir_fold_starts_add(const Vector<uint32_t> &p_starts) {
	if (p_starts.size() != int64_t(RESTIR_FOLD_START_COUNT)) {
		return;
	}
	MutexLock lock(restir_fold_starts_mutex());
	LocalVector<uint64_t> &starts = restir_fold_starts();
	if (starts.size() != RESTIR_FOLD_START_COUNT) {
		starts.resize(RESTIR_FOLD_START_COUNT);
		for (uint32_t i = 0; i < RESTIR_FOLD_START_COUNT; i++) {
			starts[i] = 0;
		}
	}
	const uint32_t *counts = p_starts.ptr();
	for (uint32_t i = 0; i < RESTIR_FOLD_START_COUNT; i++) {
		starts[i] += counts[i];
	}
	restir_fold_starts_frames()++;
}

HashMap<int, RestirFoldFamilyTag> &restir_fold_family_tags() {
	static HashMap<int, RestirFoldFamilyTag> tags;
	return tags;
}

RestirFoldFamilyCount restir_fold_families_add(int p_tag, const uint32_t *p_ids, int64_t p_count) {
	HashMap<int, int64_t> sizes;
	const RestirFoldFamilyCount count = restir_fold_families(p_ids, p_count, sizes);
	MutexLock lock(restir_frame_records_mutex());
	RestirFoldFamilyTag &tag = restir_fold_family_tags()[p_tag];
	for (const KeyValue<int, int64_t> &entry : sizes) {
		int64_t *seen = tag.sizes.getptr(entry.key);
		if (seen) {
			*seen += entry.value;
		} else {
			tag.sizes.insert(entry.key, entry.value);
		}
	}
	tag.totals.frames++;
	tag.totals.refusals += count.refusals;
	tag.totals.unknown += count.unknown;
	tag.totals.families += count.families;
	return count;
}

Vector<int64_t> restir_fold_family_pairs(int p_tag) {
	MutexLock lock(restir_frame_records_mutex());
	Vector<int64_t> pairs;
	const RestirFoldFamilyTag *tag = restir_fold_family_tags().getptr(p_tag);
	if (!tag) {
		return pairs;
	}
	LocalVector<int> keys;
	for (const KeyValue<int, int64_t> &entry : tag->sizes) {
		keys.push_back(entry.key);
	}
	keys.sort();
	pairs.resize(int64_t(keys.size()) * 2);
	int64_t *w = pairs.ptrw();
	for (uint32_t i = 0; i < keys.size(); i++) {
		w[i * 2] = int64_t(keys[i]);
		const int64_t *count = tag->sizes.getptr(keys[i]);
		w[i * 2 + 1] = count ? *count : 0;
	}
	return pairs;
}

RestirFoldFamilyTotals restir_fold_family_totals(int p_tag) {
	MutexLock lock(restir_frame_records_mutex());
	const RestirFoldFamilyTag *tag = restir_fold_family_tags().getptr(p_tag);
	return tag ? tag->totals : RestirFoldFamilyTotals();
}

void restir_fold_families_reset() {
	MutexLock lock(restir_frame_records_mutex());
	restir_fold_family_tags().clear();
}

Dictionary &restir_captured_case() {
	static Dictionary captured;
	return captured;
}

Dictionary &restir_final_capture() {
	static Dictionary captured;
	return captured;
}

RestirPixelSums &restir_pixel_sums() {
	static RestirPixelSums sums;
	return sums;
}

static void _accumulate_pixels(const RestirGiResources &p_resources) {
	RD *rd = RD::get_singleton();
	const Vector<uint8_t> radiance = rd->texture_get_data(p_resources.radiance, 0);
	const Vector<uint8_t> coverage = rd->texture_get_data(p_resources.coverage, 0);
	const Vector<uint8_t> info = rd->texture_get_data(p_resources.info, 0);
	const uint32_t count = uint32_t(p_resources.size.x) * uint32_t(p_resources.size.y);
	if (uint32_t(radiance.size()) < count * 8u || uint32_t(coverage.size()) < count * 2u || uint32_t(info.size()) < count * 4u) {
		return;
	}
	RestirPixelSums &sums = restir_pixel_sums();
	if (sums.size != p_resources.size) {
		sums = RestirPixelSums();
		sums.size = p_resources.size;
		sums.output_luma.resize(count);
		sums.covered.resize(count);
		sums.valid.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			sums.output_luma[i] = 0.0;
			sums.covered[i] = 0.0;
			sums.valid[i] = 0.0;
		}
		if (restir_test_options().pixel_detail) {
			for (uint32_t d = 0; d < RESTIR_DETAIL_COUNT; d++) {
				sums.detail[d].resize(count);
				for (uint32_t i = 0; i < count; i++) {
					sums.detail[d][i] = 0.0;
				}
			}
			const uint32_t blocks = uint32_t(Math::division_round_up(p_resources.size.x, 8)) * uint32_t(Math::division_round_up(p_resources.size.y, 8));
			sums.reject_temporal.resize(blocks * 16u);
			sums.reject_spatial.resize(blocks * 16u);
			for (uint32_t i = 0; i < blocks * 16u; i++) {
				sums.reject_temporal[i] = 0.0;
				sums.reject_spatial[i] = 0.0;
			}
		}
	}
	const bool detail = restir_test_options().pixel_detail && sums.detail[0].size() == count;
	const uint32_t blocks_x = uint32_t(Math::division_round_up(p_resources.size.x, 8));
	const uint16_t *rgba = reinterpret_cast<const uint16_t *>(radiance.ptr());
	const uint16_t *cov = reinterpret_cast<const uint16_t *>(coverage.ptr());
	const uint32_t *words = reinterpret_cast<const uint32_t *>(info.ptr());
	for (uint32_t i = 0; i < count; i++) {
		if ((words[i] & (1u << 14u)) == 0u) {
			continue;
		}
		const double luma = 0.2126 * Math::half_to_float(rgba[i * 4]) + 0.7152 * Math::half_to_float(rgba[i * 4 + 1]) + 0.0722 * Math::half_to_float(rgba[i * 4 + 2]);
		sums.output_luma[i] += luma;
		sums.valid[i] += 1.0;
		if (Math::half_to_float(cov[i]) > 0.0f) {
			sums.covered[i] += 1.0;
		}
		if (detail) {
			const uint32_t w = words[i];
			const uint32_t x = i % uint32_t(p_resources.size.x);
			const uint32_t y = i / uint32_t(p_resources.size.x);
			const uint32_t block = (y / 8u) * blocks_x + (x / 8u);
			sums.reject_temporal[block * 16u + (w & 15u)] += 1.0;
			sums.reject_spatial[block * 16u + ((w >> 4u) & 15u)] += 1.0;
			if (w & (1u << 15u)) {
				sums.detail[RESTIR_DETAIL_CAND][i] += Math::half_to_float(uint16_t(w >> 16u));
				sums.detail[RESTIR_DETAIL_CAND_N][i] += 1.0;
			}
			if (w & (1u << 9u)) {
				sums.detail[RESTIR_DETAIL_OUT_SPATIAL][i] += luma;
				sums.detail[RESTIR_DETAIL_N_SPATIAL][i] += 1.0;
			} else if (w & (1u << 8u)) {
				sums.detail[RESTIR_DETAIL_OUT_TEMPORAL][i] += luma;
				sums.detail[RESTIR_DETAIL_N_TEMPORAL][i] += 1.0;
			}
			if (w & (1u << 10u)) {
				sums.detail[RESTIR_DETAIL_BLOCKED][i] += 1.0;
			}
		}
	}
	sums.frames++;
}

static const char *const RESTIR_GI = "rendering/global_illumination/restir/gi/";

static bool _gi_bool(const char *p_name) {
	return GLOBAL_GET(String(RESTIR_GI) + p_name);
}

static int _gi_int(const char *p_name) {
	return GLOBAL_GET(String(RESTIR_GI) + p_name);
}

static uint32_t _gi_mode() {
	const String mode = GLOBAL_GET(String(RESTIR_GI) + "mode");
	if (mode == "U1") {
		return RESTIR_MODE_U1;
	}
	if (mode == "B1") {
		return RESTIR_MODE_B1;
	}
	if (mode == "B2k") {
		return RESTIR_MODE_B2K;
	}
	return RESTIR_MODE_B2;
}

static String _gi_text(const char *p_name) {
	return GLOBAL_GET(String(RESTIR_GI) + p_name);
}

static float _gi_float(const char *p_name) {
	return float(GLOBAL_GET(String(RESTIR_GI) + p_name));
}

static uint32_t _ramped_cap(uint32_t p_cap, uint32_t p_m_min, uint32_t p_frames, uint32_t p_since_cut) {
	if (p_cap <= p_m_min || p_frames == 0u) {
		return p_cap;
	}
	const float t = MIN(float(p_since_cut) / float(p_frames), 1.0f);
	return CLAMP(uint32_t(Math::round(float(p_m_min) + float(p_cap - p_m_min) * t)), 1u, p_cap);
}

static void _apply_selection(RestirFrameParams &r_params, const RestirTestOptions &p_options) {
	r_params.compat = _gi_text("neighbor_selection") == "compatibility";
	r_params.compat_k = uint32_t(CLAMP(_gi_int("compat_candidates"), 1, 64));
	const String gate = _gi_text("compat_gate");
	r_params.compat_gate = gate == "none" ? RESTIR_GATE_NONE : (gate == "binary" ? RESTIR_GATE_BINARY : RESTIR_GATE_SIMILAR);
	r_params.compat_early_stop = _gi_bool("compat_early_stop");
	if (p_options.neighbor_selection >= 0) {
		r_params.compat = p_options.neighbor_selection > 0;
	}
	if (p_options.compat_k > 0) {
		r_params.compat_k = uint32_t(CLAMP(p_options.compat_k, 1, 64));
	}
	if (p_options.compat_gate >= 0) {
		r_params.compat_gate = uint32_t(CLAMP(p_options.compat_gate, 0, 2));
	}
	if (p_options.compat_early_stop >= 0) {
		r_params.compat_early_stop = p_options.compat_early_stop > 0;
	}
}

RestirGiCore *RestirGiBackend::get_core() {
	if (!core) {
		core = memnew(RestirGiCore);
	}
	return core;
}

RestirGiBackend::~RestirGiBackend() {
	if (core) {
		memdelete(core);
		core = nullptr;
	}
}

bool RestirGiBackend::is_active() const {
	return restir_gi_enabled();
}

bool RestirGiBackend::wants_all_tiles() const {
	return _gi_bool("trace_all_tiles");
}

bool RestirGiBackend::wants_replay() const {
	return _gi_bool("replay_seed");
}

void RestirGiBackend::_free_state(RestirGiState &r_state) {
	r_state.resources.free_all();
}

void RestirGiBackend::_release_feed() {
	if (!feed_held) {
		return;
	}
	feed_held = false;
	RayScene *ray_scene = RayScene::get_singleton();
	if (ray_scene) {
		ray_scene->release_dirty_feed(RayScene::FEED_MODULE);
	}
}

bool RestirGiBackend::begin_frame(const RtgiFrameInputs &p_inputs) {
	refusal_reason = String();
	if (!p_inputs.buffers) {
		refusal_reason = "no buffers";
		refusals++;
		return false;
	}
	if (p_inputs.candidate_layout_version != RTGI_CANDIDATE_LAYOUT_VERSION) {
		refusal_reason = "candidate layout version";
		refusals++;
		return false;
	}
	if (p_inputs.trace_size.x <= 0 || p_inputs.trace_size.y <= 0 || p_inputs.candidate_first.is_null() || p_inputs.candidate_second.is_null() || p_inputs.geo_position.is_null() || p_inputs.geo_normal.is_null() || p_inputs.previous_geo_position.is_null() || p_inputs.previous_geo_normal.is_null()) {
		refusal_reason = "missing inputs";
		refusals++;
		return false;
	}
	const bool rays = p_inputs.ray_scene && p_inputs.ray_scene->is_ready();
	const bool hardware = rays && p_inputs.ray_scene->uses_hardware();
	RestirGiCore::Variant variant = !rays ? RestirGiCore::VARIANT_NONE : (hardware ? RestirGiCore::VARIANT_HW : RestirGiCore::VARIANT_SW);
	if (!get_core()->is_variant_valid(variant)) {
		if (hardware) {
			p_inputs.ray_scene->refuse_hardware("the ReSTIR GI hardware shader or its pipelines could not be made");
		}
		refusal_reason = hardware ? "hardware shader variant did not compile" : "shader variant did not compile";
		refusals++;
		return false;
	}
	if (_gi_bool("own_sampler") && !rays) {
		refusal_reason = "own sampler needs the ray scene";
		refusals++;
		return false;
	}
	if (_gi_bool("event_reshade")) {
		if (!_gi_bool("replay_seed")) {
			refusal_reason = "event_reshade needs replay_seed";
			refusals++;
			return false;
		}
		if (!rays) {
			refusal_reason = "event_reshade needs the ray scene";
			refusals++;
			return false;
		}
		if (!p_inputs.shade.valid) {
			refusal_reason = "event_reshade needs the DDGI shading inputs";
			refusals++;
			return false;
		}
		if (_gi_text("revalidate") == "budget") {
			refusal_reason = "event_reshade does not run with revalidate budget";
			refusals++;
			return false;
		}
	}
	if (restir_test_options().release_once) {
		restir_test_options().release_once = false;
		release(p_inputs.buffers);
		release_tests_total++;
	}
	RestirGiState *existing = states.getptr(p_inputs.buffers);
	const bool existed = existing != nullptr;
	RestirGiState &state = existed ? *existing : states[p_inputs.buffers];
	if (!state.resources.allocated || state.size != p_inputs.trace_size) {
		_free_state(state);
		state = RestirGiState();
		if (!state.resources.allocate(p_inputs.trace_size, p_inputs.debug_view >= 0 || restir_test_options().readback)) {
			refusal_reason = "allocation failed";
			refusals++;
			return false;
		}
		state.size = p_inputs.trace_size;
		state_serial_next++;
		state.serial = state_serial_next;
		if (existed) {
			state_rebuilds_total++;
		} else {
			state_creates_total++;
		}
		RestirGiInputs clear_inputs;
		get_core()->clear_history(state.resources, clear_inputs);
	}
	return true;
}

void RestirGiBackend::resample(const RtgiFrameInputs &p_inputs, RtgiResampleOutput &r_output) {
	r_output.valid = false;
	summary.fold_dump_active = false;
	summary.fold_lineage_active = false;
	RestirGiState *found = states.getptr(p_inputs.buffers);
	if (!found) {
		return;
	}
	RestirGiState &state = *found;
	RestirTestOptions &options = restir_test_options();
	if (options.freeze && state.have_history) {
		r_output.radiance = state.resources.radiance;
		r_output.coverage = state.resources.coverage;
		r_output.debug = p_inputs.debug_view >= 0 ? state.resources.debug : RID();
		r_output.record.flags = GI_PROVIDER_FLAG_SAMPLE_COUNT | GI_PROVIDER_FLAG_CONFIDENCE | GI_PROVIDER_FLAG_HISTORY | GI_PROVIDER_FLAG_SCENE_EPOCH | GI_PROVIDER_FLAG_LIGHT_EPOCH | GI_PROVIDER_FLAG_SKY_EPOCH | GI_PROVIDER_FLAG_ORIGIN_EPOCH;
		r_output.record.sample_count = uint32_t(MIN(state.frames_since_cut, uint32_t(_gi_int("n_target"))));
		r_output.record.validity = 1.0f;
		r_output.record.confidence = gi_provider_confidence_from_samples(r_output.record.sample_count, uint32_t(_gi_int("n_target")));
		r_output.valid = true;
		if (_gi_text("denoise") == "r2") {
			RestirGiInputs frozen;
			frozen.geo_position = p_inputs.geo_position;
			frozen.candidate_second = p_inputs.candidate_second;
			frozen.tile_texture = p_inputs.tile_texture;
			get_core()->run_pack(state.resources, frozen);
			r_output.denoise = true;
			r_output.history_cap = uint32_t(CLAMP(_gi_int("denoise_history_cap"), 1, 255));
			r_output.mode = state.route.module_output ? RTGI_OUTPUT_MODULE : RTGI_OUTPUT_PLAIN;
			r_output.n_target = state.route.n_target;
		}
		return;
	}
	RayScene *ray_scene = p_inputs.ray_scene && p_inputs.ray_scene->is_ready() ? p_inputs.ray_scene : nullptr;
	const bool events_on = _gi_bool("event_reshade") && _gi_bool("replay_seed");
	if (events_on) {
		state.light_soft_frames = 0;
	}

	const bool sky_reread_asked = _gi_bool("sky_reread");
	const bool sky_reread_inputs = ray_scene != nullptr && p_inputs.shade.valid && p_inputs.shade.ddgi_ubo.is_valid();
	const bool sky_reread = sky_reread_asked && sky_reread_inputs && get_core()->is_sky_valid(ray_scene->uses_hardware() ? RestirGiCore::VARIANT_HW : RestirGiCore::VARIANT_SW);

	const uint32_t mode = _gi_mode();
	const bool feedback = options.spatial_feedback < 0 ? mode == RESTIR_MODE_U1 : options.spatial_feedback > 0;
	RestirFoldFrame fold;
	fold.k = restir_fold_k(_gi_int("fold_k"));
	fold.mode = mode;
	fold.feedback = feedback;
	fold.k_eff = restir_fold_k_eff(fold.k, mode != RESTIR_MODE_U1, feedback);

	RestirEpochInputs epoch_inputs;
	epoch_inputs.have_history = state.have_history;
	epoch_inputs.have_epochs = state.have_epochs;
	epoch_inputs.history_cut = p_inputs.history_cut;
	epoch_inputs.hardware_changed = state.hardware != p_inputs.hardware || state.sky_reread != sky_reread;
	epoch_inputs.fold_changed = state.have_history && restir_fold_cut(state.fold, fold);
	epoch_inputs.previous = state.epochs;
	epoch_inputs.current = p_inputs.epochs;
	epoch_inputs.anchor = ray_scene ? ray_scene->get_anchor() : Vector3();
	epoch_inputs.light_soft_frames = state.light_soft_frames;
	epoch_inputs.m_light_frames = events_on ? 0u : uint32_t(CLAMP(_gi_int("m_light_frames"), 0, 64));
	LocalVector<AABB> dirty_boxes;
	if (ray_scene) {
		ray_scene->acquire_dirty_feed(RayScene::FEED_MODULE);
		feed_held = true;
	}
	epoch_inputs.have_feed = p_inputs.dirty_source && p_inputs.dirty_source->is_feed_live();
	if (epoch_inputs.have_feed && state.have_epochs && state.epochs.scene != p_inputs.epochs.scene) {
		epoch_inputs.dirty_result = p_inputs.dirty_source->get_dirty_boxes(state.epochs.scene, dirty_boxes);
		epoch_inputs.dirty_boxes = dirty_boxes.ptr();
		epoch_inputs.dirty_box_count = dirty_boxes.size();
	}
	RestirEpochPlan plan;
	restir_plan_epochs(epoch_inputs, plan);
	state.light_soft_frames = plan.light_soft_frames;
	const bool cut = plan.cut;
	if (cut) {
		state.history_cuts++;
		state.cut_counts[plan.reason]++;
		state.frames_since_cut = 0;
		if (plan.reason == RESTIR_CUT_BACKEND && state.hardware == p_inputs.hardware) {
			state.sky_reread_cuts++;
		}
	} else if (plan.dirty_count > 0) {
		state.dirty_frames++;
	}
	const bool use_history = !cut && options.history;
	const bool dump_wanted = !String(GLOBAL_GET(String(RESTIR_GI) + "dump_path")).is_empty();
	state.resources.ensure_debug(state.resources.debug.is_valid() || p_inputs.debug_view >= 0 || options.readback || options.capture || options.capture_final || dump_wanted);
	state.resources.ensure_fold_dump(options.fold_dump && options.readback);
	state.resources.ensure_fold_lineage(options.fold_lineage && options.fold_dump && options.readback);

	RestirFrameParams params;
	params.trace_size = p_inputs.trace_size;
	params.screen_size = p_inputs.screen_size;
	params.trace_scale = p_inputs.trace_scale;
	params.frame_index = p_inputs.frame_index;
	params.seed = uint32_t(int(GLOBAL_GET("rendering/global_illumination/rtgi/seed"))) + options.seed_offset;
	params.mode = mode;
	params.spatial_count = uint32_t(CLAMP(_gi_int("spatial_neighbors"), 0, 8));
	params.spatial_radius = uint32_t(CLAMP(_gi_int("spatial_radius"), 1, 128));
	params.spatial_passes = uint32_t(CLAMP(_gi_int("spatial_passes"), 1, 2));
	params.spatial_feedback = feedback;
	_apply_selection(params, options);
	params.m_max = uint32_t(CLAMP(_gi_int("m_max"), 1, 512));
	params.n_target = uint32_t(CLAMP(_gi_int("n_target"), 1, 64));
	params.m_temporal_cap = uint32_t(CLAMP(_gi_int("m_temporal_cap"), 1, 256));
	if (_gi_bool("m_cap_follows_rate") && p_inputs.ray_fraction < 1.0f) {
		const float rate = CLAMP(p_inputs.ray_fraction, 0.0f, 1.0f);
		params.m_temporal_cap = MAX(1u, uint32_t(Math::ceil(float(params.m_temporal_cap) * rate)));
		params.m_max = MIN(params.m_max, params.m_temporal_cap + 1u);
	}
	const bool ramp = _gi_bool("confidence_ramp");
	const uint32_t ramp_m_min = uint32_t(CLAMP(_gi_int("ramp_m_min"), 1, 255));
	const uint32_t ramp_frames = uint32_t(CLAMP(_gi_int("ramp_frames"), 1, 4096));
	const uint32_t ramp_since_cut = state.frames_since_cut;
	if (ramp) {
		params.m_temporal_cap = _ramped_cap(params.m_temporal_cap, MIN(ramp_m_min, params.m_temporal_cap), ramp_frames, ramp_since_cut);
	}
	const bool light_cap = state.light_soft_frames > 0;
	if (light_cap) {
		const uint32_t m_light = uint32_t(CLAMP(_gi_int("m_light"), 1, 32));
		params.m_temporal_cap = MIN(params.m_temporal_cap, m_light);
		params.m_max = MIN(params.m_max, m_light);
		state.light_soft_frames--;
	}
	params.prev_epoch_tag = state.have_history ? state.prev_epoch_tag : 0xFFFFFFFFu;
	if (use_history) {
		params.dirty_count = plan.dirty_count;
		for (uint32_t i = 0; i < plan.dirty_count; i++) {
			params.dirty_min[i] = plan.dirty_min[i];
			params.dirty_max[i] = plan.dirty_max[i];
		}
	}
	const bool own_sampler = _gi_bool("own_sampler");
	const RtgiShadeInputs &shade = p_inputs.shade;
	const bool shade_valid = shade.valid && ray_scene != nullptr;
	bool fresh_lo = _gi_bool("fresh_lo");
	if (fresh_lo && !own_sampler && !shade_valid) {
		WARN_PRINT_ONCE("gi_restir: fresh_lo needs the own sampler or the DDGI shading inputs, so it is ignored");
		fresh_lo = false;
	}
	const bool real_sky = shade_valid && (own_sampler ? !options.constant_sky : true);
	params.flags = (own_sampler ? RESTIR_F_OWN_SAMPLER : 0u) | (real_sky ? RESTIR_F_REAL_SKY : 0u) | (shade_valid ? RESTIR_F_SHADE_DDGI : 0u) | (use_history ? RESTIR_F_HISTORY : 0u) | (fresh_lo ? RESTIR_F_FRESH_LO : 0u) | (_gi_bool("deterministic") ? RESTIR_F_DETERMINISTIC : 0u) | (_gi_bool("visibility_reuse") ? RESTIR_F_VIS_REUSE : 0u) | (_gi_bool("blocked_keeps_coverage") ? RESTIR_F_KEEP_BLOCKED : 0u) | (options.readback ? RESTIR_F_PROBE_STATS : 0u) | (options.pixel_detail ? RESTIR_F_PROBE_PIXELS : 0u);
	if (_gi_bool("replay_seed")) {
		params.flags |= RESTIR_F_REPLAY;
	}
	params.sky_reread = sky_reread;
	const bool trace_all = _gi_bool("trace_all_tiles");
	const bool have_tiles = p_inputs.tile_texture.is_valid();
	uint32_t tiles_width = 0;
	uint32_t tiles_height = 0;
	if (!trace_all && have_tiles) {
		const RD::TextureFormat tile_format = RD::get_singleton()->texture_get_format(p_inputs.tile_texture);
		tiles_width = tile_format.width;
		tiles_height = tile_format.height;
	}
	const RestirTileState tile_state = restir_plan_tiles(trace_all, have_tiles, uint32_t(MAX(p_inputs.trace_size.x, 0)), uint32_t(MAX(p_inputs.trace_size.y, 0)), tiles_width, tiles_height);
	params.tile_skip = tile_state == RESTIR_TILES_SKIP;
	const String confidence = _gi_text("confidence");
	params.pack_m = confidence == "frames_and_m";
	const String reval_text = _gi_text("revalidate");
	const bool reval_budget = reval_text == "budget";
	const bool reval_share = reval_text == "share";
	const uint32_t period = uint32_t(CLAMP(_gi_int("revalidate_period"), 2, 16));
	params.fold_clock = state.frames;
	params.fold_k_eff = fold.k_eff;
	params.fold_pre_frame = fold.k_eff != 0u && restir_fold_pre_frame(reval_budget, ray_scene != nullptr, p_inputs.frame_index, period);
	if (use_history && ray_scene != nullptr && (reval_budget || reval_share)) {
		params.reval_cut_m = uint32_t(CLAMP(_gi_int("revalidate_cut_m"), 0, 16));
		params.reval_threshold = float(CLAMP(_gi_int("revalidate_threshold"), 1, 1000)) * 0.01f;
		params.reval_share_denom = uint32_t(CLAMP(_gi_int("revalidate_share"), 1, 1024));
		if (reval_share) {
			params.flags |= RESTIR_F_REVAL_SHARE;
			params.reval_pass = true;
		} else {
			if (restir_reval_frame(p_inputs.frame_index, period)) {
				params.flags |= RESTIR_F_REVAL_FRAME;
				params.reval_pass = true;
			}
		}
	}
	RestirEventPlan event_plan;
	bool hold_on = false;
	uint32_t hold_frames = 0;
	RestirHoldUpdate hold_update;
	{
		RestirEventInputs event_inputs;
		event_inputs.settings.events_on = events_on;
		event_inputs.settings.reshade_budget = uint32_t(CLAMP(_gi_int("reshade_budget"), 0, 100));
		event_inputs.settings.event_m_cap = uint32_t(CLAMP(_gi_int("event_m_cap"), 0, 256));
		event_inputs.settings.jump_m_cap = uint32_t(CLAMP(_gi_int("jump_m_cap"), 0, 256));
		event_inputs.settings.reach_scale = CLAMP(_gi_float("event_reach_scale"), 0.0f, 64.0f);
		event_inputs.settings.solid_angle = CLAMP(_gi_float("event_solid_angle"), 0.0f, float(Math::TAU * 2.0));
		LocalVector<RestirEventSphere> spheres;
		if (events_on && p_inputs.events) {
			const uint32_t count = MIN(p_inputs.event_count, RESTIR_EVENT_MAX);
			for (uint32_t i = 0; i < count; i++) {
				RestirEventSphere sphere;
				sphere.center = p_inputs.events[i].center;
				sphere.reach = p_inputs.events[i].half;
				sphere.since_last = p_inputs.events[i].since_last;
				spheres.push_back(sphere);
			}
		}
		event_inputs.events = spheres.ptr();
		event_inputs.event_count = spheres.size();
		event_inputs.events_lost = p_inputs.events_lost;
		event_inputs.directional_jump = p_inputs.directional_jump;
		event_inputs.quiet_frames = p_inputs.restart_quiet_frames;
		event_inputs.trace_width = uint32_t(MAX(p_inputs.trace_size.x, 0));
		event_inputs.trace_height = uint32_t(MAX(p_inputs.trace_size.y, 0));
		event_inputs.history = use_history && ray_scene != nullptr;
		restir_plan_events(event_inputs, event_plan);
		hold_on = event_plan.active && ray_scene != nullptr && _gi_text("spatial_hold") == "uncapped";
		hold_frames = uint32_t(CLAMP(_gi_int("spatial_hold_frames"), 1, 256));
		if (!hold_on || !event_inputs.history || event_plan.jump_frame) {
			restir_hold_clear(state.hold);
		}
		if (hold_on) {
			const Vector3 hold_anchor = ray_scene->get_anchor();
			hold_update = restir_hold_update(state.hold, spheres.ptr(), spheres.size(), hold_anchor, event_inputs.quiet_frames, hold_frames);
			restir_hold_pack(state.hold, hold_anchor, event_plan.ubo);
		}
	}
	if (event_plan.active) {
		params.flags |= RESTIR_F_EVENTS;
		if (hold_on && state.hold.count > 0u) {
			params.flags |= RESTIR_F_SPATIAL_HOLD;
		}
		params.events = event_plan.ubo;
	}
	params.epoch_tag = (p_inputs.epochs.scene & 0xFFu) | ((p_inputs.epochs.light & 0xFFu) << 8u) | ((p_inputs.epochs.origin & 0xFFu) << 16u);
	params.camera_basis = p_inputs.camera_transform.basis;
	params.previous_camera_basis = state.have_history ? p_inputs.previous_camera_transform.basis : p_inputs.camera_transform.basis;
	const Vector3 anchor = ray_scene ? ray_scene->get_anchor() : Vector3();
	params.camera_to_anchor = p_inputs.camera_transform.origin - anchor;
	params.previous_camera_to_anchor = p_inputs.previous_camera_transform.origin - anchor;
	params.view_to_previous_clip = p_inputs.view_to_previous_clip;
	const RestirJitterShift jitter = restir_jitter_shift(p_inputs.projection, p_inputs.drawn_projection);
	const RestirJitterShift previous_jitter = restir_jitter_shift(p_inputs.previous_projection, p_inputs.previous_drawn_projection);
	const double jitter_words[4] = { jitter.ax, jitter.ay, jitter.bx, jitter.by };
	const double previous_jitter_words[4] = { previous_jitter.ax, previous_jitter.ay, previous_jitter.bx, previous_jitter.by };
	for (int i = 0; i < 4; i++) {
		params.jitter_shift[i] = float(jitter_words[i]);
		params.previous_jitter_shift[i] = float(previous_jitter_words[i]);
	}
	params.test_sky = options.sky;
	if (shade_valid) {
		for (int i = 0; i < 3; i++) {
			params.shade_sky[i] = shade.sky_color_or_orientation[i];
		}
		params.shade_sky_flags = shade.sky_flags;
		params.shade_sky_border[0] = shade.sky_border_size[0];
		params.shade_sky_border[1] = shade.sky_border_size[1];
		params.shade_sky_energy = shade.sky_energy;
		params.shade_bounce_energy = shade.bounce_energy;
		params.shade_exposure = shade.emission_exposure;
		if (!options.constant_sky) {
			params.emission_exposure = shade.emission_exposure;
		}
	}
	params.origin_scale = MAX(float(GLOBAL_GET("rendering/global_illumination/rtgi/origin_scale")), 0.0f);
	params.origin_floor = MAX(float(GLOBAL_GET("rendering/global_illumination/rtgi/origin_floor")), 0.0f);
	params.debug_view = p_inputs.debug_view >= 0 ? uint32_t(57 + p_inputs.debug_view) : 0u;
	if (options.d_min >= 0.0f) {
		params.d_min = options.d_min;
	}
	if (options.cos_min >= 0.0f) {
		params.cos_min = options.cos_min;
	}
	if (options.jac_max > 0.0f) {
		params.jac_max = options.jac_max;
	}
	if (options.vis_fail_limit >= 0) {
		params.vis_fail_limit = uint32_t(options.vis_fail_limit);
	}
	if (options.w_clamp > 0.0f) {
		params.w_clamp = options.w_clamp;
	}

	RestirGiInputs inputs;
	inputs.geo_position = p_inputs.geo_position;
	inputs.geo_normal = p_inputs.geo_normal;
	inputs.previous_geo_position = p_inputs.previous_geo_position;
	inputs.previous_geo_normal = p_inputs.previous_geo_normal;
	inputs.candidate_first = p_inputs.candidate_first;
	inputs.candidate_second = p_inputs.candidate_second;
	inputs.ray_scene = ray_scene;
	inputs.hardware = ray_scene != nullptr && ray_scene->uses_hardware();
	inputs.use_rays = ray_scene != nullptr;
	inputs.ddgi_ubo = shade.ddgi_ubo;
	inputs.ddgi_probe_data = shade.probe_data;
	inputs.ddgi_irradiance = shade.irradiance;
	inputs.ddgi_distance = shade.distance;
	inputs.sky_texture = shade.sky_texture;
	inputs.tile_texture = params.tile_skip ? p_inputs.tile_texture : RID();

	if (own_sampler && (params.flags & RESTIR_F_REVAL_FRAME) == 0u) {
		get_core()->run_source(state.resources, params, inputs);
	}
	const String dump_path = GLOBAL_GET(String(RESTIR_GI) + "dump_path");
	const bool dump_now = !dump_path.is_empty() && !options.dumped && state.frames >= options.dump_after && use_history;
	if (options.capture || dump_now) {
		RestirFrameParams captured = params;
		captured.trace_size = p_inputs.trace_size;
		restir_captured_case() = RestirHarness::capture_case(*get_core(), captured, inputs, state.resources, state.resources.history_index);
		options.capture = false;
		if (dump_now) {
			RestirHarness::write_case(dump_path, restir_captured_case());
			options.dumped = true;
		}
	}
	get_core()->run_frame(state.resources, params, inputs);
	summary.fold_dump_active = state.resources.fold_dump_ran;
	if (state.resources.fold_dump_ran) {
		fold_dump_frames_total++;
	}
	summary.fold_lineage_active = state.resources.fold_lineage_ran;
	summary.fold_lineage_refusal = state.resources.fold_lineage_refusal;
	if (state.resources.fold_lineage_ran) {
		fold_lineage_frames_total++;
	}
	if (options.capture_final) {
		RestirFrameParams final_params = params;
		final_params.trace_size = p_inputs.trace_size;
		restir_final_capture() = RestirHarness::capture_final(*get_core(), final_params, inputs, state.resources);
		options.capture_final = false;
	}
	const bool denoise = _gi_text("denoise") == "r2";
	if (denoise) {
		get_core()->run_pack(state.resources, inputs);
	}
	const RestirRoute route = restir_route_output(denoise, events_on, params.pack_m, params.n_target);
	state.route = route;

	state.have_history = true;
	state.hardware = p_inputs.hardware;
	state.sky_reread = sky_reread;
	state.epochs = p_inputs.epochs;
	state.have_epochs = true;
	state.prev_epoch_tag = params.epoch_tag;
	state.fold = fold;
	state.frames++;
	state.frames_since_cut++;
	if (use_history) {
		state.history_uses++;
	}
	if (params.reval_pass) {
		state.reval_frames++;
	}

	summary.ran = true;
	summary.history_used = use_history;
	summary.own_sampler = own_sampler;
	summary.rays = inputs.use_rays;
	summary.hardware = p_inputs.hardware && inputs.use_rays;
	summary.mode = params.mode;
	summary.frame = p_inputs.frame_index;
	summary.spatial_count = params.spatial_count;
	summary.spatial_passes = params.spatial_passes;
	summary.m_cap = params.m_temporal_cap;
	summary.m_max = params.m_max;
	summary.cut = cut;
	summary.cut_reason = uint32_t(plan.reason);
	summary.dirty_count = use_history ? plan.dirty_count : 0u;
	summary.light_cap = light_cap;
	summary.light_frames_left = state.light_soft_frames;
	summary.scene_changed = plan.scene_changed;
	summary.light_changed = plan.light_changed;
	summary.sky_changed = plan.sky_changed;
	summary.origin_changed = plan.origin_changed;
	summary.world_changed = plan.world_changed;
	summary.have_feed = epoch_inputs.have_feed;
	summary.size = p_inputs.trace_size;
	summary.reval_active = params.reval_pass;
	summary.reval_frames = state.reval_frames;
	summary.events_on = event_plan.active;
	summary.jump_frame = event_plan.jump_frame;
	summary.events_read = p_inputs.events ? MIN(p_inputs.event_count, RESTIR_EVENT_MAX) : 0u;
	summary.events_lost = p_inputs.events_lost;
	summary.event_capping = event_plan.capping;
	summary.event_validate = uint32_t(event_plan.validate);
	summary.reshade_budget = event_plan.budget;
	if (summary.events_read > 0) {
		event_frames_total++;
		events_read_total += summary.events_read;
	}
	if (summary.event_validate) {
		event_validate_frames_total++;
	}
	event_capping_total += summary.event_capping;
	summary.hold_on = hold_on;
	summary.hold_frames = hold_frames;
	summary.hold_fill = hold_on ? state.hold.count : 0u;
	summary.hold_update = hold_update;
	if (summary.hold_fill > 0u) {
		hold_frames_total++;
	}
	hold_dropped_total += hold_update.dropped;
	hold_fill_max = MAX(hold_fill_max, summary.hold_fill);
	summary.sky_reread = !sky_reread_asked ? "off" : (!sky_reread_inputs ? "inactive" : (sky_reread ? "on" : "unavailable"));
	summary.tiles = restir_tile_state_name(tile_state);
	summary.confidence = params.pack_m ? "frames_and_m" : "frames";
	if (params.tile_skip) {
		tile_skip_frames_total++;
	}
	summary.route = route;
	summary.fold = fold;
	summary.fold_clock = params.fold_clock;
	summary.fold_pre_frame = params.fold_pre_frame;
	summary.jitter = jitter;
	summary.previous_jitter = previous_jitter;
	summary.serial = state.serial;
	summary.fold_tag = options.fold_tag;
	if (jitter.unmatched || previous_jitter.unmatched) {
		jitter_unmatched_frames_total++;
	}
	if (route.module_output) {
		module_output_frames_total++;
	}
	summary.stats_valid = false;
	summary.debug_valid = p_inputs.debug_view >= 0 && state.resources.debug.is_valid();
	float mean_m = float(MIN(state.frames_since_cut, params.n_target));
	float validity = use_history ? 1.0f : 0.0f;
	if (options.readback) {
		summary.stats = RestirGiCore::read_stats(state.resources);
		summary.stats_valid = true;
		if (RestirGiCore::read_sky_epoch(state.resources, state.sky_epoch)) {
			state.sky_epoch_frame = int64_t(p_inputs.frame_index);
		}
		RestirFrameRecord record;
		record.frame = p_inputs.frame_index;
		record.stats = summary.stats;
		record.fold.tag = options.fold_tag;
		record.fold.serial = int64_t(state.serial);
		record.fold.clock = int64_t(params.fold_clock);
		record.fold.k = int64_t(fold.k);
		record.fold.k_eff = int64_t(fold.k_eff);
		record.fold.mode = int64_t(fold.mode);
		record.fold.feedback = fold.feedback ? 1 : 0;
		record.fold.cut = cut ? 1 : 0;
		record.fold.cut_reason = int64_t(plan.reason);
		record.fold.pre_frame = params.fold_pre_frame ? 1 : 0;
		record.fold.reval = (params.flags & RESTIR_F_REVAL_FRAME) != 0u ? 1 : 0;
		record.fold.history_used = use_history ? 1 : 0;
		record.fold.trace_w = int64_t(p_inputs.trace_size.x);
		record.fold.trace_h = int64_t(p_inputs.trace_size.y);
		if (state.resources.fold_dump_ran) {
			RestirFoldDump dump;
			if (RestirGiCore::read_fold_dump(state.resources, dump)) {
				record.fold.hp_eq_px = int64_t(summary.stats.fold_hp_eq_px);
				record.fold.expected_phase0 = int64_t(summary.stats.fold_phase0);
				record.fold.phase0_start = int64_t(summary.stats.fold_phase0_start);
				record.fold.refused_off = int64_t(summary.stats.fold_refused_off);
				record.fold.s_max = int64_t(dump.s_max);
				record.fold.n_over = int64_t(dump.n_over);
				record.fold.hist_texels = int64_t(dump.texels);
				int64_t start_sums[RESTIR_FOLD_START_PRE];
				if (restir_fold_start_sums(dump.starts, start_sums)) {
					record.fold.start_cut = start_sums[RESTIR_FOLD_START_CUT];
					record.fold.start_frame = start_sums[RESTIR_FOLD_START_FRAME];
					record.fold.start_plane = start_sums[RESTIR_FOLD_START_PLANE];
					record.fold.start_normal = start_sums[RESTIR_FOLD_START_NORMAL];
					record.fold.start_class = start_sums[RESTIR_FOLD_START_CLASS];
					record.fold.start_no_sample = start_sums[RESTIR_FOLD_START_NO_SAMPLE];
					record.fold.start_vis = start_sums[RESTIR_FOLD_START_VIS];
					record.fold.start_other = start_sums[RESTIR_FOLD_START_OTHER];
				}
				restir_fold_starts_add(dump.starts);
			}
		}
		if (state.resources.fold_lineage_ran) {
			Vector<uint8_t> refused_raw;
			int64_t refused_texels = 0;
			if (RestirGiCore::read_fold_refused_ids(state.resources, refused_raw, refused_texels)) {
				const RestirFoldFamilyCount families = restir_fold_families_add(int(record.fold.tag), reinterpret_cast<const uint32_t *>(refused_raw.ptr()), refused_texels);
				record.fold.lineage_refusals = families.refusals;
				record.fold.lineage_unknown = families.unknown;
				record.fold.lineage_families = families.families;
			}
		}
		int record_index = 0;
		{
			MutexLock lock(restir_frame_records_mutex());
			record_index = int(restir_frame_records().size());
			restir_frame_records().push_back(record);
		}
		if (options.accumulate_from >= 0 && record_index >= options.accumulate_from && record_index < options.accumulate_to) {
			_accumulate_pixels(state.resources);
		}
		if (summary.stats.geometry > 0.0) {
			mean_m = float(summary.stats.m_sum / summary.stats.geometry);
			validity = float(summary.stats.output_pixels / summary.stats.geometry);
		}
	}

	r_output.radiance = state.resources.radiance;
	r_output.coverage = state.resources.coverage;
	r_output.debug = p_inputs.debug_view >= 0 ? state.resources.debug : RID();
	r_output.record.flags = GI_PROVIDER_FLAG_SAMPLE_COUNT | GI_PROVIDER_FLAG_CONFIDENCE | GI_PROVIDER_FLAG_HISTORY | GI_PROVIDER_FLAG_SCENE_EPOCH | GI_PROVIDER_FLAG_LIGHT_EPOCH | GI_PROVIDER_FLAG_SKY_EPOCH | GI_PROVIDER_FLAG_ORIGIN_EPOCH;
	r_output.record.sample_count = uint32_t(Math::round(mean_m));
	r_output.record.validity = CLAMP(validity, 0.0f, 1.0f);
	r_output.record.confidence = gi_provider_confidence_from_samples(r_output.record.sample_count, params.n_target);
	r_output.valid = true;
	r_output.denoise = denoise;
	r_output.history_cut = cut;
	uint32_t route_cap = uint32_t(CLAMP(_gi_int("denoise_history_cap"), 1, 255));
	if (ramp) {
		route_cap = _ramped_cap(route_cap, MIN(ramp_m_min, route_cap), ramp_frames, ramp_since_cut);
	}
	r_output.history_cap = route_cap;
	r_output.mode = route.module_output ? RTGI_OUTPUT_MODULE : RTGI_OUTPUT_PLAIN;
	r_output.n_target = route.n_target;
	summary.history_cap = route_cap;
}

void RestirGiBackend::release(RenderSceneBuffersRD *p_buffers) {
	if (p_buffers) {
		RestirGiState *found = states.getptr(p_buffers);
		if (found) {
			_free_state(*found);
			states.erase(p_buffers);
			release_calls_total++;
		}
		if (states.is_empty()) {
			_release_feed();
		}
		return;
	}
	for (KeyValue<RenderSceneBuffersRD *, RestirGiState> &entry : states) {
		_free_state(entry.value);
	}
	states.clear();
	_release_feed();
	if (core) {
		core->free();
	}
}

void RestirGiBackend::get_status(Dictionary &r_status) const {
	r_status["refuses"] = !refusal_reason.is_empty();
	r_status["ready"] = summary.ran;
	r_status["refusals"] = int64_t(refusals);
	r_status["refusal_reason"] = refusal_reason;
	r_status["states"] = int64_t(states.size());
	r_status["hw_variant_compiled"] = (core && core->hw_variant_compiled());
	r_status["lo_clamp"] = 60000.0;
	r_status["mode"] = int64_t(summary.mode);
	r_status["denoise"] = _gi_text("denoise");
	r_status["frame"] = int64_t(summary.frame);
	r_status["history_used"] = summary.history_used;
	r_status["own_sampler"] = summary.own_sampler;
	r_status["rays"] = summary.rays;
	r_status["hardware"] = summary.hardware;
	r_status["spatial_neighbors"] = int64_t(summary.spatial_count);
	r_status["spatial_passes"] = int64_t(summary.spatial_passes);
	r_status["m_temporal_cap"] = int64_t(summary.m_cap);
	r_status["m_max"] = int64_t(summary.m_max);
	r_status["cut"] = summary.cut;
	r_status["cut_reason"] = int64_t(summary.cut_reason);
	r_status["dirty_boxes"] = int64_t(summary.dirty_count);
	r_status["light_cap"] = summary.light_cap;
	r_status["history_cap"] = int64_t(summary.history_cap);
	r_status["reval_active"] = summary.reval_active;
	r_status["reval_frames"] = int64_t(summary.reval_frames);
	r_status["light_frames_left"] = int64_t(summary.light_frames_left);
	r_status["scene_changed"] = summary.scene_changed;
	r_status["light_changed"] = summary.light_changed;
	r_status["sky_changed"] = summary.sky_changed;
	r_status["origin_changed"] = summary.origin_changed;
	r_status["world_changed"] = summary.world_changed;
	r_status["dirty_feed"] = summary.have_feed;
	r_status["event_reshade"] = summary.events_on;
	r_status["events_read"] = int64_t(summary.events_read);
	r_status["events_lost"] = int64_t(summary.events_lost);
	r_status["jump_frame"] = summary.jump_frame;
	r_status["event_capping_spheres"] = int64_t(summary.event_capping);
	r_status["event_validate"] = int64_t(summary.event_validate);
	r_status["reshade_budget_pixels"] = int64_t(summary.reshade_budget);
	r_status["event_frames_total"] = int64_t(event_frames_total);
	r_status["events_read_total"] = int64_t(events_read_total);
	r_status["event_validate_frames_total"] = int64_t(event_validate_frames_total);
	r_status["event_capping_total"] = int64_t(event_capping_total);
	r_status["spatial_hold"] = summary.hold_on;
	r_status["spatial_hold_frames"] = int64_t(summary.hold_frames);
	r_status["hold_fill"] = int64_t(summary.hold_fill);
	r_status["hold_added"] = int64_t(summary.hold_update.added);
	r_status["hold_refreshed"] = int64_t(summary.hold_update.refreshed);
	r_status["hold_expired"] = int64_t(summary.hold_update.expired);
	r_status["hold_dropped"] = int64_t(summary.hold_update.dropped);
	r_status["hold_frames_total"] = int64_t(hold_frames_total);
	r_status["hold_dropped_total"] = int64_t(hold_dropped_total);
	r_status["hold_fill_max"] = int64_t(hold_fill_max);
	r_status["sky_reread"] = summary.sky_reread;
	int64_t sky_epochs = 0;
	int64_t sky_jump_frames = 0;
	int64_t sky_jump_capped = 0;
	int64_t sky_reread_cuts = 0;
	int64_t sky_epoch_frame = -1;
	for (const KeyValue<RenderSceneBuffersRD *, RestirGiState> &entry : states) {
		sky_reread_cuts += int64_t(entry.value.sky_reread_cuts);
		if (entry.value.sky_epoch_frame < 0) {
			continue;
		}
		sky_epochs += int64_t(entry.value.sky_epoch.epochs);
		sky_jump_frames += int64_t(entry.value.sky_epoch.jumps);
		sky_jump_capped += int64_t(entry.value.sky_epoch.capped);
		sky_epoch_frame = MAX(sky_epoch_frame, entry.value.sky_epoch_frame);
	}
	r_status["sky_epoch_source"] = "cached";
	r_status["sky_epoch_frame"] = sky_epoch_frame;
	r_status["sky_epochs"] = sky_epochs;
	r_status["sky_jump_frames_total"] = sky_jump_frames;
	r_status["sky_jump_capped_total"] = sky_jump_capped;
	r_status["sky_reread_cuts_total"] = sky_reread_cuts;
	r_status["tiles"] = summary.tiles;
	r_status["tile_skip_frames_total"] = int64_t(tile_skip_frames_total);
	r_status["confidence"] = summary.confidence;
	r_status["module_output"] = summary.route.module_output;
	r_status["route_n_target"] = int64_t(summary.route.n_target);
	r_status["module_output_frames_total"] = int64_t(module_output_frames_total);
	r_status["fold_k"] = int64_t(summary.fold.k);
	r_status["fold_k_eff"] = int64_t(summary.fold.k_eff);
	r_status["fold_clock"] = int64_t(summary.fold_clock);
	r_status["fold_pre_frame"] = summary.fold_pre_frame;
	r_status["fold_feedback"] = summary.fold.feedback;
	r_status["fold_tag"] = int64_t(summary.fold_tag);
	r_status["state_serial"] = int64_t(summary.serial);
	r_status["state_creates_total"] = int64_t(state_creates_total);
	r_status["state_rebuilds_total"] = int64_t(state_rebuilds_total);
	r_status["release_calls_total"] = int64_t(release_calls_total);
	r_status["release_tests_total"] = int64_t(release_tests_total);
	r_status["fold_dump_active"] = summary.fold_dump_active;
	r_status["fold_dump_frames_total"] = int64_t(fold_dump_frames_total);
	r_status["fold_lineage_active"] = summary.fold_lineage_active;
	r_status["fold_lineage_frames_total"] = int64_t(fold_lineage_frames_total);
	const int32_t lineage_refusal = restir_test_options().fold_lineage_refusal != RESTIR_FOLD_LINEAGE_REFUSAL_NONE ? restir_test_options().fold_lineage_refusal : summary.fold_lineage_refusal;
	r_status["fold_lineage_refusal"] = String(restir_fold_lineage_refusal_text(lineage_refusal));
	r_status["fold_lineage_refusal_code"] = int64_t(lineage_refusal);
	r_status["jitter_shift"] = PackedFloat64Array({ summary.jitter.ax, summary.jitter.ay, summary.jitter.bx, summary.jitter.by });
	r_status["jitter_ndc"] = PackedFloat64Array({ summary.jitter.jitter_x, summary.jitter.jitter_y });
	r_status["jitter_unmatched"] = summary.jitter.unmatched || summary.previous_jitter.unmatched;
	r_status["jitter_unmatched_frames_total"] = int64_t(jitter_unmatched_frames_total);
	Array cut_counts;
	for (const KeyValue<RenderSceneBuffersRD *, RestirGiState> &entry : states) {
		for (uint32_t i = 0; i < RESTIR_CUT_MAX; i++) {
			if (cut_counts.size() <= int(i)) {
				cut_counts.push_back(int64_t(0));
			}
			cut_counts[i] = int64_t(cut_counts[i]) + int64_t(entry.value.cut_counts[i]);
		}
	}
	r_status["cut_counts"] = cut_counts;
	r_status["trace_size"] = Vector2i(summary.size);
	for (const KeyValue<RenderSceneBuffersRD *, RestirGiState> &entry : states) {
		Array allocations;
		for (const RestirAllocEntry &item : entry.value.resources.alloc_log) {
			Dictionary row;
			row["name"] = String::utf8(item.name);
			row["expected_bytes"] = int64_t(item.expected);
			row["measured_bytes"] = int64_t(item.measured);
			row["buffer"] = item.buffer;
			allocations.push_back(row);
		}
		r_status["allocations"] = allocations;
		break;
	}
	r_status["stats_valid"] = summary.stats_valid;
	r_status["debug_valid"] = summary.debug_valid;
	if (summary.stats_valid) {
		const RestirFrameStats &s = summary.stats;
		Dictionary d;
		d["geometry"] = s.geometry;
		d["output_pixels"] = s.output_pixels;
		d["output_sum"] = Vector3(s.output_sum[0], s.output_sum[1], s.output_sum[2]);
		d["candidate_pixels"] = s.candidate_pixels;
		d["candidate_sum"] = Vector3(s.candidate_sum[0], s.candidate_sum[1], s.candidate_sum[2]);
		d["m_sum"] = s.m_sum;
		d["clamped"] = s.clamped;
		d["blocked"] = s.blocked;
		d["temporal_selected"] = s.temporal_selected;
		d["spatial_selected"] = s.spatial_selected;
		d["violations"] = s.violations;
		d["has_sample"] = s.has_sample;
		d["clamp_share"] = s.geometry > 0.0 ? s.clamped / s.geometry : 0.0;
		d["shift_tested"] = s.shift_tested;
		d["truncated_distance"] = s.rej_distance;
		d["truncated_cos"] = s.rej_cos;
		d["truncated_jacobian"] = s.rej_jacobian;
		d["truncated_candidate_share"] = s.shift_tested > 0.0 ? (s.rej_distance + s.rej_cos + s.rej_jacobian) / s.shift_tested : 0.0;
		d["energy_all"] = s.energy_all;
		d["energy_truncated"] = s.energy_truncated;
		d["truncated_energy_share"] = s.energy_all > 0.0 ? s.energy_truncated / s.energy_all : 0.0;
		d["pdf_refused"] = s.pdf_refused;
		d["techniques_present"] = s.techniques_present;
		d["techniques_sample_unusable"] = s.techniques_sample_unusable;
		d["spatial_combines"] = s.spatial_combines;
		d["lo_refused"] = s.lo_refused;
		d["no_canonical"] = s.no_canonical;
		d["rehit_miss"] = s.rehit_miss;
		d["rehit_mismatch"] = s.rehit_mismatch;
		d["reshaded"] = s.reshaded;
		d["reshade_overflow"] = s.reshade_overflow;
		d["reshade_edge_stale"] = s.reshade_edge_stale;
		d["event_capped"] = s.event_capped;
		d["fold_refused"] = s.fold_refused;
		d["fold_carried"] = s.fold_carried;
		d["fold_renewed"] = s.fold_renewed;
		d["fold_started"] = s.fold_started;
		d["fold_refused_empty"] = s.fold_refused_empty;
		d["temporal_shift_tested"] = s.temporal_shift_tested;
		d["temporal_shift_rejected"] = s.temporal_shift_rejected;
		d["had_history"] = int64_t(s.had_history);
		d["compat_taps"] = s.compat_taps;
		d["compat_early_stops"] = s.compat_early_stops;
		d["compat_empty"] = s.compat_empty;
		d["compat_h_sum"] = s.compat_h_sum;
		Array temporal;
		Array spatial;
		for (int i = 0; i < 16; i++) {
			temporal.push_back(int64_t(s.temporal_reject[i]));
			spatial.push_back(int64_t(s.spatial_reject[i]));
		}
		d["temporal_reject"] = temporal;
		d["spatial_reject"] = spatial;
		r_status["stats"] = d;
	}
}
