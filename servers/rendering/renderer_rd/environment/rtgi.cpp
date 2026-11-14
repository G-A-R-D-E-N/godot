/**************************************************************************/
/*  rtgi.cpp                                                              */
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

#include "rtgi.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/renderer_rd/environment/gi_compositor.h"
#include "servers/rendering/renderer_rd/environment/ray_scene.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/storage/utilities.h"

using namespace RendererRD;

namespace {

#define RB_RTGI_CANDIDATE_FIRST SNAME("candidate_first")
#define RB_RTGI_CANDIDATE_SECOND SNAME("candidate_second")
#define RB_RTGI_RAY SNAME("ray")
#define RB_RTGI_TILES SNAME("tiles")
#define RB_RTGI_DEBUG SNAME("debug")
#define RB_RTGI_PAIR(m_name, m_index) ((m_index) ? SNAME(m_name "_1") : SNAME(m_name "_0"))
#define RB_RTGI_GEO_POSITION(m_index) RB_RTGI_PAIR("geo_position", m_index)
#define RB_RTGI_GEO_NORMAL(m_index) RB_RTGI_PAIR("geo_normal", m_index)
#define RB_RTGI_HISTORY(m_index) RB_RTGI_PAIR("history", m_index)
#define RB_RTGI_MOMENTS(m_index) RB_RTGI_PAIR("moments", m_index)
#define RB_RTGI_FILTER(m_index) RB_RTGI_PAIR("filter", m_index)

constexpr uint32_t RTGI_COUNTERS_BINDING = 15;
constexpr uint32_t MAX_ATROUS = 6;
constexpr double CAMERA_CUT_DISTANCE = 25.0;

} // namespace

RtgiPass *RtgiPass::singleton = nullptr;

