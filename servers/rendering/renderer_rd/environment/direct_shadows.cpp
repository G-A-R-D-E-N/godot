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
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

DirectShadows::Settings DirectShadows::get_project_settings() {
	Settings settings;
	settings.entries = CLAMP(int(GLOBAL_GET("rendering/global_illumination/direct/entries")), 2, 8);
	settings.rays = CLAMP(int(GLOBAL_GET("rendering/global_illumination/direct/rays_per_pixel")), 1, 2);
	settings.reference = GLOBAL_GET("rendering/global_illumination/direct/reference");
	settings.debug_view = MAX(int(GLOBAL_GET("rendering/global_illumination/direct/debug_view")), 0);
	settings.filter = GLOBAL_GET("rendering/global_illumination/direct/filter");
	settings.force_one = GLOBAL_GET("rendering/global_illumination/direct/force_factor_one");
	settings.cold_frames = CLAMP(int(GLOBAL_GET("rendering/global_illumination/direct/debug_cold_frames")), 0, 64);
	settings.history_cap = MAX(int(GLOBAL_GET("rendering/global_illumination/direct/history_cap")), 0);
	settings.dynamic_history_cap = MAX(int(GLOBAL_GET("rendering/global_illumination/direct/dynamic_history_cap")), 1);
	settings.clamp_sigmas = GLOBAL_GET("rendering/global_illumination/direct/clamp_sigmas");
	settings.kernel_scale = GLOBAL_GET("rendering/global_illumination/direct/filter_kernel_scale");
	settings.range_sigmas = GLOBAL_GET("rendering/global_illumination/direct/filter_range_sigmas");
	settings.quarter = int(GLOBAL_GET("rendering/global_illumination/direct/trace_resolution")) == 1;
	settings.second_pass = GLOBAL_GET("rendering/global_illumination/direct/filter_second_pass");
	settings.emitter_shorten = GLOBAL_GET("rendering/global_illumination/direct/emitter_ray_shorten");
	settings.emitter_skip_instance = int(GLOBAL_GET("rendering/global_illumination/direct/emitter_skip_instance"));
	static bool direct_settings_printed = false;
	if (!direct_settings_printed) {
		direct_settings_printed = true;
		print_line(vformat("DIRECT_SHADOWS settings: force_factor_one %s block_resolve %s entries %d quarter %s bias %.4f emitter_skip %d", settings.force_one ? "true" : "false", GLOBAL_GET("rendering/global_illumination/direct/block_resolve") ? "true" : "false", int(settings.entries), settings.quarter ? "true" : "false", settings.emitter_shorten + float(GLOBAL_GET("rendering/global_illumination/direct/ray_bias")), int(settings.emitter_skip_instance)));
	}
	return settings;
}

static Vector<String> quarter_trace_modes() {
	Vector<String> modes;
	modes.push_back("\n#define TRACE_QUARTER\n#define QUARTER\n");
	return modes;
}

static RID unused_storage_texture(RD::DataFormat p_format, const String &p_name) {
	RD::TextureFormat format;
	format.format = p_format;
	format.width = 1;
	format.height = 1;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT;
	const RID texture = RD::get_singleton()->texture_create(format, RD::TextureView());
	RD::get_singleton()->set_resource_name(texture, p_name);
	return texture;
}

