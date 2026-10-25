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

#include "core/math/aabb.h"
#include "core/math/basis.h"
#include "core/math/vector2.h"
#include "core/os/mutex.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/templates/paged_array.h"
#include "servers/rendering/rendering_device.h"
#include "servers/rendering/rendering_server_enums.h"
#include "servers/rendering/rendering_server_types.h"

class RenderGeometryInstance;

namespace RendererRD {

class RayScene {
public:
	static constexpr uint32_t BLAS_LEAF_SIZE = 4;
	static constexpr uint32_t TLAS_LEAF_SIZE = 1;
	static constexpr uint32_t BLAS_MAX_DEPTH = 24;
	static constexpr uint32_t TLAS_MAX_DEPTH = 24;
	static constexpr uint32_t MAX_LIGHTS = 256;
	static constexpr uint32_t RESERVED_AREA_LIGHTS = 16;
	static constexpr uint32_t MAX_CANDIDATE_LIGHTS = 16384;
	static constexpr double ANCHOR_GRID = 64.0;

	enum MaterialFlags {
		MATERIAL_FLAG_TWO_SIDED = 1,
		MATERIAL_FLAG_SKIP = 2,
		MATERIAL_FLAG_SHADOW_PASS = 8,
	};

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

	struct Instance {
		float world_to_object[12];
		uint32_t blas_root;
		uint32_t material_offset;
		uint32_t layer_mask;
		uint32_t pad;
	};

	struct Material {
		float albedo[3];
		float pad;
		float emission[3];
		uint32_t flags;
	};

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
	static_assert(sizeof(Instance) == 64);

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
		bool operator!=(const LightCensus &p_other) const {
			return candidates != p_other.candidates || area_candidates != p_other.area_candidates || area_packed != p_other.area_packed || area_reserved != p_other.area_reserved || area_projected != p_other.area_projected || masked_candidates != p_other.masked_candidates || masked_cut != p_other.masked_cut || masked_instances != p_other.masked_instances || masked_packed_energy != p_other.masked_packed_energy || masked_cut_energy != p_other.masked_cut_energy;
		}
	};

	struct SceneData {
		float camera_to_anchor[3];
		uint32_t instance_count;
		uint32_t light_count;
		uint32_t pad[3];
	};

	struct BuildPrimitive {
		float bmin[3];
		float bmax[3];
		float centroid[3];
		uint32_t index;
	};

	static void build_bvh(LocalVector<BuildPrimitive> &p_primitives, uint32_t p_max_leaf, uint32_t p_max_depth, LocalVector<Node> &r_nodes);
	static float light_energy(float p_energy, float p_intensity, RSE::LightType p_type, bool p_physical_units, float p_exposure, const Vector2 &p_area_size, bool p_area_normalize);
	static void pack_area(const Basis &p_basis, const Vector2 &p_area_size, Light &r_light);

private:
	static RayScene *singleton;

	struct MeshEntry {
		LocalVector<Vector3> positions;
		LocalVector<uint32_t> indices;
		LocalVector<uint32_t> triangle_surface;
		uint32_t surface_count = 0;
		LocalVector<Node> nodes;
		LocalVector<Triangle> bvh_triangles;
		bool built = false;
		uint32_t node_offset = 0;
		uint32_t triangle_offset = 0;
		AABB aabb;
	};

	Mutex mesh_mutex;
	HashMap<RID, MeshEntry> meshes;
	HashSet<RID> assembled_meshes;
	bool assembly_dirty = false;

	LocalVector<Node> blas_nodes;
	LocalVector<Triangle> triangles;

	LocalVector<Instance> instances;
	struct CachedMaterial {
		Material entry;
		int32_t scale_index = -1;
		int32_t bounce_index = -1;
	};

	LocalVector<Material> materials;
	HashMap<RID, CachedMaterial> material_cache;
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
	LocalVector<Light> lights;
	LocalVector<Light> local_lights;
	LightCensus census;
	LightCensus printed_census;
	LocalVector<Node> tlas_nodes;
	uint64_t instance_signature = 0;
	Vector3 anchor;

	RID blas_node_buffer;
	RID triangle_buffer;
	RID instance_buffer;
	RID tlas_node_buffer;
	RID material_buffer;
	RID light_buffer;
	RID scene_data_buffer;
	uint32_t blas_node_capacity = 0;
	uint32_t triangle_capacity = 0;
	uint32_t instance_capacity = 0;
	uint32_t tlas_node_capacity = 0;
	uint32_t material_capacity = 0;
	uint32_t light_capacity = 0;

	MeshEntry *_capture_mesh(RID p_mesh);
	void _capture_surface(MeshEntry &r_mesh, uint32_t p_surface, const RenderingServerTypes::SurfaceData &p_surface_data);
	void _build_blas(MeshEntry &p_mesh);
	void _assemble(const LocalVector<RID> &p_meshes);
	void _material_entry(RID p_material, RID p_mesh, uint32_t p_surface, Material &r_material);
	void _warn_albedo_fallback(RID p_material, RID p_texture, RID p_mesh, uint32_t p_surface);
	void _instance_slots(RID p_material, CachedMaterial &r_cached) const;
	void _pack_light(RID p_light_instance, bool p_physical_units, float p_exposure, uint32_t p_limit, LocalVector<Light> &r_lights);
	void _select_lights(const Vector3 &p_camera);
	void _print_census();
	static void _ensure_buffer(RID &r_buffer, uint32_t &r_capacity, uint32_t p_count, uint32_t p_stride, const char *p_name);

public:
	static RayScene *get_singleton() { return singleton; }

	void mesh_surface_added(RID p_mesh, uint32_t p_surface, const RenderingServerTypes::SurfaceData &p_surface_data);
	void mesh_cleared(RID p_mesh);

	void update(const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, const Vector<RID> &p_directional_lights, RID p_camera_attributes, const Vector3 &p_camera_position);

	bool is_ready() const { return scene_data_buffer.is_valid() && blas_node_buffer.is_valid() && tlas_node_buffer.is_valid(); }
	Vector3 get_anchor() const { return anchor; }
	RID get_blas_node_buffer() const { return blas_node_buffer; }
	RID get_triangle_buffer() const { return triangle_buffer; }
	RID get_instance_buffer() const { return instance_buffer; }
	RID get_tlas_node_buffer() const { return tlas_node_buffer; }
	RID get_material_buffer() const { return material_buffer; }
	RID get_light_buffer() const { return light_buffer; }
	RID get_scene_data_buffer() const { return scene_data_buffer; }

	RayScene();
	~RayScene();
};

} // namespace RendererRD
