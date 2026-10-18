/**************************************************************************/
/*  surfel_gi.cpp                                                         */
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

#include "surfel_gi.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs_binary.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

bool SurfelGI::is_enabled() {
	return GLOBAL_GET_CACHED(bool, "rendering/global_illumination/surfels/enabled") && DDGI::is_enabled();
}

SurfelGI::Settings SurfelGI::get_project_settings() {
	Settings settings;
	settings.capacity = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/surfels/capacity"), 4096, 1048576);
	settings.updates_per_frame = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/surfels/updates_per_frame"), 256, 131072);
	settings.radius_scale = CLAMP(GLOBAL_GET_CACHED(float, "rendering/global_illumination/surfels/radius_scale"), 0.002f, 0.2f);
	settings.min_radius = MAX(GLOBAL_GET_CACHED(float, "rendering/global_illumination/surfels/min_radius"), 0.005f);
	settings.max_radius = MAX(GLOBAL_GET_CACHED(float, "rendering/global_illumination/surfels/max_radius"), settings.min_radius);
	settings.max_distance = MAX(GLOBAL_GET_CACHED(float, "rendering/global_illumination/surfels/max_distance"), 1.0f);
	settings.ray_distance = MAX(GLOBAL_GET_CACHED(float, "rendering/global_illumination/surfels/ray_distance"), 0.5f);
	settings.unseen_frames = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/surfels/unseen_frames"), 8, 100000);
	settings.max_samples = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/surfels/max_samples"), 1, 1024);
	settings.ambient_occlusion_strength = CLAMP(GLOBAL_GET_CACHED(float, "rendering/global_illumination/surfels/ambient_occlusion_strength"), 0.0f, 1.0f);
	settings.debug_view = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/surfels/debug_view"), 0, (int)DEBUG_VIEW_SATURATION);
	return settings;
}

static RID _create_buffer(uint32_t p_size, const Vector<uint8_t> &p_data, const String &p_name, BitField<RD::StorageBufferUsage> p_usage = 0) {
	RID buffer = RD::get_singleton()->storage_buffer_create(p_size, p_data, p_usage);
	RD::get_singleton()->set_resource_name(buffer, p_name);
	return buffer;
}

static Vector<uint8_t> _zeros(uint32_t p_size) {
	Vector<uint8_t> data;
	data.resize(p_size);
	data.fill(0);
	return data;
}

void SurfelGI::SurfelVolume::allocate(const Settings &p_settings) {
	settings = p_settings;
	bucket_count = Math::next_power_of_2(settings.capacity * BUCKETS_PER_SURFEL);
	cell_capacity = settings.capacity * CELLS_PER_SURFEL;

	surfels = _create_buffer(settings.capacity * SURFEL_SIZE, _zeros(settings.capacity * SURFEL_SIZE), "Surfels");

	Vector<uint8_t> counter_data = _zeros(COUNTERS_SIZE);
	reinterpret_cast<uint32_t *>(counter_data.ptrw())[0] = settings.capacity;
	counters = _create_buffer(COUNTERS_SIZE, counter_data, "Surfel Counters");

	Vector<uint8_t> free_data = _zeros(settings.capacity * sizeof(uint32_t));
	uint32_t *free_ptr = reinterpret_cast<uint32_t *>(free_data.ptrw());
	for (uint32_t i = 0; i < settings.capacity; i++) {
		free_ptr[i] = settings.capacity - 1 - i;
	}
	free_list = _create_buffer(free_data.size(), free_data, "Surfel Free List");

	alive_list = _create_buffer(settings.capacity * sizeof(uint32_t), Vector<uint8_t>(), "Surfel Alive List");
	update_list = _create_buffer(settings.updates_per_frame * sizeof(uint32_t), Vector<uint8_t>(), "Surfel Update List");
	bucket_counts = _create_buffer(bucket_count * sizeof(uint32_t), _zeros(bucket_count * sizeof(uint32_t)), "Surfel Bucket Counts");
	bucket_offsets = _create_buffer((bucket_count + 1) * sizeof(uint32_t), _zeros((bucket_count + 1) * sizeof(uint32_t)), "Surfel Bucket Offsets");
	bucket_cursor = _create_buffer(bucket_count * sizeof(uint32_t), Vector<uint8_t>(), "Surfel Bucket Cursor");
	bucket_blocks = _create_buffer(bucket_count / SCAN_GROUP * sizeof(uint32_t), Vector<uint8_t>(), "Surfel Bucket Blocks");
	cells = _create_buffer(cell_capacity * sizeof(uint32_t), Vector<uint8_t>(), "Surfel Cells");
	dispatch_buffer = _create_buffer(6 * sizeof(uint32_t), _zeros(6 * sizeof(uint32_t)), "Surfel Dispatch", RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	ubo = RD::get_singleton()->uniform_buffer_create(sizeof(UBO));
	RD::get_singleton()->set_resource_name(ubo, "Surfel UBO");
}

LocalVector<RD::Uniform> SurfelGI::SurfelVolume::gather_uniforms() const {
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, ubo));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, surfels));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, counters));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, free_list));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, bucket_offsets));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, cells));
	return uniforms;
}

