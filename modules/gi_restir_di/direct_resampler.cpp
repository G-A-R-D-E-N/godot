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
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("reservoirs"), PROPERTY_HINT_ENUM, "Off,B1,U1"), 0);
	GLOBAL_DEF(DI_RESAMPLED_SETTING("temporal_reuse"), true);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("spatial_neighbors"), PROPERTY_HINT_RANGE, "0,4,1"), 2);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, DI_RESAMPLED_SETTING("spatial_radius"), PROPERTY_HINT_RANGE, "1,64,0.5"), 16.0);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("c_full"), PROPERTY_HINT_RANGE, "1,255,1"), 32);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("m_temporal_cap"), PROPERTY_HINT_RANGE, "1,255,1"), 20);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("m_stored_cap"), PROPERTY_HINT_RANGE, "1,255,1"), 255);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("head_rank"), PROPERTY_HINT_ENUM, "T,TV"), 0);
	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, DI_RESAMPLED_SETTING("head_swap_margin"), PROPERTY_HINT_RANGE, "0,4,0.01"), 0.25);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("head_swap_frames"), PROPERTY_HINT_RANGE, "1,7,1"), 4);
	GLOBAL_DEF(DI_RESAMPLED_SETTING("debug/head_swap_window"), false);
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_RESAMPLED_SETTING("debug/tail_visibility"), PROPERTY_HINT_ENUM, "Traced,Visible,Hashed"), 0);
	GLOBAL_DEF(DI_RESAMPLED_SETTING("debug/stale_prior"), false);
	GLOBAL_DEF(DI_RESAMPLED_SETTING("debug/no_stamp"), false);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, DI_RESAMPLED_SETTING("debug/log"), PROPERTY_HINT_GLOBAL_SAVE_FILE, "*.txt"), String());
	GLOBAL_DEF(PropertyInfo(Variant::STRING, DI_RESAMPLED_SETTING("debug/lab_log"), PROPERTY_HINT_GLOBAL_SAVE_FILE, "*.txt"), String());
	DirectSamplerLab::register_settings();
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

void DirectResamplerRD::clamp_reservoir_settings(Settings &r_settings, int p_reservoirs, bool p_temporal_reuse, int p_spatial_neighbors, float p_spatial_radius, int p_c_full, int p_m_temporal_cap, int p_m_stored_cap, int p_dynamic_cap, int p_head_rank, float p_head_swap_margin, int p_head_swap_frames, bool p_head_window) {
	r_settings.reservoirs = r_settings.tail_samples == 1 ? uint32_t(CLAMP(p_reservoirs, int(RESERVOIRS_OFF), int(RESERVOIRS_U1))) : RESERVOIRS_OFF;
	r_settings.temporal_reuse = p_temporal_reuse;
	r_settings.spatial_neighbors = uint32_t(CLAMP(p_spatial_neighbors, 0, int(MAX_NEIGHBORS)));
	r_settings.spatial_radius = CLAMP(p_spatial_radius, 1.0f, 64.0f);
	r_settings.c_full = uint32_t(CLAMP(p_c_full, 1, int(MAX_MASS)));
	r_settings.m_temporal_cap = uint32_t(CLAMP(p_m_temporal_cap, 1, int(MAX_MASS)));
	r_settings.m_stored_cap = uint32_t(CLAMP(p_m_stored_cap, 1, int(MAX_MASS)));
	r_settings.dynamic_cap = uint32_t(CLAMP(p_dynamic_cap, 1, int(MAX_MASS)));
	r_settings.head_rank_visible = p_head_rank == 1;
	r_settings.head_swap_margin = CLAMP(p_head_swap_margin, 0.0f, 4.0f);
	r_settings.head_swap_frames = uint32_t(CLAMP(p_head_swap_frames, 1, int(MAX_SWAP_FRAMES)));
	r_settings.head_window = p_head_window;
}

uint32_t DirectResamplerRD::advance_counter(uint32_t p_counter, bool p_continuous) {
	const uint32_t next = p_counter + (p_continuous ? 1 : 2);
	return next == 0 ? 1 : next;
}