RtgiPass::Settings RtgiPass::get_project_settings() {
	Settings settings;
	settings.enabled = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/rtgi/enabled");
	settings.rays_per_pixel = CLAMP(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/rays_per_pixel")), 0.0f, 4.0f);
	settings.resolution_scale = CLAMP(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/resolution_scale")), 0.25f, 1.0f);
	settings.target_samples = CLAMP(int(GLOBAL_GET_CACHED(int, "rendering/global_illumination/rtgi/target_samples")), 1, 255);
	settings.history_cap = CLAMP(int(GLOBAL_GET_CACHED(int, "rendering/global_illumination/rtgi/history_cap")), 1, 255);
	settings.atrous_iterations = CLAMP(int(GLOBAL_GET_CACHED(int, "rendering/global_illumination/rtgi/atrous_iterations")), 0, int(MAX_ATROUS));
	settings.normal_sigma = CLAMP(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/normal_sigma")), 1.0f, 512.0f);
	settings.depth_sigma = CLAMP(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/depth_sigma")), 0.05f, 16.0f);
	settings.variance_sigma = CLAMP(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/variance_sigma")), 0.1f, 64.0f);
	settings.ddgi_alpha_min = CLAMP(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/ddgi_alpha_min")), 0.0f, 1.0f);
	settings.ssgi_min_frames = CLAMP(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/ssgi_min_frames")), 0.0f, 255.0f);
	settings.check_period = CLAMP(int(GLOBAL_GET_CACHED(int, "rendering/global_illumination/rtgi/check_period")), 0, 1024);
	settings.force_trace = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/rtgi/force_trace");
	settings.seed = uint32_t(int(GLOBAL_GET_CACHED(int, "rendering/global_illumination/rtgi/seed")));
	settings.origin_scale = MAX(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/origin_scale")), 0.0f);
	settings.origin_floor = MAX(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/origin_floor")), 0.0f);
	settings.soft_cut_count = CLAMP(int(GLOBAL_GET_CACHED(int, "rendering/global_illumination/rtgi/soft_cut_count")), 0, 255);
	settings.luma_dead_zone = CLAMP(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/luma_dead_zone")), 0.0f, 16.0f);
	settings.mirror_below_surface = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/rtgi/mirror_below_surface");
	settings.restart_spheres = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/rtgi/restart_spheres");
	settings.restart_screen_wide = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/rtgi/restart_screen_wide");
	settings.restart_quiet_frames = CLAMP(int(GLOBAL_GET_CACHED(int, "rendering/global_illumination/rtgi/restart_quiet_frames")), 0, 1024);
	settings.directional_jump_degrees = CLAMP(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/directional_jump_degrees")), 0.1f, 180.0f);
	settings.directional_jump_floor = MAX(float(GLOBAL_GET_CACHED(float, "rendering/global_illumination/rtgi/directional_jump_floor")), 0.0f);
	return settings;
}

bool RtgiPass::is_enabled() {
	return GLOBAL_GET_CACHED(bool, "rendering/global_illumination/rtgi/enabled");
}

RtgiPass::RtgiPass() {
	Vector<String> modes;
	modes.push_back("\n#define MODE_CLASSIFY\n");
	modes.push_back("\n#define MODE_TEMPORAL\n");
	modes.push_back("\n#define MODE_VARIANCE\n");
	modes.push_back("\n#define MODE_ATROUS\n");
	modes.push_back("\n#define MODE_COMPOSE\n");
	modes.push_back("\n#define MODE_DEBUG\n");
	const bool counters = GLOBAL_GET("rendering/global_illumination/rtgi/gpu_counters");
	shader.initialize(modes, counters ? "\n#define RTGI_COUNTERS\n" : "");
	shader_version = shader.version_create();
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].create_compute_pipeline(shader.version_get_shader(shader_version, i));
	}
	trace_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(TraceUBO));
	frame_ubo = RD::get_singleton()->uniform_buffer_create(sizeof(FrameUBO));
	for (RID &rid : atrous_ubo) {
		rid = RD::get_singleton()->uniform_buffer_create(sizeof(FrameUBO));
	}
	if (counters) {
		counter_buffer = RD::get_singleton()->storage_buffer_create(sizeof(uint32_t) * COUNTER_MAX);
	}
}

RtgiPass::~RtgiPass() {
	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i].free();
	}
	shader.version_free(shader_version);
	RD::get_singleton()->free_rid(trace_ubo);
	RD::get_singleton()->free_rid(frame_ubo);
	for (RID &rid : atrous_ubo) {
		RD::get_singleton()->free_rid(rid);
	}
	if (counter_buffer.is_valid()) {
		RD::get_singleton()->free_rid(counter_buffer);
	}
}

void RtgiPass::finalize() {
	if (singleton != nullptr) {
		memdelete(singleton);
		singleton = nullptr;
	}
}

void RtgiPass::_refuse(const Ref<RtgiBuffers> &p_buffers, const String &p_reason) {
	if (p_buffers->refused != p_reason) {
		print_line(vformat("RTGI refused: %s", p_reason));
	}
	p_buffers->refused = p_reason;
}

void RtgiPass::_ensure_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Ref<RtgiBuffers> &p_buffers, const FrameInputs &p_inputs, bool p_debug) {
	const uint32_t storage_sampling = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	const uint32_t clearable = storage_sampling | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	const Size2i trace = p_inputs.trace_size;
	const Size2i tiles = p_buffers->tile_count;
	auto create = [&](const StringName &p_name, RD::DataFormat p_format, uint32_t p_usage, const Size2i &p_size, bool p_clear) {
		if (p_render_buffers->has_texture(RB_SCOPE_RTGI, p_name)) {
			return;
		}
		RID rid = p_render_buffers->create_texture(RB_SCOPE_RTGI, p_name, p_format, p_usage, RD::TEXTURE_SAMPLES_1, p_size);
		if (p_clear && rid.is_valid()) {
			RD::get_singleton()->texture_clear(rid, Color(0, 0, 0, 0), 0, 1, 0, 1);
		}
	};
	create(RB_RTGI_CANDIDATE_FIRST, RD::DATA_FORMAT_R32G32B32A32_UINT, clearable, trace, true);
	create(RB_RTGI_CANDIDATE_SECOND, RD::DATA_FORMAT_R32G32B32A32_UINT, clearable, trace, true);
	create(RB_RTGI_RAY, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, storage_sampling, trace, false);
	create(RB_RTGI_TILES, RD::DATA_FORMAT_R32_UINT, storage_sampling, tiles, false);
	for (int i = 0; i < 2; i++) {
		create(RB_RTGI_GEO_POSITION(i), RD::DATA_FORMAT_R32G32B32A32_SFLOAT, clearable, trace, true);
		create(RB_RTGI_GEO_NORMAL(i), RD::DATA_FORMAT_R8G8B8A8_UNORM, clearable, trace, true);
		create(RB_RTGI_HISTORY(i), RD::DATA_FORMAT_R16G16B16A16_SFLOAT, clearable, trace, true);
		create(RB_RTGI_MOMENTS(i), RD::DATA_FORMAT_R16G16B16A16_SFLOAT, clearable, trace, true);
		create(RB_RTGI_FILTER(i), RD::DATA_FORMAT_R16G16B16A16_SFLOAT, clearable, trace, true);
	}
	if (p_debug) {
		create(RB_RTGI_DEBUG, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, storage_sampling | RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT, trace, false);
	}
}

void RtgiPass::_fill_frame_ubo(FrameUBO &r_ubo, const Settings &p_settings, const Ref<RtgiBuffers> &p_buffers, const FrameInputs &p_inputs, const Ref<DDGI::Volume> &p_volume, uint32_t p_flags, uint32_t p_debug_mode, int32_t p_atrous_step, int32_t p_slot_scale, const Transform3D &p_delta, const Projection &p_previous_projection) const {
	Projection correction;
	correction.set_depth_correction(true);
	const Projection inverse_projection = (correction * p_inputs.projection).inverse();
	const Projection view_to_previous_clip = (correction * p_previous_projection) * Projection(p_delta);

	r_ubo = {};
	MaterialStorage::store_camera(inverse_projection, r_ubo.inv_projection);
	MaterialStorage::store_camera(view_to_previous_clip, r_ubo.view_to_prev_clip);
	MaterialStorage::store_transform(p_delta, r_ubo.cur_to_prev_view);
	MaterialStorage::store_transform(p_inputs.camera_transform, r_ubo.cam_transform);
	r_ubo.screen_size[0] = p_inputs.screen_size.x;
	r_ubo.screen_size[1] = p_inputs.screen_size.y;
	r_ubo.trace_size[0] = p_inputs.trace_size.x;
	r_ubo.trace_size[1] = p_inputs.trace_size.y;
	r_ubo.slot_size[0] = p_inputs.slot_size.x;
	r_ubo.slot_size[1] = p_inputs.slot_size.y;
	r_ubo.tile_count[0] = p_buffers->tile_count.x;
	r_ubo.tile_count[1] = p_buffers->tile_count.y;
	r_ubo.frame = p_inputs.frame_index;
	r_ubo.seed = p_settings.seed;
	r_ubo.scale = MAX(1, int32_t(Math::round(1.0f / p_settings.resolution_scale)));
	r_ubo.flags = p_flags;
	r_ubo.target_samples = float(p_settings.target_samples);
	r_ubo.history_cap = float(p_settings.history_cap);
	r_ubo.normal_sigma = p_settings.normal_sigma;
	r_ubo.depth_sigma = p_settings.depth_sigma;
	r_ubo.variance_sigma = p_settings.variance_sigma;
	r_ubo.ddgi_alpha_min = p_settings.ddgi_alpha_min;
	r_ubo.ssgi_min_frames = p_settings.ssgi_min_frames;
	r_ubo.check_period = p_settings.check_period;
	r_ubo.force_trace = p_settings.force_trace ? 1u : 0u;
	r_ubo.debug_mode = p_debug_mode;
	r_ubo.energy = p_volume.is_valid() ? p_volume->settings.energy : 1.0f;
	r_ubo.atrous_step = p_atrous_step;
	r_ubo.slot_scale = p_slot_scale;
	r_ubo.soft_cut_count = p_settings.soft_cut_count;
	r_ubo.luma_dead_zone = p_settings.luma_dead_zone;
}

void RtgiPass::_dispatch(Mode p_mode, RID p_ubo, const LocalVector<RD::Uniform> &p_uniforms, const LocalVector<RD::Uniform> *p_ddgi_uniforms, uint32_t p_groups_x, uint32_t p_groups_y, bool p_counters) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL(uniform_set_cache);
	RID shader_rid = shader.version_get_shader(shader_version, p_mode);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, p_ubo));
	for (const RD::Uniform &uniform : p_uniforms) {
		uniforms.push_back(uniform);
	}
	if (p_counters && counter_buffer.is_valid()) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, RTGI_COUNTERS_BINDING, counter_buffer));
	}
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[p_mode].get_rid());
	RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rid, 0, uniforms), 0);
	if (p_ddgi_uniforms) {
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache_vec(shader_rid, 1, *p_ddgi_uniforms), 1);
	}
	RD::get_singleton()->compute_list_dispatch(compute_list, p_groups_x, p_groups_y, 1);
	RD::get_singleton()->compute_list_end();
}