void SurfelGI::SurfelVolume::free_data() {
	RID *owned[] = { &surfels, &counters, &free_list, &alive_list, &update_list, &bucket_counts, &bucket_offsets, &bucket_cursor, &bucket_blocks, &cells, &dispatch_buffer, &ubo };
	for (RID *rid : owned) {
		if (rid->is_valid()) {
			RD::get_singleton()->free_rid(*rid);
			*rid = RID();
		}
	}
}

void SurfelGI::init() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_RESET\n");
	modes.push_back("\n#define MODE_MAINTAIN\n");
	modes.push_back("\n#define MODE_SCAN_LOCAL\n");
	modes.push_back("\n#define MODE_SCAN_BLOCKS\n");
	modes.push_back("\n#define MODE_SCAN_APPLY\n");
	modes.push_back("\n#define MODE_INSERT\n");
	modes.push_back("\n#define MODE_VALIDATE\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].create_compute_pipeline(shader.version_get_shader(shader_version, i));
	}
}

void SurfelGI::free() {
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].free();
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
		shader_version = RID();
	}
}

void SurfelGI::update(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Vector3 &p_anchor, const Projection &p_projection, const Transform3D &p_camera_transform, const Size2i &p_screen_size) {
	Ref<SurfelVolume> volume;
	if (p_render_buffers->has_custom_data(RB_SCOPE_SURFELS)) {
		volume = p_render_buffers->get_custom_data(RB_SCOPE_SURFELS);
	}
	if (!is_enabled()) {
		if (volume.is_valid()) {
			p_render_buffers->set_custom_data(RB_SCOPE_SURFELS, Ref<RenderBufferCustomDataRD>());
			p_render_buffers->clear_context(RB_SCOPE_SURFELS);
		}
		return;
	}

	const Settings settings = get_project_settings();
	if (volume.is_null() || volume->ubo.is_null() || !volume->settings.same_storage(settings)) {
		volume.instantiate();
		volume->allocate(settings);
		p_render_buffers->set_custom_data(RB_SCOPE_SURFELS, volume);
	}
	volume->settings = settings;
	volume->frame++;
	if (volume->frame == 0) {
		volume->frame = 1;
	}

	const Vector3 shift = volume->anchor_valid ? volume->anchor - p_anchor : Vector3();
	volume->anchor = p_anchor;
	volume->anchor_valid = true;
	const Vector3 camera_to_anchor = p_camera_transform.origin - p_anchor;

	UBO ubo = {};
	for (int k = 0; k < 3; k++) {
		ubo.camera_to_anchor[k] = camera_to_anchor[k];
		ubo.anchor_shift[k] = shift[k];
	}
	ubo.base_cell = settings.min_radius * CELL_RADII;
	ubo.radius_scale = settings.radius_scale;
	ubo.capacity = settings.capacity;
	ubo.bucket_count = volume->bucket_count;
	ubo.cell_capacity = volume->cell_capacity;
	ubo.frame = volume->frame;
	ubo.max_level = uint32_t(Math::ceil(Math::log2(settings.max_radius / settings.min_radius)));
	ubo.updates_per_frame = settings.updates_per_frame;
	ubo.ray_distance = settings.ray_distance;
	const float focal = float(p_screen_size.y) * 0.5f * float(p_projection.columns[1][1]);
	const float projected = MAX(1.0f, settings.radius_scale * focal);
	ubo.spawn_chance = CLAMP(2.0f / (Math::PI * projected * projected), 4e-6f, 0.2f);
	ubo.max_distance = settings.max_distance;
	ubo.min_radius = settings.min_radius;
	ubo.max_radius = settings.max_radius;
	ubo.debug_view = settings.debug_view;
	ubo.unseen_frames = settings.unseen_frames;
	ubo.max_samples = settings.max_samples;
	ubo.ambient_occlusion_strength = settings.ambient_occlusion_strength;

	Projection correction;
	correction.set_depth_correction(true);
	const Projection clip = correction * p_projection;
	const Projection view(Transform3D(p_camera_transform.basis, Vector3()).affine_inverse());
	MaterialStorage::store_camera(clip * view, ubo.world_to_clip);
	MaterialStorage::store_camera(clip.inverse(), ubo.clip_to_view);
	RD::get_singleton()->buffer_update(volume->ubo, 0, sizeof(UBO), &ubo);
}