DirectResamplerRD::Settings DirectResamplerRD::get_project_settings() {
	Settings settings = clamp_settings(GLOBAL_GET(DI_RESAMPLED_SETTING("enabled")), GLOBAL_GET(DI_RESAMPLED_SETTING("head")), GLOBAL_GET(DI_RESAMPLED_SETTING("tail_samples")), GLOBAL_GET(DI_RESAMPLED_SETTING("floor")), GLOBAL_GET(DI_RESAMPLED_SETTING("energy_arm")));
	settings.tail_visibility = uint32_t(CLAMP(int(GLOBAL_GET(DI_RESAMPLED_SETTING("debug/tail_visibility"))), 0, 2));
	settings.stale_prior = GLOBAL_GET(DI_RESAMPLED_SETTING("debug/stale_prior"));
	settings.no_stamp = GLOBAL_GET(DI_RESAMPLED_SETTING("debug/no_stamp"));
	settings.debug_log = GLOBAL_GET(DI_RESAMPLED_SETTING("debug/log"));
	settings.lab_log = GLOBAL_GET(DI_RESAMPLED_SETTING("debug/lab_log"));
	clamp_reservoir_settings(settings, GLOBAL_GET(DI_RESAMPLED_SETTING("reservoirs")), GLOBAL_GET(DI_RESAMPLED_SETTING("temporal_reuse")), GLOBAL_GET(DI_RESAMPLED_SETTING("spatial_neighbors")), GLOBAL_GET(DI_RESAMPLED_SETTING("spatial_radius")), GLOBAL_GET(DI_RESAMPLED_SETTING("c_full")), GLOBAL_GET(DI_RESAMPLED_SETTING("m_temporal_cap")), GLOBAL_GET(DI_RESAMPLED_SETTING("m_stored_cap")), GLOBAL_GET("rendering/global_illumination/direct/dynamic_history_cap"), GLOBAL_GET(DI_RESAMPLED_SETTING("head_rank")), GLOBAL_GET(DI_RESAMPLED_SETTING("head_swap_margin")), GLOBAL_GET(DI_RESAMPLED_SETTING("head_swap_frames")), GLOBAL_GET(DI_RESAMPLED_SETTING("debug/head_swap_window")));
	return settings;
}

uint32_t DirectResamplerRD::lab_mass_bound(const Settings &p_settings) {
	return 2 * (p_settings.c_full + p_settings.m_temporal_cap) + p_settings.spatial_neighbors * p_settings.m_stored_cap;
}

String DirectResamplerRD::lab_settings_text(const Settings &p_settings) {
	static const char *arms[3] = { "Off", "B1", "U1" };
	static const char *visibility[3] = { "Traced", "Visible", "Hashed" };
	String text = "settings";
	text += " reservoirs=" + String(arms[MIN(p_settings.reservoirs, 2u)]);
	text += " temporal_reuse=" + itos(p_settings.temporal_reuse ? 1 : 0);
	text += " spatial_neighbors=" + itos(p_settings.spatial_neighbors);
	text += " spatial_radius=" + String::num_real(p_settings.spatial_radius);
	text += " c_full=" + itos(p_settings.c_full);
	text += " m_temporal_cap=" + itos(p_settings.m_temporal_cap);
	text += " m_stored_cap=" + itos(p_settings.m_stored_cap);
	text += " dynamic_cap=" + itos(p_settings.dynamic_cap);
	text += " head_rank=" + String(p_settings.head_rank_visible ? "TV" : "T");
	text += " head_swap_margin=" + String::num_real(p_settings.head_swap_margin);
	text += " head_swap_frames=" + itos(p_settings.head_swap_frames);
	text += " head_swap_window=" + itos(p_settings.head_window ? 1 : 0);
	text += " head=" + itos(p_settings.head);
	text += " tail_samples=" + itos(p_settings.tail_samples);
	text += " floor=" + String::num_real(p_settings.floor_value);
	text += " energy_arm=" + String(p_settings.live_energy ? "Live" : "Mean");
	text += " tail_visibility=" + String(visibility[MIN(p_settings.tail_visibility, 2u)]);
	text += " stale_prior=" + itos(p_settings.stale_prior ? 1 : 0);
	text += " no_stamp=" + itos(p_settings.no_stamp ? 1 : 0);
	text += " mass_bound=" + itos(lab_mass_bound(p_settings));
	return text;
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
	modes.push_back("\n#define MODE_SPATIAL\n");
	modes.push_back("\n#define LAB_COUNTS\n");
	modes.push_back("\n#define MODE_SPATIAL\n#define LAB_COUNTS\n");
	shader->initialize(modes);
	shader_version = shader->version_create();
	if (!shader->version_is_valid(shader_version)) {
		return false;
	}
	pipeline.create_compute_pipeline(shader->version_get_shader(shader_version, 0));
	spatial_pipeline.create_compute_pipeline(shader->version_get_shader(shader_version, 1));
	lab_pipeline.create_compute_pipeline(shader->version_get_shader(shader_version, 2));
	lab_spatial_pipeline.create_compute_pipeline(shader->version_get_shader(shader_version, 3));
	header_dummy = _dummy(RD::DATA_FORMAT_R32_UINT, RD::TEXTURE_TYPE_2D, "Direct Resampled Header Dummy");
	params_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(Params));
	debug_buffer = RD::get_singleton()->storage_buffer_create(DEBUG_BYTES);
	lab_buffer = RD::get_singleton()->storage_buffer_create(LAB_BYTES);
	lab_mask_buffer = RD::get_singleton()->storage_buffer_create(LAB_MASK_BYTES);
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

