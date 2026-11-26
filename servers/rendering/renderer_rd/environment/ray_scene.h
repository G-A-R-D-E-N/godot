/**************************************************************************/
/*  ray_scene.h                                                           */
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

#include "core/io/file_access.h"
#include "core/math/aabb.h"
#include "core/math/basis.h"
#include "core/math/transform_3d.h"
#include "core/math/vector2.h"
#include "core/math/vector3i.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/paged_array.h"
#include "core/templates/safe_refcount.h"
#include "servers/rendering/renderer_rd/environment/gi_epochs.h"
#include "servers/rendering/renderer_rd/environment/ray_scene_alpha.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/rendering_server_types.h"

class RenderGeometryInstance;

namespace RendererRD {

class RayAccel;

class RayScene {
public:
	enum Backend {
		BACKEND_SOFTWARE,
		BACKEND_HARDWARE,
		BACKEND_AUTO,
	};

	static constexpr uint32_t BLAS_LEAF_SIZE = 4;
	static constexpr uint32_t TLAS_LEAF_SIZE = 1;
	static constexpr uint32_t BLAS_MAX_DEPTH = 24;
	static constexpr uint32_t TLAS_MAX_DEPTH = 24;
	static constexpr uint32_t MAX_LIGHTS = 256;
	static constexpr uint32_t RESERVED_AREA_LIGHTS = 16;
	static constexpr uint32_t MAX_CANDIDATE_LIGHTS = 16384;
	static constexpr uint32_t MAX_TRIANGLE_EMITTERS = 1u << 20;
	static constexpr uint32_t MIN_LIGHTS = 16;
	static constexpr uint32_t PICK_CELL_SLOTS = 4;
	static constexpr double ANCHOR_GRID = 64.0;

	static bool emission_has_value(const Color &p_emission);

	enum FeedConsumer : uint32_t {
		FEED_MODULE = 16,
		FEED_DEBUG = 32,
	};

	static constexpr uint32_t FEED_HISTORY_FRAMES = 8;
	static constexpr uint32_t FEED_RAW_LIMIT = 4096;
	static constexpr uint32_t FEED_EPOCH_HISTORY = 32;

	struct DirtyFeed {
		LocalVector<AABB> boxes;
		LocalVector<AABB> mover_boxes;
		bool overflow = false;
		bool baseline = false;
		uint32_t structure_generation = 0;
		uint32_t serial = 0;
		uint32_t changes = 0;
		uint32_t tracked = 0;
		uint32_t mover_count = 0;
	};

	enum LightPickMode {
		LIGHT_PICK_CAMERA,
		LIGHT_PICK_STABLE,
	};

	enum MaterialFlags {
		MATERIAL_FLAG_TWO_SIDED = 1,
		MATERIAL_FLAG_SKIP = 2,
		MATERIAL_FLAG_BLEND = 4,
		MATERIAL_FLAG_SHADOW_PASS = 8,
		MATERIAL_FLAG_SHADOW_ONLY = 16,
		MATERIAL_FLAG_SHADOW_TWO_SIDED = 32,
	};

	static constexpr uint32_t MATERIAL_ALPHA_SHIFT = 8;
	static constexpr uint32_t MATERIAL_ALPHA_MASK = 0xFFFFFF00;
	static constexpr uint32_t EMISSION_FLAG_TEXTURE_RANGES = 1;
	static constexpr uint32_t CANDIDATE_FLAGS = MATERIAL_FLAG_SKIP | MATERIAL_FLAG_BLEND | MATERIAL_FLAG_SHADOW_PASS | MATERIAL_FLAG_SHADOW_ONLY | MATERIAL_ALPHA_MASK;
	static constexpr uint32_t ALPHA_ON_SALT = 0x9e3779b9u;

	enum InstanceClass {
		INSTANCE_CLASS_OPAQUE,
		INSTANCE_CLASS_CANDIDATE,
		INSTANCE_CLASS_SPLIT,
	};

