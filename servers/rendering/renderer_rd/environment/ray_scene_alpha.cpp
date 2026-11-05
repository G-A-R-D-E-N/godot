/**************************************************************************/
/*  ray_scene_alpha.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                 */
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
	uint32_t pad0;
	uint32_t pad1;
	uint32_t pad2;
};

uint64_t _signature(RID p_rd, uint32_t p_width, uint32_t p_height) {
	return (p_rd.get_id() * 1000003ULL) ^ (uint64_t(p_width) << 40) ^ (uint64_t(p_height) << 20);
}

uint64_t _entry_hash(uint32_t p_layer, const Vector2 &p_scale, const Vector2 &p_offset) {
	const float values[4] = { float(p_scale.x), float(p_scale.y), float(p_offset.x), float(p_offset.y) };
	return hash_murmur3_buffer(values, sizeof(values), hash_murmur3_one_32(p_layer)) | (uint64_t(p_layer) << 32);
}

bool _finite_uv(const Vector2 &p_scale, const Vector2 &p_offset) {
	return Math::is_finite(float(p_scale.x)) && Math::is_finite(float(p_scale.y)) && Math::is_finite(float(p_offset.x)) && Math::is_finite(float(p_offset.y));
}

} // namespace

RayAlphaStore::RayAlphaStore() {
}

RayAlphaStore::~RayAlphaStore() {
	release();
}

void RayAlphaStore::configure(uint32_t p_max_layers, uint32_t p_layer_size, uint64_t p_texels_per_frame) {
	const uint32_t layers_wanted = CLAMP(p_max_layers, 16u, 2048u);
	const uint32_t size_wanted = p_layer_size >= 512u ? 512u : 256u;
	if (max_layers == layers_wanted && layer_size == size_wanted) {
		texels_per_frame = p_texels_per_frame;
		return;
	}
	release();
	max_layers = layers_wanted;
	layer_size = size_wanted;
	texels_per_frame = p_texels_per_frame;
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
	if (layer_texture.is_valid()) {
		RD::get_singleton()->free_rid(layer_texture);
		layer_texture = RID();
	}
	if (table_buffer.is_valid()) {
		RD::get_singleton()->free_rid(table_buffer);
		table_buffer = RID();
	}
	if (fill_created) {
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
	stats = Stats();
}

bool RayAlphaStore::_ensure_pipeline() {
	if (fill_created) {
		return fill_pipeline.is_valid();
	}
	fill_created = true;
	fill_shader = memnew(RaySceneAlphaFillShaderRD);
	Vector<String> modes;
	modes.push_back("\n");
	fill_shader->initialize(modes, "");
	fill_version = fill_shader->version_create();
	RID shader = fill_shader->version_get_shader(fill_version, 0);
	if (shader.is_valid()) {
		fill_pipeline = RD::get_singleton()->compute_pipeline_create(shader);
	}
	return fill_pipeline.is_valid();
}

bool RayAlphaStore::_ensure_texture() {
	if (layer_texture.is_valid()) {
		return true;
	}
	RD::TextureFormat format;
	format.format = RD::DATA_FORMAT_R8_UNORM;
	format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	format.width = layer_size;
	format.height = layer_size;
	format.depth = 1;
	format.array_layers = max_layers;
	format.mipmaps = 1;
	format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
	layer_texture = RD::get_singleton()->texture_create(format, RD::TextureView());
	if (layer_texture.is_valid()) {
		RD::get_singleton()->set_resource_name(layer_texture, "Ray Scene Alpha Layers");
	}
	return layer_texture.is_valid();
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
	table_dirty = true;
}

void RayAlphaStore::_retire_entry(uint32_t p_entry) {
	EntryMeta &meta = entry_meta[p_entry];
	const uint32_t *mapped = entry_map.getptr(meta.key);
	if (mapped != nullptr && *mapped == p_entry) {
		entry_map.erase(meta.key);
	}
	meta = EntryMeta();
	TableEntry &entry = entries[p_entry];
	entry.layer = ENTRY_RETIRED;
	entry.state = LAYER_EMPTY;
	entry.scale[0] = 0.0f;
	entry.scale[1] = 0.0f;
	entry.offset[0] = 0.0f;
	entry.offset[1] = 0.0f;
	free_entries.push_back(p_entry);
	stats.entries_retired_total++;
	table_dirty = true;
}

void RayAlphaStore::begin_update(uint64_t p_frame) {
	frame = p_frame;
	stats.layers_refused = 0;
	stats.dead_sources_refused = 0;
	if (max_layers == 0) {
		return;
	}
	if (liveness != nullptr) {
		for (uint32_t i = 0; i < layers.size(); i++) {
			if (layers[i].source.is_valid() && !liveness(layers[i].source)) {
				_release_layer(i);
				stats.layers_released_total++;
			}
		}
	}
	for (uint32_t i = 1; i < entries.size(); i++) {
		const TableEntry &entry = entries[i];
		if (entry.layer == ENTRY_RETIRED) {
			continue;
		}
		const EntryMeta &meta = entry_meta[i];
		if (entry.layer >= layers.size() || layers[entry.layer].source.is_null() || meta.generation != layers[entry.layer].generation || meta.last_used + 1 < frame) {
			_retire_entry(i);
		}
	}
}

uint32_t RayAlphaStore::acquire(RID p_source, uint32_t p_cutoff, const Vector2 &p_scale, const Vector2 &p_offset) {
	if (max_layers == 0 || p_source.is_null() || p_cutoff == 0 || p_cutoff > 255 || !_finite_uv(p_scale, p_offset)) {
		return 0;
	}
	const LayerKey layer_key = { p_source.get_id(), p_cutoff };
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
		fresh.cutoff = p_cutoff;
		layer_map.insert(layer_key, layer_index);
	}
	Layer &layer = layers[layer_index];
	layer.last_used = frame;

	const uint64_t entry_key = _entry_hash(layer_index, p_scale, p_offset);
	const uint32_t *mapped = entry_map.getptr(entry_key);
	if (mapped != nullptr) {
		const uint32_t found = *mapped;
		const TableEntry &existing = entries[found];
		if (existing.layer == layer_index && entry_meta[found].generation == layer.generation && existing.scale[0] == float(p_scale.x) && existing.scale[1] == float(p_scale.y) && existing.offset[0] == float(p_offset.x) && existing.offset[1] == float(p_offset.y)) {
			entry_meta[found].last_used = frame;
			return found;
		}
	}
	TableEntry entry;
	entry.layer = layer_index;
	entry.state = layer.state;
	entry.scale[0] = float(p_scale.x);
	entry.scale[1] = float(p_scale.y);
	entry.offset[0] = float(p_offset.x);
	entry.offset[1] = float(p_offset.y);
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
	table_dirty = true;
	return index;
}