Vector<String> DirectShadows::_trace_modes() {
	Vector<String> modes;
	modes.push_back("\n");
	modes.push_back("\n#define PRECISE_HISTORY\n");
	modes.push_back("\n#define EXTERNAL_ENTRIES\n");
	modes.push_back("\n#define PRECISE_HISTORY\n#define EXTERNAL_ENTRIES\n");
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
	filter_modes.push_back("\n#define MODE_SECOND\n#define EXTERNAL_ENTRIES\n");
	filter_shader.initialize(filter_modes);
	filter_shader_version = filter_shader.version_create();
	for (int i = 0; i < FILTER_MAX; i++) {
		filter_pipeline[i].create_compute_pipeline(filter_shader.version_get_shader(filter_shader_version, i));
	}
	quarter_shader.initialize(quarter_trace_modes(), defines);
	quarter_shader_version = quarter_shader.version_create();
	quarter_valid = quarter_shader.version_is_valid(quarter_shader_version);
	Vector<String> quarter_filter_modes;
	quarter_filter_modes.push_back("\n#define MODE_CLAMP\n#define QUARTER\n");
	quarter_filter_modes.push_back("\n#define MODE_FIRST\n#define QUARTER\n");
	quarter_filter_modes.push_back("\n#define MODE_SECOND\n#define QUARTER\n");
	quarter_filter_modes.push_back("\n#define MODE_FIRST\n#define FIRST_FINAL\n#define QUARTER\n");
	quarter_filter_shader.initialize(quarter_filter_modes);
	quarter_filter_shader_version = quarter_filter_shader.version_create();
	quarter_valid = quarter_valid && quarter_filter_shader.version_is_valid(quarter_filter_shader_version);
	Vector<String> resolve_modes;
	resolve_modes.push_back("\n");
	resolve_shader.initialize(resolve_modes);
	resolve_shader_version = resolve_shader.version_create();
	quarter_valid = quarter_valid && resolve_shader.version_is_valid(resolve_shader_version);
	if (quarter_valid) {
		quarter_pipeline.create_compute_pipeline(quarter_shader.version_get_shader(quarter_shader_version, 0));
		for (int i = 0; i < QUARTER_MAX; i++) {
			quarter_filter_pipeline[i].create_compute_pipeline(quarter_filter_shader.version_get_shader(quarter_filter_shader_version, i));
			quarter_valid = quarter_valid && quarter_filter_pipeline[i].get_rid().is_valid();
		}
		resolve_pipeline.create_compute_pipeline(resolve_shader.version_get_shader(resolve_shader_version, 0));
		quarter_valid = quarter_valid && quarter_pipeline.get_rid().is_valid() && resolve_pipeline.get_rid().is_valid();
	}
	if (!quarter_valid) {
		WARN_PRINT("Direct shadows: the quarter-resolution shaders could not be made, so trace_resolution Quarter runs at Full.");
	}
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
	counter_buffer = RD::get_singleton()->storage_buffer_create(COUNTER_BYTES);
	emitter_hit_hist = RD::get_singleton()->storage_buffer_create((EMITTER_HIT_INSTANCES + 1) * 4 * uint32_t(sizeof(uint32_t)));
	{
		const int hist_bytes = int((EMITTER_HIT_INSTANCES + 1) * 4 * uint32_t(sizeof(uint32_t)));
		Vector<uint8_t> hist_init;
		hist_init.resize(hist_bytes);
		memset(hist_init.ptrw(), 0xFF, hist_bytes);
		RD::get_singleton()->buffer_update(emitter_hit_hist, 0, hist_init.size(), hist_init.ptr());
	}
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	format.width = 1;
	format.height = 1;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT;
	debug_unused = RD::get_singleton()->texture_create(format, RD::TextureView());
	RD::get_singleton()->set_resource_name(debug_unused, "Direct Shadows Unused Debug");
	RD::TextureFormat record_format;
	record_format.format = RD::DATA_FORMAT_R32G32_UINT;
	record_format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	record_format.width = 1;
	record_format.height = 1;
	record_format.array_layers = 1;
	record_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	Vector<uint8_t> zero;
	zero.resize_initialized(8);
	resampled_dummy = RD::get_singleton()->texture_create(record_format, RD::TextureView(), Vector<Vector<uint8_t>>{ zero });
	RD::get_singleton()->set_resource_name(resampled_dummy, "Direct Resampled Dummy");
	lamp_buffer_capacity = 4096;
	lamp_buffer = RD::get_singleton()->storage_buffer_create(lamp_buffer_capacity);
	RD::get_singleton()->set_resource_name(lamp_buffer, "Direct Shadows Lamps");
	lamp_dummy = RD::get_singleton()->storage_buffer_create(sizeof(DirectLightTable::GpuSlot));
	RD::get_singleton()->set_resource_name(lamp_dummy, "Direct Shadows Lamps Dummy");
	emitter_dummy = RD::get_singleton()->storage_buffer_create(8 * sizeof(float));
	RD::get_singleton()->set_resource_name(emitter_dummy, "Direct Shadows Emitter Samples Dummy");
	tier_unused = unused_storage_texture(RD::DATA_FORMAT_R8_UINT, "Direct Shadows Unused Tier");
	depth_unused = unused_storage_texture(RD::DATA_FORMAT_R32_SFLOAT, "Direct Shadows Unused Depth");
	normal_unused = unused_storage_texture(RD::DATA_FORMAT_R32_UINT, "Direct Shadows Unused Normal");
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
	quarter_pipeline.free();
	quarter_pipeline_hw.free();
	for (int i = 0; i < QUARTER_MAX; i++) {
		quarter_filter_pipeline[i].free();
	}
	resolve_pipeline.free();
	if (quarter_shader_version.is_valid()) {
		quarter_shader.version_free(quarter_shader_version);
	}
	if (quarter_shader_hw_version.is_valid()) {
		quarter_shader_hw.version_free(quarter_shader_hw_version);
	}
	if (quarter_filter_shader_version.is_valid()) {
		quarter_filter_shader.version_free(quarter_filter_shader_version);
	}
	if (resolve_shader_version.is_valid()) {
		resolve_shader.version_free(resolve_shader_version);
	}
	for (const RID &unused : { tier_unused, depth_unused, normal_unused }) {
		if (unused.is_valid()) {
			rd->free_rid(unused);
		}
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
	if (emitter_hit_hist.is_valid()) {
		rd->free_rid(emitter_hit_hist);
	}
	}
	if (lamp_buffer.is_valid()) {
		rd->free_rid(lamp_buffer);
	}
	if (exclude_buffer.is_valid()) {
		rd->free_rid(exclude_buffer);
	}
	if (debug_unused.is_valid()) {
		rd->free_rid(debug_unused);
	}
	if (resampled_dummy.is_valid()) {
		rd->free_rid(resampled_dummy);
	}
	if (lamp_dummy.is_valid()) {
		rd->free_rid(lamp_dummy);
	}
	if (emitter_dummy.is_valid()) {
		rd->free_rid(emitter_dummy);
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
	if (hw_valid) {
		_ensure_quarter_hw();
	}
	return hw_valid;
}

bool DirectShadows::_ensure_quarter_hw() {
	if (quarter_hw_tried) {
		return quarter_hw_valid;
	}
	quarter_hw_tried = true;
	if (!quarter_valid) {
		return false;
	}
	quarter_shader_hw.initialize(quarter_trace_modes(), defines + "\n#define SHADER_GLSL_VERSION_460\n#extension GL_EXT_ray_query : require\n#define RAY_SCENE_HW\n");
	quarter_shader_hw_version = quarter_shader_hw.version_create();
	quarter_hw_valid = quarter_shader_hw.version_is_valid(quarter_shader_hw_version);
	if (quarter_hw_valid) {
		quarter_pipeline_hw.create_compute_pipeline(quarter_shader_hw.version_get_shader(quarter_shader_hw_version, 0));
		quarter_hw_valid = quarter_pipeline_hw.get_rid().is_valid();
	}
	if (!quarter_hw_valid) {
		WARN_PRINT("Direct shadows: the quarter-resolution hardware ray shader could not be made, so trace_resolution Quarter runs at Full on hardware rays.");
	}
	return quarter_hw_valid;
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

void DirectShadows::_ensure_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const Settings &p_settings, bool p_quarter) {
	const Size2i size = p_render_buffers->get_internal_size();
	if (p_viewport->entries == p_settings.entries && p_viewport->size == size && p_viewport->precise == p_settings.precise() && p_viewport->quarter == p_quarter && p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS) && (!p_quarter || (p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS_BLOCK) && p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS_RESOLVED)))) {
		return;
	}
	p_render_buffers->clear_context(RB_SCOPE_DIRECT_SHADOWS);
	p_viewport->entries = p_settings.entries;
	p_viewport->size = size;
	p_viewport->precise = p_settings.precise();
	p_viewport->quarter = p_quarter;
	p_viewport->grid = p_quarter ? block_grid(size) : size;
	p_viewport->flip = false;
	p_viewport->last_frame = 0;
	const Size2i grid = p_viewport->grid;

	RD *rd = RD::get_singleton();
	const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	const uint32_t layers = factor_layers(p_settings.entries);
	const uint32_t accum_layers = DirectShadows::accum_layers(p_settings.entries, p_viewport->precise);
	const RD::DataFormat accum_format = p_viewport->precise ? RD::DATA_FORMAT_R32G32_SFLOAT : RD::DATA_FORMAT_R16G16_SFLOAT;
	const RD::DataFormat rest_format = p_viewport->precise ? RD::DATA_FORMAT_R32G32B32A32_SFLOAT : RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	const StringName slots[2] = { RB_DIRECT_SLOTS, RB_DIRECT_SLOTS_OTHER };
	const StringName accums[2] = { RB_DIRECT_ACCUM, RB_DIRECT_ACCUM_OTHER };
	const StringName rests[2] = { RB_DIRECT_REST, RB_DIRECT_REST_OTHER };
	const StringName guides[2] = { RB_DIRECT_GUIDE, RB_DIRECT_GUIDE_OTHER };
	for (int i = 0; i < 2; i++) {
		rd->texture_clear(p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, slots[i], RD::DATA_FORMAT_R16_UNORM, usage, RD::TEXTURE_SAMPLES_1, grid, p_settings.entries, 1, true), Color(0, 0, 0, 0), 0, 1, 0, p_settings.entries);
		rd->texture_clear(p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, accums[i], accum_format, usage, RD::TEXTURE_SAMPLES_1, grid, accum_layers, 1, true), Color(0, 0, 0, 0), 0, 1, 0, accum_layers);
		rd->texture_clear(p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, rests[i], rest_format, usage, RD::TEXTURE_SAMPLES_1, grid, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
		rd->texture_clear(p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, guides[i], RD::DATA_FORMAT_R32G32_UINT, usage, RD::TEXTURE_SAMPLES_1, grid, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
	}
	const RID factors = p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS, RD::DATA_FORMAT_R16_UNORM, usage, RD::TEXTURE_SAMPLES_1, size, layers, 1, true);
	rd->texture_clear(factors, Color(1, 1, 1, 1), 0, 1, 0, layers);
	p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SCRATCH, RD::DATA_FORMAT_R16_UNORM, usage, RD::TEXTURE_SAMPLES_1, grid, layers, 1, true);
	p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_RAY, RD::DATA_FORMAT_R32_UINT, usage, RD::TEXTURE_SAMPLES_1, grid, 1, 1, true);
	if (p_quarter) {
		rd->texture_clear(p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS_BLOCK, RD::DATA_FORMAT_R16_UNORM, usage, RD::TEXTURE_SAMPLES_1, grid, layers, 1, true), Color(1, 1, 1, 1), 0, 1, 0, layers);
		rd->texture_clear(p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS_RESOLVED, RD::DATA_FORMAT_R16_UNORM, usage, RD::TEXTURE_SAMPLES_1, size, p_settings.entries, 1, true), Color(0, 0, 0, 0), 0, 1, 0, p_settings.entries);
	}
}

