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
#include "servers/rendering/renderer_rd/environment/gi_epochs.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/texture_storage.h"
#include "servers/rendering/rendering_method.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/rendering_server_globals.h"

#include <cfloat>

using namespace RendererRD;

RayScene *RayScene::singleton = nullptr;
SafeFlag RayScene::needed;

bool RayScene::_settings_need() {
	return DDGI::is_enabled() || GLOBAL_GET_CACHED(bool, "rendering/global_illumination/direct/enabled");
}

bool RayScene::is_needed() {
	if (!needed.is_set() && _settings_need()) {
		needed.set();
	}
	return needed.is_set();
}

void RayScene::watch_needed() {
	if (needed.is_set() && !_settings_need()) {
		WARN_PRINT_ONCE("DDGI and direct shadows were turned off after the ray scene started. The ray scene stays on until the next start.");
	}
}

static bool _alpha_texture_live(RID p_texture) {
	return TextureStorage::get_singleton()->owns_texture(p_texture);
}

RayScene::RayScene() {
	singleton = this;
	alpha_enabled = GLOBAL_GET("rendering/global_illumination/ray_scene/alpha_test");
	alpha_store.set_liveness(&_alpha_texture_live);
	RD::TextureFormat emission_format;
	emission_format.format = RD::DATA_FORMAT_R8G8B8A8_UNORM;
	emission_format.texture_type = RD::TEXTURE_TYPE_2D_ARRAY;
	emission_format.width = 1;
	emission_format.height = 1;
	emission_format.array_layers = 2;
	emission_format.usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_CAN_UPDATE_BIT;
	emission_dummy = RD::get_singleton()->texture_create(emission_format, RD::TextureView());
	RD::get_singleton()->set_resource_name(emission_dummy, "Ray Scene Emission Dummy");
	if (emission_dummy.is_valid()) {
		for (int64_t layer = 0; layer < 2; layer++) {
			RD::get_singleton()->texture_update(emission_dummy, layer, Vector<uint8_t>({ 255, 255, 255, 255 }));
		}
	}
}

