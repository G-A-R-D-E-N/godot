/**************************************************************************/
/*  restir_gi_core.cpp                                                    */
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

#include "restir_gi_core.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/environment/ray_scene.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

struct RestirFrameUBO {
	float cam_rot[3][4];
	float cam_to_anchor[4];
	float prev_rot[3][4];
	float prev_to_anchor[4];
	float view_to_prev_clip[16];
	float test_sky[4];
	int32_t trace_size[2];
	uint32_t frame;
	uint32_t seed;
	uint32_t flags;
	uint32_t spatial_count;
	uint32_t spatial_radius;
	uint32_t pass_index;
	uint32_t m_temporal_cap;
	uint32_t m_max;
	uint32_t n_target;
	uint32_t epoch_tag;
	uint32_t debug_view;
	uint32_t mode_code;
	uint32_t vis_fail_limit;
	uint32_t prev_epoch_tag;
	float tol_plane;
	float cos_normal;
	float jac_max;
	float d_min;
	float cos_min;
	float w_clamp;
	float origin_scale;
	float sky_distance;
	float class_tol;
	float emission_exposure;
	uint32_t dirty_count;
	float dirty_margin;
	uint32_t compat_k;
	float origin_floor;
	uint32_t reval_cut_m;
	uint32_t reval_share_denom;
	float shade_sky[3];
	uint32_t shade_sky_flags;
	float shade_sky_border[2];
	float shade_sky_energy;
	float shade_bounce_energy;
	float shade_exposure;
	float reval_threshold;
	uint32_t shade_pad[2];
	int32_t screen_size[2];
	int32_t trace_scale;
	uint32_t replay_pad;
};

struct RestirDirtyUBO {
	float box_min[64][4];
	float box_max[64][4];
};

static_assert(sizeof(RestirDirtyUBO) == 2048, "RestirDirtyUBO must match the std140 block in restir_gi_inc.glsl");
static_assert(sizeof(RestirFrameUBO) == 400, "RestirFrameUBO must match the std140 block in restir_gi_inc.glsl");

static constexpr uint32_t PASS_STATS_ROWS = 4;
static constexpr uint32_t PASS_STATS_STRIDE = 32;

static RID _create_texture(RD::DataFormat p_format, const Size2i &p_size, RD::TextureType p_type, uint32_t p_layers, uint32_t p_extra_usage = 0) {
	RD::TextureFormat format;
	format.format = p_format;
	format.width = p_size.x;
	format.height = p_size.y;
	format.depth = 1;
	format.array_layers = p_layers;
	format.mipmaps = 1;
	format.texture_type = p_type;
	format.samples = RD::TEXTURE_SAMPLES_1;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT | p_extra_usage;
	return RD::get_singleton()->texture_create(format, RD::TextureView());
}

