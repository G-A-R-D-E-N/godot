/**************************************************************************/
/*  ray_scene_alpha.cpp                                                   */
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

#include "ray_scene_alpha.h"

#include "core/templates/hashfuncs.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"

using namespace RendererRD;

namespace {

struct FillParams {
	uint32_t layer;
	uint32_t layer_size;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t cutoff;
	uint32_t invert;
	uint32_t pad1;
	uint32_t pad2;
};

uint32_t _layer_cutoff_k(uint32_t p_cutoff) {
	return p_cutoff & RayAlphaStore::LAYER_CUTOFF_MASK;
}

bool _layer_cutoff_invert(uint32_t p_cutoff) {
	return (p_cutoff & RayAlphaStore::LAYER_CUTOFF_INVERT) != 0;
}

uint32_t _pack_layer_cutoff(uint32_t p_cutoff, bool p_invert) {
	return p_cutoff | (p_invert ? RayAlphaStore::LAYER_CUTOFF_INVERT : 0u);
}

uint64_t _entry_hash(uint32_t p_layer, const RayAlphaStore::EntryKey &p_key) {
	return hash_murmur3_buffer(&p_key, sizeof(RayAlphaStore::EntryKey), hash_murmur3_one_32(p_layer)) | (uint64_t(p_layer) << 32);
}

bool _finite_uv(const RayAlphaStore::EntryKey &p_key) {
	return Math::is_finite(p_key.scale[0]) && Math::is_finite(p_key.scale[1]) && Math::is_finite(p_key.offset[0]) && Math::is_finite(p_key.offset[1]);
}

bool _same_key(const RayAlphaStore::TableEntry &p_entry, const RayAlphaStore::EntryKey &p_key) {
	return p_entry.scale[0] == p_key.scale[0] && p_entry.scale[1] == p_key.scale[1] && p_entry.offset[0] == p_key.offset[0] && p_entry.offset[1] == p_key.offset[1] && p_entry.bits == p_key.bits && p_entry.far_gain == p_key.far_gain && p_entry.far_level == p_key.far_level && p_entry.far_veil_alpha == p_key.far_veil_alpha && p_entry.far_affine[0] == p_key.far_affine[0] && p_entry.far_affine[1] == p_key.far_affine[1];
}

float _positive_zero(float p_value) {
	if (p_value == 0.0f) {
		p_value = 0.0f;
	}
	return p_value;
}

float _layer_density(uint32_t p_width, uint32_t p_height) {
	if (p_width == 0 || p_height == 0) {
		return 0.0f;
	}
	return 0.5f * (Math::log2(float(p_width)) + Math::log2(float(p_height)));
}

} // namespace

RayAlphaStore::RayAlphaStore() {
}

RayAlphaStore::~RayAlphaStore() {
	release();
}

void RayAlphaStore::configure(uint32_t p_max_layers, uint32_t p_layer_size, uint64_t p_texels_per_frame) {
	uint32_t layers_wanted = CLAMP(p_max_layers, 16u, 2048u);
	if (RD::get_singleton() != nullptr) {
		layers_wanted = MIN(layers_wanted, uint32_t(RD::get_singleton()->limit_get(RD::LIMIT_MAX_TEXTURE_ARRAY_LAYERS)));
	}
	const uint32_t size_wanted = p_layer_size >= 512u ? 512u : 256u;
	if (max_layers == layers_wanted && layer_size == size_wanted) {
		texels_per_frame = p_texels_per_frame;
		_ensure_fallback_textures();
		return;
	}
	release();
	max_layers = layers_wanted;
	layer_size = size_wanted;
	texels_per_frame = p_texels_per_frame;
	_ensure_fallback_textures();
	entries.clear();
	entries.push_back(TableEntry());
	entries[0].layer = 0;
	entries[0].state = LAYER_EMPTY;
	entry_meta.clear();
	entry_meta.push_back(EntryMeta());
	free_layers.clear();
	free_entries.clear();
	table_dirty = true;
}

