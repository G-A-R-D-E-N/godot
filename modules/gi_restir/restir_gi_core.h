/**************************************************************************/
/*  restir_gi_core.h                                                      */
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

#include "core/math/basis.h"
#include "core/math/color.h"
#include "core/math/projection.h"
#include "core/math/vector2i.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/rendering_device.h"

#include "restir_events.h"
#include "restir_fold_mirror.h"
#include "shaders/restir_gi.glsl.gen.h"

namespace RendererRD {
class RayScene;
}

struct RestirFoldDumpObjects;
struct RestirFoldTestObjects;

enum RestirFlags : uint32_t {
	RESTIR_F_UNBIASED = 1,
	RESTIR_F_VIS_REUSE = 2,
	RESTIR_F_FRESH_LO = 4,
	RESTIR_F_DETERMINISTIC = 8,
	RESTIR_F_HISTORY = 16,
	RESTIR_F_VIS = 32,
	RESTIR_F_PROBE_STATS = 128,
	RESTIR_F_PROBE_PIXELS = 512,
	RESTIR_F_COMPAT = 1024,
	RESTIR_F_COMPAT_NONE = 2048,
	RESTIR_F_COMPAT_BINARY = 4096,
	RESTIR_F_COMPAT_STOP = 8192,
	RESTIR_F_OWN_SAMPLER = 16384,
	RESTIR_F_REAL_SKY = 32768,
	RESTIR_F_SHADE_DDGI = 65536,
	RESTIR_F_DEBUG_OUT = 131072,
	RESTIR_F_KEEP_BLOCKED = 262144,
	RESTIR_F_PRE_FRAME = 524288,
	RESTIR_F_REVAL_FRAME = 1048576,
	RESTIR_F_REVAL_SHARE = 2097152,
	RESTIR_F_KEEP_M = 4194304,
	RESTIR_F_REPLAY = 8388608,
	RESTIR_F_EVENTS = 16777216,
	RESTIR_F_SPATIAL_HOLD = 33554432,
	RESTIR_F_SKY_REREAD = 67108864,
	RESTIR_F_TILE_SKIP = 134217728,
	RESTIR_F_PACK_M = 268435456,
};

static constexpr uint32_t RESTIR_SPATIAL_SMALL_COUNT = 3;
static constexpr uint32_t RESTIR_PASS_EVENT_COUNT = 4;

enum RestirCompatGate : uint32_t {
	RESTIR_GATE_SIMILAR = 0,
	RESTIR_GATE_NONE = 1,
	RESTIR_GATE_BINARY = 2,
};

enum RestirMode : uint32_t {
	RESTIR_MODE_B1 = 0,
	RESTIR_MODE_B2 = 1,
	RESTIR_MODE_U1 = 2,
	RESTIR_MODE_B2K = 3,
};

struct RestirFrameParams {
	Size2i trace_size;
	Size2i screen_size;
	int32_t trace_scale = 1;
	uint32_t frame_index = 0;
	uint32_t seed = 0;
	uint32_t flags = 0;
	uint32_t mode = RESTIR_MODE_B1;
	uint32_t spatial_count = 3;
	uint32_t spatial_radius = 24;
	uint32_t spatial_passes = 1;
	bool spatial_feedback = true;
	bool compat = false;
	uint32_t compat_k = 16;
	uint32_t compat_gate = RESTIR_GATE_SIMILAR;
	bool compat_early_stop = true;
	uint32_t m_temporal_cap = 20;
	uint32_t m_max = 32;
	uint32_t n_target = 8;
	uint32_t epoch_tag = 0;
	uint32_t debug_view = 0;
	uint32_t vis_fail_limit = 2;
	bool reval_pass = false;
	uint32_t reval_cut_m = 1;
	uint32_t reval_share_denom = 8;
	float reval_threshold = 0.5f;
	Basis camera_basis;
	Basis previous_camera_basis;
	Vector3 camera_to_anchor;
	Vector3 previous_camera_to_anchor;
	Projection view_to_previous_clip;
	float jitter_shift[4] = {};
	float previous_jitter_shift[4] = {};
	Color test_sky = Color(1, 1, 1, 1);
	float tol_plane = 0.02f;
	float cos_normal = 0.9063078f;
	float jac_max = 8.0f;
	float d_min = 0.03f;
	float cos_min = 0.05f;
	float w_clamp = 20.0f;
	float origin_scale = 2.0f;
	float origin_floor = 0.0f;
	float sky_distance = 1000.0f;
	float class_tol = 0.35f;
	float emission_exposure = 1.0f;
	float shade_sky[3] = {};
	uint32_t shade_sky_flags = 0;
	float shade_sky_border[2] = {};
	float shade_sky_energy = 1.0f;
	float shade_bounce_energy = 0.0f;
	float shade_exposure = 1.0f;
	uint32_t prev_epoch_tag = 0xFFFFFFFFu;
	uint32_t dirty_count = 0;
	float dirty_margin = 0.01f;
	bool sky_reread = false;
	bool tile_skip = false;
	bool pack_m = false;
	uint32_t fold_clock = 0;
	uint32_t fold_k_eff = 0;
	bool fold_pre_frame = false;
	Vector3 dirty_min[64];
	Vector3 dirty_max[64];
	RestirEventsUBO events = {};
};