bool RestirGiResources::allocate(const Size2i &p_size, bool p_debug) {
	free_all();
	if (p_size.x <= 0 || p_size.y <= 0) {
		return false;
	}
	RD *rd = RD::get_singleton();
	size = p_size;
	alloc_log.clear();
	const uint64_t pixels = uint64_t(p_size.x) * uint64_t(p_size.y);
	auto texture = [&](const char *p_name, RD::DataFormat p_format, uint64_t p_bytes_per_pixel, uint32_t p_layers, RD::TextureType p_type) {
		const uint64_t before = rd->get_memory_usage(RD::MEMORY_TEXTURES);
		RID rid = _create_texture(p_format, p_size, p_type, p_layers);
		alloc_log.push_back({ p_name, pixels * p_bytes_per_pixel * p_layers, rd->get_memory_usage(RD::MEMORY_TEXTURES) - before, false });
		return rid;
	};
	const char *reservoir_names[3] = { "reservoir set 0", "reservoir set 1", "reservoir set 2" };
	for (int i = 0; i < 3; i++) {
		reservoir[i] = texture(reservoir_names[i], RD::DATA_FORMAT_R32G32B32A32_UINT, 16, 3, RD::TEXTURE_TYPE_2D_ARRAY);
	}
	vis_state = texture("vis_state", RD::DATA_FORMAT_R32_UINT, 4, 1, RD::TEXTURE_TYPE_2D);
	info = texture("info", RD::DATA_FORMAT_R32_UINT, 4, 1, RD::TEXTURE_TYPE_2D);
	radiance = texture("radiance", RD::DATA_FORMAT_R16G16B16A16_SFLOAT, 8, 1, RD::TEXTURE_TYPE_2D);
	coverage = texture("coverage", RD::DATA_FORMAT_R16_SFLOAT, 2, 1, RD::TEXTURE_TYPE_2D);
	if (p_debug) {
		debug = texture("debug", RD::DATA_FORMAT_R16G16B16A16_SFLOAT, 8, 1, RD::TEXTURE_TYPE_2D);
	}
	debug_dummy = _create_texture(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, Size2i(1, 1), RD::TEXTURE_TYPE_2D, 1);
	group_count = uint32_t(Math::division_round_up(p_size.x, 8)) * uint32_t(Math::division_round_up(p_size.y, 8));
	auto buffer = [&](const char *p_name, uint64_t p_bytes) {
		const uint64_t before = rd->get_memory_usage(RD::MEMORY_BUFFERS);
		RID rid = rd->storage_buffer_create(p_bytes);
		alloc_log.push_back({ p_name, p_bytes, rd->get_memory_usage(RD::MEMORY_BUFFERS) - before, true });
		return rid;
	};
	stats = buffer("stats", group_count * 16 * sizeof(float));
	pass_stats = buffer("pass_stats", group_count * PASS_STATS_ROWS * PASS_STATS_STRIDE * sizeof(float));
	{
		const uint64_t before = rd->get_memory_usage(RD::MEMORY_TEXTURES);
		sel_dummy = _create_texture(RD::DATA_FORMAT_R32G32_UINT, Size2i(1, 1), RD::TEXTURE_TYPE_2D_ARRAY, 2);
		alloc_log.push_back({ "sel_dummy", 16, rd->get_memory_usage(RD::MEMORY_TEXTURES) - before, false });
	}
	{
		const uint64_t before = rd->get_memory_usage(RD::MEMORY_BUFFERS);
		for (int i = 0; i < 6; i++) {
			ubo[i] = rd->uniform_buffer_create(sizeof(RestirFrameUBO));
		}
		dirty_ubo = rd->uniform_buffer_create(sizeof(RestirDirtyUBO));
		alloc_log.push_back({ "uniform buffers", 6 * sizeof(RestirFrameUBO) + sizeof(RestirDirtyUBO), rd->get_memory_usage(RD::MEMORY_BUFFERS) - before, true });
	}
	history_index = 0;
	final_index = 0;
	allocated = true;
	return true;
}

void RestirGiResources::ensure_debug(bool p_needed) {
	if (!allocated) {
		return;
	}
	RD *rd = RD::get_singleton();
	if (p_needed && debug.is_null()) {
		const uint64_t before = rd->get_memory_usage(RD::MEMORY_TEXTURES);
		debug = _create_texture(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, size, RD::TEXTURE_TYPE_2D, 1);
		alloc_log.push_back({ "debug", uint64_t(size.x) * uint64_t(size.y) * 8u, rd->get_memory_usage(RD::MEMORY_TEXTURES) - before, false });
	} else if (!p_needed && debug.is_valid()) {
		rd->free_rid(debug);
		debug = RID();
		for (uint32_t i = 0; i < alloc_log.size(); i++) {
			if (String(alloc_log[i].name) == "debug") {
				alloc_log.remove_at(i);
				break;
			}
		}
	}
}

void RestirGiResources::free_all() {
	RD *rd = RD::get_singleton();
	if (!rd) {
		allocated = false;
		return;
	}
	for (int i = 0; i < 3; i++) {
		if (reservoir[i].is_valid()) {
			rd->free_rid(reservoir[i]);
			reservoir[i] = RID();
		}
	}
	RID *singles[] = { &vis_state, &info, &radiance, &coverage, &debug, &debug_dummy, &stats, &pass_stats, &sel_image, &sel_dummy };
	for (RID *rid : singles) {
		if (rid->is_valid()) {
			rd->free_rid(*rid);
			*rid = RID();
		}
	}
	for (int i = 0; i < 6; i++) {
		if (ubo[i].is_valid()) {
			rd->free_rid(ubo[i]);
			ubo[i] = RID();
		}
	}
	if (dirty_ubo.is_valid()) {
		rd->free_rid(dirty_ubo);
		dirty_ubo = RID();
	}
	allocated = false;
	group_count = 0;
}