void DirectResamplerRD::_write_lab_line() {
	const Vector<uint8_t> data = RD::get_singleton()->buffer_get_data(lab_buffer);
	if (data.size() < int(LAB_BYTES)) {
		return;
	}
	const uint32_t *values = reinterpret_cast<const uint32_t *>(data.ptr());
	static const char *names[7] = { "path", "present", "dropped", "dropped_mass", "total_mass", "dynamic_capped", "trigger" };
	String line = "frame " + itos(int64_t(lab_frame)) + " size " + itos(lab_size.x) + "x" + itos(lab_size.y);
	for (uint32_t word = 0; word < 7; word++) {
		line += " " + String(names[word]) + " " + itos(values[word]);
	}
	line += " final";
	for (uint32_t slot = 0; slot < DEBUG_SLOTS; slot++) {
		const uint32_t count = values[LAB_HEADER_WORDS + slot];
		if (count != 0) {
			line += " " + itos(slot) + ":" + itos(count);
		}
	}
	lab_file->store_line(line);
	lab_file->flush();
}

void DirectResamplerRD::_write_lab_mask() {
	if (!lab_pending || lab_path.is_empty()) {
		return;
	}
	const Vector<uint8_t> data = RD::get_singleton()->buffer_get_data(lab_mask_buffer);
	Ref<FileAccess> file = FileAccess::open(lab_path + ".mask", FileAccess::WRITE);
	if (file.is_null() || data.size() < int(LAB_MASK_BYTES)) {
		ERR_PRINT(vformat("The direct resampler's lab mask %s.mask could not be written.", lab_path));
		return;
	}
	file->store_32(uint32_t(lab_size.x));
	file->store_32(uint32_t(lab_size.y));
	file->store_64(lab_frame);
	file->store_buffer(data.ptr(), LAB_MASK_BYTES);
}

void DirectResamplerRD::_flush_lab_line() {
	if (lab_pending && lab_file.is_valid()) {
		_write_lab_line();
	}
	lab_pending = false;
}

