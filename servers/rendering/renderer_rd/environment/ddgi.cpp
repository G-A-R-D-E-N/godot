/**************************************************************************/
/*  ddgi.cpp                                                              */
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

#include "ddgi.h"

#include "core/config/project_settings.h"
#include "core/math/random_pcg.h"
#include "servers/rendering/renderer_rd/environment/ddgi_light_tracker.h"
#include "servers/rendering/renderer_rd/environment/ray_scene.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/camera_attributes_storage.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

bool DDGI::is_enabled() {
	return GLOBAL_GET_CACHED(bool, "rendering/global_illumination/ddgi/enabled");
}

DDGI::Settings DDGI::get_project_settings() {
	Settings settings;
	const int32_t horizontal = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ddgi/probe_grid_horizontal"), 8, 64);
	const int32_t vertical_limit = MAX_ATLAS_WIDTH / (DISTANCE_SIZE + 2) / horizontal;
	const int32_t vertical = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ddgi/probe_grid_vertical"), 4, vertical_limit);
	settings.dims = Vector3i(horizontal, vertical, horizontal);
	settings.level_count = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ddgi/levels"), 1, (int)MAX_LEVELS);
	settings.spacing = MAX(GLOBAL_GET_CACHED(float, "rendering/global_illumination/ddgi/probe_spacing"), 0.1f);
	settings.rays_per_probe = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ddgi/rays_per_probe"), 32, (int)MAX_RAYS);
	settings.probes_per_frame = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ddgi/probes_per_frame"), 64, 16384);
	settings.hysteresis = CLAMP(GLOBAL_GET_CACHED(float, "rendering/global_illumination/ddgi/hysteresis"), 0.0f, 0.999f);
	settings.normal_bias = GLOBAL_GET_CACHED(float, "rendering/global_illumination/ddgi/normal_bias");
	settings.view_bias = GLOBAL_GET_CACHED(float, "rendering/global_illumination/ddgi/view_bias");
	settings.change_threshold = CLAMP(GLOBAL_GET_CACHED(float, "rendering/global_illumination/ddgi/change_threshold"), 0.05f, 1.0f);
	settings.light_change_reset = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/ddgi/light_change_reset");
	settings.energy = MAX(GLOBAL_GET_CACHED(float, "rendering/global_illumination/ddgi/energy"), 0.0f);
	settings.bounce_energy = CLAMP(GLOBAL_GET_CACHED(float, "rendering/global_illumination/ddgi/bounce_energy"), 0.0f, 1.0f);
	settings.debug_view = CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ddgi/debug_view"), 0, (int)DEBUG_VIEW_PROBE_VISIBILITY);
	return settings;
}

void DDGI::Volume::allocate(const Settings &p_settings) {
	settings = p_settings;
	const Vector3i dims = settings.dims;

	RD::TextureFormat tf;
	tf.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	tf.array_layers = settings.level_count;
	tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;

	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = dims.x * dims.y * (IRRADIANCE_SIZE + 2);
	tf.height = dims.z * (IRRADIANCE_SIZE + 2);
	irradiance = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(irradiance, "DDGI Irradiance");

	tf.format = RD::DATA_FORMAT_R16G16_SFLOAT;
	tf.width = dims.x * dims.y * (DISTANCE_SIZE + 2);
	tf.height = dims.z * (DISTANCE_SIZE + 2);
	distance = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(distance, "DDGI Distance");

	tf.format = RD::DATA_FORMAT_R32G32B32A32_UINT;
	tf.width = dims.x * dims.y;
	tf.height = dims.z;
	tf.array_layers = settings.level_count * 2;
	probe_data = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(probe_data, "DDGI Probe Data");

	RD::get_singleton()->texture_clear(irradiance, Color(0, 0, 0, 0), 0, 1, 0, settings.level_count);
	RD::get_singleton()->texture_clear(distance, Color(0, 0, 0, 0), 0, 1, 0, settings.level_count);
	RD::get_singleton()->texture_clear(probe_data, Color(0, 0, 0, 0), 0, 1, 0, settings.level_count * 2);

	RD::TextureFormat ray_format;
	ray_format.texture_type = RD::TEXTURE_TYPE_2D;
	ray_format.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	ray_format.width = settings.rays_per_probe * 2;
	ray_format.height = settings.probes_per_frame;
	ray_format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT;
	rays = RD::get_singleton()->texture_create(ray_format, RD::TextureView());
	RD::get_singleton()->set_resource_name(rays, "DDGI Rays");

	Vector<uint8_t> list_data;
	list_data.resize((LIST_HEADER + settings.probes_per_frame) * sizeof(uint32_t));
	list_data.fill(0);
	probe_list_buffer = RD::get_singleton()->storage_buffer_create(list_data.size(), list_data);
	RD::get_singleton()->set_resource_name(probe_list_buffer, "DDGI Probe List");
	Vector<uint8_t> groups_data;
	groups_data.resize(2 * (1 + Math::division_round_up(uint32_t(dims.x * dims.y * dims.z) * settings.level_count, 64u)) * 4 * sizeof(uint32_t));
	groups_data.fill(0);
	schedule_groups_buffer = RD::get_singleton()->storage_buffer_create(groups_data.size(), groups_data);
	RD::get_singleton()->set_resource_name(schedule_groups_buffer, "DDGI Schedule Groups");
	Vector<uint8_t> dispatch_data;
	dispatch_data.resize(sizeof(uint32_t) * 9);
	dispatch_data.fill(0);
	dispatch_buffer = RD::get_singleton()->storage_buffer_create(dispatch_data.size(), dispatch_data, RD::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	RD::get_singleton()->set_resource_name(dispatch_buffer, "DDGI Dispatch");
	ubo = RD::get_singleton()->uniform_buffer_create(sizeof(UBO));
	reset_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(ResetUBO));
	RD::get_singleton()->set_resource_name(ubo, "DDGI UBO");
}