RestirGiCore::~RestirGiCore() {
	free();
}


String RestirGiCore::_defines(Variant p_variant) const {
	if (p_variant == VARIANT_NONE) {
		return String();
	}
	String defines = vformat("\n#define RAY_SCENE_BLAS_STACK_SIZE %d\n#define RAY_SCENE_TLAS_STACK_SIZE %d\n#define RESTIR_RAYS\n", RayScene::BLAS_MAX_DEPTH, RayScene::TLAS_MAX_DEPTH);
	if (p_variant == VARIANT_HW) {
		defines += "\n#define SHADER_GLSL_VERSION_460\n#extension GL_EXT_ray_query : require\n#define RAY_SCENE_HW\n";
	}
	if (octmap_array) {
		defines += "\n#define USE_RADIANCE_OCTMAP_ARRAY\n";
	}
	return defines;
}

bool RestirGiCore::_ensure(Variant p_variant) {
	if (initialized[p_variant]) {
		return valid[p_variant];
	}
	initialized[p_variant] = true;
	octmap_array = GLOBAL_GET("rendering/reflections/sky_reflections/texture_array_reflections");
	if (p_variant == VARIANT_HW) {
		hw_compiles++;
	}
	Vector<String> modes;
	modes.push_back("\n#define MODE_CLEAR\n");
	modes.push_back("\n#define MODE_TEMPORAL\n");
	modes.push_back("\n#define MODE_SPATIAL\n");
	modes.push_back("\n#define MODE_FINAL\n");
	modes.push_back("\n#define MODE_SOURCE\n");
	modes.push_back("\n#define MODE_CAPTURE\n");
	modes.push_back("\n#define MODE_SPATIAL\n#define PASS_SPATIAL_COMPAT\n");
	modes.push_back("\n#define MODE_PACK\n");
	modes.push_back("\n#define MODE_SPATIAL\n#define SPATIAL_WIDE\n");
	modes.push_back("\n#define MODE_SPATIAL\n#define PASS_SPATIAL_COMPAT\n#define SPATIAL_WIDE\n");
	modes.push_back("\n#define MODE_VALIDATE\n");
	shaders[p_variant].initialize(modes, _defines(p_variant));
	versions[p_variant] = shaders[p_variant].version_create();
	bool ok = p_variant != VARIANT_HW || shaders[p_variant].version_is_valid(versions[p_variant]);
	for (int i = 0; ok && i < PASS_MAX; i++) {
		RID shader_rid = shaders[p_variant].version_get_shader(versions[p_variant], i);
		if (shader_rid.is_null()) {
			ok = false;
			continue;
		}
		pipelines[p_variant][i].create_compute_pipeline(shader_rid);
		if (p_variant == VARIANT_HW && !pipelines[p_variant][i].get_rid().is_valid()) {
			ok = false;
		}
	}
	if (!ok) {
		for (int i = 0; i < PASS_MAX; i++) {
			pipelines[p_variant][i].free();
		}
		shaders[p_variant].version_free(versions[p_variant]);
		versions[p_variant] = RID();
	}
	valid[p_variant] = ok;
	return ok;
}

bool RestirGiCore::is_variant_valid(Variant p_variant) {
	return _ensure(p_variant);
}

void RestirGiCore::free() {
	RID *dummies[] = { &dummy_ubo, &dummy_probe, &dummy_float };
	for (RID *rid : dummies) {
		if (rid->is_valid() && RD::get_singleton()) {
			RD::get_singleton()->free_rid(*rid);
		}
		*rid = RID();
	}
	for (int v = 0; v < VARIANT_MAX; v++) {
		if (!initialized[v]) {
			continue;
		}
		for (int i = 0; i < PASS_MAX; i++) {
			pipelines[v][i].free();
		}
		if (versions[v].is_valid()) {
			shaders[v].version_free(versions[v]);
			versions[v] = RID();
		}
		initialized[v] = false;
		valid[v] = false;
	}
}