void RayAlphaStore::update() {
	if (max_layers == 0) {
		return;
	}
	stats.fills_this_frame = 0;
	stats.fill_texels_this_frame = 0;
	stats.source_missing_frames = 0;
	if (!_ensure_texture()) {
		return;
	}

	TextureStorage *texture_storage = TextureStorage::get_singleton();
	LocalVector<uint32_t> pending;
	for (uint32_t i = 0; i < layers.size(); i++) {
		Layer &layer = layers[i];
		if (layer.source.is_null()) {
			continue;
		}
		const RID rd = texture_storage->texture_get_rd_texture(layer.source, false);
		if (rd.is_null()) {
			if (layer.state != LAYER_PENDING) {
				layer.state = LAYER_PENDING;
				table_dirty = true;
			}
			stats.source_missing_frames++;
			continue;
		}
		const RD::TextureFormat format = RD::get_singleton()->texture_get_format(rd);
		layer.source_rd = rd;
		layer.width = format.width;
		layer.height = format.height;
		layer.signature = _signature(rd, format.width, format.height);
		if (layer.signature != layer.filled_signature || layer.state != LAYER_READY) {
			if (layer.state != LAYER_PENDING) {
				layer.state = LAYER_PENDING;
				table_dirty = true;
			}
			pending.push_back(i);
		}
	}

	if (!pending.is_empty() && _ensure_pipeline()) {
		RID shader = fill_shader->version_get_shader(fill_version, 0);
		RID sampler = MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
		RD::ComputeListID list = RD::get_singleton()->compute_list_begin();
		RD::get_singleton()->compute_list_bind_compute_pipeline(list, fill_pipeline);
		for (uint32_t index : pending) {
			Layer &layer = layers[index];
			const uint64_t texels = uint64_t(layer.width) * uint64_t(layer.height);
			if (stats.fills_this_frame > 0 && stats.fill_texels_this_frame + texels > texels_per_frame) {
				break;
			}
			RD::Uniform u_source(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, Vector<RID>({ sampler, layer.source_rd }));
			RD::Uniform u_layers(RD::UNIFORM_TYPE_IMAGE, 1, layer_texture);
			RID set = UniformSetCacheRD::get_singleton()->get_cache(shader, 0, u_source, u_layers);
			RD::get_singleton()->compute_list_bind_uniform_set(list, set, 0);
			FillParams params = {};
			params.layer = index;
			params.layer_size = layer_size;
			params.source_width = layer.width;
			params.source_height = layer.height;
			params.cutoff = layer.cutoff;
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

	sync_entries();
	if (table_dirty) {
		_upload_table();
	}
}

void RayAlphaStore::commit_fill(uint32_t p_layer, uint64_t p_signature) {
	ERR_FAIL_UNSIGNED_INDEX(p_layer, layers.size());
	Layer &layer = layers[p_layer];
	if (layer.source.is_null()) {
		return;
	}
	layer.signature = p_signature;
	layer.filled_signature = p_signature;
	layer.state = LAYER_READY;
	table_dirty = true;
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
		if (entry.layer < layers.size() && entry.state != layers[entry.layer].state) {
			entry.state = layers[entry.layer].state;
			table_dirty = true;
		}
	}
}