bool DirectResamplerRD::run(const Input &p_input, Output &r_output) {
	sampler_lab.serve();
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

	if (settings.lab_log != lab_path) {
		_write_lab_mask();
		_flush_lab_line();
		lab_file.unref();
		lab_path = settings.lab_log;
		lab_settings = String();
		if (!lab_path.is_empty()) {
			lab_file = FileAccess::open(lab_path, FileAccess::WRITE);
			if (lab_file.is_null()) {
				ERR_PRINT(vformat("The direct resampler's lab log %s could not be opened.", lab_path));
			}
		}
	}
	const bool lab = lab_file.is_valid() && !(lab_pending && lab_frame == p_input.frame);
	if (lab) {
		_flush_lab_line();
		rd->buffer_clear(lab_buffer, 0, LAB_BYTES);
		rd->buffer_clear(lab_mask_buffer, 0, LAB_MASK_BYTES);
		const String text = lab_settings_text(settings);
		if (text != lab_settings) {
			lab_settings = text;
			lab_file->store_line(text);
		}
	}

	Ref<ResampledViewportData> viewport;
	if (rb->has_custom_data(RB_SCOPE_DI_RESAMPLED)) {
		viewport = rb->get_custom_data(RB_SCOPE_DI_RESAMPLED);
	} else {
		viewport.instantiate();
		rb->set_custom_data(RB_SCOPE_DI_RESAMPLED, viewport);
	}
	const bool reservoir_sets = settings.reservoirs != RESERVOIRS_OFF;
	const bool head_state = settings.head_rank_visible && settings.head > 0;
	if (viewport->size != size || viewport->head != settings.head || viewport->tail_samples != settings.tail_samples || viewport->reservoir_sets != reservoir_sets || viewport->head_state != head_state || !rb->has_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_RECORD)) {
		rb->clear_context(RB_SCOPE_DI_RESAMPLED);
		viewport->size = size;
		viewport->head = settings.head;
		viewport->tail_samples = settings.tail_samples;
		viewport->reservoir_sets = reservoir_sets;
		viewport->head_state = head_state;
		viewport->counter = 1;
		viewport->history_set = 0;
		viewport->head_state_set = 0;
		viewport->last_frame = 0;
		viewport->swap_sets = false;
		viewport->swap_head_state = false;
		const uint32_t private_usage = RD::TEXTURE_USAGE_STORAGE_BIT | RD::TEXTURE_USAGE_CAN_COPY_TO_BIT;
		if (reservoir_sets) {
			const Size2i tiles = Size2i(Math::division_round_up(uint32_t(size.x), 8u), Math::division_round_up(uint32_t(size.y), 8u));
			rd->texture_clear(rb->create_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_TILES, RD::DATA_FORMAT_R32_UINT, private_usage, RD::TEXTURE_SAMPLES_1, tiles, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
			rd->texture_clear(rb->create_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_SET_HEADER_0, RD::DATA_FORMAT_R32_UINT, private_usage, RD::TEXTURE_SAMPLES_1, size, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
			rd->texture_clear(rb->create_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_SET_HEADER_1, RD::DATA_FORMAT_R32_UINT, private_usage, RD::TEXTURE_SAMPLES_1, size, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
			rd->texture_clear(rb->create_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_SET_VALUES_0, RD::DATA_FORMAT_R32G32_UINT, private_usage, RD::TEXTURE_SAMPLES_1, size, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
			rd->texture_clear(rb->create_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_SET_VALUES_1, RD::DATA_FORMAT_R32G32_UINT, private_usage, RD::TEXTURE_SAMPLES_1, size, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
		}
		if (head_state) {
			rd->texture_clear(rb->create_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_HEAD_STATE_0, RD::DATA_FORMAT_R32_UINT, private_usage, RD::TEXTURE_SAMPLES_1, size, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
			rd->texture_clear(rb->create_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_HEAD_STATE_1, RD::DATA_FORMAT_R32_UINT, private_usage, RD::TEXTURE_SAMPLES_1, size, 1, 1, true), Color(0, 0, 0, 0), 0, 1, 0, 1);
		}
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
	const Projection prev_inverse = prev_projection.inverse();
	const bool continuous = history && viewport->last_frame != 0 && p_input.frame == viewport->last_frame + 1;
	if (viewport->last_frame != p_input.frame) {
		viewport->counter = advance_counter(viewport->counter, continuous);
		viewport->last_frame = p_input.frame;
		if (viewport->swap_sets) {
			viewport->history_set = 1 - viewport->history_set;
		}
		if (viewport->swap_head_state) {
			viewport->head_state_set = 1 - viewport->head_state_set;
		}
	}
	const bool spatial = reservoir_sets && settings.spatial_neighbors > 0;
	const bool feedback = spatial && settings.reservoirs == RESERVOIRS_U1;
	const StringName set_headers[2] = { RB_DI_SET_HEADER_0, RB_DI_SET_HEADER_1 };
	const StringName set_values[2] = { RB_DI_SET_VALUES_0, RB_DI_SET_VALUES_1 };
	const StringName head_states[2] = { RB_DI_HEAD_STATE_0, RB_DI_HEAD_STATE_1 };
	const uint32_t history_set = viewport->history_set;
	const uint32_t head_state_set = viewport->head_state_set;
	const RID tiles = reservoir_sets ? rb->get_texture(RB_SCOPE_DI_RESAMPLED, RB_DI_TILES) : header_dummy;
	const RID history_header = reservoir_sets ? rb->get_texture(RB_SCOPE_DI_RESAMPLED, set_headers[history_set]) : header_dummy;
	const RID history_values = reservoir_sets ? rb->get_texture(RB_SCOPE_DI_RESAMPLED, set_values[history_set]) : history_guide_dummy;
	const RID current_header = reservoir_sets ? rb->get_texture(RB_SCOPE_DI_RESAMPLED, set_headers[1 - history_set]) : header_dummy;
	const RID current_values = reservoir_sets ? rb->get_texture(RB_SCOPE_DI_RESAMPLED, set_values[1 - history_set]) : history_guide_dummy;
	const RID head_state_previous = head_state ? rb->get_texture(RB_SCOPE_DI_RESAMPLED, head_states[head_state_set]) : header_dummy;
	const RID head_state_next = head_state ? rb->get_texture(RB_SCOPE_DI_RESAMPLED, head_states[1 - head_state_set]) : header_dummy;
	Params params = {};
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			params.inv_projection[c * 4 + r] = float(inverse.columns[c][r]);
			params.prev_view_from_view[c * 4 + r] = float(prev_view_from_view.columns[c][r]);
			params.prev_projection[c * 4 + r] = float(prev_projection.columns[c][r]);
			params.prev_inv_projection[c * 4 + r] = float(prev_inverse.columns[c][r]);
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
	params.debug_flags = (settings.stale_prior ? DEBUG_FLAG_STALE_PRIOR : 0) | (counting ? DEBUG_FLAG_COUNT : 0) | (settings.no_stamp ? DEBUG_FLAG_NO_STAMP : 0);
	params.reservoirs = settings.reservoirs;
	params.c_full = settings.c_full;
	params.m_temporal_cap = settings.m_temporal_cap;
	params.dynamic_cap = settings.dynamic_cap;
	params.m_stored_cap = settings.m_stored_cap;
	params.spatial_neighbors = settings.spatial_neighbors;
	params.spatial_radius = settings.spatial_radius * float(size.y) / 1440.0f;
	params.counter = viewport->counter;
	params.head_rank = head_state ? 1 : 0;
	params.head_swap_margin = settings.head_swap_margin;
	params.head_swap_frames = settings.head_swap_frames;
	params.temporal_reuse = settings.temporal_reuse ? 1 : 0;
	params.head_window = settings.head_window ? 1 : 0;
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
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 13, tiles));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 14, history_header));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 15, history_values));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 16, current_header));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 17, current_values));
	if (lab) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 20, lab_buffer));
	}
	LocalVector<RD::Uniform> spatial_uniforms(uniforms);
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 12, debug_buffer));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 18, head_state_previous));
	uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 19, head_state_next));
	if (lab) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 21, lab_mask_buffer));
	}

	const RID shader_rid = shader->version_get_shader(shader_version, lab ? 2 : 0);
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, lab ? lab_pipeline.get_rid() : pipeline.get_rid());
	rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache_vec(shader_rid, 0, uniforms), 0);
	rd->compute_list_dispatch(list, Math::division_round_up(uint32_t(size.x), 8u), Math::division_round_up(uint32_t(size.y), 8u), 1);
	rd->compute_list_end();
	if (spatial) {
		const RID spatial_rid = shader->version_get_shader(shader_version, lab ? 3 : 1);
		list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, lab ? lab_spatial_pipeline.get_rid() : spatial_pipeline.get_rid());
		rd->compute_list_bind_uniform_set(list, UniformSetCacheRD::get_singleton()->get_cache_vec(spatial_rid, 0, spatial_uniforms), 0);
		rd->compute_list_dispatch(list, Math::division_round_up(uint32_t(size.x), 8u), Math::division_round_up(uint32_t(size.y), 8u), 1);
		rd->compute_list_end();
	}
	viewport->swap_sets = reservoir_sets && !feedback;
	viewport->swap_head_state = head_state;
	if (lab) {
		lab_pending = true;
		lab_frame = p_input.frame;
		lab_size = size;
	}
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
	sampler_lab.serve();
	if (p_render_buffers.is_null() || !p_render_buffers->has_custom_data(RB_SCOPE_DI_RESAMPLED)) {
		return;
	}
	Ref<ResampledViewportData> viewport = p_render_buffers->get_custom_data(RB_SCOPE_DI_RESAMPLED);
	if (viewport->tail_samples == 0) {
		return;
	}
	p_render_buffers->clear_context(RB_SCOPE_DI_RESAMPLED);
	viewport->size = Size2i();
	viewport->head = 0;
	viewport->tail_samples = 0;
}

