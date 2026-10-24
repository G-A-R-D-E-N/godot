/**************************************************************************/
/*  ray_scene.cpp                                                         */
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

#include "ray_scene.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "core/templates/hashfuncs.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/renderer_geometry_instance.h"
#include "servers/rendering/renderer_rd/environment/ddgi.h"
#include "servers/rendering/renderer_rd/environment/ddgi_light_tracker.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

#include <cfloat>

using namespace RendererRD;

RayScene *RayScene::singleton = nullptr;

RayScene::RayScene() {
	singleton = this;
}

RayScene::~RayScene() {
	RID *owned[] = { &blas_node_buffer, &triangle_buffer, &instance_buffer, &tlas_node_buffer, &material_buffer, &light_buffer, &scene_data_buffer };
	for (RID *rid : owned) {
		if (rid->is_valid()) {
			RD::get_singleton()->free_rid(*rid);
			*rid = RID();
		}
	}
	singleton = nullptr;
}

static float _surface_area(const float *p_min, const float *p_max) {
	const float dx = MAX(p_max[0] - p_min[0], 0.0f);
	const float dy = MAX(p_max[1] - p_min[1], 0.0f);
	const float dz = MAX(p_max[2] - p_min[2], 0.0f);
	return 2.0f * (dx * dy + dy * dz + dz * dx);
}

static void _grow(float *r_min, float *r_max, const float *p_min, const float *p_max) {
	for (int k = 0; k < 3; k++) {
		r_min[k] = MIN(r_min[k], p_min[k]);
		r_max[k] = MAX(r_max[k], p_max[k]);
	}
}

struct CentroidLess {
	int axis = 0;
	bool operator()(const RayScene::BuildPrimitive &p_a, const RayScene::BuildPrimitive &p_b) const {
		return p_a.centroid[axis] < p_b.centroid[axis];
	}
};

void RayScene::build_bvh(LocalVector<BuildPrimitive> &p_primitives, uint32_t p_max_leaf, uint32_t p_max_depth, LocalVector<Node> &r_nodes) {
	constexpr int BINS = 12;
	struct Task {
		uint32_t node;
		uint32_t start;
		uint32_t count;
		uint32_t depth;
	};
	struct Bin {
		float bmin[3];
		float bmax[3];
		uint32_t count;
	};

	r_nodes.clear();
	Node root = {};
	for (int k = 0; k < 3; k++) {
		root.bmin[k] = FLT_MAX;
		root.bmax[k] = -FLT_MAX;
	}
	r_nodes.push_back(root);
	const uint32_t total = p_primitives.size();
	if (total == 0) {
		r_nodes[0].count = 1;
		return;
	}
	r_nodes.reserve(total * 2);

	LocalVector<Task> stack;
	stack.push_back({ 0, 0, total, 0 });
	while (!stack.is_empty()) {
		const Task task = stack[stack.size() - 1];
		stack.resize(stack.size() - 1);

		float bmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
		float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		float cmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
		float cmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
		for (uint32_t i = task.start; i < task.start + task.count; i++) {
			const BuildPrimitive &p = p_primitives[i];
			_grow(bmin, bmax, p.bmin, p.bmax);
			_grow(cmin, cmax, p.centroid, p.centroid);
		}
		Node &node = r_nodes[task.node];
		for (int k = 0; k < 3; k++) {
			node.bmin[k] = bmin[k];
			node.bmax[k] = bmax[k];
		}

		if (task.count <= p_max_leaf) {
			node.left_first = task.start;
			node.count = task.count;
			continue;
		}

		int best_axis = -1;
		int best_split = 0;
		float best_cost = FLT_MAX;
		for (int axis = 0; axis < 3; axis++) {
			const float extent = cmax[axis] - cmin[axis];
			if (extent <= 1e-12f) {
				continue;
			}
			Bin bins[BINS];
			for (Bin &bin : bins) {
				bin.bmin[0] = bin.bmin[1] = bin.bmin[2] = FLT_MAX;
				bin.bmax[0] = bin.bmax[1] = bin.bmax[2] = -FLT_MAX;
				bin.count = 0;
			}
			const float scale = float(BINS) / extent;
			for (uint32_t i = task.start; i < task.start + task.count; i++) {
				const BuildPrimitive &p = p_primitives[i];
				const int b = MIN(BINS - 1, int((p.centroid[axis] - cmin[axis]) * scale));
				_grow(bins[b].bmin, bins[b].bmax, p.bmin, p.bmax);
				bins[b].count++;
			}
			float left_area[BINS - 1];
			uint32_t left_count[BINS - 1];
			float lmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
			float lmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			uint32_t lcount = 0;
			for (int b = 0; b < BINS - 1; b++) {
				if (bins[b].count) {
					_grow(lmin, lmax, bins[b].bmin, bins[b].bmax);
				}
				lcount += bins[b].count;
				left_count[b] = lcount;
				left_area[b] = lcount ? _surface_area(lmin, lmax) : 0.0f;
			}
			float rmin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
			float rmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			uint32_t rcount = 0;
			for (int b = BINS - 1; b > 0; b--) {
				if (bins[b].count) {
					_grow(rmin, rmax, bins[b].bmin, bins[b].bmax);
				}
				rcount += bins[b].count;
				if (rcount == 0 || left_count[b - 1] == 0) {
					continue;
				}
				const float cost = float(left_count[b - 1]) * left_area[b - 1] + float(rcount) * _surface_area(rmin, rmax);
				if (cost < best_cost) {
					best_cost = cost;
					best_axis = axis;
					best_split = b;
				}
			}
		}

		uint32_t mid = task.start + task.count / 2;
		if (best_axis >= 0) {
			const float extent = cmax[best_axis] - cmin[best_axis];
			const float scale = float(BINS) / extent;
			uint32_t i = task.start;
			uint32_t j = task.start + task.count;
			while (i < j) {
				const int b = MIN(BINS - 1, int((p_primitives[i].centroid[best_axis] - cmin[best_axis]) * scale));
				if (b < best_split) {
					i++;
				} else {
					j--;
					SWAP(p_primitives[i], p_primitives[j]);
				}
			}
			if (i > task.start && i < task.start + task.count) {
				mid = i;
			}
		}

		const uint32_t child_depth = task.depth + 1;
		const uint32_t child_count = MAX(mid - task.start, task.start + task.count - mid);
		if (child_depth > p_max_depth || child_count > (uint64_t(p_max_leaf) << (p_max_depth - child_depth))) {
			int axis = 0;
			for (int k = 1; k < 3; k++) {
				if (cmax[k] - cmin[k] > cmax[axis] - cmin[axis]) {
					axis = k;
				}
			}
			mid = task.start + task.count / 2;
			SortArray<BuildPrimitive, CentroidLess> sorter;
			sorter.compare.axis = axis;
			sorter.nth_element(task.start, task.start + task.count, mid, p_primitives.ptr());
		}

		const uint32_t left = r_nodes.size();
		r_nodes.push_back(Node());
		r_nodes.push_back(Node());
		r_nodes[task.node].left_first = left;
		r_nodes[task.node].count = 0;
		stack.push_back({ left, task.start, mid - task.start, child_depth });
		stack.push_back({ left + 1, mid, task.start + task.count - mid, child_depth });
	}
}

