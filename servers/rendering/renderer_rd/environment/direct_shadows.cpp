/**************************************************************************/
/*  direct_shadows.cpp                                                    */
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

#include "direct_shadows.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "servers/rendering/renderer_rd/environment/ray_scene.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

DirectShadows::Settings DirectShadows::get_project_settings() {
	Settings settings;
	settings.entries = CLAMP(int(GLOBAL_GET("rendering/global_illumination/direct/entries")), 2, 8);
	settings.rays = CLAMP(int(GLOBAL_GET("rendering/global_illumination/direct/rays_per_pixel")), 1, 2);
	settings.reference = GLOBAL_GET("rendering/global_illumination/direct/reference");
	settings.debug_view = MAX(int(GLOBAL_GET("rendering/global_illumination/direct/debug_view")), 0);
	settings.ratio = int(GLOBAL_GET("rendering/global_illumination/direct/estimator")) != 0;
	settings.filter = GLOBAL_GET("rendering/global_illumination/direct/filter");
	settings.force_one = GLOBAL_GET("rendering/global_illumination/direct/force_factor_one");
	settings.history_cap = MAX(int(GLOBAL_GET("rendering/global_illumination/direct/history_cap")), 0);
	settings.clamp_sigmas = GLOBAL_GET("rendering/global_illumination/direct/clamp_sigmas");
	settings.kernel_scale = GLOBAL_GET("rendering/global_illumination/direct/filter_kernel_scale");
	settings.range_sigmas = GLOBAL_GET("rendering/global_illumination/direct/filter_range_sigmas");
	return settings;
}

Vector<String> DirectShadows::_trace_modes() {
	Vector<String> modes;
	modes.push_back("\n");
	modes.push_back("\n#define PRECISE_HISTORY\n");
	return modes;
}

DirectShadows::DirectShadows() {
	defines = vformat("\n#define RAY_SCENE_BLAS_STACK_SIZE %d\n#define RAY_SCENE_TLAS_STACK_SIZE %d\n", RayScene::BLAS_MAX_DEPTH, RayScene::TLAS_MAX_DEPTH);
	shader.initialize(_trace_modes(), defines);
	shader_version = shader.version_create();
	for (int i = 0; i < TRACE_MAX; i++) {
		pipeline[i].create_compute_pipeline(shader.version_get_shader(shader_version, i));
	}
	Vector<String> filter_modes;
	filter_modes.push_back("\n#define MODE_CLAMP\n");
	filter_modes.push_back("\n#define MODE_FIRST\n");
	filter_modes.push_back("\n#define MODE_SECOND\n");
	filter_shader.initialize(filter_modes);
	filter_shader_version = filter_shader.version_create();
	for (int i = 0; i < FILTER_MAX; i++) {
		filter_pipeline[i].create_compute_pipeline(filter_shader.version_get_shader(filter_shader_version, i));
	}
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
	counter_buffer = RD::get_singleton()->storage_buffer_create(COUNTER_BYTES);
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	format.width = 1;
	format.height = 1;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT;
	debug_unused = RD::get_singleton()->texture_create(format, RD::TextureView());
	RD::get_singleton()->set_resource_name(debug_unused, "Direct Shadows Unused Debug");
}

DirectShadows::~DirectShadows() {
	RD *rd = RD::get_singleton();
	for (int i = 0; i < TRACE_MAX; i++) {
		pipeline[i].free();
		pipeline_hw[i].free();
	}
	for (int i = 0; i < FILTER_MAX; i++) {
		filter_pipeline[i].free();
	}
	if (filter_shader_version.is_valid()) {
		filter_shader.version_free(filter_shader_version);
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
	}
	if (shader_hw_version.is_valid()) {
		shader_hw.version_free(shader_hw_version);
	}
	if (params_buffer.is_valid()) {
		rd->free_rid(params_buffer);
	}
	if (counter_buffer.is_valid()) {
		rd->free_rid(counter_buffer);
	}
	if (lamp_buffer.is_valid()) {
		rd->free_rid(lamp_buffer);
	}
	if (debug_unused.is_valid()) {
		rd->free_rid(debug_unused);
	}
}

