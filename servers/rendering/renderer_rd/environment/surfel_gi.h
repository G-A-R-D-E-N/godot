/**************************************************************************/
/*  surfel_gi.h                                                           */
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
#include "servers/rendering/renderer_rd/environment/ddgi.h"
#include "servers/rendering/renderer_rd/pipeline_deferred_rd.h"
#include "servers/rendering/renderer_rd/shaders/environment/surfel.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_buffer_custom_data_rd.h"

#define RB_SCOPE_SURFELS SNAME("surfels")
#define RB_TEX_SURFEL_COVERAGE SNAME("surfel_coverage")

class RenderSceneBuffersRD;

namespace RendererRD {

class SurfelGI {
public:
	static constexpr uint32_t SURFEL_SIZE = 112;
	static constexpr uint32_t COUNTERS_SIZE = 16;
	static constexpr uint32_t CELL_RADII = 4;
	static constexpr uint32_t BUCKETS_PER_SURFEL = 2;
	static constexpr uint32_t CELLS_PER_SURFEL = 12;
	static constexpr uint32_t SCAN_GROUP = 1024;
	static constexpr uint32_t ALIVE_ARGS_OFFSET = 0;
	static constexpr uint32_t TRACE_ARGS_OFFSET = 12;

	enum Mode {
		MODE_RESET,
		MODE_MAINTAIN,
		MODE_SCAN_LOCAL,
		MODE_SCAN_BLOCKS,
		MODE_SCAN_APPLY,
		MODE_INSERT,
		MODE_VALIDATE,
		MODE_MAX
	};

	enum DebugView {
		DEBUG_VIEW_DISABLED,
		DEBUG_VIEW_IRRADIANCE,
		DEBUG_VIEW_SURFELS,
		DEBUG_VIEW_COVERAGE,
		DEBUG_VIEW_STATE,
	};

	struct Settings {
		uint32_t capacity = 131072;
		uint32_t updates_per_frame = 16384;
		float radius_scale = 0.02;
		float min_radius = 0.05;
		float max_radius = 2.0;
		float max_distance = 64.0;
		float ray_distance = 16.0;
		uint32_t unseen_frames = 600;
		uint32_t max_samples = 32;
		float ambient_occlusion_strength = 0.0;
		uint32_t debug_view = DEBUG_VIEW_DISABLED;

		bool same_storage(const Settings &p_other) const {
			return capacity == p_other.capacity && updates_per_frame == p_other.updates_per_frame;
		}
	};

	struct UBO {
		float camera_to_anchor[3];
		float base_cell;

		float anchor_shift[3];
		float radius_scale;

		uint32_t capacity;
		uint32_t bucket_count;
		uint32_t cell_capacity;
		uint32_t frame;

		uint32_t max_level;
		uint32_t updates_per_frame;
		float ray_distance;
		float spawn_chance;

		float max_distance;
		float min_radius;
		float max_radius;
		uint32_t debug_view;

		uint32_t unseen_frames;
		uint32_t max_samples;
		float ambient_occlusion_strength;
		float pad0;

		float world_to_clip[16];
		float clip_to_view[16];
	};

	class SurfelVolume : public RenderBufferCustomDataRD {
		GDCLASS(SurfelVolume, RenderBufferCustomDataRD)

	public:
		Settings settings;
		uint32_t bucket_count = 0;
		uint32_t cell_capacity = 0;
		uint32_t frame = 0;
		Vector3 anchor;
		bool anchor_valid = false;

		RID surfels;
		RID counters;
		RID free_list;
		RID alive_list;
		RID update_list;
		RID bucket_counts;
		RID bucket_offsets;
		RID bucket_cursor;
		RID bucket_blocks;
		RID cells;
		RID dispatch_buffer;
		RID ubo;

		void allocate(const Settings &p_settings);
		LocalVector<RD::Uniform> gather_uniforms() const;

		virtual void configure(RenderSceneBuffersRD *p_render_buffers) override {}
		virtual void free_data() override;

		~SurfelVolume() { free_data(); }
	};

private:
	SurfelShaderRD shader;
	RID shader_version;
	PipelineDeferredRD pipelines[MODE_MAX];

	void _dispatch(Mode p_mode, const Ref<SurfelVolume> &p_volume, uint32_t p_x_groups, RID p_depth = RID());

public:
	static bool is_enabled();
	static Settings get_project_settings();

	void init();
	void free();

	void update(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Vector3 &p_anchor, const Projection &p_projection, const Transform3D &p_camera_transform, const Size2i &p_screen_size);
	void process(const Ref<SurfelVolume> &p_volume, DDGI &p_ddgi, const Ref<DDGI::Volume> &p_ddgi_volume);
	void validate(const Ref<SurfelVolume> &p_volume, RID p_depth);
};

} // namespace RendererRD
