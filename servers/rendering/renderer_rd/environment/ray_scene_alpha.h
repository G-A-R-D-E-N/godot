/**************************************************************************/
/*  ray_scene_alpha.h                                                     */
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

#pragma once

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

	static constexpr uint32_t ENTRY_CULL_MASK = 3u;
	static constexpr uint32_t ENTRY_FAR_MODE_SHIFT = 2u;
	static constexpr uint32_t ENTRY_FAR_MODE_MASK = 3u;
	static constexpr uint32_t ENTRY_ALPHA_INVERT_SHIFT = 4u;
	static constexpr uint32_t ENTRY_ALPHA_INVERT_MASK = 1u << ENTRY_ALPHA_INVERT_SHIFT;
	static constexpr uint32_t LAYER_CUTOFF_INVERT = 0x100u;
	static constexpr uint32_t LAYER_CUTOFF_MASK = 0xFFu;

	struct SourceIdentity {
		RID rd;
		uint32_t width = 0;
		uint32_t height = 0;
		uint64_t data_version = 0;
		uint64_t observation = 0;

		bool operator==(const SourceIdentity &p_other) const {
			return rd == p_other.rd && width == p_other.width && height == p_other.height && data_version == p_other.data_version && observation == p_other.observation;
		}
	};

	struct FillRequest {
		uint64_t cost = 0;
		bool needs_near = false;
		bool needs_far = false;
	};

	static void plan_fills(const LocalVector<FillRequest> &p_requests, uint64_t p_budget, uint64_t p_used, uint32_t &r_cursor, LocalVector<uint32_t> &r_selected);

	struct EntryKey {
		float scale[2] = {};
		float offset[2] = {};
		uint32_t bits = 0;
		float far_gain = 0.0f;
		float far_level = 0.0f;
		float far_veil_alpha = 0.0f;
		float far_affine[2] = {};
	};

	struct TableEntry {
		uint32_t layer = 0;
		uint32_t state = 0;
		float scale[2] = {};
		float offset[2] = {};
		uint32_t bits = 0;
		float far_gain = 0.0f;
		float far_level = 0.0f;
		float far_veil_alpha = 0.0f;
		float far_affine[2] = {};
		float density = 0.0f;
		uint32_t far_state = 0;
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
		uint32_t far_invalid = 0;
	};

	typedef bool (*LivenessFunc)(RID p_texture);

	RayAlphaStore();
	~RayAlphaStore();

	void configure(uint32_t p_max_layers, uint32_t p_layer_size, uint64_t p_texels_per_frame);
	void release();

	void set_liveness(LivenessFunc p_liveness) { liveness = p_liveness; }
	void begin_update(uint64_t p_frame);
	static EntryKey canonical_key(const EntryKey &p_raw, bool &r_far_invalid);
	uint32_t acquire(RID p_source, uint32_t p_cutoff, const EntryKey &p_key);
	uint32_t acquire(RID p_source, uint32_t p_cutoff, const Vector2 &p_scale, const Vector2 &p_offset) {
		EntryKey key;
		key.scale[0] = float(p_scale.x);
		key.scale[1] = float(p_scale.y);
		key.offset[0] = float(p_offset.x);
		key.offset[1] = float(p_offset.y);
		return acquire(p_source, p_cutoff, key);
	}
	void update(bool p_refresh_dynamic = true);
	void commit_fill(uint32_t p_layer, uint64_t p_signature);
	bool layer_needs_far(uint32_t p_layer) const;
	void commit_far(uint32_t p_layer, uint64_t p_signature);
	void sync_entries();

	uint64_t observe_source(uint32_t p_layer, const SourceIdentity &p_identity);

	static bool uses_far(uint32_t p_bits) {
		const uint32_t mode = (p_bits >> ENTRY_FAR_MODE_SHIFT) & ENTRY_FAR_MODE_MASK;
		return mode == 1u || mode == 2u;
	}
	bool is_configured() const { return max_layers > 0; }
	bool has_bindable_textures() const { return textures_ok || fallback_layer_texture.is_valid(); }
	RID get_layer_texture() const { return layer_texture; }
	RID get_mean_texture() const { return mean_texture; }
	RID bind_layer_texture() const { return textures_ok ? layer_texture : fallback_layer_texture; }
	RID bind_mean_texture() const { return textures_ok ? mean_texture : fallback_mean_texture; }
	RID get_table_buffer() const { return table_buffer; }
	uint32_t get_layer_size() const { return layer_size; }
	const TableEntry &get_entry(uint32_t p_index) const { return entries[p_index]; }
	uint32_t get_entry_count() const { return entries.size(); }
	uint32_t get_entry_generation(uint32_t p_index) const { return entry_meta[p_index].generation; }
	uint32_t get_free_entry_count() const { return free_entries.size(); }
	uint32_t get_layer_count() const { return layers.size(); }
	uint32_t get_live_layer_count() const { return layers.size() - free_layers.size(); }
	RID get_layer_source(uint32_t p_layer) const { return layers[p_layer].source; }
	uint32_t get_layer_cutoff(uint32_t p_layer) const { return layers[p_layer].cutoff & LAYER_CUTOFF_MASK; }
	uint64_t get_content_revision(uint32_t p_entry) const {
		if (p_entry >= entries.size() || entries[p_entry].layer >= layers.size()) {
			return 0;
		}
		const Layer &layer = layers[entries[p_entry].layer];
		return layer.state == LAYER_READY ? layer.filled_signature : 0;
	}
	uint32_t get_layer_state(uint32_t p_layer) const { return layers[p_layer].state; }
	bool layer_requested(uint32_t p_layer) const { return layers[p_layer].last_observation == observation_revision; }
	uint32_t get_layer_generation(uint32_t p_layer) const { return layers[p_layer].generation; }
	uint32_t get_max_layers() const { return max_layers; }
	uint64_t get_change_serial() const { return change_serial; }
	const Stats &get_stats() const { return stats; }