	static constexpr uint32_t INSTANCE_CLASS_MASK = 3;
	static constexpr uint32_t INSTANCE_REMAP_SHIFT = 2;
	static constexpr uint32_t HW_GROUP_COUNT = 7;
	static constexpr uint32_t HW_BLEND_INSTANCES = 8;
	static constexpr uint32_t HW_HEADER_WORDS = 9;
	static constexpr uint32_t HW_ALL_GROUPS = 0x80;
	static constexpr uint32_t ALL_CASTERS = 0xFFFFFFFF;

	struct Node {
		float bmin[3];
		uint32_t left_first;
		float bmax[3];
		uint32_t count;
	};

	struct Triangle {
		float v0[3];
		uint32_t surface;
		float e1[4];
		float e2[4];
	};

	struct TriangleUv {
		uint32_t uv[4] = {};
	};

	struct Instance {
		float world_to_object[12];
		uint32_t blas_root;
		uint32_t material_offset;
		uint32_t layer_mask;
		uint32_t pad;
	};

	struct Material {
		float albedo[3];
		float coverage;
		float emission[3];
		uint32_t flags;
		uint32_t emission_texture;
		uint32_t emission_uv_set;
		float emission_uv_scale[2];
		float emission_uv_offset[2];
		float emission_nits_range[2];
		float emission_texel_scale;
		uint32_t emission_flags;
		float emission_mean_luminance;
		float pad;
	};

	// Ray scene space is world position minus the anchor. Layout must match ray_scene_inc.glsl: v0 at offset 0, instance at 12, v1 at 16, material at 28, v2 at 32, uv_index at 44, flags at 48, stride 64 (std430 rounds the 52-byte tail up to the 16-byte alignment of vec3).
	struct TriangleEmitter {
		float v0[3];
		uint32_t instance;
		float v1[3];
		uint32_t material;
		float v2[3];
		uint32_t uv_index;
		uint32_t flags;
		uint32_t pad[3];
	};

	static constexpr uint32_t TRIANGLE_EMITTER_PROXIED = 1u;

	struct Light {
		float color[3];
		float energy;
		float direction[3];
		uint32_t type;
		float position[3];
		float attenuation;
		float cos_spot_angle;
		float inv_spot_attenuation;
		float inv_radius;
		uint32_t caster_mask;
		float half_a[3];
		float area;
		float half_b[3];
		uint32_t pad;
	};

	static_assert(sizeof(Light) == 96);
	static_assert(sizeof(Material) == 80);
	static_assert(sizeof(TriangleEmitter) == 64, "TriangleEmitter must match the std430 64-byte stride");
	static_assert(sizeof(Instance) == 64);
	static_assert(sizeof(TriangleUv) == 16);
	static_assert((uint64_t(TLAS_LEAF_SIZE) << TLAS_MAX_DEPTH) <= (uint64_t(1) << 24), "A hardware instance custom index is 24 bits.");

	struct LightCensus {
		uint32_t candidates = 0;
		uint32_t area_candidates = 0;
		uint32_t area_packed = 0;
		uint32_t area_reserved = 0;
		uint32_t area_projected = 0;
		uint32_t masked_candidates = 0;
		uint32_t masked_cut = 0;
		uint32_t masked_instances = 0;
		double masked_packed_energy = 0.0;
		double masked_cut_energy = 0.0;
		uint32_t cut_area = 0;
		uint32_t cut_nonarea = 0;
		double cut_area_power = 0.0;
		double cut_nonarea_power = 0.0;
		double packed_area_power = 0.0;
		double packed_nonarea_power = 0.0;
		uint32_t held = 0;
		Vector3i pick_cell;
		Vector3i region_cell;
		bool has_region = false;
		uint32_t pick_mode = LIGHT_PICK_CAMERA;
		uint32_t max_lights = 0;
		bool operator!=(const LightCensus &p_other) const {
			return candidates != p_other.candidates || area_candidates != p_other.area_candidates || area_packed != p_other.area_packed || area_reserved != p_other.area_reserved || area_projected != p_other.area_projected || masked_candidates != p_other.masked_candidates || masked_cut != p_other.masked_cut || masked_instances != p_other.masked_instances || masked_packed_energy != p_other.masked_packed_energy || masked_cut_energy != p_other.masked_cut_energy || cut_area != p_other.cut_area || cut_nonarea != p_other.cut_nonarea || cut_area_power != p_other.cut_area_power || cut_nonarea_power != p_other.cut_nonarea_power || packed_area_power != p_other.packed_area_power || packed_nonarea_power != p_other.packed_nonarea_power || held != p_other.held || pick_cell != p_other.pick_cell || region_cell != p_other.region_cell || has_region != p_other.has_region || pick_mode != p_other.pick_mode || max_lights != p_other.max_lights;
		}
	};