void RestirGiCore::_upload(RID p_ubo, const RestirFrameParams &p, uint32_t p_pass_index) {
	RestirFrameUBO u = {};
	for (int i = 0; i < 3; i++) {
		const Vector3 c = p.camera_basis.get_column(i);
		const Vector3 pc = p.previous_camera_basis.get_column(i);
		u.cam_rot[i][0] = float(c.x);
		u.cam_rot[i][1] = float(c.y);
		u.cam_rot[i][2] = float(c.z);
		u.prev_rot[i][0] = float(pc.x);
		u.prev_rot[i][1] = float(pc.y);
		u.prev_rot[i][2] = float(pc.z);
	}
	u.cam_to_anchor[0] = float(p.camera_to_anchor.x);
	u.cam_to_anchor[1] = float(p.camera_to_anchor.y);
	u.cam_to_anchor[2] = float(p.camera_to_anchor.z);
	u.prev_to_anchor[0] = float(p.previous_camera_to_anchor.x);
	u.prev_to_anchor[1] = float(p.previous_camera_to_anchor.y);
	u.prev_to_anchor[2] = float(p.previous_camera_to_anchor.z);
	MaterialStorage::store_camera(p.view_to_previous_clip, u.view_to_prev_clip);
	u.test_sky[0] = p.test_sky.r;
	u.test_sky[1] = p.test_sky.g;
	u.test_sky[2] = p.test_sky.b;
	u.test_sky[3] = p.test_sky.a;
	u.trace_size[0] = p.trace_size.x;
	u.trace_size[1] = p.trace_size.y;
	u.frame = p.frame_index;
	u.seed = p.seed;
	u.flags = p.flags | (p.mode == RESTIR_MODE_U1 ? RESTIR_F_UNBIASED : 0u) | (p.mode == RESTIR_MODE_B2 ? RESTIR_F_VIS_REUSE : 0u);
	if (p.compat) {
		u.flags |= RESTIR_F_COMPAT | (p.compat_gate == RESTIR_GATE_NONE ? RESTIR_F_COMPAT_NONE : 0u) | (p.compat_gate == RESTIR_GATE_BINARY ? RESTIR_F_COMPAT_BINARY : 0u) | (p.compat_early_stop ? RESTIR_F_COMPAT_STOP : 0u);
	}
	u.compat_k = CLAMP(p.compat_k, 1u, 64u);
	u.reval_cut_m = p.reval_cut_m;
	u.reval_share_denom = p.reval_share_denom;
	u.reval_threshold = p.reval_threshold;
	u.spatial_count = p.spatial_count;
	u.spatial_radius = p.spatial_radius;
	u.pass_index = p_pass_index;
	u.screen_size[0] = p.screen_size.x;
	u.screen_size[1] = p.screen_size.y;
	u.trace_scale = MAX(1, p.trace_scale);
	u.m_temporal_cap = p.m_temporal_cap;
	u.m_max = p.m_max;
	u.n_target = p.n_target;
	u.epoch_tag = p.epoch_tag;
	u.debug_view = p.debug_view;
	u.mode_code = p.mode | ((p.flags & RESTIR_F_FRESH_LO) ? 4u : 0u);
	u.vis_fail_limit = p.vis_fail_limit;
	u.tol_plane = p.tol_plane;
	u.cos_normal = p.cos_normal;
	u.jac_max = p.jac_max;
	u.d_min = p.d_min;
	u.cos_min = p.cos_min;
	u.w_clamp = p.w_clamp;
	u.origin_scale = p.origin_scale;
	u.origin_floor = p.origin_floor;
	u.sky_distance = p.sky_distance;
	u.class_tol = p.class_tol;
	u.emission_exposure = p.emission_exposure;
	u.prev_epoch_tag = p.prev_epoch_tag == 0xFFFFFFFFu ? p.epoch_tag : p.prev_epoch_tag;
	u.dirty_count = MIN(p.dirty_count, 64u);
	u.dirty_margin = p.dirty_margin;
	for (int i = 0; i < 3; i++) {
		u.shade_sky[i] = p.shade_sky[i];
	}
	u.shade_sky_flags = p.shade_sky_flags;
	u.shade_sky_border[0] = p.shade_sky_border[0];
	u.shade_sky_border[1] = p.shade_sky_border[1];
	u.shade_sky_energy = p.shade_sky_energy;
	u.shade_bounce_energy = p.shade_bounce_energy;
	u.shade_exposure = p.shade_exposure;
	RD::get_singleton()->buffer_update(p_ubo, 0, sizeof(RestirFrameUBO), &u);
}

