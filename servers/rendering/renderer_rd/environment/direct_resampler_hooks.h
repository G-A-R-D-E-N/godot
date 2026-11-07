/**************************************************************************/
/*  direct_resampler_hooks.h                                              */
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
#include "core/templates/rid.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"

namespace RendererRD {

class DirectResampler {
public:
	struct Input {
		Ref<RenderSceneBuffersRD> render_buffers;
		RID depth;
		RID normal_roughness;
		RID cluster_buffer;
		RID omni_buffer;
		RID spot_buffer;
		RID lamp_buffer;
		RID history_slots;
		RID history_factors;
		RID history_guide;
		Transform3D history_camera;
		Projection history_projection;
		Projection projection;
		Transform3D camera;
		Vector3 region_half;
		uint32_t cluster_size = 32;
		uint32_t cluster_max_elements = 512;
		float z_far = 1.0f;
		uint64_t frame = 0;
	};

	struct Output {
		RID entries;
		RID record;
		uint32_t head_entries = 0;
		uint32_t tail_samples = 0;
	};

	virtual bool is_enabled() const = 0;
	virtual bool run(const Input &p_input, Output &r_output) = 0;
	virtual void release(const Ref<RenderSceneBuffersRD> &p_render_buffers) = 0;
	virtual void free_device_resources() = 0;
	virtual ~DirectResampler() {}
};

class DirectResamplerHooks {
	static inline DirectResampler *resampler = nullptr;

public:
	static void set_resampler(DirectResampler *p_resampler) { resampler = p_resampler; }
	static DirectResampler *get_resampler() { return resampler; }
};

} // namespace RendererRD