	struct PickKey {
		double importance = 0.0;
		float distance = 0.0f;
		uint32_t kept = 0;
		uint64_t rid = 0;
		uint32_t index = 0;
	};

	struct PickSlot {
		bool used = false;
		bool fresh = false;
		Vector3i key;
		Vector3i region_key;
		Vector3 camera;
		uint64_t last_used = 0;
		HashSet<RID> kept;
	};

	struct PickState {
		PickSlot slots[PICK_CELL_SLOTS];
		LocalVector<uint32_t> last_packed;
	};

	struct PickParams {
		Vector3 cell_origin;
		float cell_size = 16.0f;
		uint32_t slot = 0;
		uint32_t keep = 0;
		uint32_t reserved_area = RESERVED_AREA_LIGHTS;
		float keep_share = 0.25f;
		float keep_distance = 16.0f;
		double inv_exposure = 1.0;
		bool want_held = false;
	};

	struct PickCensus {
		uint32_t area_reserved = 0;
		uint32_t area_packed = 0;
		uint32_t masked_cut = 0;
		double masked_cut_energy = 0.0;
		double masked_packed_energy = 0.0;
		uint32_t cut_area = 0;
		uint32_t cut_nonarea = 0;
		double cut_area_power = 0.0;
		double cut_nonarea_power = 0.0;
		double packed_area_power = 0.0;
		double packed_nonarea_power = 0.0;
		uint32_t held = 0;
		bool slot_new = false;
		uint32_t slot_in = 0;
		uint32_t slot_out = 0;
		uint32_t packed_in = 0;
		uint32_t packed_out = 0;
	};

	static constexpr uint32_t MAX_EMITTER_ROOMS = 256;

	struct SceneData {
		float camera_to_anchor[3];
		uint32_t instance_count;
		uint32_t light_count;
		uint32_t triangle_emitter_count;
		uint32_t triangle_emitters_capped;
		float emissive_exposure_normalization;
		uint32_t triangle_emitter_generation;
		uint32_t emission_texture_count;
		uint32_t proxy_emitters_traced;
		uint32_t hit_emission_texel;
		float room_grid_origin[2];
		float room_grid_cell;
		uint32_t room_count;
		uint32_t room_start[(MAX_EMITTER_ROOMS + 2) * 4];
	};

	static_assert(sizeof(SceneData) == 4192);

	struct BuildPrimitive {
		float bmin[3];
		float bmax[3];
		float centroid[3];
		uint32_t index;
	};