LocalVector<RD::Uniform> DDGI::Volume::gather_uniforms() const {
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, ubo));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, probe_data));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, irradiance));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, distance));
	return uniforms;
}

void DDGI::Volume::free_data() {
	RID *owned[] = { &irradiance, &distance, &probe_data, &rays, &probe_list_buffer, &schedule_groups_buffer, &dispatch_buffer, &ubo, &reset_ubo, &debug_texture };
	for (RID *rid : owned) {
		if (rid->is_valid()) {
			RD::get_singleton()->free_rid(*rid);
			*rid = RID();
		}
	}
	for (uint32_t l = 0; l < MAX_LEVELS; l++) {
		level_valid[l] = false;
	}
}

void DDGI::Volume::schedule(const Vector3 &p_camera_position) {
	frame++;
	if (frame == 0) {
		frame = 1;
	}

	const Vector3i dims = settings.dims;
	for (uint32_t l = 0; l < settings.level_count; l++) {
		const double spacing = double(settings.spacing) * double(1 << l);
		const Vector3i cell(int32_t(Math::floor(double(p_camera_position.x) / spacing)), int32_t(Math::floor(double(p_camera_position.y) / spacing)), int32_t(Math::floor(double(p_camera_position.z) / spacing)));
		const Vector3i target = cell - dims / 2;
		if (level_valid[l]) {
			const Vector3i delta = target - g_min[l];
			if (Math::abs(delta.x) <= 1 && Math::abs(delta.y) <= 1 && Math::abs(delta.z) <= 1) {
				continue;
			}
		}
		g_min[l] = target;
		level_valid[l] = true;
	}
}