void RayScene::mesh_surface_added(RID p_mesh, uint32_t p_surface, const RenderingServerTypes::SurfaceData &p_surface_data) {
	MutexLock lock(mesh_mutex);
	if (!meshes.has(p_mesh) && (p_surface != 0 || !DDGI::is_enabled())) {
		return;
	}
	_capture_surface(meshes[p_mesh], p_surface, p_surface_data);
}

RayScene::MeshEntry *RayScene::_capture_mesh(RID p_mesh) {
	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	if (!mesh_storage->owns_mesh(p_mesh)) {
		return nullptr;
	}
	MeshEntry &mesh = meshes[p_mesh];
	const int surface_count = mesh_storage->mesh_get_surface_count(p_mesh);
	for (int s = 0; s < surface_count; s++) {
		_capture_surface(mesh, s, RSG::mesh_storage->mesh_get_surface(p_mesh, s));
	}
	return &mesh;
}

void RayScene::_capture_surface(MeshEntry &r_mesh, uint32_t p_surface, const RenderingServerTypes::SurfaceData &p_surface_data) {
	r_mesh.surface_count = MAX(r_mesh.surface_count, p_surface + 1);
	if (r_mesh.built) {
		r_mesh.built = false;
		r_mesh.nodes.clear();
		r_mesh.bvh_triangles.clear();
		assembly_dirty = true;
	}

	if (p_surface_data.primitive != RSE::PRIMITIVE_TRIANGLES || (p_surface_data.format & RSE::ARRAY_FORMAT_BONES) || (p_surface_data.format & RSE::ARRAY_FLAG_USE_2D_VERTICES)) {
		return;
	}

	const Array arrays = RenderingServer::get_singleton()->mesh_create_arrays_from_surface_data(p_surface_data);
	const PackedVector3Array vertices = arrays[RSE::ARRAY_VERTEX];
	const PackedInt32Array index_array = arrays[RSE::ARRAY_INDEX];
	if (vertices.is_empty()) {
		return;
	}

	const uint32_t base = r_mesh.positions.size();
	for (const Vector3 &v : vertices) {
		r_mesh.positions.push_back(v);
	}
	if (base == 0) {
		r_mesh.aabb = AABB(vertices[0], Vector3());
	}
	for (const Vector3 &v : vertices) {
		r_mesh.aabb.expand_to(v);
	}

	const uint32_t vertex_count = vertices.size();
	const uint32_t index_count = index_array.is_empty() ? vertex_count : uint32_t(index_array.size());
	for (uint32_t i = 0; i + 2 < index_count; i += 3) {
		for (uint32_t k = 0; k < 3; k++) {
			const uint32_t index = index_array.is_empty() ? i + k : uint32_t(index_array[i + k]);
			r_mesh.indices.push_back(base + MIN(index, vertex_count - 1));
		}
		r_mesh.triangle_surface.push_back(p_surface);
	}
}

