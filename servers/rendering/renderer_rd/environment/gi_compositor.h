/**************************************************************************/
/*  gi_compositor.h                                                       */
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

#include "gi_provider.h"

#include "core/object/ref_counted.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/gi_extra_compose.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/gi_extra_synthetic.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"

#define RB_SCOPE_GI_EXTRA SNAME("rbgi_extra")
#define RB_TEX_EXTRA_RADIANCE SNAME("extra_radiance")
#define RB_TEX_EXTRA_COVERAGE SNAME("extra_coverage")

class RenderSceneBuffersRD;

namespace RendererRD {

static constexpr uint32_t GI_COMPOSITOR_EXTRA_SOURCE = GI_PROVIDER_SOURCE_RTGI;
static constexpr uint32_t GI_COMPOSITOR_SYNTHETIC_SAMPLES = 64;

enum GICompositorSlotState : uint32_t {
	GI_COMPOSITOR_SLOT_NOT_AVAILABLE = 0,
	GI_COMPOSITOR_SLOT_DISABLED = 1,
	GI_COMPOSITOR_SLOT_ENABLED = 2,
};

enum GICompositorSyntheticPattern : uint32_t {
	GI_COMPOSITOR_SYNTHETIC_CONSTANT = 0,
	GI_COMPOSITOR_SYNTHETIC_RAMP = 1,
	GI_COMPOSITOR_SYNTHETIC_CHECKER = 2,
	GI_COMPOSITOR_SYNTHETIC_PATTERN_COUNT = 3,
};

inline GICompositorSlotState gi_compositor_slot_state(bool p_setting_enabled, bool p_written, const GIProviderRecord &p_record, uint32_t p_lower_units) {
	GIIncidentRadiance incident;
	if (!p_written || p_record.source != GI_COMPOSITOR_EXTRA_SOURCE || p_record.units != p_lower_units || !GIIncidentRadiance::try_from_record(p_record, &incident)) {
		return GI_COMPOSITOR_SLOT_NOT_AVAILABLE;
	}
	return p_setting_enabled ? GI_COMPOSITOR_SLOT_ENABLED : GI_COMPOSITOR_SLOT_DISABLED;
}

class GICompositor {
	class ViewportState : public RenderBufferCustomDataRD {
		GDCLASS(ViewportState, RenderBufferCustomDataRD)

	public:
		bool written = false;
		GIProviderRecord record = {};

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override { written = false; }
		virtual void free_data() override { written = false; }
	};

	struct SyntheticPushConstant {
		uint32_t pattern;
		float coverage;
		float radiance;
		uint32_t pad;
	};

	static GICompositor *singleton;

	GiExtraComposeShaderRD compose_shader;
	RID compose_version;
	PipelineDeferredRD compose_pipeline;
	GiExtraSyntheticShaderRD synthetic_shader;
	RID synthetic_version;
	PipelineDeferredRD synthetic_pipeline;

	void _process(const Ref<RenderSceneBuffersRD> &p_render_buffers, uint32_t p_view_count, bool p_setting_enabled, bool p_debug_provider);
	void _run_synthetic(const Ref<RenderSceneBuffersRD> &p_render_buffers, uint32_t p_view_count, const Size2i &p_size, ViewportState *r_state);
	void _compose(const Ref<RenderSceneBuffersRD> &p_render_buffers, uint32_t p_view_count, const Size2i &p_size);
	bool _ambient_can_copy(const Ref<RenderSceneBuffersRD> &p_render_buffers) const;
	void _dump_ambient(const Ref<RenderSceneBuffersRD> &p_render_buffers, const String &p_path) const;

public:
	GICompositor();
	~GICompositor();

	static void process(const Ref<RenderSceneBuffersRD> &p_render_buffers, uint32_t p_view_count);
	static void finalize();

	static Size2i ensure_slot_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers);
	static void publish(const Ref<RenderSceneBuffersRD> &p_render_buffers, const GIProviderRecord &p_record);
	static bool get_record(const Ref<RenderSceneBuffersRD> &p_render_buffers, GIProviderRecord &r_record);
};

} // namespace RendererRD