void DDGI::Volume::fill_ubo(UBO &r_ubo, const Vector3 &p_camera_position) const {
	memset(&r_ubo, 0, sizeof(UBO));
	const Vector3i dims = settings.dims;
	for (uint32_t l = 0; l < settings.level_count; l++) {
		const double spacing = double(settings.spacing) * double(1 << l);
		LevelData &level = r_ubo.levels[l];
		for (int k = 0; k < 3; k++) {
			level.origin[k] = float(double(g_min[l][k]) * spacing - double(p_camera_position[k]));
			level.g_min[k] = g_min[l][k];
			level.slot_offset[k] = int32_t(Math::posmod(int64_t(g_min[l][k]), int64_t(dims[k])));
		}
		level.spacing = float(spacing);
	}

	r_ubo.dims[0] = dims.x;
	r_ubo.dims[1] = dims.y;
	r_ubo.dims[2] = dims.z;
	r_ubo.level_count = settings.level_count;
	r_ubo.irradiance_texel_size[0] = 1.0f / float(dims.x * dims.y * (IRRADIANCE_SIZE + 2));
	r_ubo.irradiance_texel_size[1] = 1.0f / float(dims.z * (IRRADIANCE_SIZE + 2));
	r_ubo.distance_texel_size[0] = 1.0f / float(dims.x * dims.y * (DISTANCE_SIZE + 2));
	r_ubo.distance_texel_size[1] = 1.0f / float(dims.z * (DISTANCE_SIZE + 2));
	r_ubo.normal_bias = settings.normal_bias;
	r_ubo.view_bias = settings.view_bias;
	r_ubo.energy = settings.energy;
	r_ubo.hysteresis = settings.hysteresis;

	RandomPCG rng(frame);
	const float u1 = rng.randf();
	const float u2 = rng.randf() * Math::TAU;
	const float u3 = rng.randf() * Math::TAU;
	const Quaternion rotation(Math::sqrt(1.0f - u1) * Math::sin(u2), Math::sqrt(1.0f - u1) * Math::cos(u2), Math::sqrt(u1) * Math::sin(u3), Math::sqrt(u1) * Math::cos(u3));
	MaterialStorage::store_transform(Transform3D(Basis(rotation.normalized()), Vector3()), r_ubo.ray_rotation);

	r_ubo.rays_per_probe = MIN(settings.rays_per_probe, MAX_RAYS);
	r_ubo.frame = frame;
	r_ubo.max_distance = settings.spacing * float(1 << (settings.level_count - 1)) * float(MAX(dims.x, MAX(dims.y, dims.z)));
	r_ubo.probe_capacity = settings.probes_per_frame;
	r_ubo.change_threshold = settings.change_threshold;
}

void DDGI::init(SkyRD *p_sky) {
	Vector<String> modes;
	modes.push_back("\n#define MODE_BLEND_IRRADIANCE\n");
	modes.push_back("\n#define MODE_BLEND_DISTANCE\n");
	modes.push_back("\n#define MODE_PROBE_STATE\n");
	modes.push_back("\n#define MODE_SCHEDULE_COUNT\n");
	modes.push_back("\n#define MODE_SCHEDULE_PLAN\n");
	modes.push_back("\n#define MODE_SCHEDULE_SELECT\n");
	modes.push_back("\n#define MODE_SCHEDULE_SCAN\n");
	modes.push_back("\n#define MODE_SCHEDULE_APPEND\n");
	modes.push_back("\n#define MODE_SCHEDULE_FINALIZE\n");
	modes.push_back("\n#define MODE_RESET_COUNTS\n");
	shader.initialize(modes);
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].create_compute_pipeline(shader.version_get_shader(shader_version, i));
	}

	use_octmap_array = p_sky->sky_use_octmap_array;
	trace_defines = vformat("\n#define RAY_SCENE_BLAS_STACK_SIZE %d\n#define RAY_SCENE_TLAS_STACK_SIZE %d\n", RayScene::BLAS_MAX_DEPTH, RayScene::TLAS_MAX_DEPTH) + (use_octmap_array ? "\n#define USE_RADIANCE_OCTMAP_ARRAY\n" : "") + RayScene::alpha_shader_defines();
	trace_shader.initialize(_trace_modes(), trace_defines);
	trace_shader_version = trace_shader.version_create();
	for (int i = 0; i < TRACE_MODE_MAX; i++) {
		if (i == TRACE_MODE_RTGI) {
			continue;
		}
		trace_pipelines[i].create_compute_pipeline(trace_shader.version_get_shader(trace_shader_version, i));
	}
}

Vector<ShaderRD::VariantDefine> DDGI::_trace_modes() {
	Vector<ShaderRD::VariantDefine> trace_modes;
	trace_modes.push_back(ShaderRD::VariantDefine(TRACE_GROUP_BASE, "\n#define MODE_PROBE_TRACE\n", true));
	trace_modes.push_back(ShaderRD::VariantDefine(TRACE_GROUP_BASE, "\n#define MODE_DEBUG_RAYS\n", true));
	trace_modes.push_back(ShaderRD::VariantDefine(TRACE_GROUP_BASE, "\n#define MODE_SURFEL_TRACE\n", true));
	trace_modes.push_back(ShaderRD::VariantDefine(TRACE_GROUP_RTGI, "\n#define MODE_RTGI\n", false));
	return trace_modes;
}