bool DirectShadows::_ensure_hw() {
	if (shader_hw_version.is_valid()) {
		return hw_valid;
	}
	shader_hw.initialize(_trace_modes(), defines + "\n#define SHADER_GLSL_VERSION_460\n#extension GL_EXT_ray_query : require\n#define RAY_SCENE_HW\n");
	shader_hw_version = shader_hw.version_create();
	hw_valid = shader_hw.version_is_valid(shader_hw_version);
	for (int i = 0; hw_valid && i < TRACE_MAX; i++) {
		pipeline_hw[i].create_compute_pipeline(shader_hw.version_get_shader(shader_hw_version, i));
		hw_valid = pipeline_hw[i].get_rid().is_valid();
	}
	return hw_valid;
}

Ref<DirectShadows::ViewportData> DirectShadows::_viewport(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	if (p_render_buffers->has_custom_data(RB_SCOPE_DIRECT_SHADOWS)) {
		return p_render_buffers->get_custom_data(RB_SCOPE_DIRECT_SHADOWS);
	}
	Ref<ViewportData> viewport;
	viewport.instantiate();
	p_render_buffers->set_custom_data(RB_SCOPE_DIRECT_SHADOWS, viewport);
	return viewport;
}

void DirectShadows::_ensure_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const Settings &p_settings) {
	const Size2i size = p_render_buffers->get_internal_size();
	if (p_viewport->entries == p_settings.entries && p_viewport->size == size && p_viewport->precise == p_settings.precise() && p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS)) {
		return;
	}
	p_render_buffers->clear_context(RB_SCOPE_DIRECT_SHADOWS);
	p_viewport->entries = p_settings.entries;
	p_viewport->size = size;
	p_viewport->precise = p_settings.precise();
	p_viewport->flip = false;
	p_viewport->last_frame = 0;

	RD *rd = RD::get_singleton();
	const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	const uint32_t layers = factor_layers(p_settings.entries);
	const uint32_t accum_layers = p_viewport->precise ? p_settings.entries : p_settings.entries - 1;
	const RD::DataFormat accum_format = p_viewport->precise ? RD::DATA_FORMAT_R32G32_SFLOAT : RD::DATA_FORMAT_R16G16_SFLOAT;
	const RD::DataFormat rest_format = p_viewport->precise ? RD::DATA_FORMAT_R32G32B32A32_SFLOAT : RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	const StringName slots[2] = { RB_DIRECT_SLOTS, RB_DIRECT_SLOTS_OTHER };
	const StringName accums[2] = { RB_DIRECT_ACCUM, RB_DIRECT_ACCUM_OTHER };
	const StringName rests[2] = { RB_DIRECT_REST, RB_DIRECT_REST_OTHER };
	const StringName guides[2] = { RB_DIRECT_GUIDE, RB_DIRECT_GUIDE_OTHER };
	for (int i = 0; i < 2; i++) {
		rd->texture_clear(p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, slots[i], RD::DATA_FORMAT_R16_UNORM, usage, RD::TEXTURE_SAMPLES_1, size, p_settings.entries, 1, true), Color(0, 0, 0, 0), 0, 1, 0, p_settings.entries);
		rd->texture_clear(p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, accums[i], accum_format, usage, RD::TEXTURE_SAMPLES_1, size, accum_layers, 1, true), Color(0, 0, 0, 0), 0, 1, 0, accum_layers);
		rd->texture_clear(p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, rests[i], rest_format, usage, RD::TEXTURE_SAMPLES_1, size, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->texture_clear(p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, guides[i], RD::DATA_FORMAT_R32G32_UINT, usage, RD::TEXTURE_SAMPLES_1, size, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	const RID factors = p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS, RD::DATA_FORMAT_R16_UNORM, usage, RD::TEXTURE_SAMPLES_1, size, layers, 1, true);
	rd->texture_clear(factors, Color(1, 1, 1, 1), 0, 1, 0, layers);
	p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SCRATCH, RD::DATA_FORMAT_R16_UNORM, usage, RD::TEXTURE_SAMPLES_1, size, layers, 1, true);
	p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_RAY, RD::DATA_FORMAT_R32_UINT, usage, RD::TEXTURE_SAMPLES_1, size, 1, 1, true);
}