void RayAlphaStore::release() {
	if (max_layers > 0) {
		change_serial++;
	}
	_free_textures();
	_free_fallback_textures();
	if (table_buffer.is_valid()) {
		RD::get_singleton()->free_rid(table_buffer);
		table_buffer = RID();
	}
	if (fill_created) {
		if (mean_mip_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(mean_mip_pipeline);
			mean_mip_pipeline = RID();
		}
		if (fill_pipeline.is_valid()) {
			RD::get_singleton()->free_rid(fill_pipeline);
			fill_pipeline = RID();
		}
		fill_shader->version_free(fill_version);
		memdelete(fill_shader);
		fill_shader = nullptr;
		fill_version = RID();
		fill_created = false;
	}
	max_layers = 0;
	table_capacity = 0;
	layers.clear();
	layer_map.clear();
	free_layers.clear();
	entries.clear();
	entry_meta.clear();
	free_entries.clear();
	entry_map.clear();
	table_dirty = true;
	textures_ok = false;
	stats = Stats();
	fill_cursor = 0;
}

bool RayAlphaStore::_ensure_pipeline() {
	if (fill_created) {
		return fill_pipeline.is_valid() && mean_mip_pipeline.is_valid();
	}
	fill_created = true;
	fill_shader = memnew(RaySceneAlphaFillShaderRD);
	Vector<String> modes;
	modes.push_back("\n");
	modes.push_back("\n#define MODE_MEAN_MIP\n");
	fill_shader->initialize(modes, "");
	fill_version = fill_shader->version_create();
	RID shader = fill_shader->version_get_shader(fill_version, 0);
	if (shader.is_valid()) {
		fill_pipeline = RD::get_singleton()->compute_pipeline_create(shader);
	}
	RID mip_shader = fill_shader->version_get_shader(fill_version, 1);
	if (mip_shader.is_valid()) {
		mean_mip_pipeline = RD::get_singleton()->compute_pipeline_create(mip_shader);
	}
	return fill_pipeline.is_valid() && mean_mip_pipeline.is_valid();
}

void RayAlphaStore::_free_fallback_textures() {
	if (fallback_layer_texture.is_valid()) {
		RD::get_singleton()->free_rid(fallback_layer_texture);
		fallback_layer_texture = RID();
	}
	if (fallback_mean_texture.is_valid()) {
		RD::get_singleton()->free_rid(fallback_mean_texture);
		fallback_mean_texture = RID();
	}
}

bool RayAlphaStore::_ensure_fallback_textures() {
	if (fallback_layer_texture.is_valid() && fallback_mean_texture.is_valid()) {
		return true;
	}
	RD *rd = RD::get_singleton();
	if (rd == nullptr) {
		return false;
	}
	_free_fallback_textures();
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R8_UNORM;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.width = 1;
	format.height = 1;
	format.depth = 1;
	format.array_layers = 1;
	format.mipmaps = 1;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	fallback_layer_texture = rd->texture_create(format, RD::TextureView());
	if (fallback_layer_texture.is_null()) {
		return false;
	}
	format.mipmaps = MEAN_MIP_COUNT;
	fallback_mean_texture = rd->texture_create(format, RD::TextureView());
	if (fallback_mean_texture.is_null()) {
		_free_fallback_textures();
		return false;
	}
	const Vector<uint8_t> white{ 255 };
	rd->texture_update(fallback_layer_texture, 0, white);
	rd->texture_update(fallback_mean_texture, 0, white);
	rd->set_resource_name(fallback_layer_texture, "Ray Scene Alpha Layers Fallback");
	rd->set_resource_name(fallback_mean_texture, "Ray Scene Alpha Mean Fallback");
	return true;
}

void RayAlphaStore::_free_textures() {
	textures_ok = false;
	for (RID &view : mean_mip_views) {
		if (view.is_valid()) {
			RD::get_singleton()->free_rid(view);
			view = RID();
		}
	}
	if (mean_texture.is_valid()) {
		RD::get_singleton()->free_rid(mean_texture);
		mean_texture = RID();
	}
	if (layer_texture.is_valid()) {
		RD::get_singleton()->free_rid(layer_texture);
		layer_texture = RID();
	}
}

