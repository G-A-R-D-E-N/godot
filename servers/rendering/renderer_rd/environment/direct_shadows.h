/**************************************************************************/
/*  direct_shadows.h                                                      */
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

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/environment/direct_lights.h"
#include "servers/rendering/renderer_rd/environment/direct_resampler_hooks.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/direct_shadows.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/direct_shadows_filter.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/direct_shadows_resolve.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_SCOPE_DIRECT_SHADOWS SNAME("rb_direct_shadows")

#define RB_DIRECT_SLOTS SNAME("slots")
#define RB_DIRECT_SLOTS_OTHER SNAME("slots_other")
#define RB_DIRECT_ACCUM SNAME("accum")
#define RB_DIRECT_ACCUM_OTHER SNAME("accum_other")
#define RB_DIRECT_REST SNAME("rest")
#define RB_DIRECT_REST_OTHER SNAME("rest_other")
#define RB_DIRECT_GUIDE SNAME("guide")
#define RB_DIRECT_GUIDE_OTHER SNAME("guide_other")
#define RB_DIRECT_FACTORS SNAME("factors")
#define RB_DIRECT_SCRATCH SNAME("scratch")
#define RB_DIRECT_RAY SNAME("ray")
#define RB_DIRECT_DEBUG SNAME("debug")
#define RB_DIRECT_FACTORS_BLOCK SNAME("factors_block")
#define RB_DIRECT_SLOTS_RESOLVED SNAME("slots_resolved")
#define RB_DIRECT_TIER SNAME("tier")
#define RB_DIRECT_DEPTH_FULL SNAME("depth_full")
#define RB_DIRECT_NORMAL_FULL SNAME("normal_full")

namespace RendererRD {

class DirectShadows {
public:
	struct FrameInput : DirectFrameView {
		uint64_t table_generation = 0;
		RID entries;
		RID emitter_samples;
		uint32_t head_entries = 0;
		uint32_t tail_samples = 0;
		uint32_t tail_visibility = 0;
		Vector2 jitter;
	};

	struct Settings {
		float emitter_shorten = 0.0f;
		int emitter_skip_instance = -1;
		uint32_t entries = 6;
		uint32_t rays = 1;
		bool reference = false;
		uint32_t debug_view = 0;
		bool filter = true;
		bool force_one = false;
		uint32_t cold_frames = 0;
		uint32_t history_cap = 32;
		uint32_t dynamic_history_cap = 2;
		float clamp_sigmas = 3.0f;
		float kernel_scale = 1.0f;
		float kernel_floor = 0.6f;
		float range_sigmas = 1.5f;
		bool quarter = false;
		bool second_pass = false;

		bool precise() const { return reference || history_cap == 0; }
		bool filtered() const { return filter && !precise(); }
		bool wants_quarter() const { return quarter && !precise(); }
	};

	static Settings get_project_settings();
	static uint32_t factor_layers(uint32_t p_entries) { return p_entries + 3; }
	static uint32_t accum_layers(uint32_t p_entries, bool p_precise) { return MAX(2u, p_precise ? p_entries : p_entries - 1); }
	static Size2i block_grid(const Size2i &p_size) { return Size2i((p_size.x + 1) / 2, (p_size.y + 1) / 2); }

private:
	class ViewportData : public RenderBufferCustomDataRD {
		GDCLASS(ViewportData, RenderBufferCustomDataRD)

	public:
		uint32_t entries = 0;
		Size2i size;
		bool precise = false;
		bool quarter = false;
		Size2i grid;
		bool flip = false;
		uint64_t last_frame = 0;
		bool counted = false;
		uint64_t table_generation = 0;
		Transform3D last_camera;
		Projection last_projection;
		Vector2 last_jitter;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override {}
	};

	struct Params {
		float inv_projection[16];
		float prev_view_from_view[16];
		float prev_projection[16];
		float camera_basis[12];
		float region_half[3];
		float bias;
		int32_t size[2];
		float pixel_size[2];
		uint32_t cluster_shift;
		uint32_t cluster_width;
		uint32_t cluster_type_size;
		uint32_t max_cluster_element_count_div_32;
		float z_far;
		uint32_t frame;
		uint32_t last_frame;
		uint32_t entries;
		uint32_t rays;
		uint32_t reference;
		uint32_t debug_view;
		uint32_t history_valid;
		uint32_t filter_on;
		uint32_t force_one;
		float history_cap;
		float clamp_sigmas;
		float clamp_min_rays;
		float kernel_scale;
		float kernel_floor;
		float world_per_pixel;
		float range_scale;
		float dynamic_cap;
		uint32_t count;
		uint32_t head_entries;
		uint32_t tail_visibility;
		uint32_t cold_frames;
		int32_t full_size[2];
		float jitter_delta[2];
		uint32_t pad[2];
	};
	static_assert(sizeof(Params) == 400);

