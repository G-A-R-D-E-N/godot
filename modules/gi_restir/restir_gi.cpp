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

using namespace RendererRD;

RestirTestOptions &restir_test_options() {
	static RestirTestOptions options;
	return options;
}

LocalVector<RestirFrameRecord> &restir_frame_records() {
	static LocalVector<RestirFrameRecord> records;
	return records;
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
	return RESTIR_MODE_B2;
}

static String _gi_text(const char *p_name) {
	return GLOBAL_GET(String(RESTIR_GI) + p_name);
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

void RestirGiBackend::_free_state(RestirGiState &r_state) {
	r_state.resources.free_all();
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
	RestirGiState &state = states[p_inputs.buffers];
	if (!state.resources.allocated || state.size != p_inputs.trace_size) {
		_free_state(state);
		state = RestirGiState();
		if (!state.resources.allocate(p_inputs.trace_size, p_inputs.debug_view >= 0 || restir_test_options().readback)) {
			refusal_reason = "allocation failed";
			refusals++;
			return false;
		}
		state.size = p_inputs.trace_size;
		RestirGiInputs clear_inputs;
		get_core()->clear_history(state.resources, clear_inputs);
	}
	return true;
}

void RestirGiBackend::resample(const RtgiFrameInputs &p_inputs, RtgiResampleOutput &r_output) {
	r_output.valid = false;
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
			get_core()->run_pack(state.resources, frozen);
			r_output.denoise = true;
			r_output.history_cap = uint32_t(CLAMP(_gi_int("denoise_history_cap"), 1, 255));
		}
		return;
	}
	RayScene *ray_scene = p_inputs.ray_scene && p_inputs.ray_scene->is_ready() ? p_inputs.ray_scene : nullptr;

	RestirEpochInputs epoch_inputs;
	epoch_inputs.have_history = state.have_history;
	epoch_inputs.have_epochs = state.have_epochs;
	epoch_inputs.history_cut = p_inputs.history_cut;
	epoch_inputs.hardware_changed = state.hardware != p_inputs.hardware;
	epoch_inputs.previous = state.epochs;
	epoch_inputs.current = p_inputs.epochs;
	epoch_inputs.anchor = ray_scene ? ray_scene->get_anchor() : Vector3();
	epoch_inputs.light_soft_frames = state.light_soft_frames;
	epoch_inputs.m_light_frames = uint32_t(CLAMP(_gi_int("m_light_frames"), 0, 64));
	RestirEpochPlan plan;
	restir_plan_epochs(epoch_inputs, plan);
	state.light_soft_frames = plan.light_soft_frames;
	const bool cut = plan.cut;
	if (cut) {
		state.history_cuts++;
		state.cut_counts[plan.reason]++;
		state.frames_since_cut = 0;
	} else if (plan.dirty_count > 0) {
		state.dirty_frames++;
	}
	const bool use_history = !cut && options.history;
	const bool dump_wanted = !String(GLOBAL_GET(String(RESTIR_GI) + "dump_path")).is_empty();
	state.resources.ensure_debug(state.resources.debug.is_valid() || p_inputs.debug_view >= 0 || options.readback || options.capture || options.capture_final || dump_wanted);

	RestirFrameParams params;
	params.trace_size = p_inputs.trace_size;
	params.frame_index = p_inputs.frame_index;
	params.seed = uint32_t(int(GLOBAL_GET("rendering/global_illumination/rtgi/seed"))) + options.seed_offset;
	params.mode = _gi_mode();
	params.spatial_count = uint32_t(CLAMP(_gi_int("spatial_neighbors"), 0, 8));
	params.spatial_radius = uint32_t(CLAMP(_gi_int("spatial_radius"), 1, 128));
	params.spatial_passes = uint32_t(CLAMP(_gi_int("spatial_passes"), 1, 2));
	params.spatial_feedback = options.spatial_feedback < 0 ? params.mode == RESTIR_MODE_U1 : options.spatial_feedback > 0;
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
	const String reval_text = _gi_text("revalidate");
	const bool reval_budget = reval_text == "budget";
	const bool reval_share = reval_text == "share";
	if (use_history && ray_scene != nullptr && (reval_budget || reval_share)) {
		params.reval_cut_m = uint32_t(CLAMP(_gi_int("revalidate_cut_m"), 0, 16));
		params.reval_threshold = float(CLAMP(_gi_int("revalidate_threshold"), 1, 1000)) * 0.01f;
		params.reval_share_denom = uint32_t(CLAMP(_gi_int("revalidate_share"), 1, 1024));
		if (reval_share) {
			params.flags |= RESTIR_F_REVAL_SHARE;
			params.reval_pass = true;
		} else {
			const uint32_t period = uint32_t(CLAMP(_gi_int("revalidate_period"), 2, 16));
			if (p_inputs.frame_index % period == period - 1u) {
				params.flags |= RESTIR_F_REVAL_FRAME;
				params.reval_pass = true;
			}
		}
	}
	params.epoch_tag = (p_inputs.epochs.scene & 0xFFu) | ((p_inputs.epochs.light & 0xFFu) << 8u) | ((p_inputs.epochs.origin & 0xFFu) << 16u);
	params.camera_basis = p_inputs.camera_transform.basis;
	params.previous_camera_basis = state.have_history ? p_inputs.previous_camera_transform.basis : p_inputs.camera_transform.basis;
	const Vector3 anchor = ray_scene ? ray_scene->get_anchor() : Vector3();
	params.camera_to_anchor = p_inputs.camera_transform.origin - anchor;
	params.previous_camera_to_anchor = p_inputs.previous_camera_transform.origin - anchor;
	params.view_to_previous_clip = p_inputs.view_to_previous_clip;
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

	state.have_history = true;
	state.hardware = p_inputs.hardware;
	state.epochs = p_inputs.epochs;
	state.have_epochs = true;
	state.prev_epoch_tag = params.epoch_tag;
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
	summary.size = p_inputs.trace_size;
	summary.reval_active = params.reval_pass;
	summary.reval_frames = state.reval_frames;
	summary.stats_valid = false;
	summary.debug_valid = p_inputs.debug_view >= 0 && state.resources.debug.is_valid();
	float mean_m = float(MIN(state.frames_since_cut, params.n_target));
	float validity = use_history ? 1.0f : 0.0f;
	if (options.readback) {
		summary.stats = RestirGiCore::read_stats(state.resources);
		summary.stats_valid = true;
		RestirFrameRecord record;
		record.frame = p_inputs.frame_index;
		record.stats = summary.stats;
		const int record_index = int(restir_frame_records().size());
		restir_frame_records().push_back(record);
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
	r_output.soft_hold_frames = light_cap ? state.light_soft_frames + 1u : 0u;
	uint32_t route_cap = uint32_t(CLAMP(_gi_int("denoise_history_cap"), 1, 255));
	if (ramp) {
		route_cap = _ramped_cap(route_cap, MIN(ramp_m_min, route_cap), ramp_frames, ramp_since_cut);
	}
	r_output.history_cap = route_cap;
	summary.history_cap = route_cap;
}

void RestirGiBackend::release(RenderSceneBuffersRD *p_buffers) {
	if (p_buffers) {
		RestirGiState *found = states.getptr(p_buffers);
		if (found) {
			_free_state(*found);
			states.erase(p_buffers);
		}
		return;
	}
	for (KeyValue<RenderSceneBuffersRD *, RestirGiState> &entry : states) {
		_free_state(entry.value);
	}
	states.clear();
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