bool RayAlphaStore::_ensure_texture() {
	if (layer_texture.is_valid()) {
		return true;
	}
	RD *rd = RD::get_singleton();
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R8_UNORM;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.width = layer_size;
	format.height = layer_size;
	format.depth = 1;
	format.array_layers = max_layers;
	format.mipmaps = 1;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	layer_texture = rd->texture_create(format, RD::TextureView());
	if (layer_texture.is_null()) {
		return false;
	}
	format.width = MEAN_SIZE;
	format.height = MEAN_SIZE;
	format.mipmaps = MEAN_MIP_COUNT;
	mean_texture = rd->texture_create(format, RD::TextureView());
	if (mean_texture.is_null()) {
		_free_textures();
		return false;
	}
	for (uint32_t mip = 0; mip < MEAN_MIP_COUNT; mip++) {
		mean_mip_views[mip] = rd->texture_create_shared_from_slice(RD::TextureView(), mean_texture, 0, mip, 1, RD::TEXTURE_SLICE_2D_ARRAY, max_layers);
		if (mean_mip_views[mip].is_null()) {
			_free_textures();
			return false;
		}
	}
	rd->set_resource_name(layer_texture, "Ray Scene Alpha Layers");
	rd->set_resource_name(mean_texture, "Ray Scene Alpha Mean");
	textures_ok = true;
	return true;
}

void RayAlphaStore::_ensure_table_capacity(uint32_t p_count) {
	if (table_buffer.is_valid() && table_capacity >= p_count) {
		return;
	}
	if (table_buffer.is_valid()) {
		RD::get_singleton()->free_rid(table_buffer);
	}
	table_capacity = MAX(MAX(p_count, table_capacity * 2), 64u);
	table_buffer = RD::get_singleton()->storage_buffer_create(table_capacity * sizeof(TableEntry));
	RD::get_singleton()->set_resource_name(table_buffer, "Ray Scene Alpha Table");
}

void RayAlphaStore::_upload_table() {
	_ensure_table_capacity(entries.size());
	RD::get_singleton()->buffer_update(table_buffer, 0, entries.size() * sizeof(TableEntry), entries.ptr());
	table_dirty = false;
}

uint32_t RayAlphaStore::_find_victim() const {
	uint32_t victim = NO_LAYER;
	for (uint32_t i = 0; i < layers.size(); i++) {
		const Layer &layer = layers[i];
		if (layer.source.is_null() || layer.last_used + 1 >= frame) {
			continue;
		}
		if (victim == NO_LAYER || layer.last_used < layers[victim].last_used) {
			victim = i;
		}
	}
	return victim;
}

void RayAlphaStore::_release_layer(uint32_t p_layer) {
	Layer &layer = layers[p_layer];
	layer_map.erase(LayerKey{ layer.source.get_id(), layer.cutoff });
	const uint32_t generation = layer.generation + 1;
	layer = Layer();
	layer.generation = generation;
	free_layers.push_back(p_layer);
	_mark_changed();
}

void RayAlphaStore::_retire_entry(uint32_t p_entry) {
	EntryMeta &meta = entry_meta[p_entry];
	const uint32_t *mapped = entry_map.getptr(meta.key);
	if (mapped != nullptr && *mapped == p_entry) {
		entry_map.erase(meta.key);
	}
	meta = EntryMeta();
	TableEntry &entry = entries[p_entry];
	entry = TableEntry();
	entry.layer = ENTRY_RETIRED;
	free_entries.push_back(p_entry);
	stats.entries_retired_total++;
	_mark_changed();
}

void RayAlphaStore::_mark_changed() {
	table_dirty = true;
	change_serial++;
}

void RayAlphaStore::_mark_state_changed() {
	_mark_changed();
	state_changed = true;
	state_change_frame = frame;
}

bool RayAlphaStore::_publish_far(TableEntry &r_entry, const Layer &p_layer) {
	const float density = _layer_density(p_layer.source_identity.width, p_layer.source_identity.height);
	const uint32_t far_state = p_layer.state == LAYER_READY && p_layer.far_filled_signature == p_layer.filled_signature ? LAYER_READY : LAYER_EMPTY;
	if (r_entry.density == density && r_entry.far_state == far_state) {
		return false;
	}
	r_entry.density = density;
	r_entry.far_state = far_state;
	return true;
}

