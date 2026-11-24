**************************************************************************/
/*  direct_resampler.h                                                    */
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

#include "core/io/file_access.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "servers/rendering/renderer_rd/environment/direct_resampler_hooks.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"

#include "modules/gi_restir_di/di_resampler_config.gen.h"
#include "modules/gi_restir_di/shaders/di_resample.glsl.gen.h"

#ifdef DI_RESAMPLER_LAB
#include "modules/gi_restir_di/direct_sampler_lab.h"
#endif

#define RB_SCOPE_DI_RESAMPLED SNAME("rb_di_resampled")
#define RB_DI_ENTRIES SNAME("entries")
#define RB_DI_RECORD SNAME("record")
#define RB_DI_TILES SNAME("tiles")
#define RB_DI_SET_HEADER_0 SNAME("set_header_0")
#define RB_DI_SET_HEADER_1 SNAME("set_header_1")
#define RB_DI_SET_VALUES_0 SNAME("set_values_0")
#define RB_DI_SET_VALUES_1 SNAME("set_values_1")
#define RB_DI_HEAD_STATE_0 SNAME("head_state_0")
#define RB_DI_HEAD_STATE_1 SNAME("head_state_1")
#define RB_DI_EMITTER_HISTORY SNAME("emitter_history")

class DirectResamplerRD : public RendererRD::DirectResampler {
public:
	struct Settings {
		bool enabled = false;
		bool emitters = false;
		bool emitter_temporal = false;
		uint32_t head = 4;
		uint32_t tail_samples = 1;
		float floor_value = 0.0625f;
		bool live_energy = true;
		uint32_t tail_visibility = 0;
		bool stale_prior = false;
		String debug_log;
#ifdef DI_RESAMPLER_LAB
		bool no_stamp = false;
		String lab_log;
#endif
		uint32_t reservoirs = 0;
		bool temporal_reuse = true;
		uint32_t spatial_neighbors = 2;
		float spatial_radius = 16.0f;
		uint32_t c_full = 32;
		uint32_t m_temporal_cap = 20;
		uint32_t m_stored_cap = 255;
		uint32_t dynamic_cap = 2;
		bool head_rank_visible = false;
		float head_swap_margin = 0.25f;
		uint32_t head_swap_frames = 4;
		bool head_window = false;
	};

	struct Params {
		float inv_projection[16];
		float prev_view_from_view[16];
		float prev_projection[16];
		float prev_inv_projection[16];
		float camera_basis[12];
		float region_half[3];
		float floor_value;
		int32_t size[2];
		float pixel_size[2];
		uint32_t cluster_shift;
		uint32_t cluster_width;
		uint32_t cluster_type_size;
		uint32_t max_cluster_element_count_div_32;
		float z_far;
		uint32_t frame;
		uint32_t head_entries;
		uint32_t tail_samples;
		uint32_t history_valid;
		uint32_t live_energy;
		uint32_t debug_flags;
		uint32_t reservoirs;
		uint32_t c_full;
		uint32_t m_temporal_cap;
		uint32_t dynamic_cap;
		uint32_t m_stored_cap;
		uint32_t spatial_neighbors;
		float spatial_radius;
		uint32_t counter;
		uint32_t head_rank;
		float head_swap_margin;
		uint32_t head_swap_frames;
		uint32_t temporal_reuse;
		uint32_t head_window;
	};
	static_assert(sizeof(Params) == 432);

	static constexpr uint32_t RESERVOIRS_OFF = 0;
	static constexpr uint32_t RESERVOIRS_B1 = 1;
	static constexpr uint32_t RESERVOIRS_U1 = 2;
	static constexpr uint32_t MAX_NEIGHBORS = 4;
	static constexpr uint32_t MAX_MASS = 255;
	static constexpr uint32_t MAX_SWAP_FRAMES = 7;
	static constexpr uint32_t MAX_EMITTER_TAIL_SUM = 6;
	static constexpr uint32_t EMITTER_SAMPLE_FLOATS = 8;
	static constexpr uint32_t EMITTER_CHUNK = 4096;
	static constexpr uint32_t EMITTER_CHUNKS = 256;
	static constexpr uint32_t EMITTER_SCAN_FLOATS = EMITTER_CHUNKS * 2 + 1;

	static constexpr uint32_t DEBUG_FLAG_STALE_PRIOR = 1;
	static constexpr uint32_t DEBUG_FLAG_COUNT = 2;
	static constexpr uint32_t DEBUG_HEADER_WORDS = 4;
	static constexpr uint32_t DEBUG_SLOTS = 65536;
	static constexpr uint32_t DEBUG_BYTES = (DEBUG_HEADER_WORDS + DEBUG_SLOTS) * 4;
#ifdef DI_RESAMPLER_LAB
	static constexpr uint32_t DEBUG_FLAG_NO_STAMP = 4;
	static constexpr uint32_t LAB_HEADER_WORDS = 8;
	static constexpr uint32_t LAB_BYTES = (LAB_HEADER_WORDS + DEBUG_SLOTS) * 4;
	static constexpr uint32_t LAB_MASK_BYTES = 2560 * 1440 / 8;
#endif