void RayScene::mesh_cleared(RID p_mesh) {
	MutexLock lock(mesh_mutex);
	if (meshes.erase(p_mesh) && assembled_meshes.has(p_mesh)) {
		assembly_dirty = true;
	}
}

void RayScene::_build_blas(MeshEntry &p_mesh) {
	const uint32_t triangle_count = MIN(uint32_t(p_mesh.triangle_surface.size()), BLAS_LEAF_SIZE << BLAS_MAX_DEPTH);
	LocalVector<BuildPrimitive> primitives;
	primitives.resize(triangle_count);
	for (uint32_t t = 0; t < triangle_count; t++) {
		BuildPrimitive &p = primitives[t];
		p.index = t;
		for (int k = 0; k < 3; k++) {
			p.bmin[k] = FLT_MAX;
			p.bmax[k] = -FLT_MAX;
		}
		for (uint32_t v = 0; v < 3; v++) {
			const Vector3 &position = p_mesh.positions[p_mesh.indices[t * 3 + v]];
			for (int k = 0; k < 3; k++) {
				p.bmin[k] = MIN(p.bmin[k], float(position[k]));
				p.bmax[k] = MAX(p.bmax[k], float(position[k]));
			}
		}
		for (int k = 0; k < 3; k++) {
			p.centroid[k] = (p.bmin[k] + p.bmax[k]) * 0.5f;
		}
	}

	build_bvh(primitives, BLAS_LEAF_SIZE, BLAS_MAX_DEPTH, p_mesh.nodes);

	p_mesh.bvh_triangles.resize(triangle_count);
	for (uint32_t i = 0; i < triangle_count; i++) {
		const uint32_t t = primitives[i].index;
		const Vector3 p0 = p_mesh.positions[p_mesh.indices[t * 3 + 0]];
		const Vector3 e1 = p_mesh.positions[p_mesh.indices[t * 3 + 1]] - p0;
		const Vector3 e2 = p_mesh.positions[p_mesh.indices[t * 3 + 2]] - p0;
		Triangle &triangle = p_mesh.bvh_triangles[i];
		for (int k = 0; k < 3; k++) {
			triangle.v0[k] = p0[k];
			triangle.e1[k] = e1[k];
			triangle.e2[k] = e2[k];
		}
		triangle.e1[3] = 0.0f;
		triangle.e2[3] = 0.0f;
		triangle.surface = p_mesh.triangle_surface[t];
	}
	p_mesh.built = true;
}

void RayScene::_ensure_buffer(RID &r_buffer, uint32_t &r_capacity, uint32_t p_count, uint32_t p_stride, const char *p_name) {
	if (r_buffer.is_valid() && r_capacity >= p_count) {
		return;
	}
	if (r_buffer.is_valid()) {
		RD::get_singleton()->free_rid(r_buffer);
	}
	r_capacity = MAX(MAX(p_count, r_capacity * 2), 64u);
	r_buffer = RD::get_singleton()->storage_buffer_create(r_capacity * p_stride);
	RD::get_singleton()->set_resource_name(r_buffer, p_name);
}

void RayScene::_assemble(const LocalVector<RID> &p_meshes) {
	blas_nodes.clear();
	triangles.clear();
	assembled_meshes.clear();
	for (const RID &rid : p_meshes) {
		MeshEntry *mesh = meshes.getptr(rid);
		if (!mesh || assembled_meshes.has(rid)) {
			continue;
		}
		mesh->node_offset = blas_nodes.size();
		mesh->triangle_offset = triangles.size();
		for (const Node &source : mesh->nodes) {
			Node node = source;
			node.left_first += node.count ? mesh->triangle_offset : mesh->node_offset;
			blas_nodes.push_back(node);
		}
		for (const Triangle &triangle : mesh->bvh_triangles) {
			triangles.push_back(triangle);
		}
		assembled_meshes.insert(rid);
	}
	if (blas_nodes.is_empty()) {
		LocalVector<BuildPrimitive> no_primitives;
		build_bvh(no_primitives, BLAS_LEAF_SIZE, BLAS_MAX_DEPTH, blas_nodes);
	}
	if (triangles.is_empty()) {
		triangles.push_back(Triangle());
	}
	_ensure_buffer(blas_node_buffer, blas_node_capacity, blas_nodes.size(), sizeof(Node), "Ray Scene BLAS Nodes");
	_ensure_buffer(triangle_buffer, triangle_capacity, triangles.size(), sizeof(Triangle), "Ray Scene Triangles");
	RD::get_singleton()->buffer_update(blas_node_buffer, 0, blas_nodes.size() * sizeof(Node), blas_nodes.ptr());
	RD::get_singleton()->buffer_update(triangle_buffer, 0, triangles.size() * sizeof(Triangle), triangles.ptr());
	assembly_dirty = false;
}