void RayAlphaStore::begin_update(uint64_t p_frame) {
	if (++observation_revision == 0) {
		++observation_revision;
	}
	if (p_frame != frame) {
		stats.fills_this_frame = 0;
		stats.fill_texels_this_frame = 0;
	}
	frame = p_frame;
	stats.layers_refused = 0;
	stats.dead_sources_refused = 0;
	stats.far_invalid = 0;
	if (max_layers == 0) {
		return;
	}
	if (state_changed && p_frame > state_change_frame) {
		state_changed = false;
		change_serial++;
	}
	for (uint32_t i = 0; i < layers.size(); i++) {
		layers[i].far_wanted = false;
		if (liveness != nullptr && layers[i].source.is_valid() && !liveness(layers[i].source)) {
			_release_layer(i);
			stats.layers_released_total++;
		}
	}
	for (uint32_t i = 1; i < entries.size(); i++) {
		TableEntry &entry = entries[i];
		if (entry.layer == ENTRY_RETIRED) {
			continue;
		}
		const EntryMeta &meta = entry_meta[i];
		if (entry.layer >= layers.size() || layers[entry.layer].source.is_null() || meta.generation != layers[entry.layer].generation || meta.last_used + 1 < frame) {
			_retire_entry(i);
		} else if (uses_far(entry.bits) && _publish_far(entry, layers[entry.layer])) {
			_mark_state_changed();
		}
	}
}

RayAlphaStore::EntryKey RayAlphaStore::canonical_key(const EntryKey &p_raw, bool &r_far_invalid) {
	r_far_invalid = false;
	EntryKey key = p_raw;
	uint32_t cull = p_raw.bits & ENTRY_CULL_MASK;
	if (cull != uint32_t(RSE::CULL_MODE_FRONT) && cull != uint32_t(RSE::CULL_MODE_BACK)) {
		cull = uint32_t(RSE::CULL_MODE_DISABLED);
	}
	uint32_t mode = (p_raw.bits >> ENTRY_FAR_MODE_SHIFT) & ENTRY_FAR_MODE_MASK;
	if (mode != 1u && mode != 2u) {
		mode = 0;
	}
	if (mode != 0 && !(Math::is_finite(key.far_gain) && Math::is_finite(key.far_level) && Math::is_finite(key.far_veil_alpha) && Math::is_finite(key.far_affine[0]) && Math::is_finite(key.far_affine[1]))) {
		mode = 0;
		r_far_invalid = true;
	}
	if (mode == 0) {
		key.far_gain = 0.0f;
		key.far_level = 0.0f;
		key.far_veil_alpha = 0.0f;
		key.far_affine[0] = 0.0f;
		key.far_affine[1] = 0.0f;
	} else {
		key.far_gain = MAX(key.far_gain, 0.0f);
		key.far_veil_alpha = CLAMP(key.far_veil_alpha, 0.0f, 1.0f);
	}
	key.bits = cull | (mode << ENTRY_FAR_MODE_SHIFT) | (p_raw.bits & ENTRY_ALPHA_INVERT_MASK);
	key.far_gain = _positive_zero(key.far_gain);
	key.far_level = _positive_zero(key.far_level);
	key.far_veil_alpha = _positive_zero(key.far_veil_alpha);
	key.far_affine[0] = _positive_zero(key.far_affine[0]);
	key.far_affine[1] = _positive_zero(key.far_affine[1]);
	return key;
}

