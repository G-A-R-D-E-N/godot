/**************************************************************************/
/*  local_exposure.h                                                      */
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

#include "servers/rendering/renderer_rd/shaders/effects/local_exposure.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

#define RB_LOCAL_EXPOSURE_BUFFERS SNAME("local_exposure_buffers")

namespace RendererRD {

class LocalExposure {
public:
	static constexpr int GRID_DEPTH = 64;
	static constexpr int LOG_DIVISOR = 8;
	static constexpr int GRID_DIVISOR = 4;
	static constexpr float LOG_MIN = -20.0;
	static constexpr float LOG_MAX = 16.0;

	struct Settings {
		float spatial_sigma = 1.0;
		float range_sigma = 1.0;
		float temporal_alpha = 1.0;
		bool reset_history = false;
		uint64_t frame = 0;
	};

	class Buffers : public RenderBufferCustomDataRD {
		GDCLASS(Buffers, RenderBufferCustomDataRD);

	public:
		Size2i log_size;
		Size2i grid_size;
		RID log_luminance;
		RID grid[2];
		RID history[2];
		RID wide;
		int history_index = 0;
		bool history_valid = false;
		uint64_t last_frame = 0;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override;
		virtual void free_data() override;
	};

private:
	enum Mode {
		MODE_DOWNSAMPLE,
		MODE_BUILD,
		MODE_BLUR,
		MODE_WIDE,
		MODE_TEMPORAL,
		MODE_MAX
	};

	struct PushConstant {
		int32_t source_size[2];
		int32_t dest_size[2];
		int32_t axis[3];
		float sigma;
	};

	LocalExposureShaderRD shader;
	RID shader_version;
	RID pipelines[MODE_MAX];

public:
	LocalExposure();
	~LocalExposure();

	Ref<Buffers> get_buffers(Ref<RenderSceneBuffersRD> p_render_buffers);
	RID process(RID p_source_texture, const Size2i p_source_size, Ref<Buffers> p_buffers, const Settings &p_settings);
};

} // namespace RendererRD