void SurfelGI::_dispatch(Mode p_mode, const Ref<SurfelVolume> &p_volume, uint32_t p_x_groups, RID p_depth) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	RID shader_rid = shader.version_get_shader(shader_version, p_mode);

	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_volume->ubo));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_volume->surfels));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_volume->counters));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, p_volume->free_list));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_volume->alive_list));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_volume->update_list));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_volume->bucket_counts));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_volume->bucket_offsets));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, p_volume->bucket_cursor));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, p_volume->cells));
	if (p_mode == MODE_SCAN_BLOCKS) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 10, p_volume->dispatch_buffer));
	}
	if (p_mode == MODE_SCAN_LOCAL || p_mode == MODE_SCAN_BLOCKS || p_mode == MODE_SCAN_APPLY) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, p_volume->bucket_blocks));
	}
	if (p_mode == MODE_VALIDATE) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 11, p_depth));
	}

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[p_mode].get_rid());
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rid, 0, uniforms), 0);
	if (p_mode == MODE_INSERT || p_mode == MODE_VALIDATE) {
		RD::get_singleton()->compute_list_dispatch_indirect(compute_list, p_volume->dispatch_buffer, ALIVE_ARGS_OFFSET);
	} else {
		RD::get_singleton()->compute_list_dispatch(compute_list, p_x_groups, 1, 1);
	}
	RD::get_singleton()->compute_list_end();
}

void SurfelGI::process(const Ref<SurfelVolume> &p_volume, DDGI &p_ddgi, const Ref<DDGI::Volume> &p_ddgi_volume) {
	if (p_ddgi_volume.is_null()) {
		return;
	}
	RD::get_singleton()->draw_command_begin_label("Surfel GI");
	RENDER_TIMESTAMP("Surfel Maintain");
	_dispatch(MODE_RESET, p_volume, 1);
	_dispatch(MODE_MAINTAIN, p_volume, Math::division_round_up(p_volume->settings.capacity, 64u));
	RENDER_TIMESTAMP("Surfel Scan");
	_dispatch(MODE_SCAN_LOCAL, p_volume, p_volume->bucket_count / SCAN_GROUP);
	_dispatch(MODE_SCAN_BLOCKS, p_volume, 1);
	_dispatch(MODE_SCAN_APPLY, p_volume, p_volume->bucket_count / SCAN_GROUP);
	RENDER_TIMESTAMP("Surfel Insert");
	_dispatch(MODE_INSERT, p_volume, 0);

	RENDER_TIMESTAMP("Surfel Trace");
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_volume->ubo));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_volume->surfels));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_volume->update_list));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, p_volume->counters));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, p_volume->bucket_offsets));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, p_volume->cells));
	p_ddgi.trace_surfels(p_ddgi_volume, uniforms, p_volume->dispatch_buffer, TRACE_ARGS_OFFSET);
	RD::get_singleton()->draw_command_end_label();
}

void SurfelGI::validate(const Ref<SurfelVolume> &p_volume, RID p_depth) {
	if (p_volume.is_null() || p_depth.is_null()) {
		return;
	}
	RD::get_singleton()->draw_command_begin_label("Surfel Validate");
	RENDER_TIMESTAMP("Surfel Validate");
	_dispatch(MODE_VALIDATE, p_volume, 0, p_depth);
	RD::get_singleton()->draw_command_end_label();
}