	static constexpr uint32_t COUNTER_WORDS = 16;
	static constexpr uint32_t EMITTER_HIT_INSTANCES = 4096;
	static constexpr uint32_t COUNTER_BYTES = COUNTER_WORDS * 4;

	enum TraceMode {
		TRACE_CAPPED,
		TRACE_PRECISE,
		TRACE_CAPPED_EXTERNAL,
		TRACE_PRECISE_EXTERNAL,
		TRACE_MAX,
	};

	enum FilterMode {
		FILTER_CLAMP,
		FILTER_FIRST,
		FILTER_SECOND,
		FILTER_SECOND_EXTERNAL,
		FILTER_MAX,
	};

	enum QuarterFilterMode {
		QUARTER_CLAMP,
		QUARTER_FIRST,
		QUARTER_SECOND,
		QUARTER_FIRST_FINAL,
		QUARTER_MAX,
	};

	DirectShadowsShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipeline[TRACE_MAX];
	DirectShadowsShaderRD shader_hw;
	RID shader_hw_version;
	PipelineDeferredRD pipeline_hw[TRACE_MAX];
	bool hw_valid = false;
	DirectShadowsFilterShaderRD filter_shader;
	RID filter_shader_version;
	PipelineDeferredRD filter_pipeline[FILTER_MAX];
	DirectShadowsShaderRD quarter_shader;
	RID quarter_shader_version;
	PipelineDeferredRD quarter_pipeline;
	DirectShadowsShaderRD quarter_shader_hw;
	RID quarter_shader_hw_version;
	PipelineDeferredRD quarter_pipeline_hw;
	DirectShadowsFilterShaderRD quarter_filter_shader;
	RID quarter_filter_shader_version;
	PipelineDeferredRD quarter_filter_pipeline[QUARTER_MAX];
	DirectShadowsResolveShaderRD resolve_shader;
	RID resolve_shader_version;
	PipelineDeferredRD resolve_pipeline;
	bool quarter_valid = false;
	bool quarter_hw_tried = false;
	bool quarter_hw_valid = false;
	String defines;

	RID params_buffer;
	RID lamp_buffer;
	uint32_t lamp_buffer_capacity = 0;
	RID exclude_buffer;
	uint32_t exclude_buffer_capacity = 0;
	RID counter_buffer;
	RID emitter_hit_hist;
	RID debug_unused;
	RID resampled_dummy;
	RID lamp_dummy;
	RID emitter_dummy;
	bool lamps_prepared = false;
	String lamps_dump_request;
	RID tier_unused;
	RID depth_unused;
	RID normal_unused;
	LocalVector<DirectLightTable::GpuSlot> lamp_data;
	LocalVector<DirectLightTable::GpuExclude> exclude_data;
	LocalVector<float> lamp_dump;
	LocalVector<float> dump_lamp_data;
	uint64_t dump_hard_resets = 0;
	uint64_t dump_dynamic_lamp_frames = 0;
	String served_request;
	uint64_t moved_signature = 0;
	Vector3 moved_anchor;
	HashMap<uint64_t, AABB> moved_known;
	LocalVector<AABB> moved;
	uint64_t moved_frame = 0;
	uint64_t size_frame = 0;
	Size2i largest_size;
	Size2i largest_size_before;

	static Vector<String> _trace_modes();
	bool _ensure_hw();
	bool _ensure_quarter_hw();
	void _filter(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const Settings &p_settings, bool p_external);
	void _resolve(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const FrameInput &p_input);
	RID _dump_target(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const StringName &p_name, RD::DataFormat p_format, RID p_unused);
	RID _debug_target(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Settings &p_settings);
	void _ensure_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const Settings &p_settings, bool p_quarter);

	Ref<ViewportData> _viewport(const Ref<RenderSceneBuffersRD> &p_render_buffers);

public:
	bool prepare_lamps(const FrameInput &p_input, DirectLightTable &p_table);
	bool process(const FrameInput &p_input, DirectLightTable &p_table);
	RID get_lamp_buffer() const { return lamp_buffer; }
	bool get_history(const Ref<RenderSceneBuffersRD> &p_render_buffers, uint64_t p_table_generation, RID &r_slots, RID &r_factors, Transform3D &r_camera, Projection &r_projection, RID &r_guide) const;
	RID get_resampled_dummy() const { return resampled_dummy; }
	RID get_lamp_dummy() const { return lamp_dummy; }
	RID get_slots(const Ref<RenderSceneBuffersRD> &p_render_buffers) const;
	RID get_factors(const Ref<RenderSceneBuffersRD> &p_render_buffers) const;
	void release(const Ref<RenderSceneBuffersRD> &p_render_buffers);
	RID get_debug_texture(const Ref<RenderSceneBuffersRD> &p_render_buffers) const;
	void serve_dump_request(const Ref<RenderSceneBuffersRD> &p_render_buffers);

	DirectShadows();
	~DirectShadows();
};

} // namespace RendererRD