bool RtgiPass::process(const Ref<RenderSceneBuffersRD> &p_render_buffers, DDGI &p_ddgi, const FrameInputs &p_inputs) {
	ERR_FAIL_COND_V(p_render_buffers.is_null(), false);
	if (singleton == nullptr) {
		singleton = memnew(RtgiPass);
	}

	Ref<RtgiBuffers> buffers;
	if (p_render_buffers->has_custom_data(RB_SCOPE_RTGI)) {
		buffers = p_render_buffers->get_custom_data(RB_SCOPE_RTGI);
	} else {
		buffers.instantiate();
		p_render_buffers->set_custom_data(RB_SCOPE_RTGI, buffers);
	}

	const bool ran = singleton->_process(p_render_buffers, buffers, p_ddgi, p_inputs);
	const String status_path = GLOBAL_GET("rendering/global_illumination/rtgi/debug_status_path");
	if (!status_path.is_empty()) {
		singleton->_write_status(p_render_buffers, buffers, status_path);
	}
	return ran;
}

void RtgiPass::_read_light_changes(const Ref<RtgiBuffers> &p_buffers, const Settings &p_settings, const FrameInputs &p_inputs, RtgiRestarts::Selection &r_selection, RtgiRestarts::DirectionalRead &r_directional) {
	LocalVector<DDGILightTracker::Event> events;
	const uint64_t asked = p_buffers->have_previous ? p_buffers->event_serial : UINT64_MAX;
	const uint64_t serial = DDGILightTracker::events_since(asked, events);
	if (asked != UINT64_MAX) {
		p_buffers->events_lost += serial - asked - events.size();
	}
	p_buffers->event_serial = serial;

	const Vector3 origin = p_inputs.camera_transform.origin;
	const Vector3 region_half = RendererSceneRenderRD::get_singleton()->ray_scene_get_region(origin).size * 0.5;
	RtgiRestarts::select(events, p_inputs.camera_transform.affine_inverse(), origin, region_half, p_settings.restart_quiet_frames, r_selection);

	LocalVector<RtgiRestarts::DirectionalSample> samples;
	if (p_inputs.lights) {
		LightStorage *light_storage = LightStorage::get_singleton();
		for (uint64_t i = 0; i < p_inputs.lights->size(); i++) {
			const RID instance = (*p_inputs.lights)[i];
			if (!light_storage->owns_light_instance(instance)) {
				continue;
			}
			const RID light = light_storage->light_instance_get_base_light(instance);
			const RSE::LightType type = light_storage->light_get_type(light);
			if (type != RSE::LIGHT_DIRECTIONAL || light_storage->light_get_bake_mode(light) == RSE::LIGHT_BAKE_DISABLED) {
				continue;
			}
			RtgiRestarts::DirectionalSample sample;
			sample.light = instance;
			sample.direction = -light_storage->light_instance_get_base_transform(instance).basis.get_column(Vector3::AXIS_Z).normalized();
			const float energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY) * light_storage->light_get_param(light, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
			sample.energy = LightStorage::light_render_energy(energy, light_storage->light_get_param(light, RSE::LIGHT_PARAM_INTENSITY), type, p_inputs.physical_units);
			if (light_storage->light_is_negative(light)) {
				sample.energy = -sample.energy;
			}
			samples.push_back(sample);
		}
	}
	r_directional = RtgiRestarts::read_directionals(samples, p_buffers->directionals, RSG::rasterizer->get_frame_number(), p_settings.directional_jump_degrees, p_settings.directional_jump_floor, p_settings.restart_quiet_frames);

	GIEpochs *epochs = GIEpochs::get_singleton();
	const bool lamp_changed = serial > light_word_serial && r_selection.count + r_selection.dropped + r_selection.repeats + r_selection.behind > 0;
	if (lamp_changed || r_directional.jumps + r_directional.repeats > 0 || epochs->get(GIEpochs::SOURCE_LIGHT) == 0) {
		epochs->bump(GIEpochs::SOURCE_LIGHT);
	}
	light_word_serial = MAX(light_word_serial, serial);

	p_buffers->spheres_dropped += r_selection.dropped;
	p_buffers->spheres_behind += r_selection.behind;
	p_buffers->event_repeats += r_selection.repeats;
	p_buffers->region_crossings += r_selection.crossings;
	p_buffers->directional_jumps += r_directional.jumps;
	p_buffers->directional_repeats += r_directional.repeats;
}