RID DirectShadows::_dump_target(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const StringName &p_name, RD::DataFormat p_format, RID p_unused) {
	if (!p_viewport->counted) {
		return p_unused;
	}
	if (p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, p_name)) {
		return p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, p_name);
	}
	const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	return p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, p_name, p_format, usage, RD::TEXTURE_SAMPLES_1, p_viewport->size, 1, 1, true);
}

RID DirectShadows::_debug_target(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Settings &p_settings) {
	if (p_settings.debug_view == 0) {
		return debug_unused;
	}
	if (p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_DEBUG)) {
		return p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_DEBUG);
	}
	const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	Ref<ViewportData> viewport = p_render_buffers->get_custom_data(RB_SCOPE_DIRECT_SHADOWS);
	return p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_DEBUG, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, viewport->grid, 1, 1, true);
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
	viewport->quarter = false;
	viewport->grid = Size2i();
	viewport->last_frame = 0;
}

bool DirectShadows::prepare_lamps(const FrameInput &p_input, DirectLightTable &p_table) {
	RayScene *ray_scene = RayScene::get_singleton();
	if (!ray_scene || !ray_scene->is_ready()) {
		return false;
	}
	RD *rd = RD::get_singleton();
	lamps_dump_request = GLOBAL_GET("rendering/global_illumination/direct/dump_request");
	if (p_input.frame != moved_frame) {
		moved_frame = p_input.frame;
		ray_scene->collect_moved_instances(moved_signature, moved_anchor, moved_known, moved);
		p_table.mark_moved(moved);
	}
	struct Shells : public DirectLightTable::ShellResolver {
		const RayScene *scene = nullptr;
		virtual bool shell_material_range(RID p_instance, uint32_t &r_offset, uint32_t &r_count) const override {
			return scene->shell_material_range(p_instance, r_offset, r_count);
		}
	};
	Shells shells;
	shells.scene = ray_scene;
	p_table.write_slots(lamp_data, exclude_data, &shells);
	if (!lamps_dump_request.is_empty() && lamps_dump_request != served_request) {
		p_table.write_lamps(lamp_dump);
		LightStorage::get_singleton()->write_direct_dump_lamps(dump_lamp_data, lamp_dump.size() / DirectLightTable::LAMP_VALUES);
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
	const uint32_t exclude_bytes = exclude_data.size() * sizeof(DirectLightTable::GpuExclude);
	if (exclude_buffer.is_null() || exclude_bytes > exclude_buffer_capacity) {
		if (exclude_buffer.is_valid()) {
			rd->free_rid(exclude_buffer);
		}
		exclude_buffer_capacity = MAX(exclude_bytes * 2, uint32_t(16));
		exclude_buffer = rd->storage_buffer_create(exclude_buffer_capacity);
		rd->set_resource_name(exclude_buffer, "Direct Shadows Excludes");
	}
	if (exclude_bytes > 0) {
		rd->buffer_update(exclude_buffer, 0, exclude_bytes, exclude_data.ptr());
	}
	lamps_prepared = true;
	return true;
}

bool DirectShadows::process(const FrameInput &p_input, DirectLightTable &p_table) {
	const bool prepared = lamps_prepared;
	lamps_prepared = false;
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
	Settings settings = get_project_settings();
	const bool external = p_input.entries.is_valid();
	const bool emitters = external && p_input.emitter_samples.is_valid();
	if (external) {
		const uint32_t entries = p_input.head_entries + p_input.tail_samples + 1 + (emitters ? 1 : 0);
		ERR_FAIL_COND_V_MSG(p_input.tail_samples == 0 || entries > 8, false, "External direct entries need at least one tail sample and at most 7 head and tail entries.");
		settings.entries = entries;
	}
	Ref<ViewportData> viewport = _viewport(p_input.render_buffers);
	const bool quarter = !external && settings.wants_quarter() && quarter_valid && (!hardware || quarter_hw_valid);
	_ensure_textures(p_input.render_buffers, viewport, settings, quarter);

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

	if (!prepared) {
		prepare_lamps(p_input, p_table);
		lamps_prepared = false;
	}
	viewport->counted = !lamps_dump_request.is_empty() && lamps_dump_request != served_request;

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
	params.bias = float(GLOBAL_GET("rendering/global_illumination/direct/ray_bias")) + settings.emitter_shorten;
	params.size[0] = viewport->grid.x;
	params.size[1] = viewport->grid.y;
	params.full_size[0] = size.x;
	params.full_size[1] = size.y;
	const Vector2 jitter_delta = (viewport->last_jitter - p_input.jitter) * Vector2(size) * 0.5f;
	params.jitter_delta[0] = float(jitter_delta.x);
	params.jitter_delta[1] = float(jitter_delta.y);
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
	params.head_entries = external ? p_input.head_entries : settings.entries - 1;
	params.tail_visibility = external ? p_input.tail_visibility : 0;
	params.pad[0] = emitters ? 1u : 0u;
	params.pad[1] = uint32_t(settings.emitter_skip_instance + 1);
	params.rays = settings.rays;
	params.reference = settings.reference ? 1 : 0;
	params.debug_view = settings.debug_view;
	params.history_valid = viewport->last_frame != 0 ? 1 : 0;
	params.filter_on = settings.filtered() ? 1 : 0;
	params.force_one = settings.force_one ? 1 : 0;
	params.cold_frames = external || settings.reference ? 0 : settings.cold_frames;
	params.history_cap = float(settings.history_cap);
	params.dynamic_cap = float(settings.dynamic_history_cap);
	params.clamp_sigmas = settings.clamp_sigmas;
	params.clamp_min_rays = 4.0f;
	params.kernel_scale = settings.kernel_scale;
	params.kernel_floor = quarter ? 0.5f * settings.kernel_floor : settings.kernel_floor;
	params.range_scale = settings.range_sigmas > 0.0f ? 0.5f / (settings.range_sigmas * settings.range_sigmas) : 0.0f;
	params.world_per_pixel = (quarter ? 4.0f : 2.0f) * Math::abs(float(inverse.columns[1][1])) / float(size.y);
	params.count = viewport->counted ? 1 : 0;
	rd->buffer_update(params_buffer, 0, sizeof(Params), &params);
	if (viewport->counted) {
		rd->buffer_clear(counter_buffer, 0, COUNTER_BYTES);
	}
	{
		static bool factor_layers_printed = false;
		if (!factor_layers_printed) {
			factor_layers_printed = true;
			const uint32_t own_limit = external ? p_input.head_entries : settings.entries - 1;
			const uint32_t rest_layer = settings.entries - 1;
			const uint32_t emitter_write = settings.entries + 2;
			const uint32_t forward_read = settings.entries + 2;
			print_line(vformat("DIRECT_SHADOW_FACTOR layers: entries %d head %d rest %d tail %d emitter_write %d forward_read %d pad0 %d", int(settings.entries), int(own_limit), int(rest_layer), int(rest_layer > own_limit ? rest_layer - own_limit : 0), int(emitter_write), int(forward_read), emitters ? 1 : 0));
			ERR_FAIL_COND_V_MSG(emitter_write != forward_read, false, "The emitter factor write layer must equal the layer the forward shader reads.");
		}
		if (p_input.frame >= 31) {
			static bool emitter_counters_printed = false;
			if (!emitter_counters_printed) {
				emitter_counters_printed = true;
				const Vector<uint8_t> counters = rd->buffer_get_data(counter_buffer);
				if (counters.size() >= int(COUNTER_BYTES)) {
					const uint32_t *values = reinterpret_cast<const uint32_t *>(counters.ptr());
					print_line(vformat("DIRECT_SHADOWS counters: frames %d emitter_pixels %d emitter_rays %d emitter_blocked %d", int(p_input.frame), int(values[13]), int(values[14]), int(values[15])));
					print_line(vformat("DIRECT_EMITTERS blocked: backface %d front %d unblocked_front %d hist005 %d hist030 %d hist200 %d histfar %d emitter_end 005 %d 030 %d 200 %d far %d", int(values[16]), int(values[17]), int(values[22]), int(values[18]), int(values[19]), int(values[20]), int(values[21]), int(values[23]), int(values[24]), int(values[25]), int(values[26])));
			if (emitter_hit_hist.is_valid()) {
				const Vector<uint8_t> hist = rd->buffer_get_data(emitter_hit_hist);
				const int hist_bytes = int((EMITTER_HIT_INSTANCES + 1) * 4 * uint32_t(sizeof(uint32_t)));
				if (hist.size() >= hist_bytes) {
					const uint32_t *hits = reinterpret_cast<const uint32_t *>(hist.ptr());
					print_line(vformat("DIRECT_EMITTERS blocker: instance 1225 hits %d", int(hits[1225 * 4])));
					int printed[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
					for (int pass = 0; pass < 8; pass++) {
						int best = -1;
						uint32_t best_count = 0;
						for (int i = 0; i <= int(EMITTER_HIT_INSTANCES); i++) {
							bool seen = false;
							for (int p = 0; p < pass; p++) {
								if (printed[p] == i) {
									seen = true;
								}
							}
							if (!seen && hits[i * 4] > best_count) {
								best = i;
								best_count = hits[i * 4];
							}
						}
						if (best < 0 || best_count == 0u) {
							break;
						}
						printed[pass] = best;
						const float mean_distance = float(hits[best * 4 + 1]) / 4.0f / float(best_count);
						const float min_distance = float(hits[best * 4 + 2]) / 4.0f;
						const float max_distance = float(hits[best * 4 + 3]) / 4.0f;
						if (best == int(EMITTER_HIT_INSTANCES)) {
							print_line(vformat("DIRECT_EMITTERS blocker: unknown hits %d distance %.3f..%.3f mean %.3f", int(best_count), min_distance, max_distance, mean_distance));
						} else {
							print_line(vformat("DIRECT_EMITTERS blocker: instance %d hits %d distance %.3f..%.3f mean %.3f", best, int(best_count), min_distance, max_distance, mean_distance));
						}
					}
				}
			}
				}
			}
		}
	}

	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	const int trace_mode = external ? (viewport->precise ? TRACE_PRECISE_EXTERNAL : TRACE_CAPPED_EXTERNAL) : (viewport->precise ? TRACE_PRECISE : TRACE_CAPPED);
	RID shader_rid;
	RID pipeline_rid;
	if (quarter) {
		shader_rid = hardware ? quarter_shader_hw.version_get_shader(quarter_shader_hw_version, 0) : quarter_shader.version_get_shader(quarter_shader_version, 0);
		pipeline_rid = (hardware ? quarter_pipeline_hw : quarter_pipeline).get_rid();
	} else {
		shader_rid = hardware ? shader_hw.version_get_shader(shader_hw_version, trace_mode) : shader.version_get_shader(shader_version, trace_mode);
		pipeline_rid = (hardware ? pipeline_hw[trace_mode] : pipeline[trace_mode]).get_rid();
	}
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
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, quarter ? RB_DIRECT_FACTORS_BLOCK : RB_DIRECT_FACTORS)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_ACCUM_OTHER : RB_DIRECT_ACCUM)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_REST_OTHER : RB_DIRECT_REST)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, _debug_target(rb, settings)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, counter_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 27, emitter_hit_hist));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_SLOTS : RB_DIRECT_SLOTS_OTHER)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 14, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_ACCUM : RB_DIRECT_ACCUM_OTHER)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 15, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_REST : RB_DIRECT_REST_OTHER)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_GUIDE : RB_DIRECT_GUIDE_OTHER)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_GUIDE_OTHER : RB_DIRECT_GUIDE)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, rb->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_RAY)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 19, exclude_buffer));
	if (external) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 20, p_input.entries));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, emitters ? p_input.emitter_samples : emitter_dummy));
	}

	LocalVector<RD::Uniform> scene_uniforms;
	scene_uniforms.reserve(8);
	ray_scene->append_uniforms(scene_uniforms);

	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline_rid);
	rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 0, uniforms), 0);
	rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 1, scene_uniforms), 1);
	rd->compute_list_dispatch(list, Math::division_round_up(uint32_t(viewport->grid.x), 8u), Math::division_round_up(uint32_t(viewport->grid.y), 8u), 1);
	rd->compute_list_end();

	if (settings.filtered()) {
		_filter(rb, viewport, settings, external);
	}
	if (quarter) {
		_resolve(rb, viewport, p_input);
	}

	viewport->last_frame = p_input.frame;
	viewport->last_camera = p_input.camera;
	viewport->last_projection = p_input.projection;
	viewport->last_jitter = p_input.jitter;
	return true;
}

