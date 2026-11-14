/**************************************************************************/
/*  direct_resampler.cpp                                                  */
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

#include "direct_resampler.h"

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

#define DI_RESAMPLED_SETTING(m_name) ("rendering/global_illumination/direct/resampled/" m_name)

void DirectResamplerRD::register_settings() {
	GLOBAL_DEF(DI_RESAMPLED_SETTING("enabled"), false);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("head"), PROPERTY_HINT_RANGE, "0,6,1"), 4);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("tail_samples"), PROPERTY_HINT_RANGE, "1,7,1"), 1);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, DI_RESAMPLED_SETTING("floor"), PROPERTY_HINT_RANGE, "0.0001,1,0.0001"), 0.0625);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("energy_arm"), PROPERTY_HINT_ENUM, "Mean,Live"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("debug/tail_visibility"), PROPERTY_HINT_ENUM, "Traced,Visible,Hashed"), 0);
	GLOBAL_DEF(DI_RESAMPLED_SETTING("debug/stale_prior"), false);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, DI_RESAMPLED_SETTING("debug/log"), PROPERTY_HINT_GLOBAL_SAVE_FILE, "*.txt"), String());
}

DirectResamplerRD::Settings DirectResamplerRD::clamp_settings(bool p_enabled, int p_head, int p_tail_samples, float p_floor, int p_energy_arm) {
	Settings settings;
	settings.enabled = p_enabled;
	settings.tail_samples = uint32_t(CLAMP(p_tail_samples, 1, 7));
	settings.head = uint32_t(CLAMP(p_head, 0, 7 - int(settings.tail_samples)));
	settings.floor_value = CLAMP(p_floor, 0.0001f, 1.0f);
	settings.live_energy = p_energy_arm == 1;
	return settings;
}

DirectResamplerRD::Settings DirectResamplerRD::get_project_settings() {
	Settings settings = clamp_settings(GLOBAL_GET(DI_RESAMPLED_SETTING("enabled")), GLOBAL_GET(DI_RESAMPLED_SETTING("head")), GLOBAL_GET(DI_RESAMPLED_SETTING("tail_samples")), GLOBAL_GET(DI_RESAMPLED_SETTING("floor")), GLOBAL_GET(DI_RESAMPLED_SETTING("energy_arm")));
	settings.tail_visibility = uint32_t(CLAMP(int(GLOBAL_GET(DI_RESAMPLED_SETTING("debug/tail_visibility"))), 0, 2));
	settings.stale_prior = GLOBAL_GET(DI_RESAMPLED_SETTING("debug/stale_prior"));
	settings.debug_log = GLOBAL_GET(DI_RESAMPLED_SETTING("debug/log"));
	return settings;
}

bool DirectResamplerRD::is_enabled() const {
	return GLOBAL_GET(DI_RESAMPLED_SETTING("enabled"));
}

RID DirectResamplerRD::_dummy(RD::DataFormat p_format, RD::TextureType p_type, const char *p_name) {
	RD::TextureFormat format;
	format.format = p_format;
	format.texture_type = p_type;
	format.width = 1;
	format.height = 1;
	format.array_layers = 1;
	format.usage_bits = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT;
	RID texture = RD::get_singleton()->texture_create(format, RD::TextureView());
	RD::get_singleton()->set_resource_name(texture, p_name);
	return texture;
}

bool DirectResamplerRD::_ensure_device() {
	if (device_ready) {
		return shader_version.is_valid() && shader->version_is_valid(shader_version);
	}
	if (RD::get_singleton() == nullptr) {
		return false;
	}
	device_ready = true;
	shader = memnew(DiResampleShaderRD);
	Vector<String> modes;
	modes.push_back("\n");
	shader->initialize(modes);
	shader_version = shader->version_create();
	if (!shader->version_is_valid(shader_version)) {
		return false;
	}
	pipeline.create_compute_pipeline(shader->version_get_shader(shader_version, 0));
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
	debug_buffer = RD::get_singleton()->storage_buffer_create(DEBUG_BYTES);
	history_slots_dummy = _dummy(RD::DATA_FORMAT_R16_UNORM, RD::TEXTURE_TYPE_2D_ARRAY, "Direct Resampled History Slots Dummy");
	history_factors_dummy = _dummy(RD::DATA_FORMAT_R16_UNORM, RD::TEXTURE_TYPE_2D_ARRAY, "Direct Resampled History Factors Dummy");
	history_guide_dummy = _dummy(RD::DATA_FORMAT_R32G32_UINT, RD::TEXTURE_TYPE_2D, "Direct Resampled History Guide Dummy");
	return true;
}