RID DirectShadows::_debug_target(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Settings &p_settings) {
	if (p_settings.debug_view == 0) {
		return debug_unused;
	}
	if (p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_DEBUG)) {
		return p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_DEBUG);
	}
	const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	return p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_DEBUG, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, p_render_buffers->get_internal_size(), 1, 1, true);
}

void DirectShadows::release(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	if (!p_render_buffers->has_custom_data(RB_SCOPE_DIRECT_SHADOWS)) {
		return;
	}
	Ref<ViewportData> viewport = p_render_buffers->get_custom_data(RB_SCOPE_DIRECT_SHADOWS);
	if (viewport->entries == 0) {
		return;
	}
	p_render_buffers->clear_context(RB_SCOPE_DIRECT_SHADOWS);
	viewport->entries = 0;
	viewport->size = Size2i();
	viewport->last_frame = 0;
}

bool DirectShadows::process(const FrameInput &p_input, DirectLightTable &p_table) {
	RayScene *ray_scene = RayScene::get_singleton();
	if (!ray_scene || !ray_scene->is_ready()) {
		return false;
	}
	const bool hardware = ray_scene->uses_hardware();
	if (hardware && !_ensure_hw()) {
		ray_scene->refuse_hardware("the direct shadow shader or its pipeline could not be made");
		return false;
	}

	RD *rd = RD::get_singleton();
	const Settings settings = get_project_settings();
	Ref<ViewportData> viewport = _viewport(p_input.render_buffers);
	_ensure_textures(p_input.render_buffers, viewport, settings);

	const Size2i frame_size = p_input.render_buffers->get_internal_size();
	if (p_input.frame != size_frame) {
		size_frame = p_input.frame;
		largest_size_before = largest_size;
		largest_size = frame_size;
	} else if (frame_size.x * frame_size.y > largest_size.x * largest_size.y) {
		largest_size = frame_size;
	}

	if (viewport->table_generation != p_input.table_generation) {
		viewport->table_generation = p_input.table_generation;
		viewport->last_frame = 0;
	}
	viewport->flip = !viewport->flip;

	if (p_input.frame != moved_frame) {
		moved_frame = p_input.frame;
		ray_scene->collect_moved_instances(moved_signature, moved_anchor, moved_known, moved);
		p_table.mark_moved(moved);
	}
	p_table.write_slots(lamp_data);
	if (!String(GLOBAL_GET("rendering/global_illumination/direct/dump_request")).is_empty()) {
		p_table.write_lamps(lamp_dump);
		dump_hard_resets = p_table.get_hard_resets();
		dump_dynamic_lamp_frames = p_table.get_dynamic_lamp_frames();
	}
	const uint32_t lamp_bytes = lamp_data.size() * sizeof(DirectLightTable::GpuSlot);
	if (lamp_bytes > lamp_buffer_capacity) {
		if (lamp_buffer.is_valid()) {
			rd->free_rid(lamp_buffer);
		}
		lamp_buffer_capacity = MAX(lamp_bytes * 2, uint32_t(4096));
		lamp_buffer = rd->storage_buffer_create(lamp_buffer_capacity);
		rd->set_resource_name(lamp_buffer, "Direct Shadows Lamps");
	}
	if (lamp_bytes > 0) {
		rd->buffer_update(lamp_buffer, 0, lamp_bytes, lamp_data.ptr());
	}

	const Size2i size = p_input.render_buffers->get_internal_size();
	const Projection inverse = p_input.projection.inverse();
	const Projection prev_view_from_view = Projection(viewport->last_camera.affine_inverse() * p_input.camera);

	Params params = {};
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			params.inv_projection[c * 4 + r] = float(inverse.columns[c][r]);
			params.prev_view_from_view[c * 4 + r] = float(prev_view_from_view.columns[c][r]);
			params.prev_projection[c * 4 + r] = float(viewport->last_projection.columns[c][r]);
		}
	}
	for (int r = 0; r < 3; r++) {
		params.camera_basis[r * 4 + 0] = float(p_input.camera.basis.rows[r][0]);
		params.camera_basis[r * 4 + 1] = float(p_input.camera.basis.rows[r][1]);
		params.camera_basis[r * 4 + 2] = float(p_input.camera.basis.rows[r][2]);
		params.region_half[r] = float(p_input.region_half[r]);
	}
	params.bias = float(GLOBAL_GET("rendering/global_illumination/direct/ray_bias"));
	params.size[0] = size.x;
	params.size[1] = size.y;
	params.pixel_size[0] = 1.0f / size.x;
	params.pixel_size[1] = 1.0f / size.y;
	const uint32_t cluster_screen_width = Math::division_round_up(uint32_t(size.x), p_input.cluster_size);
	const uint32_t cluster_screen_height = Math::division_round_up(uint32_t(size.y), p_input.cluster_size);
	params.cluster_shift = Math::get_shift_from_power_of_2(p_input.cluster_size);
	params.max_cluster_element_count_div_32 = p_input.cluster_max_elements / 32;
	params.cluster_type_size = cluster_screen_width * cluster_screen_height * (params.max_cluster_element_count_div_32 + 32);
	params.cluster_width = cluster_screen_width;
	params.z_far = p_input.z_far;
	params.frame = uint32_t(p_input.frame);
	params.last_frame = uint32_t(viewport->last_frame);
	params.entries = settings.entries;
	params.rays = settings.rays;
	params.reference = settings.reference ? 1 : 0;
	params.debug_view = settings.debug_view;
	params.history_valid = viewport->last_frame != 0 ? 1 : 0;
	params.estimator = settings.ratio ? 1 : 0;
	params.filter_on = settings.filtered() ? 1 : 0;
	params.force_one = settings.force_one ? 1 : 0;
	params.history_cap = float(settings.history_cap);
	params.clamp_sigmas = settings.clamp_sigmas;
	params.clamp_min_rays = 4.0f;
	params.kernel_scale = settings.kernel_scale;
	params.kernel_floor = settings.kernel_floor;
	params.range_scale = settings.range_sigmas > 0.0f ? 0.5f / (settings.range_sigmas * settings.range_sigmas) : 0.0f;
	params.world_per_pixel = 2.0f * Math::abs(float(inverse.columns[1][1])) / float(size.y);
	rd->buffer_update(params_buffer, 0, sizeof(Params), &params);
	rd->buffer_clear(counter_buffer, 0, COUNTER_BYTES);

	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const int trace_mode = viewport->precise ? TRACE_PRECISE : TRACE_CAPPED;
	RID shader_rid = hardware ? shader_hw.version_get_shader(shader_hw_version, trace_mode) : shader.version_get_shader(shader_version, trace_mode);
	const Ref<RenderSceneBuffersRD> &rb = p_input.render_buffers;
	const bool flip = viewport->flip;

	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_input.cluster_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_input.omni_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_input.spot_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, lamp_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, params_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>{ sampler, p_input.depth }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>{ sampler, p_input.normal_roughness }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_SLOTS_OTHER : RB_DIRECT_SLOTS)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_ACCUM_OTHER : RB_DIRECT_ACCUM)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_REST_OTHER : RB_DIRECT_REST)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, _debug_target(rb, settings)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, counter_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_SLOTS : RB_DIRECT_SLOTS_OTHER)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 14, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_ACCUM : RB_DIRECT_ACCUM_OTHER)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 15, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_REST : RB_DIRECT_REST_OTHER)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_GUIDE : RB_DIRECT_GUIDE_OTHER)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_GUIDE_OTHER : RB_DIRECT_GUIDE)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_RAY)));

	LocalVector<RD::Uniform> scene_uniforms;
	scene_uniforms.reserve(8);
	ray_scene->append_uniforms(scene_uniforms);

	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, (hardware ? pipeline_hw[trace_mode] : pipeline[trace_mode]).get_rid());
	rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 0, uniforms), 0);
	rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 1, scene_uniforms), 1);
	rd->compute_list_dispatch(list, Math::division_round_up(uint32_t(size.x), 8u), Math::division_round_up(uint32_t(size.y), 8u), 1);
	rd->compute_list_end();

	if (settings.filtered()) {
		_filter(rb, viewport, settings);
	}

	viewport->last_frame = p_input.frame;
	viewport->last_camera = p_input.camera;
	viewport->last_projection = p_input.projection;
	return true;
}