RayScene::~RayScene() {
	_print_tlas_rebuilds(true);
	_hw_free();
	RID *owned[] = { &blas_node_buffer, &triangle_buffer, &instance_buffer, &tlas_node_buffer, &material_buffer, &light_buffer, &scene_data_buffer, &triangle_uv_buffer, &triangle_emitter_buffer, &emission_dummy };
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

uint32_t RayScene::float_to_half(float p_value) {
	uint32_t bits;
	memcpy(&bits, &p_value, sizeof(uint32_t));
	const uint32_t sign = (bits >> 16) & 0x8000u;
	const uint32_t magnitude = bits & 0x7FFFFFFFu;
	if (magnitude >= 0x7F800000u) {
		return sign | (magnitude > 0x7F800000u ? 0x7E00u : 0x7C00u);
	}
	if (magnitude >= 0x477FF000u) {
		return sign | 0x7BFFu;
	}
	if (magnitude <= 0x33000000u) {
		return sign;
	}
	const uint32_t exponent = magnitude >> 23;
	if (exponent < 113u) {
		const uint32_t mantissa = (magnitude & 0x007FFFFFu) | 0x00800000u;
		const uint32_t shift = 126u - exponent;
		uint32_t half = mantissa >> shift;
		const uint32_t remainder = mantissa & ((1u << shift) - 1u);
		const uint32_t halfway = 1u << (shift - 1u);
		if (remainder > halfway || (remainder == halfway && (half & 1u))) {
			half++;
		}
		return sign | half;
	}
	uint32_t half = ((exponent - 112u) << 10) | ((magnitude & 0x007FFFFFu) >> 13);
	const uint32_t remainder = magnitude & 0x1FFFu;
	if (remainder > 0x1000u || (remainder == 0x1000u && (half & 1u))) {
		half++;
	}
	return sign | half;
}

uint32_t RayScene::pack_half2(float p_x, float p_y) {
	return float_to_half(p_x) | (float_to_half(p_y) << 16);
}

void RayScene::mesh_surface_added(RID p_mesh, uint32_t p_surface, const RenderingServerTypes::SurfaceData &p_surface_data) {
	MutexLock lock(mesh_mutex);
	if (!meshes.has(p_mesh) && (p_surface != 0 || !is_needed())) {
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
	if (p_surface_data.vertex_count == 0 && p_surface_data.index_count == 0) {
		return;
	}
	if (r_mesh.built) {
		r_mesh.built = false;
		r_mesh.nodes.clear();
		r_mesh.bvh_triangles.clear();
		r_mesh.bvh_uvs.clear();
		r_mesh.hw_generation = 0;
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
	const PackedVector2Array uv_array = arrays[RSE::ARRAY_TEX_UV];
	float uv_extent = 0.0f;
	for (uint32_t v = 0; v < vertex_count; v++) {
		const Vector2 uv = v < uint32_t(uv_array.size()) ? uv_array[v] : Vector2();
		uv_extent = MAX(uv_extent, MAX(Math::abs(float(uv.x)), Math::abs(float(uv.y))));
		r_mesh.uvs.push_back(pack_half2(float(uv.x), float(uv.y)));
	}
	while (r_mesh.surface_uv_extent.size() <= p_surface) {
		r_mesh.surface_uv_extent.push_back(0.0f);
	}
	r_mesh.surface_uv_extent[p_surface] = uv_extent;
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
	_hw_mesh_removed(p_mesh);
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
	p_mesh.bvh_uvs.clear();
	p_mesh.bvh_uvs.resize(triangle_count);
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
		for (uint32_t v = 0; v < 3; v++) {
			const uint32_t index = p_mesh.indices[t * 3 + v];
			p_mesh.bvh_uvs[i].uv[v] = index < p_mesh.uvs.size() ? p_mesh.uvs[index] : 0u;
		}
	}
	p_mesh.built = true;
	p_mesh.build_serial = ++next_build_serial;
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

void RayScene::_update_triangle_emitters(const LocalVector<TriangleEmitterSource> &p_sources, const LocalVector<uint32_t> &p_order) {
	triangle_emitters.clear();
	bool capped = false;
	for (const TriangleEmitterSource &source : p_sources) {
		const MeshEntry *mesh = meshes.getptr(source.mesh);
		if (!mesh || !mesh->built || source.instance >= p_order.size()) {
			continue;
		}
		for (uint32_t t = 0; t < uint32_t(mesh->triangle_surface.size()); t++) {
			const uint32_t surface = mesh->triangle_surface[t];
			if (surface >= source.surface_count || source.material_offset + surface >= materials.size()) {
				continue;
			}
			const Material &material = materials[source.material_offset + surface];
			if (!(material.emission[0] > 0.0f || material.emission[1] > 0.0f || material.emission[2] > 0.0f) && material.emission_texture == 0) {
				continue;
			}
			if (triangle_emitters.size() >= MAX_TRIANGLE_EMITTERS) {
				capped = true;
				break;
			}
			TriangleEmitter emitter = {};
			float *const corners[3] = { emitter.v0, emitter.v1, emitter.v2 };
			for (uint32_t v = 0; v < 3; v++) {
				const Vector3 position = source.transform.xform(mesh->positions[mesh->indices[t * 3 + v]]);
				corners[v][0] = position.x;
				corners[v][1] = position.y;
				corners[v][2] = position.z;
			}
			emitter.instance = p_order[source.instance];
			emitter.material = source.material_offset + surface;
			emitter.uv_index = mesh->triangle_offset + t;
			triangle_emitters.push_back(emitter);
		}
		if (capped) {
			break;
		}
	}
	triangle_emitter_count = triangle_emitters.size();
	triangle_emitters_capped = capped;
	uint32_t emitter_instances = 0;
	{
		HashSet<uint32_t> unique_instances;
		for (const TriangleEmitter &emitter : triangle_emitters) {
			if (unique_instances.has(emitter.instance)) {
				continue;
			}
			unique_instances.insert(emitter.instance);
			emitter_instances++;
		}
	}
	if (triangle_emitters.is_empty()) {
		triangle_emitters.push_back(TriangleEmitter());
	}
	print_line(vformat("Ray scene triangle emitters: %d triangles across %d instances%s", triangle_emitter_count, emitter_instances, triangle_emitters_capped ? " (capped)" : ""));
	if (triangles_logged < 64) {
		triangles_logged++;
		for (const TriangleEmitterSource &source : p_sources) {
			const MeshEntry *mesh = meshes.getptr(source.mesh);
			if (!mesh || !mesh->built || source.instance >= p_order.size()) {
				continue;
			}
			uint32_t instance_triangles = 0;
			HashSet<uint32_t> surfaces;
			for (uint32_t t = 0; t < uint32_t(mesh->triangle_surface.size()); t++) {
				const uint32_t surface = mesh->triangle_surface[t];
				if (surface >= source.surface_count || source.material_offset + surface >= materials.size()) {
					continue;
				}
				const Material &material = materials[source.material_offset + surface];
				if (!(material.emission[0] > 0.0f || material.emission[1] > 0.0f || material.emission[2] > 0.0f) && material.emission_texture == 0) {
					continue;
				}
				instance_triangles++;
				surfaces.insert(surface);
			}
			if (instance_triangles == 0) {
				continue;
			}
			String surface_list;
			for (const uint32_t &surface : surfaces) {
				const Material &material = materials[source.material_offset + surface];
				if (!surface_list.is_empty()) {
					surface_list += ",";
				}
				surface_list += vformat("%d(layer %d scale %.4f flat %.3f)", surface, int(material.emission_texture), material.emission_texel_scale, material.emission_mean_luminance);
			}
			print_line(vformat("Ray scene emitter: mesh \"%s\" instance %d triangles %d surfaces %s", MeshStorage::get_singleton()->mesh_get_path(source.mesh), int(p_order[source.instance]), int(instance_triangles), surface_list));
		}
	}
	_ensure_buffer(triangle_emitter_buffer, triangle_emitter_capacity, triangle_emitters.size(), sizeof(TriangleEmitter), "Ray Scene Triangle Emitters");
	RD::get_singleton()->buffer_update(triangle_emitter_buffer, 0, triangle_emitters.size() * sizeof(TriangleEmitter), triangle_emitters.ptr());
}

void RayScene::_assemble(const LocalVector<RID> &p_meshes) {
	blas_nodes.clear();
	triangles.clear();
	assembled_meshes.clear();
	LocalVector<TriangleUv> triangle_uvs;
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
		for (uint32_t i = 0; i < mesh->bvh_triangles.size(); i++) {
			triangle_uvs.push_back(i < mesh->bvh_uvs.size() ? mesh->bvh_uvs[i] : TriangleUv());
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
	if (triangle_uvs.is_empty()) {
		triangle_uvs.push_back(TriangleUv());
	}
	_ensure_buffer(triangle_uv_buffer, triangle_uv_capacity, triangle_uvs.size(), sizeof(TriangleUv), "Ray Scene Triangle UVs");
	RD::get_singleton()->buffer_update(triangle_uv_buffer, 0, triangle_uvs.size() * sizeof(TriangleUv), triangle_uvs.ptr());
	_hw_assembled();
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

static float _albedo_max(float p_coverage) {
	if (!(p_coverage >= 0.0f)) {
		return ALBEDO_MAX;
	}
	return p_coverage > ALBEDO_MAX ? ALBEDO_MAX / MIN(p_coverage, 1.0f) : 1.0f;
}

static void _store_albedo(float *r_values, const Color &p_color, float p_scale, float p_max) {
	const Color albedo(MAX(p_color.r * p_scale, 0.0f), MAX(p_color.g * p_scale, 0.0f), MAX(p_color.b * p_scale, 0.0f));
	const float peak = albedo.get_v();
	const float scale = peak > p_max ? p_max / peak : 1.0f;
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

void RayScene::_material_entry(RID p_material, RID p_mesh, uint32_t p_surface, Material &r_material, float &r_alpha_threshold, uint32_t &r_cull) {
	r_material = {};
	r_material.albedo[0] = r_material.albedo[1] = r_material.albedo[2] = 0.48f;
	r_material.emission_uv_scale[0] = 1.0f;
	r_material.emission_uv_scale[1] = 1.0f;
	r_alpha_threshold = -1.0f;
	r_cull = uint32_t(RSE::CULL_MODE_DISABLED);
	if (p_material.is_null()) {
		albedo_sources[ALBEDO_DEFAULT]++;
		return;
	}
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	const float coverage = _param_float(material_storage->material_get_param(p_material, SNAME("gi_coverage")), -1.0f);
	const float albedo_max = _albedo_max(coverage);

	Color albedo;
	Color emission;
	if (_param_color(material_storage->material_get_param(p_material, SNAME("gi_albedo")), albedo) && albedo.r >= 0.0f && albedo.g >= 0.0f && albedo.b >= 0.0f) {
		if (albedo.get_v() > albedo_max) {
			albedo_sources[ALBEDO_GI_CAPPED]++;
		}
		_store_albedo(r_material.albedo, albedo, 1.0f, albedo_max);
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
		_store_albedo(r_material.albedo, albedo, weight, albedo_max);
	} else if (_param_color(material_storage->material_get_declared_param(p_material, SNAME("albedo")), albedo)) {
		albedo = albedo.srgb_to_linear();
		const RID texture = _param_texture(material_storage->material_get_declared_param(p_material, SNAME("texture_albedo")));
		if (texture.is_valid()) {
			albedo *= TEXTURE_MEAN_ALBEDO;
			_warn_albedo_fallback(p_material, texture, p_mesh, p_surface);
		}
		_store_albedo(r_material.albedo, albedo, 1.0f, albedo_max);
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
	r_material.emission_mean_luminance = 0.2126f * r_material.emission[0] + 0.7152f * r_material.emission[1] + 0.0722f * r_material.emission[2];
	if (material_storage->material_get_declared_param(p_material, SNAME("use_emission_mask_texture")).booleanize()) {
		const Variant atlas_index = material_storage->material_get_declared_param(p_material, SNAME("oc_emission_atlas_index"));
		if (atlas_index.get_type() == Variant::INT) {
			r_material.emission_texture = uint32_t(MAX(int(atlas_index), 0));
		}
		const Variant scale_value = material_storage->material_get_declared_param(p_material, SNAME("uv_scale"));
		if (scale_value.get_type() == Variant::VECTOR2) {
			const Vector2 scale = scale_value;
			r_material.emission_uv_scale[0] = scale.x;
			r_material.emission_uv_scale[1] = scale.y;
		}
		const Variant offset_value = material_storage->material_get_declared_param(p_material, SNAME("uv_offset"));
		if (offset_value.get_type() == Variant::VECTOR2) {
			const Vector2 offset = offset_value;
			r_material.emission_uv_offset[0] = offset.x;
			r_material.emission_uv_offset[1] = offset.y;
		}
		r_material.emission_nits_range[0] = 0.0f;
		r_material.emission_nits_range[1] = 1.0f;
		const Variant nits_value = material_storage->material_get_declared_param(p_material, SNAME("emission_luminance_texture_range"));
		if (nits_value.get_type() == Variant::VECTOR2) {
			const Vector2 nits = nits_value;
			r_material.emission_nits_range[0] = nits.x;
			r_material.emission_nits_range[1] = nits.y;
		}
		if (material_storage->material_get_declared_param(p_material, SNAME("use_texture_ranges")).booleanize()) {
			r_material.emission_flags |= EMISSION_FLAG_TEXTURE_RANGES;
		}
		r_material.emission_texel_scale = emission_scale;
	}

	const RSE::CullMode cull_mode = material_storage->material_get_cull_mode(p_material);
	r_cull = uint32_t(cull_mode);
	if (cull_mode == RSE::CULL_MODE_DISABLED) {
		r_material.flags |= MATERIAL_FLAG_TWO_SIDED;
	}
	if (!material_storage->material_casts_shadows(p_material)) {
		r_material.flags |= MATERIAL_FLAG_SKIP;
	} else if (skip_additive) {
		const MaterialStorage::ShaderData *shader_data = material_storage->material_get_shader_data(p_material);
		if (shader_data && shader_data->blends_additively()) {
			r_material.flags |= MATERIAL_FLAG_SKIP;
			additive_skipped++;
		}
	}
	if (coverage >= 0.0f) {
		r_material.flags |= MATERIAL_FLAG_BLEND;
		r_material.coverage = MIN(coverage, 1.0f);
	}
	const Variant shadow_pass = material_storage->material_get_param(p_material, SNAME("gi_shadow_pass"));
	if (shadow_pass.get_type() == Variant::BOOL ? bool(shadow_pass) : _param_float(shadow_pass, 0.0f) > 0.5f) {
		r_material.flags |= MATERIAL_FLAG_SHADOW_PASS;
	}
	const Variant scissor = material_storage->material_get_declared_param(p_material, SNAME("alpha_scissor_threshold"));
	const Variant body_alpha_test = material_storage->material_get_declared_param(p_material, SNAME("alpha_test"));
	if (scissor.get_type() != Variant::NIL) {
		r_alpha_threshold = _param_float(scissor, 0.5f);
	} else if (body_alpha_test.get_type() == Variant::BOOL && bool(body_alpha_test)) {
		r_alpha_threshold = _param_float(material_storage->material_get_declared_param(p_material, SNAME("alpha_threshold")), 0.5f);
	}
}

static bool _param_bool(const Variant &p_value) {
	return p_value.get_type() == Variant::BOOL && bool(p_value);
}

void RayScene::_alpha_entry(RID p_material, Material &r_material, float p_alpha_threshold, uint32_t p_cull) {
	MaterialStorage *material_storage = MaterialStorage::get_singleton();
	RayAlphaStore::EntryKey key;
	Variant texture_value;
	Vector2 scale(1.0f, 1.0f);
	Vector2 offset(0.0f, 0.0f);
	float cutoff = 0.0f;
	if (_param_bool(material_storage->material_get_declared_param(p_material, SNAME("use_base_color_opacity_texture")))) {
		texture_value = material_storage->material_get_declared_param(p_material, SNAME("base_color_opacity_texture"));
		const float opacity = _param_float(material_storage->material_get_declared_param(p_material, SNAME("geometry_opacity")), 1.0f);
		if (_param_bool(material_storage->material_get_declared_param(p_material, SNAME("base_color_opacity_custom0")))) {
			alpha_counts.solid_custom0++;
			return;
		}
		if (_param_bool(material_storage->material_get_declared_param(p_material, SNAME("use_vertex_alpha")))) {
			alpha_counts.vertex_alpha_ignored++;
		}
		const Variant scale_value = material_storage->material_get_declared_param(p_material, SNAME("uv_scale"));
		if (scale_value.get_type() == Variant::VECTOR2) {
			scale = scale_value;
		}
		const Variant offset_value = material_storage->material_get_declared_param(p_material, SNAME("uv_offset"));
		if (offset_value.get_type() == Variant::VECTOR2) {
			offset = offset_value;
		}
		if (material_storage->material_get_declared_param(p_material, SNAME("use_texture_ranges")).booleanize()) {
			Vector2 range(0.0f, 1.0f);
			const Variant range_value = material_storage->material_get_declared_param(p_material, SNAME("geometry_opacity_texture_range"));
			if (range_value.get_type() == Variant::VECTOR2) {
				range = range_value;
			}
			const float span = float(range.y - range.x);
			if (Math::abs(span) < 1e-6f) {
				alpha_counts.solid_degenerate++;
				return;
			}
			cutoff = (p_alpha_threshold - float(range.x)) / span;
			key.far_affine[0] = span;
			key.far_affine[1] = float(range.x);
		} else {
			if (opacity <= 0.0f) {
				alpha_counts.solid_degenerate++;
				return;
			}
			cutoff = p_alpha_threshold / opacity;
			key.far_affine[0] = opacity;
		}
	} else if (_param_bool(material_storage->material_get_declared_param(p_material, SNAME("has_albedo")))) {
		texture_value = material_storage->material_get_declared_param(p_material, SNAME("albedo_tex"));
		const float mat_alpha = _param_float(material_storage->material_get_declared_param(p_material, SNAME("mat_alpha")), 1.0f);
		if (mat_alpha <= 0.0f) {
			alpha_counts.solid_degenerate++;
			return;
		}
		cutoff = p_alpha_threshold / mat_alpha;
		key.far_affine[0] = mat_alpha;
	} else {
		texture_value = material_storage->material_get_declared_param(p_material, SNAME("texture_albedo"));
		Color albedo;
		if (!_param_color(material_storage->material_get_declared_param(p_material, SNAME("albedo")), albedo) || albedo.a <= 0.0f) {
			alpha_counts.solid_degenerate++;
			return;
		}
		cutoff = p_alpha_threshold / albedo.a;
		key.far_affine[0] = albedo.a;
	}
	if (texture_value.get_type() != Variant::RID || RID(texture_value).is_null()) {
		alpha_counts.solid_no_texture++;
		return;
	}
	cutoff = CLAMP(cutoff, 0.0f, 1.0f);
	const uint32_t cutoff_k = uint32_t(Math::ceil(cutoff * 255.0f));
	if (cutoff_k == 0) {
		alpha_counts.solid_always++;
		return;
	}
	key.scale[0] = float(scale.x);
	key.scale[1] = float(scale.y);
	key.offset[0] = float(offset.x);
	key.offset[1] = float(offset.y);
	key.bits = p_cull & RayAlphaStore::ENTRY_CULL_MASK;
	const Variant far_mode_value = material_storage->material_get_declared_param(p_material, SNAME("alpha_far_mode"));
	const int64_t far_mode = far_mode_value.get_type() == Variant::INT ? int64_t(far_mode_value) : 0;
	if (far_mode == 1 || far_mode == 2) {
		key.bits |= uint32_t(far_mode) << RayAlphaStore::ENTRY_FAR_MODE_SHIFT;
		key.far_gain = _param_float(material_storage->material_get_declared_param(p_material, SNAME("alpha_far_gain")), 0.0f);
		key.far_level = _param_float(material_storage->material_get_declared_param(p_material, SNAME("alpha_far_level")), 0.0f);
		key.far_veil_alpha = _param_float(material_storage->material_get_declared_param(p_material, SNAME("alpha_far_veil_alpha")), 0.0f);
	}
	const uint32_t index = alpha_store.acquire(RID(texture_value), cutoff_k, key);
	if (index == 0) {
		alpha_counts.solid_refused++;
		return;
	}
	r_material.flags |= index << MATERIAL_ALPHA_SHIFT;
	alpha_counts.cutout_materials++;
}

bool RayScene::_surface_uv_unsafe(const MeshEntry &p_mesh, uint32_t p_surface, uint32_t p_entry) const {
	const float extent = p_surface < p_mesh.surface_uv_extent.size() ? p_mesh.surface_uv_extent[p_surface] : 0.0f;
	if (extent >= 65504.0f) {
		return true;
	}
	const RayAlphaStore::TableEntry &entry = alpha_store.get_entry(p_entry);
	return alpha_uv_unsafe(extent, MAX(Math::abs(entry.scale[0]), Math::abs(entry.scale[1])), alpha_store.get_layer_size());
}

bool RayScene::alpha_uv_unsafe(float p_extent, float p_scale, uint32_t p_layer_size) {
	if (p_extent >= 65504.0f) {
		return true;
	}
	const float spacing = Math::pow(2.0f, Math::floor(Math::log2(MAX(p_extent, 6.103515625e-5f))) - 10.0f);
	return spacing * p_scale > 1.0f / float(p_layer_size);
}

bool RayScene::_alpha_live(uint32_t p_flags) const {
	const uint32_t index = p_flags >> MATERIAL_ALPHA_SHIFT;
	return index != 0 && index < alpha_store.get_entry_count() && alpha_store.get_entry(index).state == RayAlphaStore::LAYER_READY;
}

bool RayScene::_alpha_ready() const {
	return triangle_uv_buffer.is_valid() && alpha_store.get_table_buffer().is_valid() && alpha_store.get_layer_texture().is_valid() && alpha_store.get_mean_texture().is_valid();
}

String RayScene::alpha_shader_defines() {
	if (!singleton) {
		return String();
	}
	String defines = "\n#define RAY_SCENE_ALPHA\n";
	if (int(GLOBAL_GET("rendering/global_illumination/ray_scene/alpha_mode")) == 1) {
		defines += "\n#define RAY_SCENE_ALPHA_THRESHOLD\n";
	}
	return defines;
}

void RayScene::_alpha_instance(const MeshEntry &p_mesh, uint32_t p_surface, Material &r_material) {
	if (_surface_uv_unsafe(p_mesh, p_surface, r_material.flags >> MATERIAL_ALPHA_SHIFT)) {
		r_material.flags &= ~MATERIAL_ALPHA_MASK;
		alpha_counts.solid_unsafe_uv++;
	} else if (!_alpha_live(r_material.flags)) {
		r_material.flags &= ~MATERIAL_ALPHA_MASK;
		alpha_counts.pending_surfaces++;
	} else {
		alpha_counts.live_surfaces++;
	}
}

void RayScene::_print_alpha() {
	const RayAlphaStore::Stats &stats = alpha_store.get_stats();
	alpha_counts.layers = stats.layers;
	alpha_counts.layers_ready = stats.layers_ready;
	alpha_counts.table_entries = stats.table_entries;
	alpha_counts.layers_released = uint32_t(stats.layers_released_total);
	alpha_counts.layers_evicted = uint32_t(stats.layers_evicted_total);
	alpha_counts.far_invalid = stats.far_invalid;
	alpha_counts.entries_retired = uint32_t(stats.entries_retired_total);
	if (memcmp(&alpha_counts, &reported_alpha_counts, sizeof(AlphaCounts)) == 0) {
		return;
	}
	reported_alpha_counts = alpha_counts;
	print_line(vformat("Ray scene alpha: %d cut-out materials (solid: %d no texture, %d custom0, %d degenerate, %d cutoff 0, %d store full; %d vertex alpha ignored), surfaces %d live, %d pending, %d unsafe UVs, layers %d ready of %d, %d table entries, %d layers released, %d evicted, %d far values invalid, %d entries retired.",
			alpha_counts.cutout_materials, alpha_counts.solid_no_texture, alpha_counts.solid_custom0, alpha_counts.solid_degenerate, alpha_counts.solid_always, alpha_counts.solid_refused, alpha_counts.vertex_alpha_ignored,
			alpha_counts.live_surfaces, alpha_counts.pending_surfaces, alpha_counts.solid_unsafe_uv, alpha_counts.layers_ready, alpha_counts.layers, alpha_counts.table_entries, alpha_counts.layers_released, alpha_counts.layers_evicted, alpha_counts.far_invalid, alpha_counts.entries_retired));
}

void RayScene::_print_tlas_rebuilds(bool p_final) {
	if (tlas_rebuilds == reported_tlas_rebuilds && hw_tlas_builds == reported_hw_tlas_builds) {
		return;
	}
	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	if (!p_final && (reported_tlas_rebuilds != 0 || reported_hw_tlas_builds != 0) && now - tlas_report_msec < 1000) {
		return;
	}
	tlas_report_msec = now;
	reported_tlas_rebuilds = tlas_rebuilds;
	reported_hw_tlas_builds = hw_tlas_builds;
	print_line(vformat("RAYSCENE_TLAS rebuilds %d hw_builds %d%s", tlas_rebuilds, hw_tlas_builds, p_final ? " final" : ""));
}

uint32_t RayScene::shadow_cast_flags(RSE::ShadowCastingSetting p_setting, bool p_occluder) {
	if (p_setting == RSE::SHADOW_CASTING_SETTING_OFF) {
		return p_occluder ? 0 : MATERIAL_FLAG_SHADOW_PASS;
	}
	if (p_setting == RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY) {
		return MATERIAL_FLAG_SHADOW_ONLY;
	}
	if (p_setting == RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED) {
		return MATERIAL_FLAG_SHADOW_TWO_SIDED;
	}
	return 0;
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

void RayScene::pack_shape(RSE::LightType p_type, const Basis &p_basis, const Vector2 &p_area_size, float p_direct_size, Light &r_light) {
	if (p_type == RSE::LIGHT_AREA) {
		pack_area(p_basis, p_area_size, r_light);
	} else if (p_type != RSE::LIGHT_DIRECTIONAL) {
		r_light.area = p_direct_size;
	}
}

void RayScene::_pack_light(RID p_light_instance, bool p_physical_units, float p_exposure, uint32_t p_limit, LocalVector<Light> &r_lights, LocalVector<RID> *r_ids) {
	LightStorage *light_storage = LightStorage::get_singleton();
	if (r_lights.size() >= p_limit || !light_storage->owns_light_instance(p_light_instance)) {
		return;
	}
	const RID light = light_storage->light_instance_get_base_light(p_light_instance);
	const RSE::LightType type = light_storage->light_get_type(light);
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
	packed.pad = p_light_instance.get_local_index();
	pack_shape(type, transform.basis, area_size, light_storage->light_get_direct_size(light), packed);
	if (type != RSE::LIGHT_DIRECTIONAL) {
		census.candidates++;
		census.area_candidates += type == RSE::LIGHT_AREA;
		census.area_projected += type == RSE::LIGHT_AREA && light_storage->light_area_get_texture(light).is_valid();
		census.masked_candidates += packed.caster_mask != 0xFFFFFFFFu;
	}
	if (tracker_observes(type, tracker_region, light_storage->light_instance_get_base_aabb(p_light_instance))) {
		DDGILightTracker::observe(p_light_instance, packed, anchor);
	}
	if (r_ids) {
		r_ids->push_back(p_light_instance);
	}
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

static double light_pick_power(const RayScene::Light &p_light) {
	double power = double(p_light.energy) * (0.2126 * p_light.color[0] + 0.7152 * p_light.color[1] + 0.0722 * p_light.color[2]);
	if (p_light.type == uint32_t(RSE::LIGHT_AREA)) {
		power *= double(p_light.area);
	}
	return Math::is_finite(power) && power > 0.0 ? power : 0.0;
}

static constexpr uint32_t CANDIDATE_HASH_SEED = 0x9E3779B9u;
static constexpr double CANDIDATE_ENERGY_TOLERANCE = 1e-5;

static double power_share(double p_part, double p_rest) {
	return p_part + p_rest > 0.0 ? p_part / (p_part + p_rest) : 0.0;
}

static float light_pick_cell() {
	const int cell = GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/light_pick_cell");
	return (cell == 8 || cell == 32 || cell == 64) ? float(cell) : 16.0f;
}

void RayScene::select_nearest(LocalVector<Light> &r_lights, LocalVector<Light> &r_local_lights, const Vector3 &p_camera, uint32_t p_max_lights, uint32_t p_reserved_area, double p_inv_exposure, LightCensus &r_census) {
	if (r_lights.size() + r_local_lights.size() > p_max_lights) {
		const uint32_t keep = p_max_lights - r_lights.size();
		SortArray<Light, LightNearer> nearest;
		nearest.compare.camera = p_camera;
		LocalVector<Light> area;
		LocalVector<Light> rest;
		for (const Light &light : r_local_lights) {
			if (light.type == uint32_t(RSE::LIGHT_AREA)) {
				area.push_back(light);
			} else {
				rest.push_back(light);
			}
		}
		const uint32_t reserved = MIN(MIN(area.size(), p_reserved_area), keep);
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
			const double power = light_pick_power(rest[i]) * p_inv_exposure;
			if (rest[i].type == uint32_t(RSE::LIGHT_AREA)) {
				r_census.cut_area++;
				r_census.cut_area_power += power;
			} else {
				r_census.cut_nonarea++;
				r_census.cut_nonarea_power += power;
			}
			if (rest[i].caster_mask != 0xFFFFFFFFu) {
				r_census.masked_cut++;
				r_census.masked_cut_energy += light_power(rest[i]);
			}
		}
		rest.resize(MIN(rest.size(), rest_keep));
		r_census.area_reserved = reserved;
		r_local_lights.clear();
		for (uint32_t i = 0; i < reserved; i++) {
			r_local_lights.push_back(area[i]);
		}
		for (const Light &light : rest) {
			r_local_lights.push_back(light);
		}
	}
	for (const Light &light : r_local_lights) {
		r_census.area_packed += light.type == uint32_t(RSE::LIGHT_AREA);
		const double power = light_pick_power(light) * p_inv_exposure;
		if (light.type == uint32_t(RSE::LIGHT_AREA)) {
			r_census.packed_area_power += power;
		} else {
			r_census.packed_nonarea_power += power;
		}
		if (light.caster_mask != 0xFFFFFFFFu) {
			r_census.masked_packed_energy += light_power(light);
		}
	}
	SortArray<Light, LightOrder> ordered;
	ordered.sort(r_local_lights.ptr(), r_local_lights.size());
	for (const Light &light : r_local_lights) {
		r_lights.push_back(light);
	}
}

void RayScene::_select_lights(const Vector3 &p_camera, double p_inv_exposure) {
	select_nearest(lights, local_lights, p_camera, max_lights, reserved_area_lights, p_inv_exposure, census);
}

struct PickBetter {
	bool operator()(const RayScene::PickKey &p_a, const RayScene::PickKey &p_b) const {
		if (p_a.importance != p_b.importance) {
			return p_a.importance > p_b.importance;
		}
		if (p_a.distance != p_b.distance) {
			return p_a.distance < p_b.distance;
		}
		if (p_a.kept != p_b.kept) {
			return p_a.kept > p_b.kept;
		}
		if (p_a.rid != p_b.rid) {
			return p_a.rid < p_b.rid;
		}
		return p_a.index < p_b.index;
	}
};

struct PickOrder {
	const RayScene::Light *lights = nullptr;
	const RID *ids = nullptr;
	static int compare(float p_a, float p_b) {
		const bool a_nan = Math::is_nan(p_a);
		const bool b_nan = Math::is_nan(p_b);
		if (a_nan || b_nan) {
			return a_nan == b_nan ? 0 : (a_nan ? 1 : -1);
		}
		return p_a < p_b ? -1 : (p_b < p_a ? 1 : 0);
	}
	bool operator()(uint32_t p_a, uint32_t p_b) const {
		const RayScene::Light &a = lights[p_a];
		const RayScene::Light &b = lights[p_b];
		for (int k = 0; k < 3; k++) {
			const int order = compare(a.position[k], b.position[k]);
			if (order != 0) {
				return order < 0;
			}
		}
		const int radius_order = compare(a.inv_radius, b.inv_radius);
		if (radius_order != 0) {
			return radius_order < 0;
		}
		if (ids[p_a].get_id() != ids[p_b].get_id()) {
			return ids[p_a].get_id() < ids[p_b].get_id();
		}
		return p_a < p_b;
	}
};

static double pick_importance(const RayScene::Light &p_light, const Vector3 &p_cell_origin, double p_cell) {
	const double power = light_pick_power(p_light);
	if (power <= 0.0) {
		return 0.0;
	}
	for (int k = 0; k < 3; k++) {
		if (!Math::is_finite(p_light.position[k])) {
			return 0.0;
		}
	}
	double squared = 0.0;
	for (int k = 0; k < 3; k++) {
		const double low = p_cell_origin[k];
		const double position = p_light.position[k];
		const double outside = MAX(MAX(low - position, position - (low + p_cell)), 0.0);
		squared += outside * outside;
	}
	double distance = Math::sqrt(squared);
	if (p_light.type == uint32_t(RSE::LIGHT_AREA)) {
		const double half_diagonal = Math::sqrt(Vector3(p_light.half_a[0], p_light.half_a[1], p_light.half_a[2]).length_squared() + Vector3(p_light.half_b[0], p_light.half_b[1], p_light.half_b[2]).length_squared());
		distance = MAX(distance - half_diagonal, 0.0);
	}
	double falloff = distance * double(p_light.inv_radius);
	falloff *= falloff;
	falloff *= falloff;
	falloff = MAX(1.0 - falloff, 0.0);
	falloff *= falloff;
	const double importance = power * falloff * Math::pow(MAX(distance, p_cell * 0.5), -double(p_light.attenuation));
	return Math::is_nan(importance) ? 0.0 : importance;
}

static void select_best(LocalVector<RayScene::PickKey> &r_keys, uint32_t p_count) {
	if (p_count >= r_keys.size()) {
		return;
	}
	SortArray<RayScene::PickKey, PickBetter> sorter;
	sorter.nth_element(0, r_keys.size(), p_count, r_keys.ptr());
	if (p_count == 0) {
		return;
	}
	uint32_t worst = 0;
	for (uint32_t i = 1; i < p_count; i++) {
		if (sorter.compare(r_keys[worst], r_keys[i])) {
			worst = i;
		}
	}
	uint32_t best = p_count;
	for (uint32_t i = p_count + 1; i < r_keys.size(); i++) {
		if (sorter.compare(r_keys[i], r_keys[best])) {
			best = i;
		}
	}
	if (sorter.compare(r_keys[best], r_keys[worst])) {
		sorter.sort(r_keys.ptr(), r_keys.size());
	}
}

static uint32_t pick_set(LocalVector<RayScene::PickKey> &r_area, LocalVector<RayScene::PickKey> &r_rest, uint32_t p_keep, uint32_t p_reserved_area, LocalVector<uint32_t> &r_selected, LocalVector<uint32_t> *r_cut) {
	const uint32_t reserved = MIN(MIN(r_area.size(), p_reserved_area), p_keep);
	select_best(r_area, reserved);
	for (uint32_t i = reserved; i < r_area.size(); i++) {
		r_rest.push_back(r_area[i]);
	}
	const uint32_t rest_keep = p_keep - reserved;
	select_best(r_rest, rest_keep);
	for (uint32_t i = 0; i < reserved; i++) {
		r_selected.push_back(r_area[i].index);
	}
	for (uint32_t i = 0; i < r_rest.size(); i++) {
		if (i < rest_keep) {
			r_selected.push_back(r_rest[i].index);
		} else if (r_cut) {
			r_cut->push_back(r_rest[i].index);
		}
	}
	return reserved;
}

static int64_t chebyshev(const Vector3i &p_a, const Vector3i &p_b) {
	int64_t distance = 0;
	for (int k = 0; k < 3; k++) {
		const int64_t delta = int64_t(p_a[k]) - int64_t(p_b[k]);
		distance = MAX(distance, delta < 0 ? -delta : delta);
	}
	return distance;
}

static int64_t nearest_used_slot(const RayScene::PickState &p_state, const Vector3i &p_key, uint32_t p_skip) {
	int64_t found = -1;
	int64_t found_distance = 0;
	uint64_t found_used = 0;
	for (uint32_t i = 0; i < RayScene::PICK_CELL_SLOTS; i++) {
		const RayScene::PickSlot &slot = p_state.slots[i];
		if (!slot.used || i == p_skip) {
			continue;
		}
		const int64_t distance = chebyshev(slot.key, p_key);
		if (found < 0 || distance < found_distance || (distance == found_distance && slot.last_used > found_used)) {
			found = i;
			found_distance = distance;
			found_used = slot.last_used;
		}
	}
	return found;
}

static int32_t cell_index(double p_value, double p_cell) {
	const double index = Math::floor(p_value / p_cell);
	if (!Math::is_finite(index)) {
		return 0;
	}
	return int32_t(CLAMP(index, double(INT32_MIN), double(INT32_MAX)));
}

static bool region_leaves(int32_t p_region, double p_camera, double p_cell) {
	return p_camera < double(p_region) * p_cell - p_cell * 0.5 || p_camera >= (double(p_region) + 1.0) * p_cell + p_cell * 0.5;
}

static int64_t view_slot(const RayScene::PickState &p_state, const Vector3 &p_camera, double p_cell) {
	int64_t inside_slot = -1;
	int64_t follow_slot = -1;
	for (uint32_t i = 0; i < RayScene::PICK_CELL_SLOTS; i++) {
		const RayScene::PickSlot &slot = p_state.slots[i];
		if (!slot.used) {
			continue;
		}
		bool inside = true;
		bool follows = true;
		for (int k = 0; k < 3; k++) {
			inside = inside && !region_leaves(slot.region_key[k], p_camera[k], p_cell);
			follows = follows && Math::abs(double(p_camera[k]) - double(slot.camera[k])) <= p_cell * 0.5;
		}
		if (inside && (inside_slot < 0 || slot.last_used > p_state.slots[inside_slot].last_used)) {
			inside_slot = i;
		}
		if (follows && (follow_slot < 0 || slot.last_used > p_state.slots[follow_slot].last_used)) {
			follow_slot = i;
		}
	}
	return inside_slot >= 0 ? inside_slot : follow_slot;
}

Vector3i RayScene::pick_cell_key(const Vector3 &p_world, float p_cell) {
	Vector3i key;
	for (int k = 0; k < 3; k++) {
		key[k] = cell_index(double(p_world[k]), double(p_cell));
	}
	return key;
}

uint32_t RayScene::match_slot(PickState &r_state, const Vector3 &p_camera_world, float p_cell, uint64_t p_serial) {
	const Vector3i key = pick_cell_key(p_camera_world, p_cell);
	const int64_t view = view_slot(r_state, p_camera_world, p_cell);
	if (view >= 0) {
		PickSlot &slot = r_state.slots[view];
		slot.key = key;
		slot.camera = p_camera_world;
		slot.last_used = p_serial;
		return uint32_t(view);
	}
	uint32_t target = PICK_CELL_SLOTS;
	for (uint32_t i = 0; i < PICK_CELL_SLOTS; i++) {
		if (!r_state.slots[i].used) {
			target = i;
			break;
		}
	}
	if (target == PICK_CELL_SLOTS) {
		target = 0;
		for (uint32_t i = 1; i < PICK_CELL_SLOTS; i++) {
			if (r_state.slots[i].last_used < r_state.slots[target].last_used) {
				target = i;
			}
		}
	}
	const int64_t seed = nearest_used_slot(r_state, key, target);
	PickSlot &slot = r_state.slots[target];
	if (seed >= 0) {
		slot.kept = r_state.slots[seed].kept;
	} else {
		slot.kept.clear();
	}
	slot.used = true;
	slot.fresh = true;
	slot.key = key;
	slot.region_key = key;
	slot.camera = p_camera_world;
	slot.last_used = p_serial;
	return target;
}

AABB RayScene::light_region(PickSlot &r_slot, const Vector3 &p_camera_world, const Vector3 &p_region_half, float p_cell) {
	const double cell = p_cell;
	Vector3 position;
	Vector3 size;
	for (int k = 0; k < 3; k++) {
		const double camera = p_camera_world[k];
		if (region_leaves(r_slot.region_key[k], camera, cell)) {
			r_slot.region_key[k] = cell_index(camera, cell);
		}
		const double half = double(p_region_half[k]) + cell;
		const double center = (double(r_slot.region_key[k]) + 0.5) * cell;
		position[k] = real_t(center - half);
		size[k] = real_t(half * 2.0);
	}
	return AABB(position, size);
}

void RayScene::count_swaps(LocalVector<uint32_t> &r_last, LocalVector<uint32_t> &r_now, uint32_t &r_in, uint32_t &r_out) {
	r_now.sort();
	r_in = 0;
	r_out = 0;
	uint32_t i = 0;
	uint32_t j = 0;
	while (i < r_last.size() || j < r_now.size()) {
		if (j >= r_now.size() || (i < r_last.size() && r_last[i] < r_now[j])) {
			r_out++;
			i++;
		} else if (i >= r_last.size() || r_now[j] < r_last[i]) {
			r_in++;
			j++;
		} else {
			i++;
			j++;
		}
	}
	r_last = r_now;
}

void RayScene::pick_lights(const LocalVector<Light> &p_candidates, const LocalVector<RID> &p_ids, const PickParams &p_params, PickState &r_state, LocalVector<uint32_t> &r_selected, PickCensus &r_census) {
	r_census = PickCensus();
	r_selected.clear();
	ERR_FAIL_COND(p_ids.size() != p_candidates.size());
	ERR_FAIL_UNSIGNED_INDEX(p_params.slot, PICK_CELL_SLOTS);
	PickSlot &slot = r_state.slots[p_params.slot];
	const double cell = p_params.cell_size;
	const Vector3 center = p_params.cell_origin + Vector3(1, 1, 1) * real_t(p_params.cell_size * 0.5f);
	const double boost = 1.0 + double(p_params.keep_share);

	LocalVector<PickKey> area;
	LocalVector<PickKey> rest;
	LocalVector<PickKey> plain_area;
	LocalVector<PickKey> plain_rest;
	for (uint32_t i = 0; i < p_candidates.size(); i++) {
		const Light &light = p_candidates[i];
		PickKey key;
		key.importance = pick_importance(light, p_params.cell_origin, cell);
		key.distance = LightNearer::score(light, center);
		if (Math::is_nan(key.distance)) {
			key.distance = FLT_MAX;
		}
		key.kept = slot.kept.has(p_ids[i]) ? 1 : 0;
		key.rid = p_ids[i].get_id();
		key.index = i;
		const bool is_area = light.type == uint32_t(RSE::LIGHT_AREA);
		if (p_params.want_held) {
			(is_area ? plain_area : plain_rest).push_back(key);
		}
		if (key.kept) {
			key.importance *= boost;
			key.distance -= p_params.keep_distance;
		}
		(is_area ? area : rest).push_back(key);
	}

	LocalVector<uint32_t> cut;
	if (p_candidates.size() > p_params.keep) {
		r_census.area_reserved = pick_set(area, rest, p_params.keep, p_params.reserved_area, r_selected, &cut);
		if (p_params.want_held) {
			LocalVector<uint32_t> plain;
			pick_set(plain_area, plain_rest, p_params.keep, p_params.reserved_area, plain, nullptr);
			HashSet<uint32_t> plain_set;
			for (const uint32_t index : plain) {
				plain_set.insert(index);
			}
			for (const uint32_t index : r_selected) {
				if (slot.kept.has(p_ids[index]) && !plain_set.has(index)) {
					r_census.held++;
				}
			}
		}
	} else {
		for (uint32_t i = 0; i < p_candidates.size(); i++) {
			r_selected.push_back(i);
		}
	}

	for (const uint32_t index : cut) {
		const Light &light = p_candidates[index];
		const double power = light_pick_power(light) * p_params.inv_exposure;
		if (light.type == uint32_t(RSE::LIGHT_AREA)) {
			r_census.cut_area++;
			r_census.cut_area_power += power;
		} else {
			r_census.cut_nonarea++;
			r_census.cut_nonarea_power += power;
		}
		if (light.caster_mask != 0xFFFFFFFFu) {
			r_census.masked_cut++;
			r_census.masked_cut_energy += light_power(light);
		}
	}

	SortArray<uint32_t, PickOrder> order;
	order.compare.lights = p_candidates.ptr();
	order.compare.ids = p_ids.ptr();
	order.sort(r_selected.ptr(), r_selected.size());

	HashSet<RID> now;
	LocalVector<uint32_t> pads;
	pads.reserve(r_selected.size());
	for (const uint32_t index : r_selected) {
		const Light &light = p_candidates[index];
		r_census.area_packed += light.type == uint32_t(RSE::LIGHT_AREA);
		const double power = light_pick_power(light) * p_params.inv_exposure;
		if (light.type == uint32_t(RSE::LIGHT_AREA)) {
			r_census.packed_area_power += power;
		} else {
			r_census.packed_nonarea_power += power;
		}
		if (light.caster_mask != 0xFFFFFFFFu) {
			r_census.masked_packed_energy += light_power(light);
		}
		if (!slot.kept.has(p_ids[index])) {
			r_census.slot_in++;
		}
		now.insert(p_ids[index]);
		pads.push_back(light.pad);
	}
	for (const RID &id : slot.kept) {
		if (!now.has(id)) {
			r_census.slot_out++;
		}
	}
	slot.kept = now;
	r_census.slot_new = slot.fresh;
	slot.fresh = false;
	count_swaps(r_state.last_packed, pads, r_census.packed_in, r_census.packed_out);
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
		const double cut_power = census.cut_area_power + census.cut_nonarea_power;
		const double packed_power = census.packed_area_power + census.packed_nonarea_power;
		const String region = census.has_region ? vformat("%d,%d,%d", census.region_cell.x, census.region_cell.y, census.region_cell.z) : String("-");
		const String counts = vformat("RAYSCENE_CENSUS candidates %d area_candidates %d area_packed %d area_reserved %d area_projected %d masked_candidates %d masked_cut %d masked_packed_power %f masked_cut_power %f masked_instances %d",
				census.candidates, census.area_candidates, census.area_packed, census.area_reserved, census.area_projected, census.masked_candidates, census.masked_cut, census.masked_packed_energy, census.masked_cut_energy, census.masked_instances);
		const String split = vformat(" cut %d cut_area %d cut_nonarea %d cut_power %f cut_area_power %f cut_nonarea_power %f packed_power %f packed_area_power %f packed_nonarea_power %f",
				census.cut_area + census.cut_nonarea, census.cut_area, census.cut_nonarea, cut_power, census.cut_area_power, census.cut_nonarea_power, packed_power, census.packed_area_power, census.packed_nonarea_power);
		const String pick = vformat(" cut_share %f cut_share_area %f cut_share_nonarea %f held %d pick_cell %d,%d,%d region_cell %s pick_mode %s max_lights %d",
				power_share(cut_power, packed_power), power_share(census.cut_area_power, census.packed_area_power), power_share(census.cut_nonarea_power, census.packed_nonarea_power), census.held, census.pick_cell.x, census.pick_cell.y, census.pick_cell.z, region, census.pick_mode == LIGHT_PICK_STABLE ? "stable" : "camera", census.max_lights);
		print_line(counts + split + pick);
	}
}

void RayScene::_candidate_changes(double p_inv_exposure, uint32_t &r_in, uint32_t &r_out, uint32_t &r_changed) {
	LocalVector<CandidateMark> marks;
	marks.reserve(local_lights.size());
	for (const Light &light : local_lights) {
		Light hashed = light;
		hashed.energy = 0.0f;
		CandidateMark mark;
		mark.index = light.pad;
		mark.hash = (uint64_t(hash_murmur3_buffer(&hashed, sizeof(Light), HASH_MURMUR3_SEED)) << 32) | uint64_t(hash_murmur3_buffer(&hashed, sizeof(Light), CANDIDATE_HASH_SEED));
		mark.energy = double(light.energy) * p_inv_exposure;
		marks.push_back(mark);
	}
	marks.sort();
	r_in = 0;
	r_out = 0;
	r_changed = 0;
	uint32_t i = 0;
	uint32_t j = 0;
	while (i < last_candidates.size() || j < marks.size()) {
		if (j >= marks.size() || (i < last_candidates.size() && last_candidates[i].index < marks[j].index)) {
			r_out++;
			i++;
		} else if (i >= last_candidates.size() || marks[j].index < last_candidates[i].index) {
			r_in++;
			j++;
		} else {
			const CandidateMark &before = last_candidates[i];
			const CandidateMark &now = marks[j];
			if (before.hash != now.hash || Math::abs(now.energy - before.energy) > CANDIDATE_ENERGY_TOLERANCE * MAX(Math::abs(now.energy), Math::abs(before.energy))) {
				r_changed++;
			}
			i++;
			j++;
		}
	}
	last_candidates = marks;
}

void RayScene::_pick_settings(float p_cell, bool p_stable) {
	if (p_cell == pick_cell_size && p_stable == pick_stable) {
		return;
	}
	pick_cell_size = p_cell;
	pick_stable = p_stable;
	for (PickSlot &slot : pick_state.slots) {
		slot = PickSlot();
	}
	region_recorded = false;
}

AABB RayScene::get_light_region(const Vector3 &p_camera_world, const Vector3 &p_region_half) {
	MutexLock lock(mesh_mutex);
	const float cell = light_pick_cell();
	_pick_settings(cell, true);
	const uint32_t slot = match_slot(pick_state, p_camera_world, cell, update_serial + 1);
	region_camera = p_camera_world;
	region_half = p_region_half;
	region_slot = slot;
	region_recorded = true;
	return light_region(pick_state.slots[slot], p_camera_world, p_region_half, cell);
}

bool RayScene::shell_material_range(RID p_instance, uint32_t &r_offset, uint32_t &r_count) const {
	const ShellRange *range = shell_ranges.getptr(p_instance);
	if (!range || range->count == 0) {
		return false;
	}
	r_offset = range->offset;
	r_count = range->count;
	return true;
}

void RayScene::update(const PagedArray<RenderGeometryInstance *> &p_instances, const PagedArray<RID> &p_lights, const Vector<RID> &p_directional_lights, RID p_camera_attributes, const Vector3 &p_camera_position) {
	MutexLock lock(mesh_mutex);
	feed_log_mode = CLAMP(int(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/debug_dirty_feed")), 0, 2);
	if (feed_log_mode != 0) {
		acquire_dirty_feed(FEED_DEBUG);
	} else if (feed_consumers.get() & FEED_DEBUG) {
		release_dirty_feed(FEED_DEBUG);
	}
	const bool feed_on = feed_consumers.get() != 0;
	if (feed_on) {
		feed_update_start = OS::get_singleton()->get_ticks_usec();
	}
	_backend_tick();
	const bool alpha_on = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/ray_scene/alpha_test");
	alpha_edge = alpha_on != alpha_enabled;
	if (alpha_edge) {
		alpha_enabled = alpha_on;
		alpha_generation++;
		if (alpha_generation == 0) {
			alpha_generation = 1;
		}
	}

	const Vector3 snapped = (p_camera_position / ANCHOR_GRID).floor() * ANCHOR_GRID;
	const bool anchor_moved = snapped != anchor;
	anchor = snapped;
	if (anchor_moved || GIEpochs::get_singleton()->get(GIEpochs::SOURCE_ORIGIN) == 0) {
		GIEpochs::get_singleton()->bump(GIEpochs::SOURCE_ORIGIN);
	}

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
	if (accel) {
		_hw_begin();
	}

	MeshStorage *mesh_storage = MeshStorage::get_singleton();
	instances.clear();
	materials.clear();
	material_cache.clear();
	memset(albedo_sources, 0, sizeof(albedo_sources));
	additive_skipped = 0;
	emission_scale = _param_float(MaterialStorage::get_singleton()->global_shader_parameter_get_internal(SNAME("openpbr_emission_scale")), 1.0f);
	const Variant atlas_value = MaterialStorage::get_singleton()->global_shader_parameter_get_internal(SNAME("ray_emission_atlas"));
	const Variant::Type atlas_type = atlas_value.get_type();
	const RID atlas_rid = (atlas_type == Variant::RID || atlas_type == Variant::OBJECT) ? RID(atlas_value) : RID();
	const RID atlas = atlas_rid.is_valid() ? TextureStorage::get_singleton()->texture_get_rd_texture(atlas_rid) : RID();
	if (atlas != emission_texture_array) {
		emission_texture_array = atlas;
		emission_texture_count = 0;
		if (atlas.is_valid()) {
			emission_texture_count = RD::get_singleton()->texture_get_format(atlas).array_layers;
		}
		print_line(vformat("Ray scene emission atlas: %d layers", emission_texture_count));
	}
	skip_additive = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/ray_scene/skip_additive_surfaces");
	shell_ranges.clear();
	HashMap<RenderGeometryInstance *, RID> shell_geometry;
	const HashMap<RID, uint32_t> &shells = LightStorage::get_singleton()->get_direct_shells();
	if (!shells.is_empty()) {
		for (const KeyValue<RID, uint32_t> &shell : shells) {
			RenderGeometryInstance *geometry = RSG::scene->instance_get_geometry_instance(shell.key);
			if (geometry) {
				shell_geometry.insert(geometry, shell.key);
			}
		}
	}
	if (feed_on || feed_was_on) {
		_feed_begin();
	}
	alpha_counts = AlphaCounts();
	alpha_store.configure(uint32_t(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/max_alpha_layers")), uint32_t(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/alpha_layer_size")), uint64_t(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/alpha_fill_texels_per_frame")));
	alpha_store.begin_update(RSG::rasterizer->get_frame_number());
	LocalVector<BuildPrimitive> primitives;
	uint32_t signature = HASH_MURMUR3_SEED;
	uint64_t scene_signature = 0;
	uint32_t emitter_state = HASH_MURMUR3_SEED;
	uint32_t emission_state = HASH_MURMUR3_SEED;
	LocalVector<TriangleEmitterSource> emitter_sources;
	scene_members.clear();
	for (uint32_t i = 0; i < p_instances.size(); i++) {
		RenderGeometryInstanceBase *geometry = static_cast<RenderGeometryInstanceBase *>(p_instances[i]);
		if (!geometry->data || geometry->data->base_type != RSE::INSTANCE_MESH || geometry->data->skeleton.is_valid()) {
			continue;
		}
		const RID mesh_rid = geometry->data->base;
		MeshEntry *mesh = meshes.getptr(mesh_rid);
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
		if (!shell_geometry.is_empty()) {
			const RID *shell = shell_geometry.getptr(p_instances[i]);
			if (shell) {
				ShellRange range;
				range.offset = instance.material_offset;
				range.count = mesh->surface_count;
				shell_ranges.insert(*shell, range);
			}
		}
		const uint32_t cast_flags = shadow_cast_flags(geometry->data->cast_shadows, geometry->data->ray_scene_occluder);
		uint64_t material_candidates = 0;
		uint64_t candidates = 0;
		uint64_t alpha_split = 0;
		uint32_t feed_state = hash_murmur3_one_32(geometry->layer_mask);

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
				float alpha_threshold = -1.0f;
				uint32_t cull = 0;
				_material_entry(material, mesh_rid, s, entry.entry, alpha_threshold, cull);
				if (alpha_threshold >= 0.0f && (entry.entry.flags & MATERIAL_FLAG_SKIP) == 0) {
					_alpha_entry(material, entry.entry, alpha_threshold, cull);
				}
				entry.alpha_flags = entry.entry.flags & MATERIAL_ALPHA_MASK;
				entry.entry.flags = alpha_slot_flags(entry.entry.flags, alpha_enabled);
				_instance_slots(material, entry);
				cached = &material_cache.insert(material, entry)->value;
			}
			materials.push_back(cached->entry);
			if ((cached->entry.flags & MATERIAL_ALPHA_MASK) != 0) {
				_alpha_instance(*mesh, s, materials[materials.size() - 1]);
			}
			materials[materials.size() - 1].flags |= slot_cast_flags(materials[materials.size() - 1].flags, cast_flags);
			feed_state = feed_surface_state(feed_state, materials[materials.size() - 1].flags, materials[materials.size() - 1].coverage);
			if (accel) {
				material_candidates |= surface_candidates(s, cached->entry.flags);
				candidates |= surface_candidates(s, materials[materials.size() - 1].flags);
				alpha_split |= alpha_split_bits(s, cached->entry.flags, cached->alpha_flags);
			}
			const float factor = MAX(_instance_float(geometry->shader_uniforms_offset, cached->scale_index), 0.0f) * MAX(_instance_float(geometry->shader_uniforms_offset, cached->bounce_index), 0.0f);
			if (factor != 1.0f) {
				Material &instance_material = materials[materials.size() - 1];
				for (int k = 0; k < 3; k++) {
					instance_material.emission[k] *= factor;
				}
				instance_material.emission_mean_luminance *= factor;
			}
		}
		bool instance_emissive = false;
		for (uint32_t s = instance.material_offset; s < materials.size(); s++) {
			const Material &surface_material = materials[s];
			const bool emissive = surface_material.emission[0] > 0.0f || surface_material.emission[1] > 0.0f || surface_material.emission[2] > 0.0f || surface_material.emission_texture != 0;
			instance_emissive = instance_emissive || emissive;
			emitter_state = hash_murmur3_one_32(emissive ? 1u : 0u, emitter_state);
			emission_state = hash_murmur3_buffer(surface_material.emission, sizeof(surface_material.emission), emission_state);
			emission_state = hash_murmur3_one_32(surface_material.emission_texture, emission_state);
			emission_state = hash_murmur3_one_32(surface_material.emission_flags, emission_state);
		}
		if (instance_emissive) {
			TriangleEmitterSource emitter_source;
			emitter_source.transform = transform;
			emitter_source.mesh = mesh_rid;
			emitter_source.instance = instances.size();
			emitter_source.material_offset = instance.material_offset;
			emitter_source.surface_count = mesh->surface_count;
			emitter_sources.push_back(emitter_source);
		}
		bool hw_instance_usable = true;
		if (accel) {
			if (hw_split_stale(mesh->hw_generation, mesh->hw_alpha_on, alpha_enabled, alpha_split)) {
				mesh->hw_generation = 0;
				mesh->hw_region = 0;
			}
			const bool hw_fresh = mesh->hw_generation == 0;
			_hw_instance(mesh_rid, *mesh, material_candidates, candidates, transform, instance);
			if (hw_fresh) {
				mesh->hw_alpha_on = alpha_enabled;
			}
			hw_instance_usable = hw_usable[hw_usable.size() - 1] != 0;
		}

		const AABB bounds = transform.xform(mesh->aabb);
		BuildPrimitive primitive;
		for (int k = 0; k < 3; k++) {
			primitive.bmin[k] = bounds.position[k];
			primitive.bmax[k] = bounds.position[k] + bounds.size[k];
			primitive.centroid[k] = bounds.position[k] + bounds.size[k] * 0.5f;
		}
		primitive.index = instances.size();
		if (feed_on) {
			_feed_observe(geometry, geometry->transform, mesh_rid, *mesh, hw_instance_usable, feed_state);
		}
		primitives.push_back(primitive);
		instances.push_back(instance);
		signature = hash_murmur3_buffer(&instance, sizeof(Instance), signature);

		uint32_t scene_hash = hash_murmur3_one_64(mesh_rid.get_id());
		scene_hash = hash_murmur3_buffer(&geometry->transform, sizeof(Transform3D), scene_hash);
		scene_hash = hash_murmur3_one_32(geometry->layer_mask, scene_hash);
		scene_hash = hash_murmur3_one_32(mesh->build_serial, scene_hash);
		scene_hash = hash_murmur3_buffer(materials.ptr() + instance.material_offset, sizeof(Material) * mesh->surface_count, scene_hash);
		if (alpha_enabled) {
			for (uint32_t s = 0; s < mesh->surface_count; s++) {
				const uint32_t index = materials[instance.material_offset + s].flags >> MATERIAL_ALPHA_SHIFT;
				if (index != 0) {
					const RayAlphaStore::TableEntry &entry = alpha_store.get_entry(index);
					if (RayAlphaStore::uses_far(entry.bits)) {
						scene_hash = hash_murmur3_one_32(entry.far_state, scene_hash);
						scene_hash = hash_murmur3_one_float(entry.density, scene_hash);
					}
				}
			}
		}
		scene_signature = GISceneTracker::add_instance(scene_signature, scene_hash);
		scene_members.push_back(GISceneTracker::member_key(p_instances[i], mesh_rid.get_id()));
	}
	scene_signature = alpha_scene_signature(scene_signature, alpha_enabled);
	GIEpochs::get_singleton()->apply(scene_tracker.update(scene_signature, scene_members));
	alpha_store.update();
	if (alpha_enabled) {
		_print_alpha();
	}

	const bool rebuilt = signature != instance_signature || anchor_moved || need_assembly || tlas_node_buffer.is_null();
	if (rebuilt) {
		instance_signature = signature;
		tlas_rebuilds++;
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

	const bool emitters_dirty = !triangle_emitters_built || rebuilt || emitter_state != triangle_emitter_state;
	if (emitters_dirty) {
		LocalVector<uint32_t> instance_order;
		instance_order.resize(instances.size());
		for (uint32_t i = 0; i < primitives.size(); i++) {
			instance_order[primitives[i].index] = i;
		}
		_update_triangle_emitters(emitter_sources, instance_order);
		triangle_emitter_state = emitter_state;
		triangle_emitters_built = true;
	}
	if (emitters_dirty || emission_state != triangle_emitter_emission_state) {
		triangle_emitter_emission_state = emission_state;
		triangle_emitter_generation++;
	}

	const int64_t additive_state = skip_additive ? int64_t(additive_skipped) : -1;
	if (additive_state != reported_additive_state) {
		reported_additive_state = additive_state;
		print_line(vformat("Ray scene additive skip: %s, %d materials skipped.", skip_additive ? "on" : "off", additive_skipped));
	}

	if (materials.is_empty()) {
		materials.push_back(Material());
	}
	_ensure_buffer(material_buffer, material_capacity, materials.size(), sizeof(Material), "Ray Scene Materials");
	RD::get_singleton()->buffer_update(material_buffer, 0, materials.size() * sizeof(Material), materials.ptr());

	const bool physical_units = RendererSceneRenderRD::get_singleton()->is_using_physical_light_units();
	const float exposure = p_camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_camera_attributes) : 1.0f;
	const double inv_exposure = exposure > 0.0f ? 1.0 / double(exposure) : 0.0;
	max_lights = uint32_t(CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/max_lights"), int(MIN_LIGHTS), int(MAX_CANDIDATE_LIGHTS)));
	reserved_area_lights = uint32_t(CLAMP(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/reserved_area_lights"), 0, int(max_lights)));
	const bool stable = GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/light_pick_mode") == LIGHT_PICK_STABLE;
	const float cell = light_pick_cell();
	_pick_settings(cell, stable);
	const bool verbose = OS::get_singleton()->is_stdout_verbose();
	const bool reset_on = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/ddgi/light_change_reset");
	update_serial++;
	lights.clear();
	local_lights.clear();
	local_ids.clear();
	census = LightCensus();
	DDGILightTracker::begin(reset_on, RSG::rasterizer->get_frame_number(), MAX(GLOBAL_GET_CACHED(float, "rendering/global_illumination/ddgi/probe_spacing"), 0.1f), GLOBAL_GET_CACHED(bool, "rendering/global_illumination/rtgi/enabled"));
	const uint32_t fired_before = DDGILightTracker::stats().fired;
	tracker_region = tracker_region_for(stable, p_camera_position, region_half);
	for (const RID &light_instance : p_directional_lights) {
		_pack_light(light_instance, physical_units, exposure, max_lights, lights);
	}
	for (uint32_t i = 0; i < p_lights.size(); i++) {
		_pack_light(p_lights[i], physical_units, exposure, stable ? UINT32_MAX : MAX_CANDIDATE_LIGHTS, local_lights, stable ? &local_ids : nullptr);
	}
	uint32_t cand_in = 0;
	uint32_t cand_out = 0;
	uint32_t cand_changed = 0;
	if (verbose) {
		_candidate_changes(inv_exposure, cand_in, cand_out, cand_changed);
	}
	const uint32_t cand = local_lights.size();
	const Vector3i cell_key = pick_cell_key(p_camera_position, cell);
	PickCensus pick;
	int64_t slot_index = -1;
	if (stable) {
		const uint32_t slot = region_recorded && region_camera == p_camera_position ? region_slot : match_slot(pick_state, p_camera_position, cell, update_serial);
		region_recorded = false;
		PickParams params;
		for (int k = 0; k < 3; k++) {
			params.cell_origin[k] = real_t(double(cell_key[k]) * double(cell) - double(anchor[k]));
		}
		params.cell_size = cell;
		params.slot = slot;
		params.keep = max_lights - MIN(lights.size(), max_lights);
		params.reserved_area = reserved_area_lights;
		params.keep_share = MAX(GLOBAL_GET_CACHED(float, "rendering/global_illumination/ray_scene/light_keep_share"), 0.0f);
		const float keep_distance = GLOBAL_GET_CACHED(float, "rendering/global_illumination/ray_scene/light_keep_distance");
		params.keep_distance = keep_distance < 0.0f ? cell : keep_distance;
		params.inv_exposure = inv_exposure;
		params.want_held = verbose;
		LocalVector<uint32_t> selected;
		pick_lights(local_lights, local_ids, params, pick_state, selected, pick);
		LocalVector<Light> picked;
		picked.reserve(selected.size());
		for (const uint32_t index : selected) {
			picked.push_back(local_lights[index]);
		}
		local_lights = picked;
		for (const Light &light : local_lights) {
			lights.push_back(light);
		}
		census.area_reserved = pick.area_reserved;
		census.area_packed = pick.area_packed;
		census.masked_cut = pick.masked_cut;
		census.masked_cut_energy = pick.masked_cut_energy;
		census.masked_packed_energy = pick.masked_packed_energy;
		census.cut_area = pick.cut_area;
		census.cut_nonarea = pick.cut_nonarea;
		census.cut_area_power = pick.cut_area_power;
		census.cut_nonarea_power = pick.cut_nonarea_power;
		census.packed_area_power = pick.packed_area_power;
		census.packed_nonarea_power = pick.packed_nonarea_power;
		census.held = pick.held;
		census.region_cell = pick_state.slots[slot].region_key;
		census.has_region = true;
		slot_index = slot;
	} else {
		region_recorded = false;
		_select_lights(p_camera_position - anchor, inv_exposure);
		if (verbose) {
			LocalVector<uint32_t> pads;
			pads.reserve(local_lights.size());
			for (const Light &light : local_lights) {
				pads.push_back(light.pad);
			}
			count_swaps(pick_state.last_packed, pads, pick.packed_in, pick.packed_out);
		}
	}
	census.pick_cell = cell_key;
	census.pick_mode = stable ? LIGHT_PICK_STABLE : LIGHT_PICK_CAMERA;
	census.max_lights = max_lights;
	DDGILightTracker::end();
	if (verbose) {
		_print_census();
		const uint32_t fired = DDGILightTracker::stats().fired - fired_before;
		const uint32_t pending = DDGILightTracker::pending_count();
		const bool region_moved = census.has_region && census.region_cell != last_pick_region;
		const bool moved = !pick_logged || cell_key != last_pick_cell || region_moved || slot_index != last_pick_slot;
		if (moved || pick.slot_new || cand_in || cand_out || cand_changed || pick.packed_in || pick.packed_out || pick.slot_in || pick.slot_out || fired) {
			const String region = census.has_region ? vformat("%d,%d,%d", census.region_cell.x, census.region_cell.y, census.region_cell.z) : String("-");
			const String slot = slot_index >= 0 ? itos(slot_index) : String("-");
			const String fired_text = reset_on ? itos(fired) : String("-");
			const String pending_text = reset_on ? itos(pending) : String("-");
			print_line(vformat("RAYSCENE_PICK serial %d frame %d mode %s cell %d,%d,%d region %s slot %s slot_new %d", update_serial, RSG::rasterizer->get_frame_number(), stable ? "stable" : "camera", cell_key.x, cell_key.y, cell_key.z, region, slot, pick.slot_new ? 1 : 0) +
					vformat(" cand %d cand_in %d cand_out %d cand_changed %d in %d out %d slot_in %d slot_out %d fired %s pending %s", cand, cand_in, cand_out, cand_changed, pick.packed_in, pick.packed_out, pick.slot_in, pick.slot_out, fired_text, pending_text));
		}
		pick_logged = true;
		last_pick_cell = cell_key;
		last_pick_region = census.region_cell;
		last_pick_slot = slot_index;
	}
	_ensure_buffer(light_buffer, light_capacity, max_lights, sizeof(Light), "Ray Scene Lights");
	if (lights.size()) {
		RD::get_singleton()->buffer_update(light_buffer, 0, lights.size() * sizeof(Light), lights.ptr());
	}
	if (accel) {
		_hw_update(primitives, rebuilt);
	}
	_print_tlas_rebuilds(false);

	SceneData scene_data = {};
	const Vector3 camera_to_anchor = p_camera_position - anchor;
	scene_data.camera_to_anchor[0] = camera_to_anchor.x;
	scene_data.camera_to_anchor[1] = camera_to_anchor.y;
	scene_data.camera_to_anchor[2] = camera_to_anchor.z;
	scene_data.instance_count = instances.size();
	scene_data.light_count = lights.size();
	scene_data.triangle_emitter_count = triangle_emitter_count;
	scene_data.triangle_emitters_capped = triangle_emitters_capped ? 1u : 0u;
	const float emissive_exposure_normalization = p_camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(p_camera_attributes) : 1.0f;
	scene_data.emissive_exposure_normalization = emissive_exposure_normalization > 0.0f ? emissive_exposure_normalization : 1.0f;
	scene_data.triangle_emitter_generation = triangle_emitter_generation;
	scene_data.emission_texture_count = emission_texture_count;
	if (scene_data_buffer.is_null()) {
		scene_data_buffer = RD::get_singleton()->uniform_buffer_create(sizeof(SceneData));
		RD::get_singleton()->set_resource_name(scene_data_buffer, "Ray Scene Data");
	}
	RD::get_singleton()->buffer_update(scene_data_buffer, 0, sizeof(SceneData), &scene_data);

	if (feed_on) {
		_feed_end();
	}
	GIEpochs::get_singleton()->sync_ray_scene(feed_was_on ? feed_serial : 0);
	if (feed_was_on) {
		GIEpochs::get_singleton()->publish_dirty_frame(feed.boxes, feed.overflow, real_t(GLOBAL_GET_CACHED(float, "rendering/global_illumination/ray_scene/dirty_merge_distance")));
		_feed_log_line();
	}
}