	static void register_settings();
	static Settings clamp_settings(bool p_enabled, int p_head, int p_tail_samples, float p_floor, int p_energy_arm);
	static void clamp_reservoir_settings(Settings &r_settings, int p_reservoirs, bool p_temporal_reuse, int p_spatial_neighbors, float p_spatial_radius, int p_c_full, int p_m_temporal_cap, int p_m_stored_cap, int p_dynamic_cap, int p_head_rank, float p_head_swap_margin, int p_head_swap_frames, bool p_head_window);
	static uint32_t advance_counter(uint32_t p_counter, bool p_continuous);
	static Settings get_project_settings();
#ifdef DI_RESAMPLER_LAB
	static uint32_t lab_mass_bound(const Settings &p_settings);
	static String lab_settings_text(const Settings &p_settings);
#endif

	virtual bool is_enabled() const override;
	virtual bool run(const Input &p_input, Output &r_output) override;
	virtual void release(const Ref<RenderSceneBuffersRD> &p_render_buffers) override;
	virtual void free_device_resources() override;

	~DirectResamplerRD();

private:
	class ResampledViewportData : public RenderBufferCustomDataRD {
		GDCLASS(ResampledViewportData, RenderBufferCustomDataRD)

	public:
		Size2i size;
		uint32_t head = 0;
		uint32_t tail_samples = 0;
		bool reservoir_sets = false;
		bool head_state = false;
		bool emitters = false;
		bool emitter_temporal = false;
		uint32_t counter = 1;
		uint32_t history_set = 0;
		uint32_t head_state_set = 0;
		uint64_t last_frame = 0;
		bool swap_sets = false;
		bool swap_head_state = false;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override {}
	};

	enum ShaderVariant {
		VARIANT_MAIN = 0,
		VARIANT_SPATIAL = 1,
		VARIANT_MAIN_EMITTER = 2,
		VARIANT_EMITTER_SUM = 3,
		VARIANT_EMITTER_SCAN = 4,
		VARIANT_EMITTER_PREFIX = 5,
		VARIANT_MAIN_EMITTER_TEMPORAL = 6,
		VARIANT_EMITTER_SPATIAL = 9,
#ifdef DI_RESAMPLER_LAB
		VARIANT_MAIN_LAB = 7,
		VARIANT_SPATIAL_LAB = 8,
#endif
	};

	DiResampleShaderRD *shader = nullptr;
	RID shader_version;
	PipelineDeferredRD pipeline;
	PipelineDeferredRD spatial_pipeline;
	PipelineDeferredRD emitter_main_pipeline;
	PipelineDeferredRD emitter_temporal_pipeline;
	PipelineDeferredRD emitter_spatial_pipeline;
	PipelineDeferredRD emitter_sum_pipeline;
	PipelineDeferredRD emitter_scan_pipeline;
	PipelineDeferredRD emitter_prefix_pipeline;
	RID params_buffer;
	RID header_dummy;
	RID history_array_dummy;
	RID history_guide_dummy;
	RID debug_buffer;
	RID emitter_cdf;
	RID emitter_scan;
	RID emitter_samples;
	uint32_t emitter_cdf_capacity = 0;
	uint32_t emitter_samples_bytes = 0;
	bool emitter_device_ready = false;
	bool device_ready = false;
	Ref<FileAccess> debug_file;
	String debug_path;
	bool debug_pending = false;
	uint64_t debug_frame = 0;
	Size2i debug_size;
#ifdef DI_RESAMPLER_LAB
	PipelineDeferredRD lab_pipeline;
	PipelineDeferredRD lab_spatial_pipeline;
	RID lab_buffer;
	RID lab_mask_buffer;
	Ref<FileAccess> lab_file;
	String lab_path;
	String lab_settings;
	bool lab_pending = false;
	uint64_t lab_frame = 0;
	Size2i lab_size;
	DirectSamplerLab sampler_lab;
#endif

	bool _ensure_device();
	bool _ensure_emitter_buffers(uint32_t p_count);
	bool _ensure_emitter_samples(uint32_t p_pixels);
	void _free_emitter_buffers();
	void _write_debug_line();
	void _flush_debug_line();
#ifdef DI_RESAMPLER_LAB
	void _write_lab_line();
	void _write_lab_mask();
	void _flush_lab_line();
#endif
	RID _dummy(RD::DataFormat p_format, RD::TextureType p_type, const char *p_name);
};