bool RtgiPass::_process(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Ref<RtgiBuffers> &p_buffers, DDGI &p_ddgi, const FrameInputs &p_inputs) {
	const Settings settings = get_project_settings();
	RayScene *ray_scene = RayScene::get_singleton();
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	TextureStorage *texture_storage = TextureStorage::get_singleton();

	if (GLOBAL_GET_CACHED(bool, "rendering/global_illumination/compositor/debug_provider")) {
		_refuse(p_buffers, "compositor debug_provider is on");
		return false;
	}
	if (p_render_buffers->get_view_count() != 1) {
		_refuse(p_buffers, "multiview is not supported");
		return false;
	}
	Ref<DDGI::Volume> volume = p_render_buffers->get_custom_data(RB_SCOPE_DDGI);
	if (volume.is_null() || volume->ubo.is_null()) {
		_refuse(p_buffers, "DDGI volume is not ready");
		return false;
	}
	if (!ray_scene || !ray_scene->is_ready()) {
		_refuse(p_buffers, "ray scene is not ready");
		return false;
	}
	if (p_inputs.depth.is_null() || p_inputs.normal_roughness.is_null()) {
		_refuse(p_buffers, "no depth or normal buffer");
		return false;
	}

	FrameInputs inputs = p_inputs;
	inputs.screen_size = p_render_buffers->get_internal_size();
	const int32_t scale = MAX(1, int32_t(Math::round(1.0f / settings.resolution_scale)));
	inputs.trace_size = Size2i(MAX(1, Math::division_round_up(inputs.screen_size.x, scale)), MAX(1, Math::division_round_up(inputs.screen_size.y, scale)));
	const Size2i tile_count = Size2i(Math::division_round_up(inputs.trace_size.x, int32_t(TILE_SIZE)), Math::division_round_up(inputs.trace_size.y, int32_t(TILE_SIZE)));
	inputs.slot_size = GICompositor::ensure_slot_textures(p_render_buffers);
	if (inputs.slot_size.x <= 0 || inputs.slot_size.y <= 0) {
		return false;
	}
	const int32_t slot_scale = MAX(1, inputs.screen_size.x / MAX(1, inputs.slot_size.x));

	const bool resized = p_buffers->trace_size != inputs.trace_size || p_buffers->screen_size != inputs.screen_size || p_buffers->slot_size != inputs.slot_size;
	if (resized) {
		p_render_buffers->clear_context(RB_SCOPE_RTGI);
		p_buffers->have_previous = false;
		p_buffers->index = 0;
	}
	p_buffers->screen_size = inputs.screen_size;
	p_buffers->trace_size = inputs.trace_size;
	p_buffers->slot_size = inputs.slot_size;
	p_buffers->tile_count = tile_count;

	const DebugView debug_view = DebugView(CLAMP(int(GLOBAL_GET_CACHED(int, "rendering/global_illumination/rtgi/debug_view")), int(DEBUG_VIEW_NONE), int(DEBUG_VIEW_HISTORY_COUNT)));
	_ensure_textures(p_render_buffers, p_buffers, inputs, debug_view != DEBUG_VIEW_NONE);

	uint32_t cut = CUT_NONE;
	if (!p_buffers->have_previous) {
		cut = resized ? CUT_RESIZE : CUT_FIRST;
	} else if (inputs.history_cut) {
		cut = CUT_REQUEST;
	} else if (p_buffers->previous_hardware != inputs.hardware) {
		cut = CUT_BACKEND;
	} else if ((inputs.camera_transform.origin - p_buffers->previous_transform.origin).length() > CAMERA_CUT_DISTANCE) {
		cut = CUT_CAMERA;
	}
	const bool epoch_changed = p_buffers->have_epochs && (p_buffers->previous_epochs.scene != inputs.epochs.scene || p_buffers->previous_epochs.sky != inputs.epochs.sky);
	RtgiRestarts::Selection selection;
	RtgiRestarts::DirectionalRead directional;
	_read_light_changes(p_buffers, settings, inputs, selection, directional);

	uint32_t flags = 0;
	if (inputs.projection.is_orthogonal()) {
		flags |= FRAME_FLAG_ORTHOGONAL;
	}
	switch (cut) {
		case CUT_NONE:
			break;
		case CUT_RESIZE:
			flags |= FRAME_FLAG_RESIZE | FRAME_FLAG_HISTORY_CUT;
			break;
		case CUT_CAMERA:
			flags |= FRAME_FLAG_CAMERA_CUT | FRAME_FLAG_HISTORY_CUT;
			break;
		case CUT_BACKEND:
			flags |= FRAME_FLAG_BACKEND_SWITCH | FRAME_FLAG_HISTORY_CUT;
			break;
		default:
			flags |= FRAME_FLAG_HISTORY_CUT;
			break;
	}
	p_buffers->last_spheres = 0;
	if (cut == CUT_NONE) {
		if (epoch_changed || directional.jumps > 0 || (settings.restart_screen_wide && selection.count > 0)) {
			flags |= FRAME_FLAG_SOFT_CUT;
			p_buffers->soft_cuts++;
			p_buffers->frames_since_cut = MIN(p_buffers->frames_since_cut, settings.soft_cut_count);
		} else if (settings.restart_spheres && selection.count > 0) {
			flags |= FRAME_FLAG_RESTART_SPHERES;
			p_buffers->last_spheres = selection.count;
			p_buffers->sphere_frames++;
			p_buffers->spheres_total += selection.count;
		}
	}
	if (cut != CUT_NONE) {
		p_buffers->cut_counts[cut]++;
		p_buffers->last_cut = cut;
		p_buffers->frames_since_cut = 0;
	}
	const bool ssgi_texture = inputs.ssgi_length.is_valid();
	if (ssgi_texture) {
		flags |= FRAME_FLAG_SSGI_VALID_TEXTURE;
	}

	const Transform3D previous_transform = p_buffers->have_previous ? p_buffers->previous_transform : inputs.camera_transform;
	const Projection previous_projection = p_buffers->have_previous ? p_buffers->previous_projection : inputs.projection;
	const Transform3D delta = previous_transform.affine_inverse() * inputs.camera_transform;

	const uint32_t current = p_buffers->index;
	const uint32_t previous = current ^ 1u;
	const uint32_t atrous_count = settings.atrous_iterations;

	FrameUBO frame_data;
	_fill_frame_ubo(frame_data, settings, p_buffers, inputs, volume, flags, DEBUG_VIEW_NONE, 1, slot_scale, delta, previous_projection);
	if ((flags & FRAME_FLAG_RESTART_SPHERES) != 0) {
		frame_data.restart_count = selection.count;
		memcpy(frame_data.restart_spheres, selection.spheres, sizeof(selection.spheres));
	}
	RD::get_singleton()->buffer_update(frame_ubo, 0, sizeof(FrameUBO), &frame_data);
	for (uint32_t i = 0; i < atrous_count; i++) {
		FrameUBO step_data = frame_data;
		step_data.atrous_step = 1 << i;
		RD::get_singleton()->buffer_update(atrous_ubo[i], 0, sizeof(FrameUBO), &step_data);
	}

	TraceUBO trace_data = {};
	MaterialStorage::store_camera(inputs.drawn_projection.inverse(), trace_data.inv_projection);
	memcpy(trace_data.cam_transform, frame_data.cam_transform, sizeof(trace_data.cam_transform));
	trace_data.screen_size[0] = inputs.screen_size.x;
	trace_data.screen_size[1] = inputs.screen_size.y;
	trace_data.trace_size[0] = inputs.trace_size.x;
	trace_data.trace_size[1] = inputs.trace_size.y;
	trace_data.frame = inputs.frame_index;
	trace_data.seed = settings.seed;
	trace_data.scale = scale;
	trace_data.flags = flags;
	trace_data.origin_scale = settings.origin_scale;
	trace_data.origin_floor = settings.origin_floor;
	trace_data.mirror_below_surface = settings.mirror_below_surface ? 1u : 0u;
	const float full_pixels = float(inputs.screen_size.x) * float(inputs.screen_size.y);
	const float trace_pixels = float(inputs.trace_size.x) * float(inputs.trace_size.y);
	const float ray_fraction = trace_pixels > 0.0f ? CLAMP(settings.rays_per_pixel * full_pixels / trace_pixels, 0.0f, 1.0f) : 0.0f;
	trace_data.ray_fraction = ray_fraction;
	RD::get_singleton()->buffer_update(trace_ubo, 0, sizeof(TraceUBO), &trace_data);
	p_buffers->last_ray_fraction = ray_fraction;
	p_buffers->last_trace_pixel_capacity = uint32_t(Math::floor(settings.rays_per_pixel * full_pixels));
	p_buffers->last_atrous_iterations = atrous_count;

	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID ssgi_texture_rid = ssgi_texture ? inputs.ssgi_length : texture_storage->texture_rd_get_default(TextureStorage::DEFAULT_RD_TEXTURE_WHITE);

	const RID tiles = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_TILES);
	const RID candidate_first = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_CANDIDATE_FIRST);
	const RID candidate_second = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_CANDIDATE_SECOND);
	const RID ray_texture = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_RAY);
	const RID geo_position_current = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_GEO_POSITION(current));
	const RID geo_normal_current = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_GEO_NORMAL(current));
	const RID geo_position_previous = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_GEO_POSITION(previous));
	const RID geo_normal_previous = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_GEO_NORMAL(previous));
	const RID history_current = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_HISTORY(current));
	const RID history_previous = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_HISTORY(previous));
	const RID moments_current = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_MOMENTS(current));
	const RID moments_previous = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_MOMENTS(previous));

	if (counter_buffer.is_valid()) {
		RD::get_singleton()->buffer_clear(counter_buffer, 0, sizeof(uint32_t) * COUNTER_MAX);
	}

	const uint32_t trace_groups_x = Math::division_round_up(uint32_t(inputs.trace_size.x), 8u);
	const uint32_t trace_groups_y = Math::division_round_up(uint32_t(inputs.trace_size.y), 8u);

	RD::get_singleton()->draw_command_begin_label("RTGI");

	RENDER_TIMESTAMP("RTGI Classify");
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, inputs.depth));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, inputs.normal_roughness));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, ssgi_texture_rid));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, geo_position_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, geo_normal_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 6, tiles));
		LocalVector<RD::Uniform> ddgi_uniforms = volume->gather_uniforms();
		ddgi_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER, 4, linear_sampler));
		_dispatch(MODE_CLASSIFY, frame_ubo, uniforms, &ddgi_uniforms, uint32_t(tile_count.x), uint32_t(tile_count.y), true);
	}

	RENDER_TIMESTAMP("RTGI Trace");
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 0, trace_ubo));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, inputs.depth));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, inputs.normal_roughness));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, tiles));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 4, candidate_first));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, candidate_second));
		if (!p_ddgi.trace_rtgi(volume, ray_texture, uniforms, trace_groups_x, trace_groups_y)) {
			RD::get_singleton()->draw_command_end_label();
			_refuse(p_buffers, "the trace did not run");
			return false;
		}
	}

	RENDER_TIMESTAMP("RTGI Temporal");
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, candidate_second));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, geo_position_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, geo_normal_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, geo_position_previous));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 5, geo_normal_previous));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, history_previous));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, moments_previous));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, history_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, moments_current));
		_dispatch(MODE_TEMPORAL, frame_ubo, uniforms, nullptr, trace_groups_x, trace_groups_y, true);
	}

	RENDER_TIMESTAMP("RTGI Filter");
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, history_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, moments_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, geo_position_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, geo_normal_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_FILTER(0))));
		_dispatch(MODE_VARIANCE, frame_ubo, uniforms, nullptr, trace_groups_x, trace_groups_y, false);
	}
	uint32_t filter_index = 0;
	for (uint32_t i = 0; i < atrous_count; i++) {
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_FILTER(filter_index))));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, geo_position_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, geo_normal_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, history_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 5, p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_FILTER(filter_index ^ 1u))));
		_dispatch(MODE_ATROUS, atrous_ubo[i], uniforms, nullptr, trace_groups_x, trace_groups_y, false);
		filter_index ^= 1u;
	}
	const RID final_filter = p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_FILTER(filter_index));

	RENDER_TIMESTAMP("RTGI Compose");
	{
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, inputs.depth));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, inputs.normal_roughness));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, final_filter));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, geo_position_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 5, geo_normal_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, history_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 7, moments_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, p_render_buffers->get_texture(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_RADIANCE)));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, p_render_buffers->get_texture(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_COVERAGE)));
		_dispatch(MODE_COMPOSE, frame_ubo, uniforms, nullptr, Math::division_round_up(uint32_t(inputs.slot_size.x), 8u), Math::division_round_up(uint32_t(inputs.slot_size.y), 8u), false);
	}

	if (debug_view != DEBUG_VIEW_NONE) {
		RENDER_TIMESTAMP("RTGI Debug");
		FrameUBO debug_data = frame_data;
		debug_data.debug_mode = uint32_t(debug_view);
		RD::get_singleton()->buffer_update(atrous_ubo[7], 0, sizeof(FrameUBO), &debug_data);
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 1, tiles));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 2, candidate_second));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 3, history_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 4, moments_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 5, final_filter));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_TEXTURE, 6, geo_position_current));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_DEBUG)));
		_dispatch(MODE_DEBUG, atrous_ubo[7], uniforms, nullptr, trace_groups_x, trace_groups_y, false);
	}

	RD::get_singleton()->draw_command_end_label();

	if (debug_view != DEBUG_VIEW_NONE) {
		const String dump_path = GLOBAL_GET("rendering/global_illumination/rtgi/debug_dump_path");
		if (!dump_path.is_empty()) {
			_dump_debug(p_render_buffers, dump_path, inputs.trace_size, debug_view);
		}
	}
	_read_counters(p_buffers);

	p_buffers->index ^= 1u;
	p_buffers->have_previous = true;
	p_buffers->previous_transform = inputs.camera_transform;
	p_buffers->previous_projection = inputs.projection;
	p_buffers->previous_hardware = inputs.hardware;
	p_buffers->previous_epochs = inputs.epochs;
	p_buffers->have_epochs = true;
	p_buffers->frames_processed++;
	p_buffers->frames_since_cut++;

	const uint32_t accumulated = MIN(p_buffers->frames_since_cut, settings.history_cap);
	const float confidence = gi_provider_confidence_from_samples(accumulated, settings.target_samples);
	GIProviderRecord record = gi_provider_make_record(gi_provider_units(inputs.physical_units), GI_COMPOSITOR_EXTRA_SOURCE, GI_PROVIDER_FLAG_SAMPLE_COUNT | GI_PROVIDER_FLAG_CONFIDENCE | GI_PROVIDER_FLAG_HISTORY, accumulated, cut == CUT_NONE ? 1.0f : 0.0f, confidence);
	GIEpochs::get_singleton()->fill_record(record);
	GICompositor::publish(p_render_buffers, record);

	Dictionary summary;
	summary["flags"] = int64_t(flags);
	summary["cut"] = int64_t(cut);
	summary["ray_fraction"] = ray_fraction;
	summary["scale"] = int64_t(scale);
	summary["slot_scale"] = int64_t(slot_scale);
	summary["frame"] = int64_t(inputs.frame_index);
	p_buffers->last_inputs = summary;
	p_buffers->refused = String();
	return true;
}