static bool _param_color(const Variant &p_value, Color &r_color) {
	if (p_value.get_type() == Variant::COLOR) {
		r_color = p_value;
		return true;
	}
	if (p_value.get_type() == Variant::VECTOR3) {
		const Vector3 v = p_value;
		r_color = Color(v.x, v.y, v.z);
		return true;
	}
	return false;
}

static float _param_float(const Variant &p_value, float p_default) {
	return (p_value.get_type() == Variant::FLOAT || p_value.get_type() == Variant::INT) ? float(p_value) : p_default;
}

static void _store_color(float *r_values, const Color &p_color, float p_scale) {
	r_values[0] = p_color.r * p_scale;
	r_values[1] = p_color.g * p_scale;
	r_values[2] = p_color.b * p_scale;
}

static float _instance_float(int32_t p_offset, int32_t p_index) {
	if (p_offset < 0 || p_index < 0) {
		return 1.0f;
	}
	float value = 1.0f;
	return MaterialStorage::get_singleton()->global_shader_parameters_instance_read_float(p_offset + p_index, value) ? value : 1.0f;
}

void RayScene::_instance_slots(RID p_material, CachedMaterial &r_cached) const {
	if (p_material.is_null()) {
		return;
	}
	const MaterialStorage *material_storage = MaterialStorage::get_singleton();
	r_cached.scale_index = material_storage->material_get_instance_float_index(p_material, SNAME("oc_emission_scale"));
	r_cached.bounce_index = material_storage->material_get_instance_float_index(p_material, SNAME("oc_emission_bounce"));
}

static constexpr float ALBEDO_MAX = 0.9f;
static const Color TEXTURE_MEAN_ALBEDO = Color(0.1759f, 0.1502f, 0.1185f);

static void _store_albedo(float *r_values, const Color &p_color, float p_scale) {
	const Color albedo(MAX(p_color.r * p_scale, 0.0f), MAX(p_color.g * p_scale, 0.0f), MAX(p_color.b * p_scale, 0.0f));
	const float peak = albedo.get_v();
	const float scale = peak > ALBEDO_MAX ? ALBEDO_MAX / peak : 1.0f;
	r_values[0] = albedo.r * scale;
	r_values[1] = albedo.g * scale;
	r_values[2] = albedo.b * scale;
}

static RID _param_texture(const Variant &p_value) {
	return p_value.get_type() == Variant::RID ? RID(p_value) : RID();
}

void RayScene::_warn_albedo_fallback(RID p_material, RID p_texture, RID p_mesh, uint32_t p_surface) {
	if (albedo_fallback_warned.has(p_material)) {
		return;
	}
	albedo_fallback_warned.insert(p_material);
	WARN_PRINT(vformat("Ray scene: a material without gi_albedo takes the texture-mean albedo fallback (mesh \"%s\", surface %d, texture \"%s\").", MeshStorage::get_singleton()->mesh_get_path(p_mesh), p_surface, TextureStorage::get_singleton()->texture_get_path(p_texture)));
}