bool DDGI::_ensure_trace_hw() {
	if (trace_shader_hw_version.is_valid()) {
		return trace_hw_valid;
	}
	trace_shader_hw.initialize(_trace_modes(), trace_defines + "\n#define SHADER_GLSL_VERSION_460\n#extension GL_EXT_ray_query : require\n#define RAY_SCENE_HW\n");
	trace_shader_hw_version = trace_shader_hw.version_create();
	trace_hw_valid = trace_shader_hw.version_is_valid(trace_shader_hw_version);
	if (trace_hw_valid) {
		for (int i = 0; i < TRACE_MODE_MAX; i++) {
			if (i == TRACE_MODE_RTGI) {
				continue;
			}
			trace_pipelines_hw[i].create_compute_pipeline(trace_shader_hw.version_get_shader(trace_shader_hw_version, i));
		}
		for (int i = 0; i < TRACE_MODE_MAX; i++) {
			trace_hw_valid = trace_hw_valid && (i == TRACE_MODE_RTGI || trace_pipelines_hw[i].get_rid().is_valid());
		}
	}
	return trace_hw_valid;
}

bool DDGI::_trace_usable(RayScene *p_ray_scene) {
	if (!p_ray_scene->uses_hardware() || _ensure_trace_hw()) {
		return true;
	}
	p_ray_scene->refuse_hardware("the hardware trace shader or its pipelines could not be made");
	return false;
}

void DDGI::free() {
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].free();
	}
	if (shader_version.is_valid()) {
		shader.version_free(shader_version);
		shader_version = RID();
	}
	for (int i = 0; i < TRACE_MODE_MAX; i++) {
		trace_pipelines[i].free();
	}
	if (trace_shader_version.is_valid()) {
		trace_shader.version_free(trace_shader_version);
		trace_shader_version = RID();
	}
	for (int i = 0; i < TRACE_MODE_MAX; i++) {
		trace_pipelines_hw[i].free();
	}
	if (trace_shader_hw_version.is_valid()) {
		trace_shader_hw.version_free(trace_shader_hw_version);
		trace_shader_hw_version = RID();
		trace_hw_valid = false;
	}
}

void DDGI::update(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Vector3 &p_camera_position) {
	Ref<Volume> volume;
	if (p_render_buffers->has_custom_data(RB_SCOPE_DDGI)) {
		volume = p_render_buffers->get_custom_data(RB_SCOPE_DDGI);
	}
	if (!is_enabled()) {
		if (volume.is_valid()) {
			p_render_buffers->set_custom_data(RB_SCOPE_DDGI, Ref<RenderBufferCustomDataRD>());
		}
		return;
	}

	const Settings settings = get_project_settings();
	if (volume.is_null() || volume->ubo.is_null() || !volume->settings.same_storage(settings)) {
		volume.instantiate();
		volume->allocate(settings);
		p_render_buffers->set_custom_data(RB_SCOPE_DDGI, volume);
	}
	volume->settings = settings;
	volume->schedule(p_camera_position);

	UBO ubo;
	volume->fill_ubo(ubo, p_camera_position);
	RD::get_singleton()->buffer_update(volume->ubo, 0, sizeof(UBO), &ubo);
}

void DDGI::_dispatch(Mode p_mode, const Ref<Volume> &p_volume, uint32_t p_x_groups, int32_t p_indirect_offset) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	RID shader_rid = shader.version_get_shader(shader_version, p_mode);

	RD::Uniform u_ubo(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_volume->ubo);
	RD::Uniform u_list(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_volume->probe_list_buffer);
	RD::Uniform u_rays(RD::UNIFORM_TYPE_IMAGE, 2, p_volume->rays);
	RD::Uniform u_irradiance(RD::UNIFORM_TYPE_IMAGE, 3, p_volume->irradiance);
	RD::Uniform u_distance(RD::UNIFORM_TYPE_IMAGE, 4, p_volume->distance);
	RD::Uniform u_probe_data(RD::UNIFORM_TYPE_IMAGE, 5, p_volume->probe_data);

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[p_mode].get_rid());
	RID uniform_set;
	if (p_mode == MODE_SCHEDULE_FINALIZE) {
		uniform_set = uniform_set_cache->get_cache(shader_rid, 0, u_ubo, u_list, u_rays, u_irradiance, u_distance, u_probe_data, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 6, p_volume->dispatch_buffer));
	} else if (p_mode == MODE_SCHEDULE_SELECT || p_mode == MODE_SCHEDULE_SCAN || p_mode == MODE_SCHEDULE_APPEND) {
		uniform_set = uniform_set_cache->get_cache(shader_rid, 0, u_ubo, u_list, u_rays, u_irradiance, u_distance, u_probe_data, RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 7, p_volume->schedule_groups_buffer));
	} else {
		uniform_set = uniform_set_cache->get_cache(shader_rid, 0, u_ubo, u_list, u_rays, u_irradiance, u_distance, u_probe_data);
	}
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set, 0);
	if (p_indirect_offset >= 0) {
		RD::get_singleton()->compute_list_dispatch_indirect(compute_list, p_volume->dispatch_buffer, p_indirect_offset);
	} else {
		RD::get_singleton()->compute_list_dispatch(compute_list, p_x_groups, 1, 1);
	}
	RD::get_singleton()->compute_list_end();
}

