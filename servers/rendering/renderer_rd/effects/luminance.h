/**************************************************************************/
/*  luminance.h                                                           */
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

#include "servers/rendering/renderer_rd/pipeline_cache_rd.h"
#include "servers/rendering/renderer_rd/shaders/effects/luminance_histogram.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/luminance_reduce.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/effects/luminance_reduce_raster.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_LUMINANCE_BUFFERS SNAME("luminance_buffers")

namespace RendererRD {

class Luminance {
private:
	bool prefer_raster_effects;

	enum LuminanceReduceMode {
		LUMINANCE_REDUCE_READ,
		LUMINANCE_REDUCE,
		LUMINANCE_REDUCE_WRITE,
		LUMINANCE_REDUCE_MAX
	};

	struct LuminanceReducePushConstant {
		int32_t source_size[2];
		float max_luminance;
		float min_luminance;
		float exposure_adjust;
		float block_scale;
		float highlight_limit;
		float pad;
	};

	struct LuminanceReduce {
		LuminanceReduceShaderRD shader;
		RID shader_version;
		RID pipelines[LUMINANCE_REDUCE_MAX];
	} luminance_reduce;

	enum LuminanceReduceRasterMode {
		LUMINANCE_REDUCE_FRAGMENT_FIRST,
		LUMINANCE_REDUCE_FRAGMENT,
		LUMINANCE_REDUCE_FRAGMENT_FINAL,
		LUMINANCE_REDUCE_FRAGMENT_MAX
	};

	struct LuminanceReduceRasterPushConstant {
		int32_t source_size[2];
		float exposure_adjust;
		float min_luminance;
		float max_luminance;
		float block_scale;
		float highlight_limit;
		float pad;
	};

	struct LuminanceReduceFragment {
		LuminanceReduceRasterShaderRD shader;
		RID shader_version;
		PipelineCacheRD pipelines[LUMINANCE_REDUCE_FRAGMENT_MAX];
	} luminance_reduce_raster;

	static constexpr int LUMINANCE_HISTOGRAM_BINS = 256;
	static constexpr int LUMINANCE_HISTOGRAM_MAX_SAMPLES = 1 << 21;

	enum LuminanceHistogramMode {
		LUMINANCE_HISTOGRAM_ACCUMULATE,
		LUMINANCE_HISTOGRAM_RESOLVE,
		LUMINANCE_HISTOGRAM_MAX
	};

	struct LuminanceHistogramPushConstant {
		int32_t source_size[2];
		int32_t stride;
		float center_weight;
		float clip_low;
		float clip_high;
		float min_luminance;
		float max_luminance;
		float exposure_adjust;
		uint32_t set_immediate;
		uint32_t pad[2];
	};

	struct LuminanceHistogram {
		LuminanceHistogramShaderRD shader;
		RID shader_version;
		RID pipelines[LUMINANCE_HISTOGRAM_MAX];
	} luminance_histogram;

public:
	class LuminanceBuffers : public RenderBufferCustomDataRD {
		GDCLASS(LuminanceBuffers, RenderBufferCustomDataRD);

	private:
		bool prefer_raster_effects;
		uint32_t current_usage = 0;
		float current_multiplier = 1.0;

	public:
		Vector<RID> reduce;
		RID current;
		RID histogram;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
		virtual void free_data() override;

		~LuminanceBuffers();

		void set_prefer_raster_effects(bool p_prefer_raster_effects);
	};

	Ref<LuminanceBuffers> get_luminance_buffers(Ref<RenderSceneBuffersRD> p_render_buffers);
	RID get_current_luminance_buffer(Ref<RenderSceneBuffersRD> p_render_buffers);
	void luminance_reduction(RID p_source_texture, const Size2i p_source_size, Ref<LuminanceBuffers> p_luminance_buffers, float p_min_luminance, float p_max_luminance, float p_adjust, bool p_set = false, float p_highlight_limit = 0.0);
	void luminance_histogram_meter(RID p_source_texture, const Size2i p_source_size, Ref<LuminanceBuffers> p_luminance_buffers, float p_min_luminance, float p_max_luminance, float p_adjust, bool p_set, float p_clip_low, float p_clip_high, float p_center_weight);
	static int histogram_stride(const Size2i p_size);
	bool supports_histogram_meter() const { return !prefer_raster_effects; }

	Luminance(bool p_prefer_raster_effects);
	~Luminance();
};

} // namespace RendererRD