void RayScene::_material_entry(RID p_material, RID p_mesh, uint32_t p_surface, Material &r_material) {
	r_material = {};
	r_material.albedo[0] = r_material.albedo[1] = r_material.albedo[2] = 0.48f;
	if (p_material.is_null()) {
		albedo_sources[ALBEDO_DEFAULT]++;
		return;
	}
	MaterialStorage *material_storage = MaterialStorage::get_singleton();

	Color albedo;
	Color emission;
	if (_param_color(material_storage->material_get_param(p_material, SNAME("gi_albedo")), albedo) && albedo.r >= 0.0f && albedo.g >= 0.0f && albedo.b >= 0.0f) {
		if (albedo.get_v() > ALBEDO_MAX) {
			albedo_sources[ALBEDO_GI_CAPPED]++;
		}
		_store_albedo(r_material.albedo, albedo, 1.0f);
		albedo_sources[ALBEDO_GI]++;
	} else if (_param_color(material_storage->material_get_declared_param(p_material, SNAME("base_color")), albedo)) {
		const float weight = CLAMP(_param_float(material_storage->material_get_declared_param(p_material, SNAME("base_weight")), 1.0f), 0.0f, 1.0f);
		RID texture;
		if (material_storage->material_get_declared_param(p_material, SNAME("use_base_color_opacity_texture")).booleanize()) {
			texture = _param_texture(material_storage->material_get_declared_param(p_material, SNAME("base_color_opacity_texture")));
		}
		if (texture.is_valid()) {
			Color tint = albedo;
			if (material_storage->material_get_declared_param(p_material, SNAME("use_texture_ranges")).booleanize()) {
				_param_color(material_storage->material_get_declared_param(p_material, SNAME("base_color_texture_tint")), tint);
			}
			albedo = tint.srgb_to_linear() * TEXTURE_MEAN_ALBEDO;
			_warn_albedo_fallback(p_material, texture, p_mesh, p_surface);
			albedo_sources[ALBEDO_TEXTURED]++;
		} else {
			albedo = albedo.srgb_to_linear();
			albedo_sources[ALBEDO_UNTEXTURED]++;
		}
		_store_albedo(r_material.albedo, albedo, weight);
	} else if (_param_color(material_storage->material_get_declared_param(p_material, SNAME("albedo")), albedo)) {
		albedo = albedo.srgb_to_linear();
		const RID texture = _param_texture(material_storage->material_get_declared_param(p_material, SNAME("texture_albedo")));
		if (texture.is_valid()) {
			albedo *= TEXTURE_MEAN_ALBEDO;
			_warn_albedo_fallback(p_material, texture, p_mesh, p_surface);
		}
		_store_albedo(r_material.albedo, albedo, 1.0f);
		albedo_sources[ALBEDO_STANDARD]++;
	} else {
		albedo_sources[ALBEDO_DEFAULT]++;
	}

	if (_param_color(material_storage->material_get_param(p_material, SNAME("gi_emission")), emission) && emission.r >= 0.0f && emission.g >= 0.0f && emission.b >= 0.0f) {
		_store_color(r_material.emission, emission, emission_scale);
	} else if (_param_color(material_storage->material_get_declared_param(p_material, SNAME("emission_color")), emission)) {
		const float luminance = _param_float(material_storage->material_get_declared_param(p_material, SNAME("emission_luminance")), 0.0f);
		const float bounce = _param_float(material_storage->material_get_declared_param(p_material, SNAME("emission_bounce_weight")), 1.0f);
		emission = emission.srgb_to_linear();
		const float multiplier = luminance * bounce * emission_scale;
		_store_color(r_material.emission, emission, multiplier);
	} else if (_param_color(material_storage->material_get_declared_param(p_material, SNAME("emission")), emission)) {
		const float multiplier = _param_float(material_storage->material_get_declared_param(p_material, SNAME("emission_energy")), 1.0f);
		emission = emission.srgb_to_linear();
		_store_color(r_material.emission, emission, multiplier);
	}

	if (material_storage->material_get_cull_mode(p_material) == RSE::CULL_MODE_DISABLED) {
		r_material.flags |= MATERIAL_FLAG_TWO_SIDED;
	}
	if (!material_storage->material_casts_shadows(p_material)) {
		r_material.flags |= MATERIAL_FLAG_SKIP;
	}
	const float coverage = _param_float(material_storage->material_get_param(p_material, SNAME("gi_coverage")), -1.0f);
	if (coverage >= 0.0f) {
		r_material.flags |= MATERIAL_FLAG_BLEND;
		r_material.coverage = MIN(coverage, 1.0f);
	}
	const Variant shadow_pass = material_storage->material_get_param(p_material, SNAME("gi_shadow_pass"));
	if (shadow_pass.get_type() == Variant::BOOL ? bool(shadow_pass) : _param_float(shadow_pass, 0.0f) > 0.5f) {
		r_material.flags |= MATERIAL_FLAG_SHADOW_PASS;
	}
}

float RayScene::light_energy(float p_energy, float p_intensity, RSE::LightType p_type, bool p_physical_units, float p_exposure, const Vector2 &p_area_size, bool p_area_normalize) {
	float energy = LightStorage::light_render_energy(p_energy, p_intensity, p_type, p_physical_units);
	energy *= p_exposure;
	if (p_type == RSE::LIGHT_AREA && p_area_normalize) {
		energy /= p_area_size.x * p_area_size.y;
	}
	return energy;
}

void RayScene::pack_area(const Basis &p_basis, const Vector2 &p_area_size, Light &r_light) {
	const Vector3 half_a = p_basis.get_column(Vector3::AXIS_X).normalized() * (p_area_size.x * 0.5f);
	const Vector3 half_b = p_basis.get_column(Vector3::AXIS_Y).normalized() * (p_area_size.y * 0.5f);
	for (int k = 0; k < 3; k++) {
		r_light.half_a[k] = half_a[k];
		r_light.half_b[k] = half_b[k];
	}
	r_light.area = p_area_size.x * p_area_size.y;
}