static uint32_t count_touched(const DDGI::Volume &p_volume, const LocalVector<DDGILightTracker::Box> &p_boxes, uint32_t &r_total) {
	const Vector3i dims = p_volume.settings.dims;
	const uint32_t per_level = uint32_t(dims.x) * uint32_t(dims.y) * uint32_t(dims.z);
	uint32_t touched = 0;
	r_total = 0;
	LocalVector<uint8_t> marked;
	for (uint32_t l = 0; l < p_volume.settings.level_count; l++) {
		if (!p_volume.level_valid[l]) {
			continue;
		}
		r_total += per_level;
		marked.resize(per_level);
		for (uint32_t i = 0; i < per_level; i++) {
			marked[i] = 0;
		}
		const double spacing = double(p_volume.settings.spacing) * double(1 << l);
		for (const DDGILightTracker::Box &box : p_boxes) {
			Vector3i low, high;
			for (int k = 0; k < 3; k++) {
				low[k] = MAX(0, int32_t(Math::ceil((double(box.center[k]) - double(box.half)) / spacing)) - p_volume.g_min[l][k]);
				high[k] = MIN(dims[k] - 1, int32_t(Math::floor((double(box.center[k]) + double(box.half)) / spacing)) - p_volume.g_min[l][k]);
			}
			for (int32_t z = low.z; z <= high.z; z++) {
				for (int32_t y = low.y; y <= high.y; y++) {
					for (int32_t x = low.x; x <= high.x; x++) {
						marked[uint32_t(x + dims.x * (y + dims.y * z))] = 1;
					}
				}
			}
		}
		for (uint32_t i = 0; i < per_level; i++) {
			touched += marked[i];
		}
	}
	return touched;
}

void DDGI::_reset_counts(const Ref<Volume> &p_volume, const Vector3 &p_camera_position) {
	LocalVector<DDGILightTracker::Box> boxes;
	DDGILightTracker::take(boxes);
	if (boxes.is_empty()) {
		return;
	}
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	ResetUBO reset = {};
	reset.count = MIN(uint32_t(boxes.size()), RESET_BOXES);
	for (uint32_t i = 0; i < reset.count; i++) {
		const Vector3 relative = boxes[i].center - p_camera_position;
		reset.boxes[i][0] = float(relative.x);
		reset.boxes[i][1] = float(relative.y);
		reset.boxes[i][2] = float(relative.z);
		reset.boxes[i][3] = boxes[i].half;
	}
	RD::get_singleton()->buffer_update(p_volume->reset_ubo, 0, sizeof(ResetUBO), &reset);

	DDGILightTracker::Stats &stats = DDGILightTracker::stats();
	stats.dispatches++;
	stats.dispatched += reset.count;
	if (GLOBAL_GET_CACHED(bool, "rendering/global_illumination/ddgi/light_change_reset_log")) {
		uint32_t total = 0;
		const uint32_t touched = count_touched(*p_volume.ptr(), boxes, total);
		stats.touched += touched;
		stats.total_probes = total;
		stats.share_sum += total > 0 ? double(touched) / double(total) : 0.0;
	}

	RID shader_rid = shader.version_get_shader(shader_version, MODE_RESET_COUNTS);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_volume->ubo));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_volume->probe_list_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_volume->rays));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 3, p_volume->irradiance));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, p_volume->distance));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, p_volume->probe_data));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 8, p_volume->reset_ubo));

	const Vector3i dims = p_volume->settings.dims;
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_RESET_COUNTS].get_rid());
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rid, 0, uniforms), 0);
	RD::get_singleton()->compute_list_dispatch(compute_list, uint32_t(dims.x * dims.y), uint32_t(dims.z), p_volume->settings.level_count);
	RD::get_singleton()->compute_list_end();
}