private:
	struct Layer {
		RID source;
		SourceIdentity source_identity;
		uint32_t cutoff = 0;
		uint32_t state = LAYER_EMPTY;
		uint64_t signature = 0;
		uint64_t filled_signature = 0;
		uint64_t far_filled_signature = 0;
		uint32_t generation = 0;
		uint64_t last_used = 0;
		uint64_t last_observation = 0;
		bool far_wanted = false;
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
	static constexpr uint32_t MEAN_MIP_COUNT = 8;
	static constexpr uint32_t MEAN_SIZE = 1u << (MEAN_MIP_COUNT - 1);
	static constexpr uint32_t MEAN_MIP_READS = 4u * (MEAN_SIZE * MEAN_SIZE - 1u) / 3u;

	static bool _publish_far(TableEntry &r_entry, const Layer &p_layer);
	uint32_t _find_victim() const;
	void _release_layer(uint32_t p_layer);
	void _retire_entry(uint32_t p_entry);
	void _mark_changed();
	void _mark_state_changed();
	bool _ensure_pipeline();
	bool _ensure_texture();
	bool _ensure_fallback_textures();
	void _free_textures();
	void _free_fallback_textures();
	void _update_far_mips(const LocalVector<uint32_t> &p_pending);
	void _ensure_table_capacity(uint32_t p_count);
	void _upload_table();

	uint32_t max_layers = 0;
	uint32_t layer_size = 256;
	bool textures_ok = false;
	uint64_t texels_per_frame = 0;
	RID layer_texture;
	RID mean_texture;
	RID mean_mip_views[MEAN_MIP_COUNT];
	RID fallback_layer_texture;
	RID fallback_mean_texture;
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
	uint64_t source_revision = 0;
	uint64_t observation_revision = 0;
	uint32_t fill_cursor = 0;
	bool state_changed = false;
	uint64_t state_change_frame = 0;
	RaySceneAlphaFillShaderRD *fill_shader = nullptr;
	RID fill_version;
	RID fill_pipeline;
	RID mean_mip_pipeline;
	bool fill_created = false;
	Stats stats;
};

static_assert(sizeof(RayAlphaStore::EntryKey) == 40);
static_assert(sizeof(RayAlphaStore::TableEntry) == 56);
static_assert(offsetof(RayAlphaStore::TableEntry, bits) == 24);
static_assert(offsetof(RayAlphaStore::TableEntry, far_affine) == 40);
static_assert(offsetof(RayAlphaStore::TableEntry, density) == 48);
static_assert(offsetof(RayAlphaStore::TableEntry, far_state) == 52);