void RayScene::_pack_light(RID p_light_instance, bool p_physical_units, float p_exposure, uint32_t p_limit, LocalVector<Light> &r_lights) {
	LightStorage *light_storage = LightStorage::get_singleton();
	if (r_lights.size() >= p_limit || !light_storage->owns_light_instance(p_light_instance)) {
		return;
	}
	const RID light = light_storage->light_instance_get_base_light(p_light_instance);
	const RSE::LightType type = light_storage->light_get_type(light);
	if (light_storage->light_get_bake_mode(light) == RSE::LIGHT_BAKE_DISABLED) {
		return;
	}
	const Vector2 area_size = type == RSE::LIGHT_AREA ? light_storage->light_area_get_size(light) : Vector2();
	if (type == RSE::LIGHT_AREA && !(area_size.x > 0.0f && area_size.y > 0.0f)) {
		return;
	}

	const Transform3D transform = light_storage->light_instance_get_base_transform(p_light_instance);
	const Vector3 direction = -transform.basis.get_column(Vector3::AXIS_Z).normalized();
	const Vector3 position = transform.origin - anchor;
	const Color color = light_storage->light_get_color(light).srgb_to_linear();

	float energy = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ENERGY) * light_storage->light_get_param(light, RSE::LIGHT_PARAM_INDIRECT_ENERGY);
	if (light_storage->light_is_negative(light)) {
		energy = -energy;
	}
	energy = light_energy(energy, light_storage->light_get_param(light, RSE::LIGHT_PARAM_INTENSITY), type, p_physical_units, p_exposure, area_size, type == RSE::LIGHT_AREA && light_storage->light_area_get_normalize_energy(light));

	Light packed = {};
	for (int k = 0; k < 3; k++) {
		packed.direction[k] = direction[k];
		packed.position[k] = position[k];
	}
	packed.color[0] = color.r;
	packed.color[1] = color.g;
	packed.color[2] = color.b;
	packed.energy = energy;
	packed.type = type;
	packed.attenuation = light_storage->light_get_param(light, RSE::LIGHT_PARAM_ATTENUATION);
	packed.inv_radius = 1.0f / MAX(0.001f, light_storage->light_get_param(light, RSE::LIGHT_PARAM_RANGE));
	packed.cos_spot_angle = Math::cos(Math::deg_to_rad(light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ANGLE)));
	packed.inv_spot_attenuation = 1.0f / MAX(0.0001f, light_storage->light_get_param(light, RSE::LIGHT_PARAM_SPOT_ATTENUATION));
	packed.caster_mask = light_storage->light_get_shadow_caster_mask(light);
	if (type == RSE::LIGHT_AREA) {
		pack_area(transform.basis, area_size, packed);
	}
	if (type != RSE::LIGHT_DIRECTIONAL) {
		census.candidates++;
		census.area_candidates += type == RSE::LIGHT_AREA;
		census.area_projected += type == RSE::LIGHT_AREA && light_storage->light_area_get_texture(light).is_valid();
		census.masked_candidates += packed.caster_mask != 0xFFFFFFFFu;
	}
	DDGILightTracker::observe(p_light_instance, packed, anchor);
	r_lights.push_back(packed);
}

struct LightOrder {
	bool operator()(const RayScene::Light &p_a, const RayScene::Light &p_b) const {
		for (int k = 0; k < 3; k++) {
			if (p_a.position[k] != p_b.position[k]) {
				return p_a.position[k] < p_b.position[k];
			}
		}
		return p_a.inv_radius < p_b.inv_radius;
	}
};

struct LightNearer {
	Vector3 camera;
	static float score(const RayScene::Light &p_light, const Vector3 &p_camera) {
		float reach = 1.0f / p_light.inv_radius;
		if (p_light.type == uint32_t(RSE::LIGHT_AREA)) {
			reach += Math::sqrt(Vector3(p_light.half_a[0], p_light.half_a[1], p_light.half_a[2]).length_squared() + Vector3(p_light.half_b[0], p_light.half_b[1], p_light.half_b[2]).length_squared());
		}
		return Vector3(p_light.position[0], p_light.position[1], p_light.position[2]).distance_to(p_camera) - reach;
	}
	bool operator()(const RayScene::Light &p_a, const RayScene::Light &p_b) const {
		return score(p_a, camera) < score(p_b, camera);
	}
};

static double light_power(const RayScene::Light &p_light) {
	return double(p_light.energy) * (0.2126 * p_light.color[0] + 0.7152 * p_light.color[1] + 0.0722 * p_light.color[2]);
}

void RayScene::_select_lights(const Vector3 &p_camera) {
	if (lights.size() + local_lights.size() > MAX_LIGHTS) {
		const uint32_t keep = MAX_LIGHTS - lights.size();
		SortArray<Light, LightNearer> nearest;
		nearest.compare.camera = p_camera;
		LocalVector<Light> area;
		LocalVector<Light> rest;
		for (const Light &light : local_lights) {
			if (light.type == uint32_t(RSE::LIGHT_AREA)) {
				area.push_back(light);
			} else {
				rest.push_back(light);
			}
		}
		const uint32_t reserved = MIN(MIN(area.size(), RESERVED_AREA_LIGHTS), keep);
		if (area.size() > reserved) {
			nearest.nth_element(0, area.size(), reserved, area.ptr());
		}
		for (uint32_t i = reserved; i < area.size(); i++) {
			rest.push_back(area[i]);
		}
		const uint32_t rest_keep = keep - reserved;
		if (rest.size() > rest_keep) {
			nearest.nth_element(0, rest.size(), rest_keep, rest.ptr());
		}
		for (uint32_t i = rest_keep; i < rest.size(); i++) {
			if (rest[i].caster_mask != 0xFFFFFFFFu) {
				census.masked_cut++;
				census.masked_cut_energy += light_power(rest[i]);
			}
		}
		rest.resize(MIN(rest.size(), rest_keep));
		census.area_reserved = reserved;
		local_lights.clear();
		for (uint32_t i = 0; i < reserved; i++) {
			local_lights.push_back(area[i]);
		}
		for (const Light &light : rest) {
			local_lights.push_back(light);
		}
	}
	for (const Light &light : local_lights) {
		census.area_packed += light.type == uint32_t(RSE::LIGHT_AREA);
		if (light.caster_mask != 0xFFFFFFFFu) {
			census.masked_packed_energy += light_power(light);
		}
	}
	SortArray<Light, LightOrder> ordered;
	ordered.sort(local_lights.ptr(), local_lights.size());
	for (const Light &light : local_lights) {
		lights.push_back(light);
	}
}