void DDGI::_trace(TraceMode p_mode, const Ref<Volume> &p_volume, RID p_output, const TracePushConstant &p_push_constant, uint32_t p_x_groups, uint32_t p_y_groups, RID p_indirect_buffer, uint32_t p_indirect_offset, const LocalVector<RD::Uniform> *p_surfel_uniforms) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	TextureStorage *texture_storage = TextureStorage::get_singleton();
	RayScene *ray_scene = RayScene::get_singleton();
	const bool hardware = ray_scene->uses_hardware();
	RID shader_rid = hardware ? trace_shader_hw.version_get_shader(trace_shader_hw_version, p_mode) : trace_shader.version_get_shader(trace_shader_version, p_mode);

	RD::Uniform u_ubo(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_volume->ubo);
	RD::Uniform u_list(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_volume->probe_list_buffer);
	RD::Uniform u_output(RD::UNIFORM_TYPE_IMAGE, 2, p_output);
	RD::Uniform u_probe_data(RD::UNIFORM_TYPE_TEXTURE, 3, p_volume->probe_data);
	RD::Uniform u_irradiance(RD::UNIFORM_TYPE_TEXTURE, 4, p_volume->irradiance);
	RD::Uniform u_distance(RD::UNIFORM_TYPE_TEXTURE, 5, p_volume->distance);
	RD::Uniform u_linear(RD::UNIFORM_TYPE_SAMPLER, 6, material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));

	LocalVector<RD::Uniform> scene_uniforms;
	scene_uniforms.reserve(8);
	ray_scene->append_uniforms(scene_uniforms);

	RID sky_texture = p_volume->sky_texture;
	if (sky_texture.is_null()) {
		sky_texture = texture_storage->texture_rd_get_default(use_octmap_array ? TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE : TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	}
	RD::Uniform u_sky(RD::UNIFORM_TYPE_TEXTURE, 0, sky_texture);
	RD::Uniform u_sky_sampler(RD::UNIFORM_TYPE_SAMPLER, 1, material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, (hardware ? trace_pipelines_hw : trace_pipelines)[p_mode].get_rid());
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 0, u_ubo, u_list, u_output, u_probe_data, u_irradiance, u_distance, u_linear), 0);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rid, 1, scene_uniforms), 1);
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(shader_rid, 2, u_sky, u_sky_sampler), 2);
	if (p_surfel_uniforms) {
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rid, 3, *p_surfel_uniforms), 3);
	}
	RD::get_singleton()->compute_list_set_push_constant(compute_list, &p_push_constant, sizeof(TracePushConstant));
	if (p_indirect_buffer.is_valid()) {
		RD::get_singleton()->compute_list_dispatch_indirect(compute_list, p_indirect_buffer, p_indirect_offset);
	} else {
		RD::get_singleton()->compute_list_dispatch(compute_list, p_x_groups, p_y_groups, 1);
	}
	RD::get_singleton()->compute_list_end();
}

void DDGI::_fill_sky(TracePushConstant &r_push_constant, RID &r_sky_texture, RID p_environment, SkyRD::Sky *p_sky, RID p_camera_attributes) const {
	RendererSceneRenderRD *scene_render = RendererSceneRenderRD::get_singleton();
	const float exposure = p_camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_camera_attributes) : 1.0f;
	r_push_constant.emission_exposure = exposure;
	r_sky_texture = RID();
	if (p_environment.is_null()) {
		return;
	}

	const RSE::EnvironmentBG background = scene_render->environment_get_background(p_environment);
	if (background == RSE::ENV_BG_CLEAR_COLOR || background == RSE::ENV_BG_COLOR) {
		const Color color = background == RSE::ENV_BG_CLEAR_COLOR ? RSG::texture_storage->get_default_clear_color().srgb_to_linear() : scene_render->environment_get_bg_color(p_environment);
		r_push_constant.sky_flags = SKY_FLAGS_MODE_COLOR;
		r_push_constant.sky_color_or_orientation[0] = color.r;
		r_push_constant.sky_color_or_orientation[1] = color.g;
		r_push_constant.sky_color_or_orientation[2] = color.b;
		r_push_constant.sky_energy = scene_render->environment_get_bg_energy_multiplier(p_environment) * exposure;
	} else if (background == RSE::ENV_BG_SKY && p_sky && p_sky->radiance.is_valid()) {
		const RID sky_rid = scene_render->environment_get_sky(p_environment);
		float sky_energy = 1.0f;
		if (scene_render->is_using_physical_light_units()) {
			sky_energy = exposure * scene_render->environment_get_bg_intensity(p_environment) / MAX(1e-6f, scene_render->get_sky()->sky_get_baked_exposure(sky_rid));
		} else if (p_camera_attributes.is_valid()) {
			sky_energy = exposure * scene_render->environment_get_bg_intensity(p_environment) / MAX(0.001f, scene_render->get_sky()->sky_get_baked_exposure(sky_rid));
		}
		r_sky_texture = p_sky->radiance;
		r_push_constant.sky_flags = SKY_FLAGS_MODE_SKY;
		r_push_constant.sky_energy = sky_energy * scene_render->environment_get_bg_energy_multiplier(p_environment);
		r_push_constant.sky_border_size[0] = p_sky->uv_border_size;
		r_push_constant.sky_border_size[1] = 1.0f - p_sky->uv_border_size * 2.0f;
		const Quaternion orientation = scene_render->environment_get_sky_orientation(p_environment).get_quaternion().inverse();
		r_push_constant.sky_color_or_orientation[0] = orientation.x;
		r_push_constant.sky_color_or_orientation[1] = orientation.y;
		r_push_constant.sky_color_or_orientation[2] = orientation.z;
		r_push_constant.sky_flags |= orientation.w < 0.0f ? 0 : SKY_FLAGS_ORIENTATION_SIGN;
	}
}