void RestirGiCore::_bind_shade(RID p_shader, RD::ComputeListID p_list, const RestirGiInputs &p_inputs) {
	RD *rd = RD::get_singleton();
	if (dummy_ubo.is_null()) {
		Vector<uint8_t> zeros;
		zeros.resize(512);
		zeros.fill(0);
		dummy_ubo = rd->uniform_buffer_create(512, zeros);
		dummy_probe = _create_texture(RD::DATA_FORMAT_R32G32B32A32_UINT, Size2i(1, 1), RD::TEXTURE_TYPE_2D_ARRAY, 1);
		dummy_float = _create_texture(RD::DATA_FORMAT_R16G16B16A16_SFLOAT, Size2i(1, 1), RD::TEXTURE_TYPE_2D_ARRAY, 1);
	}
	const bool real = p_inputs.ddgi_ubo.is_valid() && p_inputs.ddgi_probe_data.is_valid() && p_inputs.ddgi_irradiance.is_valid() && p_inputs.ddgi_distance.is_valid();
	RID sky = p_inputs.sky_texture;
	if (sky.is_null()) {
		sky = TextureStorage::get_singleton()->texture_rd_get_default(octmap_array ? TextureStorage::DEFAULT_RD_TEXTURE_2D_ARRAY_WHITE : TextureStorage::DEFAULT_RD_TEXTURE_WHITE);
	}
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	LocalVector<RD::Uniform> set2;
	set2.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, real ? p_inputs.ddgi_ubo : dummy_ubo));
	set2.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, real ? p_inputs.ddgi_probe_data : dummy_probe));
	set2.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, real ? p_inputs.ddgi_irradiance : dummy_float));
	set2.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 5, real ? p_inputs.ddgi_distance : dummy_float));
	set2.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 6, material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)));
	set2.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, sky));
	set2.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 8, material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED)));
	rd->compute_list_bind_uniform_set(p_list, UniformSetCacheRD::get_singleton()->get_cache_vec(p_shader, 2, set2), 2);
}

void RestirGiCore::_dispatch(Variant p_variant, PassMode p_mode, RID p_ubo, const LocalVector<RD::Uniform> &p_set0, const RestirGiInputs &p_inputs, const Size2i &p_size) {
	UniformSetCacheRD *cache = UniformSetCacheRD::get_singleton();
	RID shader_rid = shaders[p_variant].version_get_shader(versions[p_variant], p_mode);
	LocalVector<RD::Uniform> set0;
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_ubo));
	for (const RD::Uniform &uniform : p_set0) {
		set0.push_back(uniform);
	}
	RD *rd = RD::get_singleton();
	static const char *const marker_names[PASS_MAX] = { "ReSTIR Clear", "ReSTIR Temporal", "ReSTIR Spatial", "ReSTIR Final", "ReSTIR Source", "ReSTIR Capture", "ReSTIR Spatial", "ReSTIR Pack", "ReSTIR Spatial", "ReSTIR Spatial", "ReSTIR Validate" };
	RENDER_TIMESTAMP(marker_names[p_mode]);
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipelines[p_variant][p_mode].get_rid());
	rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 0, set0), 0);
	if (p_variant != VARIANT_NONE) {
		LocalVector<RD::Uniform> set1;
		p_inputs.ray_scene->append_uniforms(set1);
		rd->compute_list_bind_uniform_set(list, cache->get_cache_vec(shader_rid, 1, set1), 1);
		if (p_mode == PASS_FINAL || p_mode == PASS_SOURCE || p_mode == PASS_VALIDATE) {
			_bind_shade(shader_rid, list, p_inputs);
		}
	}
	rd->compute_list_dispatch(list, uint32_t(Math::division_round_up(p_size.x, 8)), uint32_t(Math::division_round_up(p_size.y, 8)), 1);
	rd->compute_list_end();
}

static RestirGiCore::Variant _variant_for(const RestirGiInputs &p_inputs) {
	if (!p_inputs.use_rays || !p_inputs.ray_scene) {
		return RestirGiCore::VARIANT_NONE;
	}
	return p_inputs.ray_scene->uses_hardware() ? RestirGiCore::VARIANT_HW : RestirGiCore::VARIANT_SW;
}

