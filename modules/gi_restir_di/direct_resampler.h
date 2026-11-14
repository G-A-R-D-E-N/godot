/**************************************************************************/
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
#include "modules/gi_restir_di/direct_sampler_lab.h"
#include "modules/gi_restir_di/shaders/di_resample.glsl.gen.h"
#include "servers/rendering/renderer_rd/environment/direct_resampler_hooks.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"

#define RB_SCOPE_DI_RESAMPLED SNAME("rb_di_resampled")
#define RB_DI_ENTRIES SNAME("entries")
#define RB_DI_RECORD SNAME("record")

class DirectResamplerRD : public RendererRD::DirectResampler {
public:
	struct Settings {
		bool enabled = false;
		uint32_t head = 4;
		uint32_t tail_samples = 1;
		float floor_value = 0.0625f;
		bool live_energy = false;
		uint32_t tail_visibility = 0;
		bool stale_prior = false;
		String debug_log;
	};

	struct Params {
		float inv_projection[16];
		float prev_view_from_view[16];
		float prev_projection[16];
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
		uint32_t pad;
	};
	static_assert(sizeof(Params) == 320);

	static constexpr uint32_t DEBUG_FLAG_STALE_PRIOR = 1;
	static constexpr uint32_t DEBUG_FLAG_COUNT = 2;
	static constexpr uint32_t DEBUG_HEADER_WORDS = 4;
	static constexpr uint32_t DEBUG_SLOTS = 65536;
	static constexpr uint32_t DEBUG_BYTES = (DEBUG_HEADER_WORDS + DEBUG_SLOTS) * 4;

	static void register_settings();
	static Settings clamp_settings(bool p_enabled, int p_head, int p_tail_samples, float p_floor, int p_energy_arm);
	static Settings get_project_settings();

	virtual bool is_enabled() const override;
	virtual bool run(const Input &p_input, Output &r_output) override;
	virtual void release(const Ref<RenderSceneBuffersRD> &p_render_buffers) override;
	virtual void free_device_resources() override;

	~DirectResamplerRD();

private:
	class ViewportData : public RenderBufferCustomDataRD {
		GDCLASS(ViewportData, RenderBufferCustomDataRD)

	public:
		Size2i size;
		uint32_t head = 0;
		uint32_t tail_samples = 0;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override {}
	};

	DiResampleShaderRD *shader = nullptr;
	RID shader_version;
	PipelineDeferredRD pipeline;
	RID params_buffer;
	RID history_slots_dummy;
	RID history_factors_dummy;
	RID history_guide_dummy;
	RID debug_buffer;
	bool device_ready = false;
	Ref<FileAccess> debug_file;
	String debug_path;
	bool debug_pending = false;
	uint64_t debug_frame = 0;
	Size2i debug_size;
	DirectSamplerLab sampler_lab;

	bool _ensure_device();
	void _write_debug_line();
	void _flush_debug_line();
	RID _dummy(RD::DataFormat p_format, RD::TextureType p_type, const char *p_name);
};