	static void build_bvh(LocalVector<BuildPrimitive> &p_primitives, uint32_t p_max_leaf, uint32_t p_max_depth, LocalVector<Node> &r_nodes);
	static float light_energy(float p_energy, float p_intensity, RSE::LightType p_type, bool p_physical_units, float p_exposure, const Vector2 &p_area_size, bool p_area_normalize);
	static void pack_area(const Basis &p_basis, const Vector2 &p_area_size, Light &r_light);
	static void pack_shape(RSE::LightType p_type, const Basis &p_basis, const Vector2 &p_area_size, float p_direct_size, Light &r_light);
	static bool feed_frame_overflow(bool p_raw_overflow, bool p_baseline) { return p_raw_overflow || p_baseline; }
	static bool feed_frame_needs_epoch(uint32_t p_changes, bool p_raw_overflow, bool p_epoch_moved) { return (p_changes > 0 || p_raw_overflow) && !p_epoch_moved; }
	static uint32_t feed_surface_state(uint32_t p_state, uint32_t p_flags, float p_coverage) { return hash_murmur3_one_float(p_coverage, hash_murmur3_one_32(p_flags, p_state)); }
	static uint32_t shadow_cast_flags(RSE::ShadowCastingSetting p_setting, bool p_occluder = false);
	static uint32_t slot_cast_flags(uint32_t p_slot_flags, uint32_t p_cast_flags) { return (p_slot_flags & MATERIAL_ALPHA_MASK) != 0 ? p_cast_flags : p_cast_flags & ~uint32_t(MATERIAL_FLAG_SHADOW_TWO_SIDED); }
	static uint64_t surface_candidates(uint32_t p_surface, uint32_t p_flags);
	static uint32_t alpha_slot_flags(uint32_t p_flags, bool p_alpha_on) { return p_alpha_on ? p_flags : p_flags & ~MATERIAL_ALPHA_MASK; }
	static uint64_t alpha_split_bits(uint32_t p_surface, uint32_t p_flags, uint32_t p_alpha_flags) { return surface_candidates(p_surface, p_flags | p_alpha_flags) & ~surface_candidates(p_surface, p_flags & ~MATERIAL_ALPHA_MASK); }
	static bool hw_split_stale(uint64_t p_hw_generation, bool p_hw_alpha_on, bool p_alpha_on, uint64_t p_alpha_split_bits) { return p_hw_generation != 0 && p_hw_alpha_on != p_alpha_on && p_alpha_split_bits != 0; }
	static uint64_t alpha_scene_signature(uint64_t p_signature, bool p_alpha_on) { return p_alpha_on ? GISceneTracker::add_instance(p_signature, ALPHA_ON_SALT) : p_signature; }
	static uint32_t instance_class(uint64_t p_candidates, uint64_t p_split);
	static uint32_t hw_instance_mask(uint32_t p_layer_mask, const uint32_t *p_groups, uint32_t p_group_count);
	static uint32_t float_to_half(float p_value);
	static uint32_t pack_half2(float p_x, float p_y);
	static bool alpha_uv_unsafe(float p_extent, float p_scale, uint32_t p_layer_size);
	static bool ray_backface(const Vector3 &p_e1, const Vector3 &p_e2, const Vector3 &p_direction) { return p_e1.dot(p_direction.cross(p_e2)) > real_t(0); }
	static bool alpha_culled(uint32_t p_bits, uint32_t p_flags, bool p_backface, bool p_shadow) {
		const uint32_t cull = p_bits & RayAlphaStore::ENTRY_CULL_MASK;
		if (cull == 0u || (p_shadow && (p_flags & uint32_t(MATERIAL_FLAG_SHADOW_TWO_SIDED)) != 0u)) {
			return false;
		}
		const bool seen_back = p_backface != p_shadow;
		return cull == uint32_t(RSE::CULL_MODE_BACK) ? seen_back : !seen_back;
	}
	static Vector3i pick_cell_key(const Vector3 &p_world, float p_cell);
	static uint32_t match_slot(PickState &r_state, const Vector3 &p_camera_world, float p_cell, uint64_t p_serial);
	static AABB light_region(PickSlot &r_slot, const Vector3 &p_camera_world, const Vector3 &p_region_half, float p_cell);
	static AABB tracker_region_for(bool p_stable, const Vector3 &p_camera, const Vector3 &p_region_half) { return p_stable && p_region_half != Vector3() ? AABB(p_camera - p_region_half, p_region_half * 2.0) : AABB(); }
	static bool tracker_observes(RSE::LightType p_type, const AABB &p_region, const AABB &p_light_box) { return p_type == RSE::LIGHT_DIRECTIONAL || !p_region.has_volume() || p_region.intersects(p_light_box); }
	static void pick_lights(const LocalVector<Light> &p_candidates, const LocalVector<RID> &p_ids, const PickParams &p_params, PickState &r_state, LocalVector<uint32_t> &r_selected, PickCensus &r_census);
	static void select_nearest(LocalVector<Light> &r_lights, LocalVector<Light> &r_local_lights, const Vector3 &p_camera, uint32_t p_max_lights, uint32_t p_reserved_area, double p_inv_exposure, LightCensus &r_census);
	static void count_swaps(LocalVector<uint32_t> &r_last, LocalVector<uint32_t> &p_now, uint32_t &r_in, uint32_t &r_out);

private:
	static RayScene *singleton;
	static SafeFlag needed;
	static bool _settings_need();