void RestirGiCore::clear_history(RestirGiResources &r_resources, const RestirGiInputs &p_inputs) {
	Variant variant = VARIANT_NONE;
	if (!r_resources.allocated || !_ensure(variant)) {
		return;
	}
	RestirFrameParams params;
	params.trace_size = r_resources.size;
	_upload(r_resources.ubo[5], params, 0);
	for (int i = 0; i < 3; i++) {
		LocalVector<RD::Uniform> set0;
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, r_resources.reservoir[i]));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, r_resources.vis_state));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, r_resources.info));
		_dispatch(variant, PASS_CLEAR, r_resources.ubo[5], set0, p_inputs, r_resources.size);
	}
	r_resources.history_index = 0;
	r_resources.final_index = 0;
}

bool RestirGiCore::run_capture(const RestirGiInputs &p_inputs, const Size2i &p_size, RID r_out[6]) {
	Variant variant = VARIANT_NONE;
	if (!_ensure(variant)) {
		return false;
	}
	const RD::DataFormat formats[6] = { RD::DATA_FORMAT_R32G32B32A32_SFLOAT, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, RD::DATA_FORMAT_R32G32B32A32_SFLOAT, RD::DATA_FORMAT_R32G32B32A32_UINT, RD::DATA_FORMAT_R32G32B32A32_UINT };
	for (int i = 0; i < 6; i++) {
		r_out[i] = _create_texture(formats[i], p_size, RD::TEXTURE_TYPE_2D, 1);
	}
	RID ubo = RD::get_singleton()->uniform_buffer_create(sizeof(RestirFrameUBO));
	RestirFrameParams params;
	params.trace_size = p_size;
	_upload(ubo, params, 0);
	LocalVector<RD::Uniform> set0;
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_inputs.geo_position));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_inputs.geo_normal));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, p_inputs.previous_geo_position));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, p_inputs.previous_geo_normal));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 5, p_inputs.candidate_first));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, p_inputs.candidate_second));
	for (int i = 0; i < 6; i++) {
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17 + i, r_out[i]));
	}
	_dispatch(variant, PASS_CAPTURE, ubo, set0, p_inputs, p_size);
	RD::get_singleton()->free_rid(ubo);
	return true;
}

void RestirGiCore::run_source(RestirGiResources &r_resources, const RestirFrameParams &p_params, const RestirGiInputs &p_inputs) {
	Variant variant = _variant_for(p_inputs);
	if (variant == VARIANT_NONE || !_ensure(variant)) {
		return;
	}
	_upload(r_resources.ubo[4], p_params, 0);
	LocalVector<RD::Uniform> set0;
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_inputs.geo_position));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_inputs.geo_normal));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 15, p_inputs.candidate_first));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, p_inputs.candidate_second));
	_dispatch(variant, PASS_SOURCE, r_resources.ubo[4], set0, p_inputs, p_params.trace_size);
}

