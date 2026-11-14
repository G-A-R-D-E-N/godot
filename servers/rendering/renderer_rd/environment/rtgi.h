/**************************************************************************/
/*  rtgi.h                                                                */
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
#include "core/templates/paged_array.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/environment/ddgi.h"
#include "servers/rendering/renderer_rd/environment/gi_epochs.h"
#include "servers/rendering/renderer_rd/environment/gi_module_hooks.h"
#include "servers/rendering/renderer_rd/environment/rtgi_restarts.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/rtgi.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"

#define RB_SCOPE_RTGI SNAME("rtgi")

class RenderSceneBuffersRD;

namespace RendererRD {

class RtgiPass {
public:
	static constexpr uint32_t TILE_SIZE = 8;

	enum Mode {
		MODE_CLASSIFY,
		MODE_TEMPORAL,
		MODE_VARIANCE,
		MODE_ATROUS,
		MODE_COMPOSE,
		MODE_DEBUG,
		MODE_MAX
	};

	enum DebugView {
		DEBUG_VIEW_NONE,
		DEBUG_VIEW_TILE_CLASS,
		DEBUG_VIEW_RAY_VALIDITY,
		DEBUG_VIEW_CONTRIBUTION,
		DEBUG_VIEW_HISTORY_COUNT,
	};

	enum FrameFlags {
		FRAME_FLAG_ORTHOGONAL = 1,
		FRAME_FLAG_HISTORY_CUT = 2,
		FRAME_FLAG_CAMERA_CUT = 4,
		FRAME_FLAG_RESIZE = 8,
		FRAME_FLAG_BACKEND_SWITCH = 16,
		FRAME_FLAG_SSGI_VALID_TEXTURE = 32,
		FRAME_FLAG_SOFT_CUT = 64,
		FRAME_FLAG_RESTART_SPHERES = 128,
		FRAME_FLAG_MODULE_OUTPUT = 256,
		FRAME_FLAG_REPLAY = 512,
	};

	enum DdgiWrongBits {
		DDGI_WRONG_EVENTS = 1,
		DDGI_WRONG_MEMORY = 2,
		DDGI_WRONG_ACTIVE = 4,
		DDGI_WRONG_READ = 8,
	};

	static constexpr uint32_t TILE_MEMORY_FRAMES = 255;

	enum CutReason {
		CUT_NONE,
		CUT_FIRST,
		CUT_RESIZE,
		CUT_CAMERA,
		CUT_BACKEND,
		CUT_REQUEST,
		CUT_MODULE,
		CUT_MAX
	};

	enum Counter {
		COUNTER_TILES_TOTAL,
		COUNTER_TILES_TRACED,
		COUNTER_TRACE_PIXELS,
		COUNTER_RAYS,
		COUNTER_HITS,
		COUNTER_SKY_MISSES,
		COUNTER_BACKFACE,
		COUNTER_HISTORY_REJECTS,
		COUNTER_RESTARTED,
		COUNTER_TILES_WRONG_EVENTS,
		COUNTER_TILES_WRONG_MEMORY,
		COUNTER_MAX
	};

	struct Settings {
		bool enabled = false;
		float rays_per_pixel = 0.25f;
		float resolution_scale = 0.5f;
		uint32_t target_samples = 6;
		uint32_t history_cap = 6;
		uint32_t atrous_iterations = 3;
		float normal_sigma = 32.0f;
		float depth_sigma = 1.0f;
		float variance_sigma = 4.0f;
		float ddgi_alpha_min = 0.6f;
		float ssgi_min_frames = 4.0f;
		uint32_t check_period = 8;
		bool force_trace = false;
		uint32_t seed = 1;
		float origin_scale = 2.0f;
		float origin_floor = 0.0f;
		uint32_t soft_cut_count = 0;
		float luma_dead_zone = 16.0f;
		bool mirror_below_surface = true;
		bool restart_spheres = true;
		bool restart_screen_wide = false;
		uint32_t restart_quiet_frames = 0;
		float directional_jump_degrees = 2.0f;
		float directional_jump_floor = 0.0f;
		bool ddgi_wrong_events = false;
		bool ddgi_wrong_memory = false;
		uint32_t ddgi_wrong_frames = 16;
		float ddgi_wrong_tau = 0.5f;
		float ddgi_wrong_target = 2.0f;
		float ddgi_wrong_noise = 0.25f;
		float ddgi_wrong_floor = 0.05f;
		float ddgi_wrong_rho = 1.0f;
	};

	struct FrameUBO {
		float inv_projection[16];
		float view_to_prev_clip[16];
		float cur_to_prev_view[16];
		float cam_transform[16];

		int32_t screen_size[2];
		int32_t trace_size[2];
		int32_t slot_size[2];
		int32_t tile_count[2];

		uint32_t frame;
		uint32_t seed;
		int32_t scale;
		uint32_t flags;

		float target_samples;
		float history_cap;
		float normal_sigma;
		float depth_sigma;

		float variance_sigma;
		float ddgi_alpha_min;
		float ssgi_min_frames;
		uint32_t check_period;

		uint32_t force_trace;
		uint32_t debug_mode;
		float energy;
		int32_t atrous_step;

		int32_t slot_scale;
		uint32_t soft_cut_count;
		float luma_dead_zone;
		uint32_t restart_count;

		float restart_spheres[RtgiRestarts::MAX_SPHERES][4];

		uint32_t ddgi_wrong_frames;
		uint32_t ddgi_wrong_bits;
		uint32_t n_target;
		float ddgi_wrong_rho;

		float ddgi_wrong_tau;
		float ddgi_wrong_target;
		float ddgi_wrong_noise;
		float ddgi_wrong_floor;
	};

