/**************************************************************************/
/*  gi_module_hooks.h                                                     */
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

#include "ddgi_light_tracker.h"
#include "gi_epochs.h"
#include "gi_provider.h"

#include "core/math/projection.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2i.h"
#include "core/templates/rid.h"
#include "core/variant/dictionary.h"

class RenderSceneBuffersRD;

namespace RendererRD {

class RayScene;

static constexpr uint32_t RTGI_CANDIDATE_LAYOUT_VERSION = 1;

struct RtgiShadeInputs {
	RID ddgi_ubo;
	RID probe_data;
	RID irradiance;
	RID distance;
	RID sky_texture;
	float sky_color_or_orientation[3] = {};
	uint32_t sky_flags = 0;
	float sky_border_size[2] = {};
	float sky_energy = 1.0f;
	float bounce_energy = 0.0f;
	float emission_exposure = 1.0f;
	bool valid = false;
};

struct RtgiFrameInputs {
	RenderSceneBuffersRD *buffers = nullptr;
	Size2i screen_size;
	Size2i trace_size;
	Size2i slot_size;
	int32_t trace_scale = 1;
	float ray_fraction = 1.0f;
	RID depth;
	RID normal_roughness;
	RID geo_position;
	RID geo_normal;
	RID previous_geo_position;
	RID previous_geo_normal;
	RID candidate_first;
	RID candidate_second;
	RID tile_texture;
	uint32_t candidate_layout_version = RTGI_CANDIDATE_LAYOUT_VERSION;
	Projection projection;
	Projection previous_projection;
	Transform3D camera_transform;
	Transform3D previous_camera_transform;
	Projection view_to_previous_clip;
	uint32_t frame_index = 0;
	GIEpochs::Snapshot epochs = {};
	const GIEpochs *dirty_source = nullptr;
	bool history_cut = false;
	bool hardware = false;
	bool deterministic = false;
	bool physical_units = false;
	float exposure = 1.0f;
	int32_t debug_view = -1;
	RayScene *ray_scene = nullptr;
	RtgiShadeInputs shade;
	const DDGILightTracker::Event *events = nullptr;
	uint32_t event_count = 0;
	uint32_t events_lost = 0;
	bool directional_jump = false;
	uint32_t restart_quiet_frames = 0;
};

enum RtgiOutputMode : uint32_t {
	RTGI_OUTPUT_PLAIN,
	RTGI_OUTPUT_MODULE,
};

struct RtgiResampleOutput {
	RID radiance;
	RID coverage;
	RID debug;
	GIProviderRecord record = {};
	bool valid = false;
	bool denoise = false;
	bool history_cut = false;
	uint32_t history_cap = 0;
	RtgiOutputMode mode = RTGI_OUTPUT_PLAIN;
	uint32_t n_target = 0;
};

class RtgiResampleBackend {
public:
	virtual ~RtgiResampleBackend() {}

	virtual bool is_active() const = 0;
	virtual bool wants_all_tiles() const { return true; }
	virtual bool wants_replay() const { return false; }
	virtual bool begin_frame(const RtgiFrameInputs &p_inputs) = 0;
	virtual void resample(const RtgiFrameInputs &p_inputs, RtgiResampleOutput &r_output) = 0;
	virtual void release(RenderSceneBuffersRD *p_buffers) = 0;
	virtual void get_status(Dictionary &r_status) const = 0;
	virtual const char *get_name() const = 0;
};

class GIModuleHooks {
	RtgiResampleBackend *rtgi_backend = nullptr;

	GIModuleHooks() {}

public:
	static GIModuleHooks *get_singleton();

	void register_rtgi_backend(RtgiResampleBackend *p_backend);
	void unregister_all();
	void release_all();

	RtgiResampleBackend *get_rtgi_backend() const { return rtgi_backend; }
};

} // namespace RendererRD