	struct MeshEntry {
		LocalVector<Vector3> positions;
		LocalVector<uint32_t> uvs;
		LocalVector<uint32_t> indices;
		LocalVector<uint32_t> triangle_surface;
		LocalVector<float> surface_uv_extent;
		uint32_t surface_count = 0;
		LocalVector<Node> nodes;
		LocalVector<Triangle> bvh_triangles;
		LocalVector<TriangleUv> bvh_uvs;
		bool built = false;
		uint32_t build_serial = 0;
		uint32_t node_offset = 0;
		uint32_t triangle_offset = 0;
		AABB aabb;
		uint64_t hw_generation = 0;
		uint64_t hw_split = 0;
		uint32_t hw_opaque = 0;
		uint32_t hw_region = 0;
		bool hw_alpha_on = false;
		LocalVector<uint32_t> hw_order;
	};

	struct TriangleEmitterSource {
		Transform3D transform;
		RID mesh;
		uint32_t instance = 0;
		uint32_t material_offset = 0;
		uint32_t surface_count = 0;
		bool proxied = false;
	};

	Mutex mesh_mutex;
	HashMap<RID, MeshEntry> meshes;
	HashSet<RID> assembled_meshes;
	bool assembly_dirty = false;
	SafeNumeric<uint32_t> feed_consumers;
	struct FeedRecord {
		Transform3D world;
		RID mesh;
		uint32_t build_serial = 0;
		AABB bounds;
		uint32_t stamp = 0;
		uint32_t state = 0;
		uint8_t history = 0;
		uint8_t usable = 1;
		bool listed = false;
	};
	HashMap<const void *, FeedRecord> feed_records;
	LocalVector<const void *> feed_active;
	LocalVector<AABB> feed_raw;
	DirtyFeed feed;
	bool feed_was_on = false;
	bool feed_baseline = false;
	bool feed_raw_overflow = false;
	uint32_t feed_serial = 0;
	uint32_t feed_stamp = 0;
	uint32_t feed_seen = 0;
	uint32_t feed_survivors = 0;
	uint32_t feed_tracked_before = 0;
	uint32_t feed_generation_before = 0;
	uint32_t feed_epoch_before = 0;
	uint32_t structure_generation = 0;
	Ref<FileAccess> feed_log;
	int feed_log_mode = 0;
	uint64_t feed_update_start = 0;
	uint32_t feed_epoch_history[FEED_EPOCH_HISTORY] = {};

	LocalVector<Node> blas_nodes;
	LocalVector<Triangle> triangles;

	LocalVector<Instance> instances;
	struct ShellRange {
		uint32_t offset = 0;
		uint32_t count = 0;
	};
	HashMap<RID, ShellRange> shell_ranges;
	struct CachedMaterial {
		Material entry;
		uint32_t alpha_flags = 0;
		int32_t scale_index = -1;
		int32_t bounce_index = -1;
	};

	struct AlphaCounts {
		uint32_t cutout_materials = 0;
		uint32_t solid_no_texture = 0;
		uint32_t solid_custom0 = 0;
		uint32_t solid_degenerate = 0;
		uint32_t solid_always = 0;
		uint32_t solid_refused = 0;
		uint32_t vertex_alpha_ignored = 0;
		uint32_t live_surfaces = 0;
		uint32_t pending_surfaces = 0;
		uint32_t solid_unsafe_uv = 0;
		uint32_t layers = 0;
		uint32_t layers_ready = 0;
		uint32_t table_entries = 0;
		uint32_t layers_released = 0;
		uint32_t layers_evicted = 0;
		uint32_t far_invalid = 0;
		uint32_t entries_retired = 0;
	};

