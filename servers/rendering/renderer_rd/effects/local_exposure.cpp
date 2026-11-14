/**************************************************************************/
/*  local_exposure.cpp                                                    */
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

#include "local_exposure.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

LocalExposure::LocalExposure() {
	GLOBAL_DEF(PropertyInfo(Variant::INT, "rendering/environment/local_exposure/method", PROPERTY_HINT_ENUM, "Bilateral Grid,Edge-Checked Blur"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/environment/local_exposure/spatial_sigma", PROPERTY_HINT_RANGE, "0.25,2,0.01"), 1.0);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/environment/local_exposure/range_sigma", PROPERTY_HINT_RANGE, "0.1,1.5,0.01"), 1.0);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/environment/local_exposure/wide_blend", PROPERTY_HINT_RANGE, "0,1,0.01"), 0.0);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/environment/local_exposure/edge_range", PROPERTY_HINT_RANGE, "0.1,6,0.01"), 1.0);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "rendering/environment/local_exposure/temporal_seconds", PROPERTY_HINT_RANGE, "0,2,0.01"), 0.0);

	Vector<String> modes;
	modes.push_back("\n#define MODE_DOWNSAMPLE\n");
	modes.push_back("\n#define MODE_BUILD\n");
	modes.push_back("\n#define MODE_BLUR\n");
	modes.push_back("\n#define MODE_WIDE\n");
	modes.push_back("\n#define MODE_TEMPORAL\n");

	shader.initialize(modes);
	shader_version = shader.version_create();

	for (int i = 0; i < MODE_MAX; i++) {
		pipelines[i] = RD::get_singleton()->compute_pipeline_create(shader.version_get_shader(shader_version, i));
	}
}

LocalExposure::~LocalExposure() {
	shader.version_free(shader_version);
}

void LocalExposure::Buffers::configure(RenderSceneBuffersRD *p_render_buffers) {
	const Size2i internal_size = p_render_buffers->get_internal_size();
	log_size = Size2i(MAX(Math::division_round_up(internal_size.x, LOG_DIVISOR), 1), MAX(Math::division_round_up(internal_size.y, LOG_DIVISOR), 1));
	grid_size = Size2i(MAX(Math::division_round_up(log_size.x, GRID_DIVISOR), 1), MAX(Math::division_round_up(log_size.y, GRID_DIVISOR), 1));

	RD::TextureFormat tf;
	tf.format = RD::DATA_FORMAT_R32_SFLOAT;
	tf.width = log_size.x;
	tf.height = log_size.y;
	tf.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	log_luminance = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(log_luminance, "Local exposure log luminance");

	tf.format = RD::DATA_FORMAT_R16G16B16A16_SFLOAT;
	tf.width = grid_size.x;
	tf.height = grid_size.y;
	wide = RD::get_singleton()->texture_create(tf, RD::TextureView());
	RD::get_singleton()->set_resource_name(wide, "Local exposure wide base");

	tf.texture_type = RD::TEXTURE_TYPE_3D;
	tf.depth = GRID_DEPTH;
	RD::TextureFormat history_format = tf;
	history_format.format = RD::DATA_FORMAT_R32G32B32A32_SFLOAT;
	for (int i = 0; i < 2; i++) {
		grid[i] = RD::get_singleton()->texture_create(tf, RD::TextureView());
		RD::get_singleton()->set_resource_name(grid[i], "Local exposure grid");
		history[i] = RD::get_singleton()->texture_create(history_format, RD::TextureView());
		RD::get_singleton()->set_resource_name(history[i], "Local exposure grid history");
	}
	history_index = 0;
	history_valid = false;
}

void LocalExposure::Buffers::free_data() {
	if (log_luminance.is_valid()) {
		RD::get_singleton()->free_rid(log_luminance);
		log_luminance = RID();
	}
	if (wide.is_valid()) {
		RD::get_singleton()->free_rid(wide);
		wide = RID();
	}
	for (int i = 0; i < 2; i++) {
		if (grid[i].is_valid()) {
			RD::get_singleton()->free_rid(grid[i]);
			grid[i] = RID();
		}
		if (history[i].is_valid()) {
			RD::get_singleton()->free_rid(history[i]);
			history[i] = RID();
		}
	}
	history_valid = false;
}

Ref<LocalExposure::Buffers> LocalExposure::get_buffers(Ref<RenderSceneBuffersRD> p_render_buffers) {
	if (p_render_buffers->has_custom_data(RB_LOCAL_EXPOSURE_BUFFERS)) {
		return p_render_buffers->get_custom_data(RB_LOCAL_EXPOSURE_BUFFERS);
	}

	Ref<Buffers> buffers;
	buffers.instantiate();
	buffers->configure(p_render_buffers.ptr());

	p_render_buffers->set_custom_data(RB_LOCAL_EXPOSURE_BUFFERS, buffers);

	return buffers;
}