void DirectResamplerRD::_write_debug_line() {
	const Vector<uint8_t> data = RD::get_singleton()->buffer_get_data(debug_buffer);
	if (data.size() < int(DEBUG_BYTES)) {
		return;
	}
	const uint32_t *values = reinterpret_cast<const uint32_t *>(data.ptr());
	String line = "frame " + itos(int64_t(debug_frame)) + " size " + itos(debug_size.x) + "x" + itos(debug_size.y) + " tail_pixels " + itos(values[0]);
	for (uint32_t slot = 0; slot < DEBUG_SLOTS; slot++) {
		const uint32_t count = values[DEBUG_HEADER_WORDS + slot];
		if (count != 0) {
			line += " " + itos(slot) + ":" + itos(count);
		}
	}
	debug_file->store_line(line);
	debug_file->flush();
}

void DirectResamplerRD::_flush_debug_line() {
	if (debug_pending && debug_file.is_valid()) {
		_write_debug_line();
	}
	debug_pending = false;
}

bool DirectResamplerRD::run(const Input &p_input, Output &r_output) {
	const Settings settings = get_project_settings();
	const Ref<RenderSceneBuffersRD> &rb = p_input.render_buffers;
	if (!settings.enabled || rb.is_null() || !_ensure_device()) {
		return false;
	}
	RD *rd = RD::get_singleton();
	const Size2i size = rb->get_internal_size();

	if (settings.debug_log != debug_path) {
		_flush_debug_line();
		debug_file.unref();
		debug_path = settings.debug_log;
		if (!debug_path.is_empty()) {
			debug_file = FileAccess::open(debug_path, FileAccess::WRITE);
			if (debug_file.is_null()) {
				ERR_PRINT(vformat("The direct resampler's debug log %s could not be opened.", debug_path));
			}
		}
	}
	const bool counting = debug_file.is_valid() && !(debug_pending && debug_frame == p_input.frame);
	if (counting) {
		_flush_debug_line();
		rd->buffer_clear(debug_buffer, 0, DEBUG_BYTES);
	}

	Ref<ViewportData> viewport;
	if (rb->has_custom_data(RB_SCOPE_DI_RESAMPLED)) {
		viewport = rb->get_custom_data(RB_SCOPE_DI_RESAMPLED);
	} else {
		viewport.instantiate();
		rb->set_custom_data(RB_SCOPE_DI_RESAMPLED, viewport);
	}
	if (viewport->size != size || viewport->head != settings.head || viewport->tail_samples != settings.tail_samples || !rb->has_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_RECORD)) {
		rb->clear_context(RB_SCOPE_DI_RESAMPLED);
		viewport->size = size;
		viewport->head = settings.head;
		viewport->tail_samples = settings.tail_samples;
		rb->create_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_ENTRIES, RD::DATA_FORMAT_R32_UINT, RD::TEXTURE_USAGE_STORAGE_BIT, RD::TEXTURE_SAMPLES_1, size, settings.head + 3 * settings.tail_samples, 1, true);
		rb->create_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_RECORD, RD::DATA_FORMAT_R32G32_UINT, RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_SAMPLING_BIT, RD::TEXTURE_SAMPLES_1, size, settings.tail_samples + 1, 1, true);
	}
	const RID entries = rb->get_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_ENTRIES);
	const RID record = rb->get_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_RECORD);

	bool history = p_input.history_slots.is_valid() && p_input.history_factors.is_valid() && p_input.history_guide.is_valid();
	if (history) {
		const RD::TextureFormat format = rd->texture_get_format(p_input.history_slots);
		history = int(format.width) == size.x && int(format.height) == size.y && format.array_layers >= 2 && format.array_layers <= 8;
	}

	const Projection inverse = p_input.projection.inverse();
	const Projection prev_view_from_view = history ? Projection(p_input.history_camera.affine_inverse() * p_input.camera) : Projection();
	const Projection prev_projection = history ? p_input.history_projection : Projection();
	Params params = {};
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			params.inv_projection[c * 4 + r] = float(inverse.columns[c][r]);
			params.prev_view_from_view[c * 4 + r] = float(prev_view_from_view.columns[c][r]);
			params.prev_projection[c * 4 + r] = float(prev_projection.columns[c][r]);
		}
	}
	for (int r = 0; r < 3; r++) {
		params.camera_basis[r * 4 + 0] = float(p_input.camera.basis.rows[r][0]);
		params.camera_basis[r * 4 + 1] = float(p_input.camera.basis.rows[r][1]);
		params.camera_basis[r * 4 + 2] = float(p_input.camera.basis.rows[r][2]);
		params.region_half[r] = float(p_input.region_half[r]);
	}
	params.floor_value = settings.floor_value;
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
	params.head_entries = settings.head;
	params.tail_samples = settings.tail_samples;
	params.history_valid = history ? 1 : 0;
	params.live_energy = settings.live_energy ? 1 : 0;
	params.debug_flags = (settings.stale_prior ? DEBUG_FLAG_STALE_PRIOR : 0) | (counting ? DEBUG_FLAG_COUNT : 0);
	rd->buffer_update(params_buffer, 0, sizeof(Params), &params);

	RID sampler = RendererRD::MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_NEAREST, RSE::CANVAS_ITEM_TEXTURE_REPEAT_DISABLED);
	LocalVector<RD::Uniform> uniforms;
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, p_input.cluster_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, p_input.omni_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, p_input.spot_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, p_input.lamp_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 4, params_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 5, Vector<RID>{ sampler, p_input.depth }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 6, Vector<RID>{ sampler, p_input.normal_roughness }));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 7, history ? p_input.history_slots : history_slots_dummy));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 8, history ? p_input.history_factors : history_factors_dummy));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 9, history ? p_input.history_guide : history_guide_dummy));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 10, entries));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 11, record));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, debug_buffer));

	const RID shader_rid = shader->version_get_shader(shader_version, 0);
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline.get_rid());
	rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 0, uniforms), 0);
	rd->compute_list_dispatch(list, Math::division_round_up(uint32_t(size.x), 8u), Math::division_round_up(uint32_t(size.y), 8u), 1);
	rd->compute_list_end();
	if (counting) {
		debug_pending = true;
		debug_frame = p_input.frame;
		debug_size = size;
	}

	r_output.entries = entries;
	r_output.record = record;
	r_output.head_entries = settings.head;
	r_output.tail_samples = settings.tail_samples;
	r_output.tail_visibility = settings.tail_visibility;
	return true;
}