void DirectResamplerRD::free_device_resources() {
	sampler_lab.free_device_resources();
	if (!device_ready) {
		return;
	}
	RD *rd = RD::get_singleton();
	pipeline.free();
	spatial_pipeline.free();
	lab_pipeline.free();
	lab_spatial_pipeline.free();
	if (shader_version.is_valid()) {
		shader->version_free(shader_version);
		shader_version = RID();
	}
	if (shader) {
		memdelete(shader);
		shader = nullptr;
	}
	_flush_debug_line();
	_write_lab_mask();
	_flush_lab_line();
	const RID owned[] = { params_buffer, debug_buffer, lab_buffer, lab_mask_buffer, history_slots_dummy, history_factors_dummy, history_guide_dummy, header_dummy };
	for (const RID &rid : owned) {
		if (rid.is_valid()) {
			rd->free_rid(rid);
		}
	}
	params_buffer = RID();
	debug_buffer = RID();
	lab_buffer = RID();
	lab_mask_buffer = RID();
	history_slots_dummy = RID();
	history_factors_dummy = RID();
	history_guide_dummy = RID();
	header_dummy = RID();
	device_ready = false;
}

DirectResamplerRD::~DirectResamplerRD() {
	ERR_FAIL_COND_MSG(device_ready, "The direct resampler's device resources were not freed before shutdown.");
}