	LocalVector<Material> materials;
	HashMap<RID, CachedMaterial> material_cache;
	RayAlphaStore alpha_store;
	AlphaCounts alpha_counts;
	AlphaCounts reported_alpha_counts;
	uint64_t tlas_rebuilds = 0;
	uint64_t hw_tlas_builds = 0;
	uint64_t reported_tlas_rebuilds = 0;
	uint64_t reported_hw_tlas_builds = 0;
	uint64_t tlas_report_msec = 0;
	enum AlbedoSource {
		ALBEDO_GI,
		ALBEDO_GI_CAPPED,
		ALBEDO_TEXTURED,
		ALBEDO_UNTEXTURED,
		ALBEDO_STANDARD,
		ALBEDO_DEFAULT,
		ALBEDO_SOURCE_MAX,
	};
	uint32_t albedo_sources[ALBEDO_SOURCE_MAX] = {};
	uint32_t reported_albedo_sources[ALBEDO_SOURCE_MAX] = {};
	HashSet<RID> albedo_fallback_warned;
	float emission_scale = 1.0f;
	bool skip_additive = true;
	uint32_t additive_skipped = 0;
	uint32_t emission_debug_logged = 0;
	uint32_t pipboy_scan_logged = 0;
	int64_t reported_additive_state = -2;
	LocalVector<Light> lights;
	LocalVector<Light> local_lights;
	LightCensus census;
	LightCensus printed_census;
	LocalVector<Node> tlas_nodes;
	uint64_t instance_signature = 0;
	GISceneTracker scene_tracker;
	uint32_t next_build_serial = 0;
	LocalVector<uint64_t> scene_members;
	Vector3 anchor;
	uint32_t max_lights = MAX_LIGHTS;
	uint32_t reserved_area_lights = RESERVED_AREA_LIGHTS;
	PickState pick_state;
	float pick_cell_size = 0.0f;
	bool pick_stable = false;
	uint64_t update_serial = 0;
	Vector3 region_camera;
	uint32_t region_slot = 0;
	bool region_recorded = false;
	Vector3 region_half;
	AABB tracker_region;
	LocalVector<RID> local_ids;
	struct CandidateMark {
		uint32_t index = 0;
		uint64_t hash = 0;
		double energy = 0.0;
		bool operator<(const CandidateMark &p_other) const { return index < p_other.index; }
	};
	LocalVector<CandidateMark> last_candidates;
	Vector3i last_pick_cell;
	Vector3i last_pick_region;
	int64_t last_pick_slot = -1;
	bool pick_logged = false;

	RID blas_node_buffer;
	RID triangle_buffer;
	RID instance_buffer;
	RID tlas_node_buffer;
	RID material_buffer;
	RID light_buffer;
	RID scene_data_buffer;
	RID triangle_uv_buffer;
	RID triangle_emitter_buffer;
	RID emission_dummy;
	RID room_grid_texture;
	RID room_grid_dummy;
	RID emission_texture_array;
	uint32_t emission_texture_count = 0;
	uint32_t blas_node_capacity = 0;
	uint32_t triangle_capacity = 0;
	uint32_t triangle_uv_capacity = 0;
	uint32_t triangle_emitter_capacity = 0;
	uint32_t triangle_emitter_count = 0;
	uint32_t triangle_emitter_generation = 0;
	uint32_t triangle_emitter_state = 0;
	uint32_t triangles_logged = 0;
	bool ray_scene_instance_table_printed = false;
	LocalVector<String> instance_mesh_names;
	LocalVector<Vector3> instance_mesh_positions;
	uint32_t triangle_emitter_emission_state = 0;
	bool triangle_emitters_capped = false;
	bool triangle_emitters_built = false;
	LocalVector<TriangleEmitter> triangle_emitters;