uint32_t RayAlphaStore::acquire(RID p_source, uint32_t p_cutoff, const EntryKey &p_key) {
	if (max_layers == 0 || p_source.is_null() || p_cutoff > 255 || !_finite_uv(p_key)) {
		return 0;
	}
	if (p_cutoff == 0 && (p_key.bits & ENTRY_ALPHA_INVERT_MASK) == 0) {
		return 0;
	}
	bool far_invalid = false;
	const EntryKey key = canonical_key(p_key, far_invalid);
	if (far_invalid) {
		stats.far_invalid++;
	}
	const bool invert = (key.bits & ENTRY_ALPHA_INVERT_MASK) != 0;
	const LayerKey layer_key = { p_source.get_id(), _pack_layer_cutoff(p_cutoff, invert) };
	uint32_t layer_index = 0;
	if (const uint32_t *found = layer_map.getptr(layer_key)) {
		layer_index = *found;
	} else {
		if (liveness != nullptr && !liveness(p_source)) {
			stats.dead_sources_refused++;
			return 0;
		}
		if (free_layers.is_empty() && layers.size() >= max_layers) {
			const uint32_t victim = _find_victim();
			if (victim == NO_LAYER) {
				stats.layers_refused++;
				return 0;
			}
			_release_layer(victim);
			for (uint32_t i = 1; i < entries.size(); i++) {
				if (entries[i].layer == victim) {
					_retire_entry(i);
				}
			}
			stats.layers_evicted_total++;
		}
		if (!free_layers.is_empty()) {
			layer_index = free_layers[free_layers.size() - 1];
			free_layers.remove_at(free_layers.size() - 1);
		} else {
			layer_index = layers.size();
			layers.push_back(Layer());
		}
		Layer &fresh = layers[layer_index];
		fresh.source = p_source;
		fresh.cutoff = layer_key.cutoff;
		layer_map.insert(layer_key, layer_index);
	}
	Layer &layer = layers[layer_index];
	layer.last_used = frame;
	layer.last_observation = observation_revision;
	const bool wants_far = uses_far(key.bits);
	if (wants_far) {
		layer.far_wanted = true;
	}

	const uint64_t entry_key = _entry_hash(layer_index, key);
	const uint32_t *mapped = entry_map.getptr(entry_key);
	if (mapped != nullptr) {
		const uint32_t found = *mapped;
		const TableEntry &existing = entries[found];
		if (existing.layer == layer_index && entry_meta[found].generation == layer.generation && _same_key(existing, key)) {
			entry_meta[found].last_used = frame;
			return found;
		}
	}
	TableEntry entry = {};
	entry.layer = layer_index;
	entry.state = layer.state;
	entry.scale[0] = key.scale[0];
	entry.scale[1] = key.scale[1];
	entry.offset[0] = key.offset[0];
	entry.offset[1] = key.offset[1];
	entry.bits = key.bits;
	entry.far_gain = key.far_gain;
	entry.far_level = key.far_level;
	entry.far_veil_alpha = key.far_veil_alpha;
	entry.far_affine[0] = key.far_affine[0];
	entry.far_affine[1] = key.far_affine[1];
	if (wants_far) {
		_publish_far(entry, layer);
	}
	EntryMeta meta;
	meta.generation = layer.generation;
	meta.last_used = frame;
	meta.key = entry_key;
	uint32_t index = 0;
	if (!free_entries.is_empty()) {
		index = free_entries[free_entries.size() - 1];
		free_entries.remove_at(free_entries.size() - 1);
		entries[index] = entry;
		entry_meta[index] = meta;
	} else {
		index = entries.size();
		entries.push_back(entry);
		entry_meta.push_back(meta);
	}
	if (mapped == nullptr) {
		entry_map.insert(entry_key, index);
	}
	_mark_changed();
	return index;
}

uint64_t RayAlphaStore::observe_source(uint32_t p_layer, const SourceIdentity &p_identity) {
	ERR_FAIL_UNSIGNED_INDEX_V(p_layer, layers.size(), 0);
	Layer &layer = layers[p_layer];
	if (!(layer.source_identity == p_identity)) {
		layer.source_identity = p_identity;
		if (p_identity.rd.is_valid()) {
			if (++source_revision == 0) {
				++source_revision;
			}
			layer.signature = source_revision;
		}
		if (layer.state != LAYER_PENDING) {
			layer.state = LAYER_PENDING;
			_mark_state_changed();
		}
	}
	return layer.signature;
}

void RayAlphaStore::plan_fills(const LocalVector<FillRequest> &p_requests, uint64_t p_budget, uint64_t p_used, uint32_t &r_cursor, LocalVector<uint32_t> &r_selected) {
	r_selected.clear();
	if (p_requests.is_empty()) {
		r_cursor = 0;
		return;
	}
	const uint32_t count = p_requests.size();
	const uint32_t start = r_cursor % count;
	uint64_t remaining = p_budget > p_used ? p_budget - p_used : 0;
	for (uint32_t n = 0; n < count; n++) {
		const uint32_t index = (start + n) % count;
		const FillRequest &request = p_requests[index];
		if (!request.needs_near && !request.needs_far) {
			continue;
		}
		if (request.cost > remaining && (p_used != 0 || !r_selected.is_empty())) {
			r_cursor = index;
			return;
		}
		r_selected.push_back(index);
		remaining = request.cost < remaining ? remaining - request.cost : 0;
		r_cursor = (index + 1u) % count;
	}
}