void RtgiPass::release(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	if (p_render_buffers.is_null() || !p_render_buffers->has_custom_data(RB_SCOPE_RTGI)) {
		return;
	}
	p_render_buffers->clear_context(RB_SCOPE_RTGI);
	p_render_buffers->set_custom_data(RB_SCOPE_RTGI, Ref<RenderBufferCustomDataRD>());
}

void RtgiPass::_read_counters(const Ref<RtgiBuffers> &p_buffers) {
	if (counter_buffer.is_null()) {
		return;
	}
	const Vector<uint8_t> data = RD::get_singleton()->buffer_get_data(counter_buffer);
	if (data.size() == int64_t(sizeof(p_buffers->last_counters))) {
		memcpy(p_buffers->last_counters, data.ptr(), sizeof(p_buffers->last_counters));
	}
}

void RtgiPass::_dump_debug(const Ref<RenderSceneBuffersRD> &p_render_buffers, const String &p_path, const Size2i &p_size, DebugView p_view) const {
	const Vector<uint8_t> data = RD::get_singleton()->texture_get_data(p_render_buffers->get_texture(RB_SCOPE_RTGI, RB_RTGI_DEBUG), 0);
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_buffer(data.ptr(), data.size());
	}
	Ref<FileAccess> info = FileAccess::open(p_path + ".txt", FileAccess::WRITE);
	if (info.is_valid()) {
		info->store_line(itos(p_size.width) + " " + itos(p_size.height) + " " + itos(p_view));
	}
}