	struct EmitterRoomGrid {
		Vector2 origin;
		float cell_size = 1.0f;
		Vector2i size;
		uint32_t room_count = 0;
		PackedByteArray ids;
		bool valid = false;
	};
	EmitterRoomGrid emitter_room_grid;
	uint32_t emitter_room_count = 0;
	uint32_t emitter_room_table_generation = 0;
	LocalVector<uint32_t> emitter_room_starts;
	bool alpha_enabled = false;
	uint32_t alpha_generation = 0;
	bool alpha_edge = false;
	uint32_t instance_capacity = 0;
	uint32_t tlas_node_capacity = 0;
	uint32_t material_capacity = 0;
	uint32_t light_capacity = 0;

	RayAccel *accel = nullptr;
	int32_t backend_requested = -1;
	bool backend_warned = false;
	bool hardware_refused = false;
	LocalVector<uint32_t> hw_remap;
	RID hw_remap_buffer;
	uint32_t hw_remap_capacity = 0;
	bool hw_remap_dirty = false;
	uint64_t hw_mesh_generation = 0;
	LocalVector<uint32_t> hw_order;
	LocalVector<Transform3D> hw_transforms;
	LocalVector<RID> hw_meshes;
	LocalVector<uint32_t> hw_classes;
	LocalVector<uint8_t> hw_usable;
	uint32_t hw_skipped_unbuilt = 0;
	uint32_t hw_skipped_cap = 0;
	uint32_t hw_groups[HW_GROUP_COUNT] = {};
	uint32_t hw_group_count = 0;

	void _feed_begin();
	void _feed_push_raw(const AABB &p_box);
	void _feed_observe(const void *p_key, const Transform3D &p_world, RID p_mesh_rid, const MeshEntry &p_mesh, bool p_usable, uint32_t p_state);
	void _feed_end();
	void _feed_stop();
	void _feed_log_line();
	MeshEntry *_capture_mesh(RID p_mesh);
	void _capture_surface(MeshEntry &r_mesh, uint32_t p_surface, const RenderingServerTypes::SurfaceData &p_surface_data);
	void _build_blas(MeshEntry &p_mesh);
	void _assemble(const LocalVector<RID> &p_meshes);
	void _material_entry(RID p_material, RID p_mesh, uint32_t p_surface, Material &r_material, float &r_alpha_threshold, uint32_t &r_cull);
	void _alpha_entry(RID p_material, Material &r_material, float p_alpha_threshold, uint32_t p_cull);
	bool _surface_uv_unsafe(const MeshEntry &p_mesh, uint32_t p_surface, uint32_t p_entry) const;
	bool _alpha_live(uint32_t p_flags) const;
	bool _alpha_ready() const;
	void _alpha_instance(const MeshEntry &p_mesh, uint32_t p_surface, Material &r_material);
	void _print_alpha();
	void _print_tlas_rebuilds(bool p_final);
	void _warn_albedo_fallback(RID p_material, RID p_texture, RID p_mesh, uint32_t p_surface);
	void _instance_slots(RID p_material, CachedMaterial &r_cached) const;
	void _pack_light(RID p_light_instance, bool p_physical_units, float p_exposure, uint32_t p_limit, LocalVector<Light> &r_lights, LocalVector<RID> *r_ids = nullptr);
	void _select_lights(const Vector3 &p_camera, double p_inv_exposure);
	void _print_census();
	void _candidate_changes(double p_inv_exposure, uint32_t &r_in, uint32_t &r_out, uint32_t &r_changed);
	void _pick_settings(float p_cell, bool p_stable);
	static void _ensure_buffer(RID &r_buffer, uint32_t &r_capacity, uint32_t p_count, uint32_t p_stride, const char *p_name);
	void _update_triangle_emitters(const LocalVector<TriangleEmitterSource> &p_sources, const LocalVector<uint32_t> &p_order);
	void _refresh_emitter_room_grid();
	void _poll_room_grid();
	uint32_t _emitter_room_at(const Vector3 &p_position) const;
	void _backend_tick();
	void _hw_begin();
	void _hw_instance(RID p_mesh_rid, MeshEntry &r_mesh, uint64_t p_material_candidates, uint64_t p_candidates, const Transform3D &p_transform, Instance &r_instance);
	void _hw_update(const LocalVector<BuildPrimitive> &p_primitives, bool p_rebuilt);
	void _hw_assembled();
	void _hw_mesh_removed(RID p_mesh);
	void _hw_free();
	bool _hw_ready() const;

public:
	static RayScene *get_singleton() { return singleton; }
	uint32_t get_emitter_room_count() const { return emitter_room_count; }
	const LocalVector<uint32_t> &get_emitter_room_starts() const { return emitter_room_starts; }
	uint32_t get_emitter_room_table_generation() const { return emitter_room_table_generation; }
	static bool is_needed();
	static void watch_needed();
	static void reset_needed_for_tests() { needed.clear(); }