void DDGI::trace_surfels(const Ref<Volume> &p_volume, const LocalVector<RD::Uniform> &p_surfel_uniforms, RID p_indirect_buffer, uint32_t p_indirect_offset) {
	RayScene *ray_scene = RayScene::get_singleton();
	if (!ray_scene || !ray_scene->is_ready() || !_trace_usable(ray_scene)) {
		return;
	}
	RD::get_singleton()->draw_command_begin_label("Surfel Trace");
	_trace(TRACE_MODE_SURFELS, p_volume, p_volume->rays, p_volume->trace_constant, 0, 0, p_indirect_buffer, p_indirect_offset, &p_surfel_uniforms);
	RD::get_singleton()->draw_command_end_label();
}

bool DDGI::trace_rtgi(const Ref<Volume> &p_volume, RID p_output, const LocalVector<RD::Uniform> &p_uniforms, uint32_t p_x_groups, uint32_t p_y_groups) {
	RayScene *ray_scene = RayScene::get_singleton();
	if (p_volume.is_null() || !ray_scene || !ray_scene->is_ready() || p_output.is_null() || !_trace_usable(ray_scene)) {
		return false;
	}
	const bool hardware = ray_scene->uses_hardware();
	DdgiTraceShaderRD &rtgi_shader = hardware ? trace_shader_hw : trace_shader;
	PipelineDeferredRD &rtgi_pipeline = hardware ? trace_pipelines_hw[TRACE_MODE_RTGI] : trace_pipelines[TRACE_MODE_RTGI];
	if (!rtgi_shader.is_group_enabled(TRACE_GROUP_RTGI)) {
		rtgi_shader.enable_group(TRACE_GROUP_RTGI);
		RID rtgi_shader_rid = rtgi_shader.version_get_shader(hardware ? trace_shader_hw_version : trace_shader_version, TRACE_MODE_RTGI);
		if (rtgi_shader_rid.is_valid()) {
			rtgi_pipeline.create_compute_pipeline(rtgi_shader_rid);
		}
	}
	if (!rtgi_pipeline.get_rid().is_valid()) {
		if (hardware) {
			ray_scene->refuse_hardware("the hardware RTGI trace pipeline could not be made");
		}
		return false;
	}
	_trace(TRACE_MODE_RTGI, p_volume, p_output, p_volume->trace_constant, p_x_groups, p_y_groups, RID(), 0, &p_uniforms);
	return true;
}