void DirectShadows::_resolve(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const FrameInput &p_input) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	const bool flip = p_viewport->flip;
	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, params_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>{ sampler, p_input.depth }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>{ sampler, p_input.normal_roughness }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_SLOTS_OTHER : RB_DIRECT_SLOTS)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, counter_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS_RESOLVED)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_GUIDE_OTHER : RB_DIRECT_GUIDE)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 19, p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS_BLOCK)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 20, _dump_target(p_render_buffers, p_viewport, RB_DIRECT_TIER, RD::DATA_FORMAT_R8_UINT, tier_unused)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 21, _dump_target(p_render_buffers, p_viewport, RB_DIRECT_DEPTH_FULL, RD::DATA_FORMAT_R32_SFLOAT, depth_unused)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 22, _dump_target(p_render_buffers, p_viewport, RB_DIRECT_NORMAL_FULL, RD::DATA_FORMAT_R32_UINT, normal_unused)));
	const RID shader_rid = resolve_shader.version_get_shader(resolve_shader_version, 0);
	RENDER_TIMESTAMP("Direct Shadows Resolve");
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, resolve_pipeline.get_rid());
	rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 0, uniforms), 0);
	rd->compute_list_dispatch(list, Math::division_round_up(uint32_t(p_viewport->grid.x), 8u), Math::division_round_up(uint32_t(p_viewport->grid.y), 8u), 1);
	rd->compute_list_end();
}