void DirectShadows::_filter(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const Settings &p_settings) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	const bool flip = p_viewport->flip;
	const RID slots = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_SLOTS_OTHER : RB_DIRECT_SLOTS);
	const RID accum = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_ACCUM_OTHER : RB_DIRECT_ACCUM);
	const RID rest = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_REST_OTHER : RB_DIRECT_REST);
	const RID guide = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_GUIDE_OTHER : RB_DIRECT_GUIDE);
	const RID ray = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_RAY);
	const RID scratch = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SCRATCH);
	const RID factors = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS);
	const uint32_t groups_x = Math::division_round_up(uint32_t(p_viewport->size.x), 8u);
	const uint32_t groups_y = Math::division_round_up(uint32_t(p_viewport->size.y), 8u);

	const auto run = [&](FilterMode p_mode, LocalVector<RD::Uniform> &p_uniforms) {
		p_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, lamp_buffer));
		p_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, params_buffer));
		p_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, slots));
		p_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, guide));
		const RID shader_rid = filter_shader.version_get_shader(filter_shader_version, p_mode);
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, filter_pipeline[p_mode].get_rid());
		rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 0, p_uniforms), 0);
		rd->compute_list_dispatch(list, groups_x, groups_y, 1);
		rd->compute_list_end();
	};

	if (p_settings.clamp_sigmas > 0.0f) {
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, accum));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, ray));
		run(FILTER_CLAMP, uniforms);
	}
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, scratch));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, accum));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, rest));
		run(FILTER_FIRST, uniforms);
	}
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, factors));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, accum));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, _debug_target(p_render_buffers, p_settings)));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 19, scratch));
		run(FILTER_SECOND, uniforms);
	}
}