	void mesh_surface_added(RID p_mesh, uint32_t p_surface, const RenderingServerTypes::SurfaceData &p_surface_data);
	void mesh_cleared(RID p_mesh);

	void update(const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, const Vector<RID> &p_directional_lights, RID p_camera_attributes, const Vector3 &p_camera_position);
	void acquire_dirty_feed(FeedConsumer p_consumer) { feed_consumers.bit_or(p_consumer); }
	void release_dirty_feed(FeedConsumer p_consumer) { feed_consumers.bit_and(~uint32_t(p_consumer)); }
	bool is_dirty_feed_held() const { return feed_consumers.get() != 0; }
	const DirtyFeed &get_dirty_feed() const { return feed; }
	uint32_t get_structure_generation() const { return structure_generation; }
	AABB get_light_region(const Vector3 &p_camera_world, const Vector3 &p_region_half);

	bool is_ready() const { return scene_data_buffer.is_valid() && blas_node_buffer.is_valid() && tlas_node_buffer.is_valid() && triangle_emitter_buffer.is_valid() && (!accel || _hw_ready()) && _alpha_ready(); }
	bool uses_hardware() const { return accel != nullptr; }
	void refuse_hardware(const String &p_reason);
	RID get_tlas() const;
	RID get_hw_remap_buffer() const { return hw_remap_buffer; }
	void append_uniforms(LocalVector<RD::Uniform> &r_uniforms) const;
	Vector3 get_anchor() const { return anchor; }
	bool shell_material_range(RID p_instance, uint32_t &r_offset, uint32_t &r_count) const;
	void collect_moved_instances(uint64_t &r_signature, Vector3 &r_anchor, HashMap<uint64_t, AABB> &r_known, LocalVector<AABB> &r_moved) const;
	RID get_blas_node_buffer() const { return blas_node_buffer; }
	RID get_triangle_buffer() const { return triangle_buffer; }
	RID get_instance_buffer() const { return instance_buffer; }
	RID get_tlas_node_buffer() const { return tlas_node_buffer; }
	RID get_material_buffer() const { return material_buffer; }
	RID get_light_buffer() const { return light_buffer; }
	RID get_scene_data_buffer() const { return scene_data_buffer; }
	RID get_triangle_uv_buffer() const { return triangle_uv_buffer; }
	RID get_triangle_emitters() const { return triangle_emitter_buffer; }
	RID get_emission_textures() const { return emission_texture_array.is_valid() ? emission_texture_array : emission_dummy; }
	uint32_t get_emission_texture_count() const { return emission_texture_count; }
	uint32_t get_triangle_emitter_count() const { return triangle_emitter_count; }
	bool get_triangle_emitters_capped() const { return triangle_emitters_capped; }
	uint32_t get_triangle_emitter_generation() const { return triangle_emitter_generation; }
	bool is_alpha_enabled() const { return alpha_enabled; }
	uint32_t get_alpha_generation() const { return alpha_generation; }
	static String alpha_shader_defines();
	RID get_alpha_table_buffer() const { return alpha_store.get_table_buffer(); }
	uint64_t get_alpha_change_serial() const { return alpha_store.get_change_serial(); }
	RID get_alpha_layer_texture() const { return alpha_store.get_layer_texture(); }
	const RayAlphaStore &get_alpha_store() const { return alpha_store; }

	RayScene();
	~RayScene();
};

} // namespace RendererRD