	static_assert(offsetof(FrameUBO, restart_spheres) == 368);
	static_assert(offsetof(FrameUBO, ddgi_wrong_frames) == 624);
	static_assert(offsetof(FrameUBO, ddgi_wrong_tau) == 640);
	static_assert(sizeof(FrameUBO) == 656);

	struct TraceUBO {
		float inv_projection[16];
		float cam_transform[16];
		int32_t screen_size[2];
		int32_t trace_size[2];
		uint32_t frame;
		uint32_t seed;
		int32_t scale;
		uint32_t flags;
		float origin_scale;
		float ray_fraction;
		float origin_floor;
		uint32_t mirror_below_surface;
	};

	struct FrameInputs {
		RID depth;
		RID normal_roughness;
		RID ssgi_length;
		Projection projection;
		Projection drawn_projection;
		Transform3D camera_transform;
		Size2i screen_size;
		Size2i trace_size;
		Size2i slot_size;
		uint32_t frame_index = 0;
		GIEpochs::Snapshot epochs = {};
		bool history_cut = false;
		bool hardware = false;
		bool physical_units = false;
		const PagedArray<RID> *lights = nullptr;
	};

	class RtgiBuffers : public RenderBufferCustomDataRD {
		GDCLASS(RtgiBuffers, RenderBufferCustomDataRD)

	public:
		Size2i screen_size;
		Size2i trace_size;
		Size2i slot_size;
		Size2i tile_count;
		uint32_t index = 0;
		uint64_t frames_processed = 0;
		uint32_t frames_since_cut = 0;
		uint32_t cut_counts[CUT_MAX] = {};
		uint32_t last_cut = CUT_NONE;
		uint32_t soft_cuts = 0;
		bool have_previous = false;
		bool have_epochs = false;
		bool previous_hardware = false;
		Transform3D previous_transform;
		Projection previous_projection;
		GIEpochs::Snapshot previous_epochs = {};
		String refused;
		float last_ray_fraction = 0.0f;
		uint32_t last_trace_pixel_capacity = 0;
		uint32_t last_atrous_iterations = 0;
		uint32_t last_counters[COUNTER_MAX] = {};
		Dictionary last_inputs;
		uint64_t event_serial = 0;
		LocalVector<RtgiRestarts::DirectionalRecord> directionals;
		uint32_t last_spheres = 0;
		uint64_t sphere_frames = 0;
		uint64_t spheres_total = 0;
		uint64_t spheres_dropped = 0;
		uint64_t spheres_behind = 0;
		uint64_t events_lost = 0;
		LocalVector<DDGILightTracker::Event> backend_events;
		uint32_t backend_events_lost = 0;
		uint64_t event_repeats = 0;
		uint64_t region_crossings = 0;
		uint64_t directional_jumps = 0;
		uint64_t directional_repeats = 0;
		bool previous_denoise = false;
		bool backend_active = false;
		uint64_t backend_frames = 0;
		uint64_t backend_refusals = 0;
		bool backend_ran = false;
		bool tile_memory_written = false;
		uint32_t last_ddgi_wrong_bits = 0;
		uint64_t r5_frames = 0;
		uint64_t r5_overflows = 0;
		uint64_t module_output_frames = 0;
		uint64_t route_frames = 0;
		uint32_t last_n_target = 0;
		RenderSceneBuffersRD *owner = nullptr;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override { owner = p_render_buffers; }
		virtual void free_data() override;
	};

private:
	static RtgiPass *singleton;

	RtgiShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipelines[MODE_MAX];
	RID trace_ubo;
	RID frame_ubo;
	RID atrous_ubo[8];
	RID counter_buffer;
	uint64_t light_word_serial = 0;

	void _read_light_changes(const Ref<RtgiBuffers> &p_buffers, const Settings &p_settings, const FrameInputs &p_inputs, RtgiRestarts::Selection &r_selection, RtgiRestarts::DirectionalRead &r_directional);
	void _ensure_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Ref<RtgiBuffers> &p_buffers, const FrameInputs &p_inputs, bool p_debug);
	void _fill_frame_ubo(FrameUBO &r_ubo, const Settings &p_settings, const Ref<RtgiBuffers> &p_buffers, const FrameInputs &p_inputs, const Ref<DDGI::Volume> &p_volume, uint32_t p_flags, uint32_t p_debug_mode, int32_t p_atrous_step, int32_t p_slot_scale, const Transform3D &p_delta, const Projection &p_previous_projection) const;
	void _dispatch(Mode p_mode, RID p_ubo, const LocalVector<RD::Uniform> &p_uniforms, const LocalVector<RD::Uniform> *p_ddgi_uniforms, uint32_t p_groups_x, uint32_t p_groups_y, bool p_counters);
	void _refuse(const Ref<RtgiBuffers> &p_buffers, const String &p_reason);
	bool _process(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Ref<RtgiBuffers> &p_buffers, DDGI &p_ddgi, const FrameInputs &p_inputs);
	void _read_counters(const Ref<RtgiBuffers> &p_buffers);
	void _dump_debug(const Ref<RenderSceneBuffersRD> &p_render_buffers, const String &p_path, const Size2i &p_size, DebugView p_view) const;
	void _write_status(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Ref<RtgiBuffers> &p_buffers, const String &p_path) const;

	RtgiPass();

public:
	~RtgiPass();

	static Settings get_project_settings();
	static bool is_enabled();

	static bool process(const Ref<RenderSceneBuffersRD> &p_render_buffers, DDGI &p_ddgi, const FrameInputs &p_inputs);
	static void release(const Ref<RenderSceneBuffersRD> &p_render_buffers);
	static void finalize();
};

} // namespace RendererRD