void DirectShadows::_filter(const Ref<RenderSceneBuffersRD> &p_render_buffers, Ref<ViewportData> p_viewport, const Settings &p_settings, bool p_external) {
	RD *rd = RD::get_singleton();
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	const bool flip = p_viewport->flip;
	const RID slots = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_SLOTS_OTHER : RB_DIRECT_SLOTS);
	const RID accum = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_ACCUM_OTHER : RB_DIRECT_ACCUM);
	const RID rest = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_REST_OTHER : RB_DIRECT_REST);
	const RID guide = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, flip ? RB_DIRECT_GUIDE_OTHER : RB_DIRECT_GUIDE);
	const RID ray = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_RAY);
	const RID scratch = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SCRATCH);
	const bool quarter = p_viewport->quarter;
	const RID factors = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, quarter ? RB_DIRECT_FACTORS_BLOCK : RB_DIRECT_FACTORS);
	const uint32_t groups_x = Math::division_round_up(uint32_t(p_viewport->grid.x), 8u);
	const uint32_t groups_y = Math::division_round_up(uint32_t(p_viewport->grid.y), 8u);

	const auto run = [&](int p_mode, LocalVector<RD::Uniform> &p_uniforms) {
		p_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, lamp_buffer));
		p_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, params_buffer));
		p_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, slots));
		p_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, guide));
		const RID shader_rid = quarter ? quarter_filter_shader.version_get_shader(quarter_filter_shader_version, p_mode) : filter_shader.version_get_shader(filter_shader_version, p_mode);
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, quarter ? quarter_filter_pipeline[p_mode].get_rid() : filter_pipeline[p_mode].get_rid());
		rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 0, p_uniforms), 0);
		rd->compute_list_dispatch(list, groups_x, groups_y, 1);
		rd->compute_list_end();
	};

	bool any_dynamic = false;
	for (const DirectLightTable::GpuSlot &lamp : lamp_data) {
		if (lamp.dynamic != 0) {
			any_dynamic = true;
			break;
		}
	}
	if (p_settings.clamp_sigmas > 0.0f && any_dynamic) {
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, accum));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, ray));
		RENDER_TIMESTAMP("Direct Shadows Clamp");
		run(FILTER_CLAMP, uniforms);
	}
	if (quarter && !p_settings.second_pass) {
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, factors));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, accum));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, rest));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, _debug_target(p_render_buffers, p_settings)));
		RENDER_TIMESTAMP("Direct Shadows Filter First");
		run(QUARTER_FIRST_FINAL, uniforms);
		return;
	}
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, scratch));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, accum));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, rest));
		RENDER_TIMESTAMP("Direct Shadows Filter First");
		run(FILTER_FIRST, uniforms);
	}
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, factors));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, accum));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, _debug_target(p_render_buffers, p_settings)));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 19, scratch));
		RENDER_TIMESTAMP("Direct Shadows Filter Second");
		run(p_external ? FILTER_SECOND_EXTERNAL : FILTER_SECOND, uniforms);
	}
}