void DirectResamplerRD::release(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	if (p_render_buffers.is_null() || !p_render_buffers->has_custom_data(RB_SCOPE_DI_RESAMPLED)) {
		return;
	}
	Ref<ViewportData> viewport = p_render_buffers->get_custom_data(RB_SCOPE_DI_RESAMPLED);
	if (viewport->tail_samples == 0) {
		return;
	}
	p_render_buffers->clear_context(RB_SCOPE_DI_RESAMPLED);
	viewport->size = Size2i();
	viewport->head = 0;
	viewport->tail_samples = 0;
}

void DirectResamplerRD::free_device_resources() {
	if (!device_ready) {
		return;
	}
	RD *rd = RD::get_singleton();
	pipeline.free();
	if (shader_version.is_valid()) {
		shader->version_free(shader_version);
		shader_version = RID();
	}
	if (shader) {
		memdelete(shader);
		shader = nullptr;
	}
	_flush_debug_line();
	const RID owned[] = { params_buffer, debug_buffer, history_slots_dummy, history_factors_dummy, history_guide_dummy };
	for (const RID &rid : owned) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	params_buffer = RID();
	debug_buffer = RID();
	history_slots_dummy = RID();
	history_factors_dummy = RID();
	history_guide_dummy = RID();
	device_ready = false;
}

DirectResamplerRD::~DirectResamplerRD() {
	ERR_FAIL_COND_MSG(device_ready, "The direct resampler's device resources were not freed before shutdown.");
}