void DDGI::process(const Ref<Volume> &p_volume, RID p_environment, SkyRD::Sky *p_sky, RID p_camera_attributes, const Projection &p_projection, const Transform3D &p_camera_transform, const Size2i &p_screen_size) {
	RayScene *ray_scene = RayScene::get_singleton();
	if (!ray_scene || !ray_scene->is_ready() || !_trace_usable(ray_scene)) {
		return;
	}

	TracePushConstant push_constant = {};
	_fill_sky(push_constant, p_volume->sky_texture, p_environment, p_sky, p_camera_attributes);
	push_constant.bounce_energy = p_volume->settings.bounce_energy;

	const Vector3i dims = p_volume->settings.dims;
	const uint32_t probe_groups = Math::division_round_up(uint32_t(dims.x * dims.y * dims.z) * p_volume->settings.level_count, 64u);
	RD::get_singleton()->draw_command_begin_label("DDGI Update");
	if (p_volume->settings.light_change_reset) {
		_reset_counts(p_volume, p_camera_transform.origin);
		if (GLOBAL_GET_CACHED(bool, "rendering/global_illumination/ddgi/light_change_reset_log") && p_volume->frame % 100 == 0) {
			DDGILightTracker::Stats &stats = DDGILightTracker::stats();
			const double total = double(MAX(stats.total_probes, 1u));
			const double per_dispatch = stats.dispatches > 0 ? stats.share_sum / double(stats.dispatches) : 0.0;
			print_line(vformat("DDGI_RESET frame %d fired %d dispatched %d pending %d dispatches %d touched %d share_total %.5f share_per_dispatch %.5f", int64_t(p_volume->frame), int64_t(stats.fired), int64_t(stats.dispatched), int64_t(DDGILightTracker::pending_count()), int64_t(stats.dispatches), int64_t(stats.touched), double(stats.touched) / total, per_dispatch));
			stats = DDGILightTracker::Stats();
		}
	}
	RENDER_TIMESTAMP("DDGI Schedule");
	_dispatch(MODE_SCHEDULE_COUNT, p_volume, probe_groups);
	_dispatch(MODE_SCHEDULE_PLAN, p_volume, 1);
	_dispatch(MODE_SCHEDULE_SELECT, p_volume, probe_groups);
	_dispatch(MODE_SCHEDULE_SCAN, p_volume, 1);
	_dispatch(MODE_SCHEDULE_APPEND, p_volume, probe_groups);
	_dispatch(MODE_SCHEDULE_FINALIZE, p_volume, 1);
	RENDER_TIMESTAMP("DDGI Trace");
	p_volume->trace_constant = push_constant;
	_trace(TRACE_MODE_PROBES, p_volume, p_volume->rays, push_constant, 0, 0, p_volume->dispatch_buffer, TRACE_ARGS_OFFSET);
	RENDER_TIMESTAMP("DDGI Blend");
	_dispatch(MODE_BLEND_IRRADIANCE, p_volume, 0, BLEND_ARGS_OFFSET);
	_dispatch(MODE_BLEND_DISTANCE, p_volume, 0, BLEND_ARGS_OFFSET);
	_dispatch(MODE_PROBE_STATE, p_volume, 0, STATE_ARGS_OFFSET);
	RD::get_singleton()->draw_command_end_label();

	if (p_volume->settings.debug_view == DEBUG_VIEW_DISABLED) {
		if (p_volume->debug_texture.is_valid()) {
			RD::get_singleton()->free_rid(p_volume->debug_texture);
			p_volume->debug_texture = RID();
		}
		return;
	}
	if (p_volume->debug_texture.is_null() || p_volume->debug_size != p_screen_size) {
		if (p_volume->debug_texture.is_valid()) {
			RD::get_singleton()->free_rid(p_volume->debug_texture);
		}
		RD::TextureFormat tf;
		tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
		tf.width = p_screen_size.x;
		tf.height = p_screen_size.y;
		tf.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		p_volume->debug_texture = RD::get_singleton()->texture_create(tf, RD::TextureView());
		RD::get_singleton()->set_resource_name(p_volume->debug_texture, "DDGI Debug");
		p_volume->debug_size = p_screen_size;
	}
	push_constant.debug_mode = p_volume->settings.debug_view;
	push_constant.screen_size[0] = p_screen_size.x;
	push_constant.screen_size[1] = p_screen_size.y;
	const Projection view_projection = p_projection * Projection(Transform3D(p_camera_transform.basis, Vector3()).affine_inverse());
	MaterialStorage::store_camera(view_projection.inverse(), push_constant.debug_inv_view_projection);
	RD::get_singleton()->draw_command_begin_label("DDGI Debug");
	_trace(TRACE_MODE_DEBUG, p_volume, p_volume->debug_texture, push_constant, Math::division_round_up(uint32_t(p_screen_size.x), 8u), Math::division_round_up(uint32_t(p_screen_size.y), 8u));
	RD::get_singleton()->draw_command_end_label();
}