void RayAlphaStore::update(bool p_refresh_dynamic) {
	if (max_layers == 0) {
		return;
	}
	stats.source_missing_frames = 0;
	if (!_ensure_texture()) {
		_ensure_fallback_textures();
	}

	TextureStorage *texture_storage = TextureStorage::get_singleton();
	LocalVector<FillRequest> requests;
	requests.resize(layers.size());
	for (uint32_t i = 0; i < layers.size(); i++) {
		Layer &layer = layers[i];
		if (layer.source.is_null()) {
			continue;
		}
		const bool dynamic = texture_storage->texture_has_external_writes(layer.source);
		if (dynamic && (!p_refresh_dynamic || !layer_requested(i))) {
			continue;
		}
		const RID rd = texture_storage->texture_get_rd_texture(layer.source, false);
		if (rd.is_null()) {
			observe_source(i, SourceIdentity{ RID(), 0, 0, 0, dynamic ? observation_revision : 0 });
			if (layer.state != LAYER_PENDING) {
				layer.state = LAYER_PENDING;
				_mark_state_changed();
			}
			stats.source_missing_frames++;
			continue;
		}
		const RD::TextureFormat format = RD::get_singleton()->texture_get_format(rd);
		const uint64_t observation = dynamic ? observation_revision : 0;
		const SourceIdentity identity{ rd, format.width, format.height, texture_storage->texture_get_data_version(layer.source), observation };
		observe_source(i, identity);
		FillRequest &request = requests[i];
		request.needs_near = layer.signature != layer.filled_signature || layer.state != LAYER_READY;
		if (request.needs_near && layer.state != LAYER_PENDING) {
			layer.state = LAYER_PENDING;
			_mark_state_changed();
		}
		request.needs_far = layer.far_wanted && (request.needs_near || layer_needs_far(i));
		request.cost = (request.needs_near ? uint64_t(format.width) * uint64_t(format.height) : 0) + (request.needs_far ? MEAN_MIP_READS : 0);
	}

	LocalVector<uint32_t> selected;
	plan_fills(requests, texels_per_frame, stats.fill_texels_this_frame, fill_cursor, selected);
	if (!selected.is_empty() && _ensure_pipeline()) {
		RID shader = fill_shader->version_get_shader(fill_version, 0);
		RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
		RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(list, fill_pipeline);
		for (uint32_t index : selected) {
			if (!requests[index].needs_near) {
				continue;
			}
			Layer &layer = layers[index];
			const uint64_t texels = uint64_t(layer.source_identity.width) * uint64_t(layer.source_identity.height);
			RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, layer.source_identity.rd }));
			RD::Uniform u_layers(RD::UNIFORM_TYPE_IMAGE, 1, layer_texture);
			RD::Uniform u_mean(RD::UNIFORM_TYPE_IMAGE, 2, mean_mip_views[0]);
			RID set = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_source, u_layers, u_mean);
			if (set.is_null()) {
				continue;
			}
			RD::get_singleton()->compute_list_bind_uniform_set(list, set, 0);
			FillParams params = {};
			params.layer = index;
			params.layer_size = layer_size;
			params.source_width = layer.source_identity.width;
			params.source_height = layer.source_identity.height;
			params.cutoff = _layer_cutoff_k(layer.cutoff);
			params.invert = _layer_cutoff_invert(layer.cutoff) ? 1u : 0u;
			RD::get_singleton()->compute_list_set_push_constant(list, &params, sizeof(params));
			const uint32_t groups = layer_size / 8u;
			RD::get_singleton()->compute_list_dispatch(list, groups, groups, 1);
			commit_fill(index, layer.signature);
			stats.fills_this_frame++;
			stats.fill_texels_this_frame += texels;
			stats.fills_total++;
			stats.fill_texels_total += texels;
		}
		RD::get_singleton()->compute_list_end();
	}

	LocalVector<uint32_t> far_pending;
	for (uint32_t index : selected) {
		if (requests[index].needs_far && layer_needs_far(index)) {
			far_pending.push_back(index);
		}
	}
	_update_far_mips(far_pending);
	sync_entries();
	if (table_dirty) {
		_upload_table();
	}
}