struct RestirGiInputs {
	RID geo_position;
	RID geo_normal;
	RID previous_geo_position;
	RID previous_geo_normal;
	RID candidate_first;
	RID candidate_second;
	RID ddgi_ubo;
	RID ddgi_probe_data;
	RID ddgi_irradiance;
	RID ddgi_distance;
	RID sky_texture;
	RID tile_texture;
	RendererRD::RayScene *ray_scene = nullptr;
	bool hardware = false;
	bool use_rays = false;
};

struct RestirAllocEntry {
	const char *name = "";
	uint64_t expected = 0;
	uint64_t measured = 0;
	bool buffer = false;
};

struct RestirGiResources {
	Size2i size;
	LocalVector<RestirAllocEntry> alloc_log;
	RID reservoir[3];
	RID vis_state;
	RID info;
	RID radiance;
	RID coverage;
	RID debug;
	RID debug_dummy;
	RID stats;
	RID pass_stats;
	RID sel_image;
	RID sel_dummy;
	RID ubo[6];
	RID dirty_ubo;
	RID count_ubo;
	RID event_ubo;
	RID event_bins;
	RID sky_epoch;
	uint32_t group_count = 0;
	uint32_t history_index = 0;
	uint32_t final_index = 0;
	bool allocated = false;
	RID fold_dump;
	bool fold_dump_ran = false;
	RID fold_lineage[2];
	RID fold_refused_id;
	bool fold_lineage_ran = false;
	int32_t fold_lineage_written = -1;
	int32_t fold_lineage_refusal = RESTIR_FOLD_LINEAGE_REFUSAL_NONE;
	bool fold_lineage_failed = false;

	bool allocate(const Size2i &p_size, bool p_debug = true);
	void ensure_debug(bool p_needed);
	void ensure_fold_dump(bool p_needed);
	void ensure_fold_lineage(bool p_needed);
	void free_fold_lineage();
	void free_all();
};

struct RestirFoldDump {
	uint32_t s_max = 0;
	uint32_t n_over = 0;
	uint32_t texels = 0;
	Vector<uint32_t> starts;
};

struct RestirFrameStats {
	uint32_t groups = 0;
	double geometry = 0.0;
	double output_pixels = 0.0;
	double output_sum[3] = {};
	double candidate_pixels = 0.0;
	double candidate_sum[3] = {};
	double m_sum = 0.0;
	double clamped = 0.0;
	double blocked = 0.0;
	double temporal_selected = 0.0;
	double spatial_selected = 0.0;
	double violations = 0.0;
	double has_sample = 0.0;
	double shift_tested = 0.0;
	double rej_distance = 0.0;
	double rej_cos = 0.0;
	double rej_jacobian = 0.0;
	double energy_all = 0.0;
	double energy_truncated = 0.0;
	double pdf_refused = 0.0;
	double techniques_present = 0.0;
	double techniques_sample_unusable = 0.0;
	double spatial_combines = 0.0;
	double lo_refused = 0.0;
	double no_canonical = 0.0;
	double temporal_shift_tested = 0.0;
	double temporal_shift_rejected = 0.0;
	double compat_taps = 0.0;
	double compat_early_stops = 0.0;
	double compat_empty = 0.0;
	double compat_h_sum = 0.0;
	double rehit_miss = 0.0;
	double rehit_mismatch = 0.0;
	double event_capped = 0.0;
	double reshaded = 0.0;
	double reshade_overflow = 0.0;
	double reshade_edge_stale = 0.0;
	double fold_refused = 0.0;
	double fold_carried = 0.0;
	double fold_renewed = 0.0;
	double fold_started = 0.0;
	double fold_refused_empty = 0.0;
	double fold_hp_eq_px = 0.0;
	double fold_phase0 = 0.0;
	double fold_phase0_start = 0.0;
	double fold_refused_off = 0.0;
	uint32_t temporal_reject[16] = {};
	uint32_t spatial_reject[16] = {};
	uint32_t had_history = 0;
};

class RestirGiCore {
public:
	enum Variant {
		VARIANT_NONE,
		VARIANT_SW,
		VARIANT_HW,
		VARIANT_MAX
	};

