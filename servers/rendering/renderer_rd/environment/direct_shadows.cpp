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
	return settings;
}

DirectShadows::DirectShadows() {
	defines = vformat("\n#define RAY_SCENE_BLAS_STACK_SIZE %d\n#define RAY_SCENE_TLAS_STACK_SIZE %d\n", RayScene::BLAS_MAX_DEPTH, RayScene::TLAS_MAX_DEPTH);
	Vector<String> modes;
	modes.push_back("\n");
	shader.initialize(modes, defines);
	shader_version = shader.version_create();
	pipeline.create_compute_pipeline(shader.version_get_shader(shader_version, 0));
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
	pipeline.free();
	pipeline_hw.free();
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
	Vector<String> modes;
	modes.push_back("\n");
	shader_hw.initialize(modes, defines + "\n#define SHADER_GLSL_VERSION_460\n#extension GL_EXT_ray_query : require\n#define RAY_SCENE_HW\n");
	shader_hw_version = shader_hw.version_create();
	hw_valid = shader_hw.version_is_valid(shader_hw_version);
	if (hw_valid) {
		pipeline_hw.create_compute_pipeline(shader_hw.version_get_shader(shader_hw_version, 0));
		hw_valid = pipeline_hw.get_rid().is_valid();
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
	if (p_viewport->entries == p_settings.entries && p_viewport->size == size && p_render_buffers->has_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS)) {
		return;
	}
	p_render_buffers->clear_context(RB_SCOPE_DIRECT_SHADOWS);
	p_viewport->entries = p_settings.entries;
	p_viewport->size = size;
	p_viewport->last_frame = 0;

	RD *rd = RD::get_singleton();
	const uint32_t usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	const uint32_t layers = factor_layers(p_settings.entries);
	const RID slots = p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS, RD::DATA_FORMAT_R16_UNORM, usage, RD::TEXTURE_SAMPLES_1, size, p_settings.entries, 1, true);
	const RID factors = p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS, RD::DATA_FORMAT_R16_UNORM, usage, RD::TEXTURE_SAMPLES_1, size, layers, 1, true);
	const RID accum = p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_ACCUM, RD::DATA_FORMAT_R32G32_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, p_settings.entries, 1, true);
	const RID rest = p_render_buffers->create_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_REST, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, usage, RD::TEXTURE_SAMPLES_1, size, 1, 1, true);
	rd->texture_clear(slots, Color(0, 0, 0, 0), 0, 1, 0, p_settings.entries);
	rd->texture_clear(factors, Color(1, 1, 1, 1), 0, 1, 0, layers);
	rd->texture_clear(accum, Color(0, 0, 0, 0), 0, 1, 0, p_settings.entries);
	rd->texture_clear(rest, Color(0, 0, 0, 0), 0, 1, 0, 1);
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

void DirectShadows::process(const FrameInput &p_input, const DirectLightTable &p_table) {
	RayScene *ray_scene = RayScene::get_singleton();
	if (!ray_scene || !ray_scene->is_ready()) {
		return;
	}
	const bool hardware = ray_scene->uses_hardware();
	if (hardware && !_ensure_hw()) {
		ray_scene->refuse_hardware("the direct shadow shader or its pipeline could not be made");
		return;
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
		const uint32_t layers = settings.entries;
		rd->texture_clear(p_input.render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS), Color(0, 0, 0, 0), 0, 1, 0, layers);
		rd->texture_clear(p_input.render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_ACCUM), Color(0, 0, 0, 0), 0, 1, 0, layers);
		rd->texture_clear(p_input.render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_REST), Color(0, 0, 0, 0), 0, 1, 0, 1);
	}

	p_table.write_slots(lamp_data);
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

	Params params = {};
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			params.inv_projection[c * 4 + r] = float(inverse.columns[c][r]);
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
	rd->buffer_update(params_buffer, 0, sizeof(Params), &params);
	rd->buffer_clear(counter_buffer, 0, COUNTER_BYTES);

	RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID shader_rid = hardware ? shader_hw.version_get_shader(shader_hw_version, 0) : shader.version_get_shader(shader_version, 0);

	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_input.cluster_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_input.omni_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_input.spot_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, lamp_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, params_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>{ sampler, p_input.depth }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>{ sampler, p_input.normal_roughness }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, p_input.render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_SLOTS)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_input.render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_FACTORS)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, p_input.render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_ACCUM)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, p_input.render_buffers->get_texture(RB_SCOPE_DIRECT_SHADOWS, RB_DIRECT_REST)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, _debug_target(p_input.render_buffers, settings)));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, counter_buffer));

	LocalVector<RD::Uniform> scene_uniforms;
	scene_uniforms.reserve(8);
	ray_scene->append_uniforms(scene_uniforms);

	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, (hardware ? pipeline_hw : pipeline).get_rid());
	rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 0, uniforms), 0);
	rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 1, scene_uniforms), 1);
	rd->compute_list_dispatch(list, Math::division_round_up(uint32_t(size.x), 8u), Math::division_round_up(uint32_t(size.y), 8u), 1);
	rd->compute_list_end();

	viewport->last_frame = p_input.frame;
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
		{ RB_DIRECT_SLOTS, "slots", viewport->entries },
		{ RB_DIRECT_FACTORS, "factors", factor_layers(viewport->entries) },
		{ RB_DIRECT_ACCUM, "accum", viewport->entries },
		{ RB_DIRECT_REST, "rest", 1 },
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
	const Vector<uint8_t> counters = rd->buffer_get_data(counter_buffer);
	ok = ok && counters.size() >= int(COUNTER_BYTES);

	Dictionary report;
	report["ok"] = ok;
	report["width"] = viewport->size.x;
	report["height"] = viewport->size.y;
	report["entries"] = viewport->entries;
	report["frame"] = viewport->last_frame;
	report["files"] = files;
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