void RtgiPass::_write_status(const Ref<RenderSceneBuffersRD> &p_render_buffers, const Ref<RtgiBuffers> &p_buffers, const String &p_path) const {
	Dictionary status;
	status["enabled"] = is_enabled();
	status["active"] = p_buffers->refused.is_empty() && p_buffers->frames_processed > 0;
	status["refused"] = p_buffers->refused;
	status["frames_processed"] = int64_t(p_buffers->frames_processed);
	status["frames_since_cut"] = int64_t(p_buffers->frames_since_cut);
	status["soft_cuts"] = int64_t(p_buffers->soft_cuts);
	status["last_cut"] = int64_t(p_buffers->last_cut);
	Dictionary restarts;
	restarts["last_spheres"] = int64_t(p_buffers->last_spheres);
	restarts["sphere_frames"] = int64_t(p_buffers->sphere_frames);
	restarts["spheres_total"] = int64_t(p_buffers->spheres_total);
	restarts["spheres_dropped"] = int64_t(p_buffers->spheres_dropped);
	restarts["spheres_behind"] = int64_t(p_buffers->spheres_behind);
	restarts["events_lost"] = int64_t(p_buffers->events_lost);
	restarts["event_repeats"] = int64_t(p_buffers->event_repeats);
	restarts["region_crossings"] = int64_t(p_buffers->region_crossings);
	restarts["directional_jumps"] = int64_t(p_buffers->directional_jumps);
	restarts["directional_repeats"] = int64_t(p_buffers->directional_repeats);
	status["restarts"] = restarts;
	Dictionary cuts;
	cuts["first"] = int64_t(p_buffers->cut_counts[CUT_FIRST]);
	cuts["resize"] = int64_t(p_buffers->cut_counts[CUT_RESIZE]);
	cuts["camera"] = int64_t(p_buffers->cut_counts[CUT_CAMERA]);
	cuts["backend"] = int64_t(p_buffers->cut_counts[CUT_BACKEND]);
	cuts["request"] = int64_t(p_buffers->cut_counts[CUT_REQUEST]);
	status["cuts"] = cuts;
	status["screen_width"] = p_buffers->screen_size.width;
	status["screen_height"] = p_buffers->screen_size.height;
	status["trace_width"] = p_buffers->trace_size.width;
	status["trace_height"] = p_buffers->trace_size.height;
	status["slot_width"] = p_buffers->slot_size.width;
	status["slot_height"] = p_buffers->slot_size.height;
	status["tile_width"] = p_buffers->tile_count.width;
	status["tile_height"] = p_buffers->tile_count.height;
	status["ray_fraction"] = p_buffers->last_ray_fraction;
	status["ray_capacity"] = int64_t(p_buffers->last_trace_pixel_capacity);
	status["atrous_iterations"] = int64_t(p_buffers->last_atrous_iterations);
	status["last_inputs"] = p_buffers->last_inputs;
	GIProviderRecord published;
	if (GICompositor::get_record(p_render_buffers, published)) {
		Dictionary record;
		record["source"] = int64_t(published.source);
		record["units"] = int64_t(published.units);
		record["flags"] = int64_t(published.flags);
		record["sample_count"] = int64_t(published.sample_count);
		record["validity"] = published.validity;
		record["confidence"] = published.confidence;
		record["scene_epoch"] = int64_t(published.scene_epoch);
		record["light_epoch"] = int64_t(published.light_epoch);
		record["sky_epoch"] = int64_t(published.sky_epoch);
		record["origin_epoch"] = int64_t(published.origin_epoch);
		status["record"] = record;
	}
	if (counter_buffer.is_valid()) {
		const uint32_t *values = p_buffers->last_counters;
		Dictionary counters;
		counters["tiles_total"] = int64_t(values[COUNTER_TILES_TOTAL]);
		counters["tiles_traced"] = int64_t(values[COUNTER_TILES_TRACED]);
		counters["trace_pixels"] = int64_t(values[COUNTER_TRACE_PIXELS]);
		counters["rays"] = int64_t(values[COUNTER_RAYS]);
		counters["hits"] = int64_t(values[COUNTER_HITS]);
		counters["sky_misses"] = int64_t(values[COUNTER_SKY_MISSES]);
		counters["backface"] = int64_t(values[COUNTER_BACKFACE]);
		counters["valid_rays"] = int64_t(values[COUNTER_RAYS]) - int64_t(values[COUNTER_BACKFACE]);
		counters["history_rejects"] = int64_t(values[COUNTER_HISTORY_REJECTS]);
		counters["restarted"] = int64_t(values[COUNTER_RESTARTED]);
		status["gpu_counters"] = counters;
	}
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(JSON::stringify(status));
	}
}
