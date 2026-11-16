/**************************************************************************/
/*  ddgi.h                                                                */
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

#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/environment/sky.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/ddgi.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/environment/ddgi_trace.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"

#define RB_SCOPE_DDGI SNAME("ddgi")

class RenderSceneBuffersRD;

namespace RendererRD {

class RayScene;

class DDGI {
public:
	static constexpr uint32_t MAX_LEVELS = 4;
	static constexpr uint32_t IRRADIANCE_SIZE = 6;
	static constexpr uint32_t DISTANCE_SIZE = 14;
	static constexpr uint32_t MAX_RAYS = 256;
	static constexpr uint32_t MAX_ATLAS_WIDTH = 16384;
	static constexpr uint32_t LIST_HEADER = 48;
	static constexpr uint32_t TRACE_ARGS_OFFSET = 0;
	static constexpr uint32_t BLEND_ARGS_OFFSET = 12;
	static constexpr uint32_t STATE_ARGS_OFFSET = 24;

	enum Mode {
		MODE_BLEND_IRRADIANCE,
		MODE_BLEND_DISTANCE,
		MODE_PROBE_STATE,
		MODE_SCHEDULE_COUNT,
		MODE_SCHEDULE_PLAN,
		MODE_SCHEDULE_SELECT,
		MODE_SCHEDULE_SCAN,
		MODE_SCHEDULE_APPEND,
		MODE_SCHEDULE_FINALIZE,
		MODE_RESET_COUNTS,
		MODE_MAX
	};

	enum TraceMode {
		TRACE_MODE_PROBES,
		TRACE_MODE_DEBUG,
		TRACE_MODE_SURFELS,
		TRACE_MODE_RTGI,
		TRACE_MODE_MAX
	};

	enum TraceGroup {
		TRACE_GROUP_BASE,
		TRACE_GROUP_RTGI,
	};

	enum SkyFlags {
		SKY_FLAGS_MODE_COLOR = 1,
		SKY_FLAGS_MODE_SKY = 2,
		SKY_FLAGS_ORIENTATION_SIGN = 4,
	};

	enum DebugView {
		DEBUG_VIEW_DISABLED,
		DEBUG_VIEW_IRRADIANCE,
		DEBUG_VIEW_RAY_HITS,
		DEBUG_VIEW_NORMALS,
		DEBUG_VIEW_PROBE_STATE,
		DEBUG_VIEW_PROBE_VISIBILITY,
	};

	struct Settings {
		Vector3i dims = Vector3i(32, 16, 32);
		uint32_t level_count = 4;
		float spacing = 1.0;
		uint32_t rays_per_probe = 128;
		uint32_t probes_per_frame = 4096;
		float hysteresis = 0.97;
		float normal_bias = 0.1;
		float view_bias = 0.25;
		float change_threshold = 0.25;
		bool light_change_reset = true;
		float energy = 1.0;
		float bounce_energy = 1.0;
		uint32_t debug_view = DEBUG_VIEW_DISABLED;

		bool same_storage(const Settings &p_other) const {
			return dims == p_other.dims && level_count == p_other.level_count && spacing == p_other.spacing && rays_per_probe == p_other.rays_per_probe && probes_per_frame == p_other.probes_per_frame;
		}
	};

	struct LevelData {
		float origin[3];
		float spacing;
		int32_t g_min[3];
		uint32_t pad0;
		int32_t slot_offset[3];
		uint32_t pad1;
	};

	struct UBO {
		LevelData levels[MAX_LEVELS];

		int32_t dims[3];
		uint32_t level_count;

		float irradiance_texel_size[2];
		float distance_texel_size[2];

		float normal_bias;
		float view_bias;
		float energy;
		float hysteresis;

		float ray_rotation[16];

		uint32_t rays_per_probe;
		uint32_t frame;
		float max_distance;
		uint32_t probe_capacity;

		float change_threshold;
		float pad[3];
	};

	static constexpr uint32_t RESET_BOXES = 32;

	struct ResetUBO {
		float boxes[RESET_BOXES][4];
		uint32_t count;
		uint32_t pad[3];
	};

	struct TracePushConstant {
		float sky_color_or_orientation[3];
		uint32_t sky_flags;

		float sky_border_size[2];
		float sky_energy;
		float bounce_energy;

		float emission_exposure;
		uint32_t debug_mode;
		float screen_size[2];

		float debug_inv_view_projection[16];
	};

	class Volume : public RenderBufferCustomDataRD {
		GDCLASS(Volume, RenderBufferCustomDataRD)

	public:
		Settings settings;
		Vector3i g_min[MAX_LEVELS];
		bool level_valid[MAX_LEVELS] = {};
		uint32_t frame = 0;

		RID irradiance;
		RID distance;
		RID probe_data;
		RID rays;
		RID probe_list_buffer;
		RID schedule_groups_buffer;
		RID dispatch_buffer;
		RID ubo;
		RID reset_ubo;
		RID debug_texture;
		Size2i debug_size;
		RID sky_texture;
		TracePushConstant trace_constant = {};

		void allocate(const Settings &p_settings);
		void schedule(const Vector3 &p_camera_position);
		void fill_ubo(UBO &r_ubo, const Vector3 &p_camera_position) const;
		LocalVector<RD::Uniform> gather_uniforms() const;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override;

		~Volume() { free_data(); }
	};

private:
	DdgiShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipelines[MODE_MAX];

	DdgiTraceShaderRD trace_shader;
	RID trace_shader_version;
	PipelineDeferredRD trace_pipelines[TRACE_MODE_MAX];
	DdgiTraceShaderRD trace_shader_hw;
	RID trace_shader_hw_version;
	PipelineDeferredRD trace_pipelines_hw[TRACE_MODE_MAX];
	String trace_defines;
	bool trace_hw_valid = false;
	bool use_octmap_array = false;

	static Vector<ShaderRD::VariantDefine> _trace_modes();
	bool _ensure_trace_hw();
	bool _trace_usable(RayScene *p_ray_scene);

	void _dispatch(Mode p_mode, const Ref<Volume> &p_volume, uint32_t p_x_groups, int32_t p_indirect_offset = -1);
	void _reset_counts(const Ref<Volume> &p_volume, const Vector3 &p_camera_position);
	void _trace(TraceMode p_mode, const Ref<Volume> &p_volume, RID p_output, const TracePushConstant &p_push_constant, uint32_t p_x_groups, uint32_t p_y_groups, RID p_indirect_buffer = RID(), uint32_t p_indirect_offset = 0, const LocalVector<RD::Uniform> *p_surfel_uniforms = nullptr);
	void _fill_sky(TracePushConstant &r_push_constant, RID &r_sky_texture, RID p_environment, SkyRD::Sky *p_sky, RID p_camera_attributes) const;

public:
	static bool is_enabled();
	static Settings get_project_settings();

	void init(SkyRD *p_sky);
	void free();

	void update(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Vector3 &p_camera_position);
	void trace_surfels(const Ref<Volume> &p_volume, const LocalVector<RD::Uniform> &p_surfel_uniforms, RID p_indirect_buffer, uint32_t p_indirect_offset);
	bool trace_rtgi(const Ref<Volume> &p_volume, RID p_output, const LocalVector<RD::Uniform> &p_uniforms, uint32_t p_x_groups, uint32_t p_y_groups);
	void process(const Ref<Volume> &p_volume, RID p_environment, SkyRD::Sky *p_sky, RID p_camera_attributes, const Projection &p_projection, const Transform3D &p_camera_transform, const Size2i &p_screen_size);
};

} // namespace RendererRD