RID DirectShadows::get_slots(const Ref<RenderSceneBuffersRD> &p_render_buffers) const {
	Ref<ViewportData> viewport = p_render_buffers->get_custom_data(RB_SCOPE_DIRECT_SHADOWS);
	if (viewport->quarter) {
		return p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS_RESOLVED);
	}
	return p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, viewport->flip ? RB_DIRECT_SLOTS_OTHER : RB_DIRECT_SLOTS);
}

RID DirectShadows::get_factors(const Ref<RenderSceneBuffersRD> &p_render_buffers) const {
	return p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS);
}

bool DirectShadows::get_history(const Ref<RenderSceneBuffersRD> &p_render_buffers, uint64_t p_table_generation, RID &r_slots, RID &r_factors, Transform3D &r_camera, Projection &r_projection, RID &r_guide) const {
	if (!p_render_buffers->has_custom_data(RB_SCOPE_DIRECT_SHADOWS) || !p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS)) {
		return false;
	}
	Ref<ViewportData> viewport = p_render_buffers->get_custom_data(RB_SCOPE_DIRECT_SHADOWS);
	if (viewport->last_frame == 0 || viewport->table_generation != p_table_generation) {
		return false;
	}
	r_slots = get_slots(p_render_buffers);
	if (r_slots.is_null()) {
		return false;
	}
	r_factors = get_factors(p_render_buffers);
	r_camera = viewport->last_camera;
	r_projection = viewport->last_projection;
	r_guide = p_render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, viewport->flip ? RB_DIRECT_GUIDE_OTHER : RB_DIRECT_GUIDE);
	return true;
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
	if (served_request == request || viewport->size != largest_size_before || !p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS) || viewport->last_frame == 0 || !viewport->counted) {
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
	LocalVector<Layered> textures;
	if (viewport->quarter) {
		textures.push_back({ RB_DIRECT_SLOTS_RESOLVED, "slots", viewport->entries });
		textures.push_back({ RB_DIRECT_FACTORS, "factors", factor_layers(viewport->entries) });
		textures.push_back({ viewport->flip ? RB_DIRECT_SLOTS_OTHER : RB_DIRECT_SLOTS, "slots_block", viewport->entries });
		textures.push_back({ RB_DIRECT_FACTORS_BLOCK, "factors_block", factor_layers(viewport->entries) });
		textures.push_back({ viewport->flip ? RB_DIRECT_ACCUM_OTHER : RB_DIRECT_ACCUM, "accum_block", viewport->entries - 1 });
		textures.push_back({ viewport->flip ? RB_DIRECT_REST_OTHER : RB_DIRECT_REST, "rest_block", 1 });
		textures.push_back({ viewport->flip ? RB_DIRECT_GUIDE_OTHER : RB_DIRECT_GUIDE, "guide_block", 1 });
		for (const StringName &name : { RB_DIRECT_DEPTH_FULL, RB_DIRECT_NORMAL_FULL, RB_DIRECT_TIER }) {
			if (p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, name)) {
				textures.push_back({ name, name == RB_DIRECT_DEPTH_FULL ? "depth_full" : (name == RB_DIRECT_NORMAL_FULL ? "normal_full" : "tier"), 1 });
			}
		}
	} else {
		textures.push_back({ viewport->flip ? RB_DIRECT_SLOTS_OTHER : RB_DIRECT_SLOTS, "slots", viewport->entries });
		textures.push_back({ RB_DIRECT_FACTORS, "factors", factor_layers(viewport->entries) });
		textures.push_back({ viewport->flip ? RB_DIRECT_ACCUM_OTHER : RB_DIRECT_ACCUM, "accum", viewport->precise ? viewport->entries : viewport->entries - 1 });
		textures.push_back({ viewport->flip ? RB_DIRECT_REST_OTHER : RB_DIRECT_REST, "rest", 1 });
	}
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
	const String lamps2_path = request + ".lamps2.bin";
	Ref<FileAccess> lamps2_file = FileAccess::open(lamps2_path, FileAccess::WRITE);
	ok = ok && lamps2_file.is_valid();
	if (lamps2_file.is_valid() && !dump_lamp_data.is_empty()) {
		ok = lamps2_file->store_buffer(reinterpret_cast<const uint8_t *>(dump_lamp_data.ptr()), dump_lamp_data.size() * sizeof(float)) && ok;
	}
	lamps2_file.unref();
	files["lamps2"] = lamps2_path.get_file();
	const Vector<uint8_t> counters = rd->buffer_get_data(counter_buffer);
	ok = ok && counters.size() >= int(COUNTER_BYTES);

	Dictionary report;
	report["ok"] = ok;
	report["width"] = viewport->size.x;
	report["height"] = viewport->size.y;
	report["entries"] = viewport->entries;
	report["mode"] = viewport->quarter ? "quarter" : "full";
	if (viewport->quarter) {
		report["block_width"] = viewport->grid.x;
		report["block_height"] = viewport->grid.y;
	}
	const Projection inverse = viewport->last_projection.inverse();
	Array inverse_values;
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			inverse_values.push_back(float(inverse.columns[c][r]));
		}
	}
	report["inv_projection"] = inverse_values;
	Array camera_origin;
	Array camera_basis;
	for (int r = 0; r < 3; r++) {
		camera_origin.push_back(double(viewport->last_camera.origin[r]));
		for (int c = 0; c < 3; c++) {
			camera_basis.push_back(double(viewport->last_camera.basis.rows[r][c]));
		}
	}
	report["camera_origin"] = camera_origin;
	report["camera_basis"] = camera_basis;
	Array jitter;
	jitter.push_back(double(viewport->last_jitter.x));
	jitter.push_back(double(viewport->last_jitter.y));
	report["jitter"] = jitter;
	report["frame"] = viewport->last_frame;
	report["precise"] = viewport->precise;
	report["files"] = files;
	report["lamp_slots"] = lamp_dump.size() / DirectLightTable::LAMP_VALUES;
	report["lamp2_values"] = DirectLightTable::DUMP_LAMP_VALUES;
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
		report["walked_lights"] = values[5];
		report["most_walked"] = values[6];
		report["passed_lights"] = values[7];
		report["history_accepted"] = values[8];
		report["resolve_tier1"] = values[9];
		report["resolve_tier2"] = values[10];
		report["resolve_tier3"] = values[11];
		report["resolve_cut"] = values[12];
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