void RayScene::_print_census() {
	LocalVector<uint32_t> masks;
	for (const Light &light : lights) {
		if (light.caster_mask != 0xFFFFFFFFu && masks.find(light.caster_mask) < 0) {
			masks.push_back(light.caster_mask);
		}
	}
	for (const Instance &instance : instances) {
		for (const uint32_t mask : masks) {
			if ((instance.layer_mask & mask) == 0u) {
				census.masked_instances++;
				break;
			}
		}
	}
	if (census != printed_census) {
		printed_census = census;
		print_line(vformat("RAYSCENE_CENSUS candidates %d area_candidates %d area_packed %d area_reserved %d area_projected %d masked_candidates %d masked_cut %d masked_packed_power %f masked_cut_power %f masked_instances %d",
				census.candidates, census.area_candidates, census.area_packed, census.area_reserved, census.area_projected, census.masked_candidates, census.masked_cut, census.masked_packed_energy, census.masked_cut_energy, census.masked_instances));
	}
}

void RayScene::update(const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, const Vector<RID> &p_directional_lights, RID p_camera_attributes, const Vector3 &p_camera_position) {
	MutexLock lock(mesh_mutex);

	const Vector3 snapped = (p_camera_position / ANCHOR_GRID).floor() * ANCHOR_GRID;
	const bool anchor_moved = snapped != anchor;
	anchor = snapped;

	LocalVector<RID> referenced;
	bool need_assembly = assembly_dirty || blas_node_buffer.is_null();
	for (uint32_t i = 0; i < p_instances.size(); i++) {
		RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(p_instances[i]);
		if (!geometry->data || geometry->data->base_type != RSE::INSTANCE_MESH || geometry->data->skeleton.is_valid()) {
			continue;
		}
		MeshEntry *mesh = meshes.getptr(geometry->data->base);
		if (!mesh) {
			mesh = _capture_mesh(geometry->data->base);
		}
		if (!mesh || mesh->triangle_surface.is_empty()) {
			continue;
		}
		if (!mesh->built) {
			_build_blas(*mesh);
			need_assembly = true;
		}
		if (!assembled_meshes.has(geometry->data->base)) {
			need_assembly = true;
		}
		referenced.push_back(geometry->data->base);
	}
	if (need_assembly) {
		_assemble(referenced);
	}

	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	instances.clear();
	materials.clear();
	material_cache.clear();
	memset(albedo_sources, 0, sizeof(albedo_sources));
	emission_scale = _param_float(MaterialStorage::get_singleton()->global_shader_parameter_get_internal(SNAME("openpbr_emission_scale")), 1.0f);
	LocalVector<BuildPrimitive> primitives;
	uint32_t signature = HASH_MURMUR3_SEED;
	for (uint32_t i = 0; i < p_instances.size(); i++) {
		RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(p_instances[i]);
		if (!geometry->data || geometry->data->base_type != RSE::INSTANCE_MESH || geometry->data->skeleton.is_valid()) {
			continue;
		}
		const RID mesh_rid = geometry->data->base;
		const MeshEntry *mesh = meshes.getptr(mesh_rid);
		if (!mesh || !mesh->built || !assembled_meshes.has(mesh_rid)) {
			continue;
		}
		if (instances.size() >= (TLAS_LEAF_SIZE << TLAS_MAX_DEPTH)) {
			break;
		}

		Transform3D transform = geometry->transform;
		transform.origin -= anchor;
		const Transform3D inverse = transform.affine_inverse();

		Instance instance = {};
		MaterialStorage::store_transform_transposed_3x4(inverse, instance.world_to_object);
		instance.blas_root = mesh->node_offset;
		instance.material_offset = materials.size();
		instance.layer_mask = geometry->layer_mask;

		for (uint32_t s = 0; s < mesh->surface_count; s++) {
			RID material = geometry->data->material_override;
			if (material.is_null() && s < uint32_t(geometry->data->surface_materials.size())) {
				material = geometry->data->surface_materials[s];
			}
			if (material.is_null()) {
				material = mesh_storage->mesh_surface_get_material(mesh_rid, s);
			}
			CachedMaterial *cached = material_cache.getptr(material);
			if (!cached) {
				CachedMaterial entry;
				_material_entry(material, mesh_rid, s, entry.entry);
				_instance_slots(material, entry);
				cached = &material_cache.insert(material, entry)->value;
			}
			materials.push_back(cached->entry);
			const float factor = MAX(_instance_float(geometry->shader_uniforms_offset, cached->scale_index), 0.0f) * MAX(_instance_float(geometry->shader_uniforms_offset, cached->bounce_index), 0.0f);
			if (factor != 1.0f) {
				Material &instance_material = materials[materials.size() - 1];
				for (int k = 0; k < 3; k++) {
					instance_material.emission[k] *= factor;
				}
			}
		}

		const AABB bounds = transform.xform(mesh->aabb);
		BuildPrimitive primitive;
		for (int k = 0; k < 3; k++) {
			primitive.bmin[k] = bounds.position[k];
			primitive.bmax[k] = bounds.position[k] + bounds.size[k];
			primitive.centroid[k] = bounds.position[k] + bounds.size[k] * 0.5f;
		}
		primitive.index = instances.size();
		primitives.push_back(primitive);
		instances.push_back(instance);
		signature = hash_murmur3_buffer(&instance, sizeof(Instance), signature);
	}

	if (signature != instance_signature || anchor_moved || need_assembly || tlas_node_buffer.is_null()) {
		instance_signature = signature;
		if (memcmp(albedo_sources, reported_albedo_sources, sizeof(albedo_sources)) != 0) {
			memcpy(reported_albedo_sources, albedo_sources, sizeof(albedo_sources));
			print_line(vformat("Ray scene albedo: gi_albedo %d (%d capped at 0.9), textured fallback %d, untextured %d, standard %d, default %d.", albedo_sources[ALBEDO_GI], albedo_sources[ALBEDO_GI_CAPPED], albedo_sources[ALBEDO_TEXTURED], albedo_sources[ALBEDO_UNTEXTURED], albedo_sources[ALBEDO_STANDARD], albedo_sources[ALBEDO_DEFAULT]));
		}
		build_bvh(primitives, TLAS_LEAF_SIZE, TLAS_MAX_DEPTH, tlas_nodes);
		LocalVector<Instance> ordered;
		ordered.resize(MAX(1u, instances.size()));
		for (uint32_t i = 0; i < primitives.size(); i++) {
			ordered[i] = instances[primitives[i].index];
		}
		if (instances.is_empty()) {
			ordered[0] = Instance();
		}
		_ensure_buffer(tlas_node_buffer, tlas_node_capacity, tlas_nodes.size(), sizeof(Node), "Ray Scene TLAS Nodes");
		_ensure_buffer(instance_buffer, instance_capacity, ordered.size(), sizeof(Instance), "Ray Scene Instances");
		RD::get_singleton()->buffer_update(tlas_node_buffer, 0, tlas_nodes.size() * sizeof(Node), tlas_nodes.ptr());
		RD::get_singleton()->buffer_update(instance_buffer, 0, ordered.size() * sizeof(Instance), ordered.ptr());
	}

	if (materials.is_empty()) {
		materials.push_back(Material());
	}
	_ensure_buffer(material_buffer, material_capacity, materials.size(), sizeof(Material), "Ray Scene Materials");
	RD::get_singleton()->buffer_update(material_buffer, 0, materials.size() * sizeof(Material), materials.ptr());

	const bool physical_units = RendererSceneRenderRD::get_singleton()->is_using_physical_light_units();
	const float exposure = p_camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_camera_attributes) : 1.0f;
	lights.clear();
	local_lights.clear();
	census = LightCensus();
	DDGILightTracker::begin(GLOBAL_GET_CACHED(bool, "rendering/global_illumination/ddgi/light_change_reset"), RSG::rasterizer->get_frame_number(), MAX(GLOBAL_GET_CACHED(float, "rendering/global_illumination/ddgi/probe_spacing"), 0.1f));
	for (const RID &light_instance : p_directional_lights) {
		_pack_light(light_instance, physical_units, exposure, MAX_LIGHTS, lights);
	}
	for (uint32_t i = 0; i < p_lights.size(); i++) {
		_pack_light(p_lights[i], physical_units, exposure, MAX_CANDIDATE_LIGHTS, local_lights);
	}
	_select_lights(p_camera_position - anchor);
	DDGILightTracker::end();
	if (OS::get_singleton()->is_stdout_verbose()) {
		_print_census();
	}
	_ensure_buffer(light_buffer, light_capacity, MAX_LIGHTS, sizeof(Light), "Ray Scene Lights");
	if (lights.size()) {
		RD::get_singleton()->buffer_update(light_buffer, 0, lights.size() * sizeof(Light), lights.ptr());
	}

	SceneData scene_data = {};
	const Vector3 camera_to_anchor = p_camera_position - anchor;
	scene_data.camera_to_anchor[0] = camera_to_anchor.x;
	scene_data.camera_to_anchor[1] = camera_to_anchor.y;
	scene_data.camera_to_anchor[2] = camera_to_anchor.z;
	scene_data.instance_count = instances.size();
	scene_data.light_count = lights.size();
	if (scene_data_buffer.is_null()) {
		scene_data_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(SceneData));
		RD::get_singleton()->set_resource_name(scene_data_buffer, "Ray Scene Data");
	}
	RD::get_singleton()->buffer_update(scene_data_buffer, 0, sizeof(SceneData), &scene_data);
}