void RestirGiCore::run_frame(RestirGiResources &r_resources, const RestirFrameParams &p_params, const RestirGiInputs &p_inputs) {
	Variant variant = _variant_for(p_inputs);
	if (!r_resources.allocated || !_ensure(variant)) {
		return;
	}
	RestirFrameParams params = p_params;
	params.trace_size = r_resources.size;
	if (variant != VARIANT_NONE) {
		params.flags |= RESTIR_F_VIS;
	}
	if (r_resources.debug.is_valid()) {
		params.flags |= RESTIR_F_DEBUG_OUT;
	}
	if (!p_inputs.ddgi_ubo.is_valid()) {
		params.flags &= ~(RESTIR_F_REAL_SKY | RESTIR_F_SHADE_DDGI);
	}
	if (params.compat && (params.flags & RESTIR_F_PROBE_STATS) != 0u && r_resources.sel_image.is_null()) {
		r_resources.sel_image = _create_texture(RD::DATA_FORMAT_R32G32_UINT, r_resources.size, RD::TEXTURE_TYPE_2D_ARRAY, 2);
	}
	const uint32_t hist = r_resources.history_index;
	RD::get_singleton()->buffer_clear(r_resources.pass_stats, 0, r_resources.group_count * PASS_STATS_ROWS * PASS_STATS_STRIDE * sizeof(float));

	_upload(r_resources.ubo[0], params, 0);
	{
		RestirDirtyUBO dirty = {};
		for (uint32_t i = 0; i < MIN(params.dirty_count, 64u); i++) {
			dirty.box_min[i][0] = float(params.dirty_min[i].x);
			dirty.box_min[i][1] = float(params.dirty_min[i].y);
			dirty.box_min[i][2] = float(params.dirty_min[i].z);
			dirty.box_max[i][0] = float(params.dirty_max[i].x);
			dirty.box_max[i][1] = float(params.dirty_max[i].y);
			dirty.box_max[i][2] = float(params.dirty_max[i].z);
		}
		RD::get_singleton()->buffer_update(r_resources.dirty_ubo, 0, sizeof(RestirDirtyUBO), &dirty);
	}
	if (params.reval_pass && variant != VARIANT_NONE && (params.flags & RESTIR_F_HISTORY) != 0u) {
		LocalVector<RD::Uniform> set0;
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, p_inputs.previous_geo_position));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, p_inputs.previous_geo_normal));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, p_inputs.candidate_second));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, r_resources.reservoir[hist]));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, r_resources.pass_stats));
		_dispatch(variant, PASS_VALIDATE, r_resources.ubo[0], set0, p_inputs, r_resources.size);
	}
	{
		LocalVector<RD::Uniform> set0;
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_inputs.geo_position));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_inputs.geo_normal));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, p_inputs.previous_geo_position));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, p_inputs.previous_geo_normal));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 5, p_inputs.candidate_first));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, p_inputs.candidate_second));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, r_resources.reservoir[hist]));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, r_resources.reservoir[hist ^ 1u]));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, r_resources.vis_state));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, r_resources.info));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, r_resources.pass_stats));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 24, r_resources.dirty_ubo));
		_dispatch(variant, PASS_TEMPORAL, r_resources.ubo[0], set0, p_inputs, r_resources.size);
	}

	const uint32_t passes = params.spatial_count > 0 ? CLAMP(params.spatial_passes, 1u, 2u) : 0u;
	for (uint32_t s = 0; s < passes; s++) {
		_upload(r_resources.ubo[1 + s], params, 1 + s);
		LocalVector<RD::Uniform> set0;
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_inputs.geo_position));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_inputs.geo_normal));
		const uint32_t read_set = params.spatial_feedback ? ((hist ^ 1u) ^ (s & 1u)) : (s == 0u ? (hist ^ 1u) : 2u);
		const uint32_t write_set = params.spatial_feedback ? (hist ^ (s & 1u)) : (s == 0u ? 2u : hist);
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, r_resources.reservoir[read_set]));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, r_resources.reservoir[write_set]));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, r_resources.info));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 23, r_resources.pass_stats));
		if (params.compat) {
			set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 25, (params.flags & RESTIR_F_PROBE_STATS) != 0u ? r_resources.sel_image : r_resources.sel_dummy));
		}
		const bool wide = params.spatial_count > RESTIR_SPATIAL_SMALL_COUNT;
		const PassMode spatial_pass = params.compat ? (wide ? PASS_SPATIAL_COMPAT_WIDE : PASS_SPATIAL_COMPAT) : (wide ? PASS_SPATIAL_WIDE : PASS_SPATIAL);
		_dispatch(variant, spatial_pass, r_resources.ubo[1 + s], set0, p_inputs, r_resources.size);
	}
	const uint32_t final_index = params.spatial_feedback ? ((hist ^ 1u) ^ (passes & 1u)) : (passes == 0u ? (hist ^ 1u) : (passes == 1u ? 2u : hist));

	_upload(r_resources.ubo[3], params, 3);
	{
		LocalVector<RD::Uniform> set0;
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_inputs.geo_position));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, p_inputs.geo_normal));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 5, p_inputs.candidate_first));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, p_inputs.candidate_second));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, r_resources.reservoir[final_index]));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, r_resources.vis_state));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, r_resources.info));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, r_resources.radiance));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 12, r_resources.coverage));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, r_resources.debug.is_valid() ? r_resources.debug : r_resources.debug_dummy));
		set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 14, r_resources.stats));
		_dispatch(variant, PASS_FINAL, r_resources.ubo[3], set0, p_inputs, r_resources.size);
	}
	r_resources.final_index = final_index;
	r_resources.history_index = params.spatial_feedback ? final_index : (hist ^ 1u);
}

void RestirGiCore::run_pack(RestirGiResources &r_resources, const RestirGiInputs &p_inputs) {
	Variant variant = VARIANT_NONE;
	if (!r_resources.allocated || !_ensure(variant)) {
		return;
	}
	LocalVector<RD::Uniform> set0;
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_inputs.geo_position));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, r_resources.info));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 11, r_resources.radiance));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 12, r_resources.coverage));
	set0.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, p_inputs.candidate_second));
	_dispatch(variant, PASS_PACK, r_resources.ubo[3], set0, p_inputs, r_resources.size);
}