RID DirectShadows::get_slots(const Ref<RenderSceneBuffersRD> &p_render_buffers) const {
	Ref<ViewportData> viewport = p_render_buffers->get_custom_data(RB_SCOPE_DIRECT_SHADOWS);
	return p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, viewport->flip ? RB_DIRECT_SLOTS_OTHER : RB_DIRECT_SLOTS);
}

RID DirectShadows::get_factors(const Ref<RenderSceneBuffersRD> &p_render_buffers) const {
	return p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS);
}

RID DirectShadows::get_debug_texture(const Ref<RenderSceneBuffersRD> &p_render_buffers) const {
	if (!p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_DEBUG)) {
		return RID();
	}
	return p_render_buffers->get_texture_slice(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_DEBUG, 0, 0);
}

void DirectShadows::serve_dump_request(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	if (!p_render_buffers->has_custom_data(RB_SCOPE_DIRECT_SHADOWS)) {
		return;
	}
	const String request = GLOBAL_GET("rendering/global_illumination/direct/dump_request");
	if (request.is_empty()) {
		return;
	}
	Ref<ViewportData> viewport = p_render_buffers->get_custom_data(RB_SCOPE_DIRECT_SHADOWS);
	if (served_request == request || viewport->size != largest_size_before || !p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS) || viewport->last_frame == 0) {
		return;
	}
	served_request = request;

	RD *rd = RD::get_singleton();
	const String dir = request.get_base_dir();
	if (!dir.is_empty()) {
		DirAccess::make_dir_recursive_absolute(dir);
	}
	struct Layered {
		StringName name;
		const char *file;
		uint32_t layers;
	};
	const Layered textures[] = {
		{ viewport->flip ? RB_DIRECT_SLOTS_OTHER : RB_DIRECT_SLOTS, "slots", viewport->entries },
		{ RB_DIRECT_FACTORS, "factors", factor_layers(viewport->entries) },
		{ viewport->flip ? RB_DIRECT_ACCUM_OTHER : RB_DIRECT_ACCUM, "accum", viewport->precise ? viewport->entries : viewport->entries - 1 },
		{ viewport->flip ? RB_DIRECT_REST_OTHER : RB_DIRECT_REST, "rest", 1 },
	};
	Dictionary files;
	bool ok = true;
	for (const Layered &texture : textures) {
		const String path = request + "." + texture.file + ".bin";
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		ok = ok && file.is_valid();
		if (file.is_valid()) {
			const RID rid = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, texture.name);
			for (uint32_t layer = 0; layer < texture.layers; layer++) {
				const Vector<uint8_t> data = rd->texture_get_data(rid, layer);
				ok = ok && file->store_buffer(data.ptr(), data.size());
			}
		}
		files[texture.file] = path.get_file();
	}
	const String lamps_path = request + ".lamps.bin";
	Ref<FileAccess> lamps_file = FileAccess::open(lamps_path, FileAccess::WRITE);
	ok = ok && lamps_file.is_valid();
	if (lamps_file.is_valid() && !lamp_dump.is_empty()) {
		ok = lamps_file->store_buffer(reinterpret_cast<const uint8_t *>(lamp_dump.ptr()), lamp_dump.size() * sizeof(float)) && ok;
	}
	lamps_file.unref();
	files["lamps"] = lamps_path.get_file();
	const Vector<uint8_t> counters = rd->buffer_get_data(counter_buffer);
	ok = ok && counters.size() >= int(COUNTER_BYTES);

	Dictionary report;
	report["ok"] = ok;
	report["width"] = viewport->size.x;
	report["height"] = viewport->size.y;
	report["entries"] = viewport->entries;
	report["frame"] = viewport->last_frame;
	report["precise"] = viewport->precise;
	report["files"] = files;
	report["lamp_slots"] = lamp_dump.size() / DirectLightTable::LAMP_VALUES;
	report["hard_resets"] = dump_hard_resets;
	report["dynamic_lamp_frames"] = dump_dynamic_lamp_frames;
	RayScene *ray_scene = RayScene::get_singleton();
	report["hardware"] = ray_scene && ray_scene->uses_hardware();
	if (ok) {
		const uint32_t *values = reinterpret_cast<const uint32_t *>(counters.ptr());
		report["lit_pixels"] = values[0];
		report["rest_pixels"] = values[1];
		report["rays"] = values[2];
		report["past_region_pixels"] = values[3];
		report["most_lamps"] = values[4];
	}
	Ref<FileAccess> json = FileAccess::open(request, FileAccess::WRITE);
	if (json.is_valid()) {
		json->store_string(JSON::stringify(report, "\t"));
		json.unref();
		Ref<FileAccess> done = FileAccess::open(request + ".done", FileAccess::WRITE);
		if (done.is_valid()) {
			done->store_string(ok ? "ok\n" : "failed\n");
		}
	}
}
