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
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/direct_shadows.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_SCOPE_DIRECT_SHADOWS SNAME("rb_direct_shadows")

#define RB_DIRECT_SLOTS SNAME("slots")
#define RB_DIRECT_FACTORS SNAME("factors")
#define RB_DIRECT_ACCUM SNAME("accum")
#define RB_DIRECT_REST SNAME("rest")
#define RB_DIRECT_DEBUG SNAME("debug")

namespace RendererRD {

class DirectShadows {
public:
	struct FrameInput {
		Ref<RenderSceneBuffersRD> render_buffers;
		RID depth;
		RID normal_roughness;
		RID cluster_buffer;
		RID omni_buffer;
		RID spot_buffer;
		Projection projection;
		Transform3D camera;
		Vector3 region_half;
		uint32_t cluster_size = 32;
		uint32_t cluster_max_elements = 512;
		float z_far = 1.0f;
		uint64_t frame = 0;
		uint64_t table_generation = 0;
	};

	struct Settings {
		uint32_t entries = 6;
		uint32_t rays = 1;
		bool reference = false;
		uint32_t debug_view = 0;
	};

	static Settings get_project_settings();
	static uint32_t factor_layers(uint32_t p_entries) { return p_entries + 2; }

private:
	class ViewportData : public RenderBufferCustomDataRD {
		GDCLASS(ViewportData, RenderBufferCustomDataRD)

	public:
		uint32_t entries = 0;
		Size2i size;
		uint64_t last_frame = 0;
		uint64_t table_generation = 0;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override {}
	};

	struct Params {
		float inv_projection[16];
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
		uint32_t pad;
	};

	static constexpr uint32_t COUNTER_BYTES = 32;

	DirectShadowsShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipeline;
	DirectShadowsShaderRD shader_hw;
	RID shader_hw_version;
	PipelineDeferredRD pipeline_hw;
	bool hw_valid = false;
	String defines;

	RID params_buffer;
	RID lamp_buffer;
	uint32_t lamp_buffer_capacity = 0;
	RID counter_buffer;
	RID debug_unused;
	LocalVector<DirectLightTable::GpuSlot> lamp_data;
	String served_request;
	uint64_t size_frame = 0;
	Size2i largest_size;
	Size2i largest_size_before;

	bool _ensure_hw();
	RID _debug_target(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Settings &p_settings);
	void _ensure_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const Settings &p_settings);
	Ref<ViewportData> _viewport(const Ref<RenderSceneBuffersRD> &p_render_buffers);

public:
	void process(const FrameInput &p_input, const DirectLightTable &p_table);
	void release(const Ref<RenderSceneBuffersRD> &p_render_buffers);
	RID get_debug_texture(const Ref<RenderSceneBuffersRD> &p_render_buffers) const;
	void serve_dump_request(const Ref<RenderSceneBuffersRD> &p_render_buffers);

	DirectShadows();
	~DirectShadows();
};

} // namespace RendererRD