RID LocalExposure::process(RID p_source_texture, const Size2i p_source_size, Ref<Buffers> p_buffers, const Settings &p_settings) {
	UniformSetCacheRD *uniform_set_cache = UniformSetCacheRD::get_singleton();
	ERR_FAIL_NULL_V(uniform_set_cache, RID());
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	ERR_FAIL_NULL_V(material_storage, RID());
	ERR_FAIL_COND_V(p_buffers.is_null() || p_buffers->log_luminance.is_null(), RID());

	RID linear_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	RID nearest_sampler = material_storage->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);

	const Size2i log_size = p_buffers->log_size;
	const Size2i grid_size = p_buffers->grid_size;
	const float spatial_cells = CLAMP(p_settings.spatial_sigma, 0.25f, 2.0f);
	const float range_cells = CLAMP(p_settings.range_sigma, 0.1f, 1.5f) * float(GRID_DEPTH) / (LOG_MAX - LOG_MIN);
	if (p_settings.frame != p_buffers->last_frame + 1) {
		p_buffers->history_valid = false;
	}
	p_buffers->last_frame = p_settings.frame;

	PushConstant push_constant;
	memset(&push_constant, 0, sizeof(PushConstant));

	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();

	{
		RID mode_shader = shader.version_get_shader(shader_version, MODE_DOWNSAMPLE);
		push_constant.source_size[0] = p_source_size.x;
		push_constant.source_size[1] = p_source_size.y;
		push_constant.dest_size[0] = log_size.x;
		push_constant.dest_size[1] = log_size.y;
		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ linear_sampler, p_source_texture }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, p_buffers->log_luminance);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_DOWNSAMPLE]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(mode_shader, 0, u_source), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(mode_shader, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, log_size.x, log_size.y, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);
	}

	{
		RID mode_shader = shader.version_get_shader(shader_version, MODE_BUILD);
		push_constant.source_size[0] = log_size.x;
		push_constant.source_size[1] = log_size.y;
		push_constant.dest_size[0] = grid_size.x;
		push_constant.dest_size[1] = grid_size.y;
		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_buffers->log_luminance }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, p_buffers->grid[0]);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_BUILD]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(mode_shader, 0, u_source), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(mode_shader, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, grid_size.x, grid_size.y, 1);
		RD::get_singleton()->compute_list_add_barrier(compute_list);
	}

	const int blur_axis[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	const float blur_sigma[3] = { spatial_cells, spatial_cells, range_cells };
	const int blur_source[3] = { 0, 1, 0 };

	for (int pass = 0; pass < 3; pass++) {
		RID mode_shader = shader.version_get_shader(shader_version, MODE_BLUR);
		push_constant.axis[0] = blur_axis[pass][0];
		push_constant.axis[1] = blur_axis[pass][1];
		push_constant.axis[2] = blur_axis[pass][2];
		push_constant.sigma = blur_sigma[pass];
		RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_buffers->grid[blur_source[pass]] }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, p_buffers->grid[1 - blur_source[pass]]);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_BLUR]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(mode_shader, 0, u_source), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(mode_shader, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, grid_size.x, grid_size.y, GRID_DEPTH);
		RD::get_singleton()->compute_list_add_barrier(compute_list);
	}

	RID result = p_buffers->grid[1];
	if (p_settings.temporal_alpha < 1.0) {
		const int next = 1 - p_buffers->history_index;
		RID mode_shader = shader.version_get_shader(shader_version, MODE_TEMPORAL);
		push_constant.sigma = p_buffers->history_valid && !p_settings.reset_history ? p_settings.temporal_alpha : 1.0;
		RD::Uniform u_now(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_buffers->grid[1] }));
		RD::Uniform u_before(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, Vector<RID>({ nearest_sampler, p_buffers->history[p_buffers->history_index] }));
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 0, p_buffers->history[next]);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_TEMPORAL]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(mode_shader, 0, u_now, u_before), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(mode_shader, 1, u_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, grid_size.x, grid_size.y, GRID_DEPTH);
		RD::get_singleton()->compute_list_add_barrier(compute_list);
		p_buffers->history_index = next;
		p_buffers->history_valid = true;
		result = p_buffers->history[next];
	} else {
		p_buffers->history_valid = false;
	}

	{
		RID wide_shader = shader.version_get_shader(shader_version, MODE_WIDE);
		RD::Uniform u_wide_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ nearest_sampler, p_settings.temporal_alpha < 1.0 ? result : p_buffers->grid[0] }));
		RD::Uniform u_wide_dest(RD::UNIFORM_TYPE_IMAGE, 0, p_buffers->wide);
		RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, pipelines[MODE_WIDE]);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(wide_shader, 0, u_wide_source), 0);
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, uniform_set_cache->get_cache(wide_shader, 1, u_wide_dest), 1);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(PushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, grid_size.x, grid_size.y, 1);
	}

	RD::get_singleton()->compute_list_end();

	return result;
}