	enum PassMode {
		PASS_CLEAR,
		PASS_TEMPORAL,
		PASS_SPATIAL,
		PASS_FINAL,
		PASS_SOURCE,
		PASS_CAPTURE,
		PASS_SPATIAL_COMPAT,
		PASS_PACK,
		PASS_SPATIAL_WIDE,
		PASS_SPATIAL_COMPAT_WIDE,
		PASS_VALIDATE,
		PASS_TEMPORAL_SKY,
		PASS_SPATIAL_SKY,
		PASS_SPATIAL_COMPAT_SKY,
		PASS_SPATIAL_WIDE_SKY,
		PASS_SPATIAL_COMPAT_WIDE_SKY,
		PASS_SKY_EPOCH,
		PASS_MAX
	};

	static constexpr int PASS_SKY_FIRST = PASS_TEMPORAL_SKY;

private:
	RestirGiShaderRD shaders[VARIANT_MAX];
	RID versions[VARIANT_MAX];
	RestirGiShaderRD sky_shaders[VARIANT_MAX];
	RID sky_versions[VARIANT_MAX];
	PipelineDeferredRD pipelines[VARIANT_MAX][PASS_MAX];
	bool initialized[VARIANT_MAX] = {};
	bool valid[VARIANT_MAX] = {};
	bool sky_valid[VARIANT_MAX] = {};
	uint32_t hw_compiles = 0;
	RID dummy_ubo;
	RID dummy_probe;
	RID dummy_float;
	RID dummy_tiles;
	bool octmap_array = false;
	RestirFoldDumpObjects *dump_objects[VARIANT_MAX] = {};
	bool dump_tried[VARIANT_MAX] = {};
	RestirFoldDumpObjects *lineage_objects[VARIANT_MAX] = {};
	bool lineage_tried[VARIANT_MAX] = {};
	RestirFoldTestObjects *fold_test = nullptr;
	bool fold_test_tried = false;

	bool _ensure_dump(Variant p_variant, bool p_lineage);
	bool _dispatch_dump(Variant p_variant, bool p_sky, bool p_lineage, RID p_ubo, const LocalVector<RD::Uniform> &p_set0, const RestirGiInputs &p_inputs, const Size2i &p_size);
	RID _shade_set(RID p_shader, const RestirGiInputs &p_inputs);
	bool _ensure_fold_test();
	void _free_fold_debug();

	bool _ensure(Variant p_variant);
	bool _compile(RestirGiShaderRD &r_shader, RID &r_version, const Vector<String> &p_modes, Variant p_variant, int p_first_pass);
	RID _shader_rid(Variant p_variant, PassMode p_mode);
	String _defines(Variant p_variant) const;
	void _upload(RID p_ubo, const RestirFrameParams &p_params, uint32_t p_pass_index);
	RID _sky_texture(const RestirGiInputs &p_inputs) const;
	RID _tile_texture(const RestirGiInputs &p_inputs);
	void _bind_shade(RID p_shader, RD::ComputeListID p_list, const RestirGiInputs &p_inputs);
	void _dispatch(Variant p_variant, PassMode p_mode, RID p_ubo, const LocalVector<RD::Uniform> &p_set0, const RestirGiInputs &p_inputs, const Size2i &p_size);

public:
	~RestirGiCore();

	bool is_variant_valid(Variant p_variant);
	bool is_sky_valid(Variant p_variant);
	bool hw_variant_compiled() const { return initialized[VARIANT_HW] && valid[VARIANT_HW]; }
	uint32_t get_hw_compiles() const { return hw_compiles; }

	void clear_history(RestirGiResources &r_resources, const RestirGiInputs &p_inputs);
	bool run_capture(const RestirGiInputs &p_inputs, const Size2i &p_size, RID r_out[6]);
	void run_source(RestirGiResources &r_resources, const RestirFrameParams &p_params, const RestirGiInputs &p_inputs);
	void run_frame(RestirGiResources &r_resources, const RestirFrameParams &p_params, const RestirGiInputs &p_inputs);
	void run_pack(RestirGiResources &r_resources, const RestirGiInputs &p_inputs);
	bool run_fold_test(const RestirFoldTestArgs &p_args, Vector<uint32_t> &r_words, String &r_error);
	void free();

	static RestirFrameStats read_stats(const RestirGiResources &p_resources);
	static bool read_fold_dump(const RestirGiResources &p_resources, RestirFoldDump &r_dump);
	static bool read_fold_refused_ids(const RestirGiResources &p_resources, Vector<uint8_t> &r_raw, int64_t &r_texels);
	static Vector<uint8_t> read_selection(const RestirGiResources &p_resources);
	static bool read_sky_epoch(const RestirGiResources &p_resources, RestirSkyEpoch &r_epoch);
};
