/**************************************************************************/
/*  ray_scene_alpha.h                                                     */
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

#ifndef RAY_SCENE_ALPHA_H
#define RAY_SCENE_ALPHA_H

#include "core/math/vector2.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/environment/ray_scene_alpha_fill.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RayAlphaStore {
public:
	enum LayerState : uint32_t {
		LAYER_EMPTY = 0,
		LAYER_PENDING = 1,
		LAYER_READY = 2,
	};

	static constexpr uint32_t ENTRY_RETIRED = 0xFFFFFFFF;

	struct TableEntry {
		uint32_t layer;
		uint32_t state;
		float scale[2];
		float offset[2];
	};

	struct Stats {
		uint32_t layers = 0;
		uint32_t layers_ready = 0;
		uint32_t layers_pending = 0;
		uint32_t layers_refused = 0;
		uint32_t table_entries = 0;
		uint32_t fills_this_frame = 0;
		uint64_t fill_texels_this_frame = 0;
		uint64_t fills_total = 0;
		uint64_t fill_texels_total = 0;
		uint64_t source_missing_frames = 0;
		uint32_t layer_slots = 0;
		uint32_t entries_free = 0;
		uint32_t dead_sources_refused = 0;
		uint64_t layers_released_total = 0;
		uint64_t layers_evicted_total = 0;
		uint64_t entries_retired_total = 0;
	};

	typedef bool (*LivenessFunc)(RID p_texture);

	RayAlphaStore();
	~RayAlphaStore();

	void configure(uint32_t p_max_layers, uint32_t p_layer_size, uint64_t p_texels_per_frame);
	void release();

	void set_liveness(LivenessFunc p_liveness) { liveness = p_liveness; }
	void begin_update(uint64_t p_frame);
	uint32_t acquire(RID p_source, uint32_t p_cutoff, const Vector2 &p_scale, const Vector2 &p_offset);
	void update();
	void commit_fill(uint32_t p_layer, uint64_t p_signature);
	void sync_entries();

	bool is_configured() const { return max_layers > 0; }
	RID get_layer_texture() const { return layer_texture; }
	RID get_table_buffer() const { return table_buffer; }
	uint32_t get_layer_size() const { return layer_size; }
	const TableEntry &get_entry(uint32_t p_index) const { return entries[p_index]; }
	uint32_t get_entry_count() const { return entries.size(); }
	uint32_t get_entry_generation(uint32_t p_index) const { return entry_meta[p_index].generation; }
	uint32_t get_free_entry_count() const { return free_entries.size(); }
	uint32_t get_layer_count() const { return layers.size(); }
	uint32_t get_live_layer_count() const { return layers.size() - free_layers.size(); }
	RID get_layer_source(uint32_t p_layer) const { return layers[p_layer].source; }
	uint32_t get_layer_cutoff(uint32_t p_layer) const { return layers[p_layer].cutoff; }
	uint32_t get_layer_state(uint32_t p_layer) const { return layers[p_layer].state; }
	uint32_t get_layer_generation(uint32_t p_layer) const { return layers[p_layer].generation; }
	uint32_t get_max_layers() const { return max_layers; }
	uint64_t get_change_serial() const { return change_serial; }
	const Stats &get_stats() const { return stats; }

private:
	struct Layer {
		RID source;
		RID source_rd;
		uint32_t cutoff = 0;
		uint32_t state = LAYER_EMPTY;
		uint32_t width = 0;
		uint32_t height = 0;
		uint64_t signature = 0;
		uint64_t filled_signature = 0;
		uint32_t generation = 0;
		uint64_t last_used = 0;
	};

	struct LayerKey {
		uint64_t id = 0;
		uint32_t cutoff = 0;

		bool operator==(const LayerKey &p_other) const { return id == p_other.id && cutoff == p_other.cutoff; }
	};

	struct LayerKeyHasher {
		static _FORCE_INLINE_ uint32_t hash(const LayerKey &p_key) { return hash_fmix32(hash_murmur3_one_32(p_key.cutoff, hash_murmur3_one_64(p_key.id))); }
	};

	struct EntryMeta {
		uint32_t generation = 0;
		uint64_t last_used = 0;
		uint64_t key = 0;
	};

	static constexpr uint32_t NO_LAYER = 0xFFFFFFFF;

	uint32_t _find_victim() const;
	void _release_layer(uint32_t p_layer);
	void _retire_entry(uint32_t p_entry);
	void _mark_changed();
	void _mark_state_changed();
	bool _ensure_pipeline();
	bool _ensure_texture();
	void _ensure_table_capacity(uint32_t p_count);
	void _upload_table();

	uint32_t max_layers = 0;
	uint32_t layer_size = 256;
	uint64_t texels_per_frame = 0;
	RID layer_texture;
	RID table_buffer;
	uint32_t table_capacity = 0;
	LocalVector<Layer> layers;
	HashMap<LayerKey, uint32_t, LayerKeyHasher> layer_map;
	LocalVector<uint32_t> free_layers;
	LocalVector<TableEntry> entries;
	LocalVector<EntryMeta> entry_meta;
	LocalVector<uint32_t> free_entries;
	HashMap<uint64_t, uint32_t> entry_map;
	uint64_t frame = 0;
	LivenessFunc liveness = nullptr;
	bool table_dirty = true;
	uint64_t change_serial = 0;
	bool state_changed = false;
	uint64_t state_change_frame = 0;
	RaySceneAlphaFillShaderRD *fill_shader = nullptr;
	RID fill_version;
	RID fill_pipeline;
	bool fill_created = false;
	Stats stats;
};

#endif