bool RayAlphaStore::layer_needs_far(uint32_t p_layer) const {
	ERR_FAIL_UNSIGNED_INDEX_V(p_layer, layers.size(), false);
	const Layer &layer = layers[p_layer];
	return layer.source.is_valid() && layer.state == LAYER_READY && layer.far_wanted && layer.far_filled_signature != layer.filled_signature;
}

void RayAlphaStore::_update_far_mips(const LocalVector<uint32_t> &p_pending) {
	if (p_pending.is_empty() || !_ensure_pipeline()) {
		return;
	}

	const RID shader = fill_shader->version_get_shader(fill_version, 1);
	RID sets[MEAN_MIP_COUNT - 1];
	for (uint32_t mip = 1; mip < MEAN_MIP_COUNT; mip++) {
		RD::Uniform u_source(RD::UNIFORM_TYPE_IMAGE, 0, mean_mip_views[mip - 1]);
		RD::Uniform u_dest(RD::UNIFORM_TYPE_IMAGE, 1, mean_mip_views[mip]);
		sets[mip - 1] = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_source, u_dest);
		if (sets[mip - 1].is_null()) {
			return;
		}
	}

	RD *rd = RD::get_singleton();
	RD::ComputeListID list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, mean_mip_pipeline);
	for (uint32_t mip = 1; mip < MEAN_MIP_COUNT; mip++) {
		if (mip > 1) {
			rd->compute_list_add_barrier(list);
		}
		rd->compute_list_bind_uniform_set(list, sets[mip - 1], 0);
		FillParams params = {};
		params.layer_size = MEAN_SIZE >> mip;
		const uint32_t groups = (params.layer_size + 7u) / 8u;
		for (uint32_t index : p_pending) {
			params.layer = index;
			rd->compute_list_set_push_constant(list, &params, sizeof(params));
			rd->compute_list_dispatch(list, groups, groups, 1);
		}
	}
	rd->compute_list_end();

	for (uint32_t index : p_pending) {
		commit_far(index, layers[index].filled_signature);
	}
	const uint64_t added = uint64_t(p_pending.size()) * MEAN_MIP_READS;
	stats.fill_texels_this_frame += added;
	stats.fill_texels_total += added;
}

void RayAlphaStore::commit_fill(uint32_t p_layer, uint64_t p_signature) {
	ERR_FAIL_UNSIGNED_INDEX(p_layer, layers.size());
	Layer &layer = layers[p_layer];
	if (layer.source.is_null()) {
		return;
	}
	const bool changed = layer.state != LAYER_READY || layer.filled_signature != p_signature;
	layer.signature = p_signature;
	layer.filled_signature = p_signature;
	layer.state = LAYER_READY;
	if (changed) {
		_mark_state_changed();
	}
}

void RayAlphaStore::commit_far(uint32_t p_layer, uint64_t p_signature) {
	ERR_FAIL_UNSIGNED_INDEX(p_layer, layers.size());
	Layer &layer = layers[p_layer];
	if (layer.source.is_valid() && layer.state == LAYER_READY && layer.filled_signature == p_signature) {
		layer.far_filled_signature = p_signature;
	}
}

void RayAlphaStore::sync_entries() {
	stats.layers = get_live_layer_count();
	stats.layer_slots = layers.size();
	stats.layers_ready = 0;
	stats.layers_pending = 0;
	for (const Layer &layer : layers) {
		if (layer.source.is_null()) {
			continue;
		}
		stats.layers_ready += layer.state == LAYER_READY ? 1 : 0;
		stats.layers_pending += layer.state != LAYER_READY ? 1 : 0;
	}
	stats.table_entries = entries.size();
	stats.entries_free = free_entries.size();

	for (uint32_t i = 1; i < entries.size(); i++) {
		TableEntry &entry = entries[i];
		if (entry.layer < layers.size()) {
			const Layer &layer = layers[entry.layer];
			if (entry.state != layer.state) {
				entry.state = layer.state;
				_mark_state_changed();
			}
			if (layer.source_identity.observation != 0 && uses_far(entry.bits) && _publish_far(entry, layer)) {
				_mark_state_changed();
			}
		}
	}
}