RestirFrameStats RestirGiCore::read_stats(const RestirGiResources &p_resources) {
	RestirFrameStats stats;
	if (!p_resources.allocated) {
		return stats;
	}
	RD *rd = RD::get_singleton();
	const Vector<uint8_t> raw = rd->buffer_get_data(p_resources.stats, 0, p_resources.group_count * 16 * sizeof(float));
	const float *values = reinterpret_cast<const float *>(raw.ptr());
	stats.groups = p_resources.group_count;
	if (size_t(raw.size()) >= p_resources.group_count * 16 * sizeof(float)) {
		for (uint32_t g = 0; g < p_resources.group_count; g++) {
			const float *v = values + g * 16;
			stats.geometry += v[0];
			stats.output_pixels += v[1];
			stats.output_sum[0] += v[2];
			stats.output_sum[1] += v[3];
			stats.output_sum[2] += v[4];
			stats.candidate_pixels += v[5];
			stats.candidate_sum[0] += v[6];
			stats.candidate_sum[1] += v[7];
			stats.candidate_sum[2] += v[8];
			stats.m_sum += v[9];
			stats.clamped += v[10];
			stats.blocked += v[11];
			stats.temporal_selected += v[12];
			stats.spatial_selected += v[13];
			stats.violations += v[14];
			stats.has_sample += v[15];
		}
	}
	const size_t pass_bytes = p_resources.group_count * PASS_STATS_ROWS * PASS_STATS_STRIDE * sizeof(float);
	const Vector<uint8_t> pass_raw = rd->buffer_get_data(p_resources.pass_stats, 0, pass_bytes);
	if (size_t(pass_raw.size()) >= pass_bytes) {
		const float *pv = reinterpret_cast<const float *>(pass_raw.ptr());
		for (uint32_t i = 0; i < p_resources.group_count * 3; i++) {
			const float *v = pv + i * PASS_STATS_STRIDE;
			stats.shift_tested += v[0];
			stats.rej_distance += v[1];
			stats.rej_cos += v[2];
			stats.rej_jacobian += v[3];
			stats.energy_all += v[4];
			stats.energy_truncated += v[5];
			stats.pdf_refused += v[6];
			stats.techniques_present += v[7];
			stats.techniques_sample_unusable += v[8];
			stats.spatial_combines += v[9];
			stats.lo_refused += v[10];
			stats.no_canonical += v[11];
			stats.compat_taps += v[12];
			stats.compat_early_stops += v[13];
			stats.compat_empty += v[14];
			stats.compat_h_sum += v[15];
			if (i < p_resources.group_count) {
				stats.temporal_shift_tested += v[0];
				stats.temporal_shift_rejected += v[1] + v[2] + v[3];
			}
		}
		for (uint32_t i = p_resources.group_count * 3; i < p_resources.group_count * PASS_STATS_ROWS; i++) {
			const float *v = pv + i * PASS_STATS_STRIDE;
			stats.rehit_miss += v[16];
			stats.rehit_mismatch += v[17];
		}
	}
	const Vector<uint8_t> info = rd->texture_get_data(p_resources.info, 0);
	const uint32_t *words = reinterpret_cast<const uint32_t *>(info.ptr());
	const uint32_t count = uint32_t(info.size() / sizeof(uint32_t));
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t w = words[i];
		if ((w & (1u << 14u)) == 0u) {
			continue;
		}
		stats.temporal_reject[w & 15u]++;
		stats.spatial_reject[(w >> 4u) & 15u]++;
		if (w & (1u << 13u)) {
			stats.had_history++;
		}
	}
	return stats;
}

Vector<uint8_t> RestirGiCore::read_selection(const RestirGiResources &p_resources) {
	if (!p_resources.allocated || p_resources.sel_image.is_null()) {
		return Vector<uint8_t>();
	}
	RD *rd = RD::get_singleton();
	Vector<uint8_t> data;
	for (uint32_t layer = 0; layer < 2; layer++) {
		data.append_array(rd->texture_get_data(p_resources.sel_image, layer));
	}
	return data;
}
