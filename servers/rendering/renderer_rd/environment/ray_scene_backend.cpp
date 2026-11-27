/**************************************************************************/
/*  ray_scene_backend.cpp                                                 */
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

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/environment/ray_accel.h"
#include "servers/rendering/renderer_rd/environment/ray_scene.h"
#include "servers/rendering/renderer_rd/storage_rd/material_storage.h"

using namespace RendererRD;

void RayScene::collect_moved_instances(uint64_t &r_signature, Vector3 &r_anchor, HashMap<uint64_t, AABB> &r_known, LocalVector<AABB> &r_moved) const {
	r_moved.clear();
	if (r_signature == instance_signature) {
		return;
	}
	const bool report = !r_known.is_empty() && r_anchor == anchor;
	MutexLock lock(mesh_mutex);
	HashMap<uint32_t, AABB> mesh_bounds;
	for (const RID &rid : assembled_meshes) {
		const MeshEntry *mesh = meshes.getptr(rid);
		if (mesh) {
			mesh_bounds.insert(uses_hardware() ? mesh->triangle_offset : mesh->node_offset, mesh->aabb);
		}
	}
	HashMap<uint64_t, AABB> current;
	current.reserve(instances.size());
	for (const Instance &instance : instances) {
		const AABB *mesh_box = mesh_bounds.getptr(instance.blas_root);
		const AABB box = mesh_box ? *mesh_box : AABB();
		const uint32_t seed = hash_murmur3_buffer(&box, sizeof(AABB));
		const uint64_t key = (uint64_t(hash_murmur3_buffer(instance.world_to_object, sizeof(instance.world_to_object), seed)) << 32) | hash_murmur3_buffer(instance.world_to_object, sizeof(instance.world_to_object), ~seed);
		if (current.has(key)) {
			continue;
		}
		const AABB *known = r_known.getptr(key);
		if (known) {
			current.insert(key, *known);
			continue;
		}
		const float *m = instance.world_to_object;
		const Transform3D world_to_object(m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[3], m[7], m[11]);
		AABB bounds = world_to_object.affine_inverse().xform(box);
		bounds.position += anchor;
		current.insert(key, bounds);
		if (report) {
			r_moved.push_back(bounds);
		}
	}
	if (report) {
		for (const KeyValue<uint64_t, AABB> &entry : r_known) {
			if (!current.has(entry.key)) {
				r_moved.push_back(entry.value);
			}
		}
	}
	r_known = current;
	r_anchor = anchor;
	r_signature = instance_signature;
}

uint64_t RayScene::surface_candidates(uint32_t p_surface, uint32_t p_flags) {
	if ((p_flags & CANDIDATE_FLAGS) == 0) {
		return 0;
	}
	return p_surface < 64 ? uint64_t(1) << p_surface : ~uint64_t(0);
}

uint32_t RayScene::instance_class(uint64_t p_candidates, uint64_t p_split) {
	if (p_candidates == 0) {
		return INSTANCE_CLASS_OPAQUE;
	}
	if (p_split != 0 && p_split != ~uint64_t(0) && (p_candidates & ~p_split) == 0) {
		return INSTANCE_CLASS_SPLIT;
	}
	return INSTANCE_CLASS_CANDIDATE;
}

uint32_t RayScene::hw_instance_mask(uint32_t p_layer_mask, const uint32_t *p_groups, uint32_t p_group_count) {
	uint32_t mask = HW_ALL_GROUPS;
	for (uint32_t g = 0; g < MIN(p_group_count, HW_GROUP_COUNT); g++) {
		if ((p_layer_mask & p_groups[g]) != 0) {
			mask |= 1u << g;
		}
	}
	return mask;
}

void RayScene::_backend_tick() {
	const int32_t requested = CLAMP(int32_t(GLOBAL_GET_CACHED(int, "rendering/global_illumination/ray_scene/backend")), int32_t(BACKEND_SOFTWARE), int32_t(BACKEND_AUTO));
	if (requested == backend_requested) {
		return;
	}
	backend_requested = requested;
	RD *rd = RD::get_singleton();
	const bool supported = rd->get_device_api_name() == "Vulkan" && rd->has_feature(RD::SUPPORTS_RAY_QUERY);
	if (requested == BACKEND_HARDWARE && !supported && !backend_warned) {
		backend_warned = true;
		WARN_PRINT("Ray scene: the hardware backend needs Vulkan ray queries, so the software backend is used.");
	}
	const bool hardware = requested != BACKEND_SOFTWARE && supported && !hardware_refused;
	if (hardware == (accel != nullptr)) {
		return;
	}
	_hw_free();
	if (hardware) {
		accel = memnew(RayAccel);
		_hw_assembled();
	}
	instance_signature = 0;
	structure_generation++;
	print_line(hardware ? "Ray scene backend: hardware." : "Ray scene backend: software.");
}

void RayScene::_hw_free() {
	if (accel) {
		memdelete(accel);
		accel = nullptr;
	}
	if (hw_remap_buffer.is_valid()) {
		RD::get_singleton()->free_rid(hw_remap_buffer);
		hw_remap_buffer = RID();
	}
	hw_remap_capacity = 0;
	hw_remap.clear();
	hw_order.clear();
	hw_transforms.clear();
	hw_meshes.clear();
	hw_classes.clear();
	hw_usable.clear();
	hw_group_count = 0;
	memset(hw_groups, 0, sizeof(hw_groups));
	for (KeyValue<RID, MeshEntry> &entry : meshes) {
		entry.value.hw_generation = 0;
		entry.value.hw_region = 0;
		entry.value.hw_order.clear();
	}
}

void RayScene::_hw_assembled() {
	if (!accel) {
		return;
	}
	hw_remap.clear();
	hw_remap.resize(HW_HEADER_WORDS);
	memset(hw_remap.ptr(), 0, HW_HEADER_WORDS * sizeof(uint32_t));
	for (uint32_t g = 0; g < hw_group_count; g++) {
		hw_remap[g] = hw_groups[g];
	}
	hw_remap[HW_GROUP_COUNT] = hw_group_count;
	for (KeyValue<RID, MeshEntry> &entry : meshes) {
		entry.value.hw_region = 0;
	}
	hw_remap_dirty = true;
}

void RayScene::refuse_hardware(const String &p_reason) {
	MutexLock lock(mesh_mutex);
	if (hardware_refused) {
		return;
	}
	hardware_refused = true;
	backend_requested = -1;
	WARN_PRINT(vformat("Ray scene: the hardware backend was turned off (%s), so the software backend is used.", p_reason));
}

void RayScene::_hw_mesh_removed(RID p_mesh) {
	if (accel) {
		accel->mesh_removed(p_mesh);
	}
}

bool RayScene::_hw_ready() const {
	return accel && accel->is_ready() && hw_remap_buffer.is_valid();
}

RID RayScene::get_tlas() const {
	return accel ? accel->get_tlas() : RID();
}

void RayScene::append_uniforms(LocalVector<RD::Uniform> &r_uniforms) const {
	const bool hardware = accel != nullptr;
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 0, hardware ? hw_remap_buffer : blas_node_buffer));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 1, triangle_buffer));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 2, instance_buffer));
	if (!hardware) {
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 3, tlas_node_buffer));
	}
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 4, material_buffer));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 5, light_buffer));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_UNIFORM_BUFFER, 6, scene_data_buffer));
	if (hardware) {
		r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_ACCELERATION_STRUCTURE, 7, accel->get_tlas()));
	}
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 8, triangle_emitter_buffer));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 9, triangle_uv_buffer));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 10, Vector<RID>({ MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED), alpha_store.get_mean_texture() })));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, 13, alpha_store.get_table_buffer()));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 14, Vector<RID>({ MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED), alpha_store.get_layer_texture() })));
	r_uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 15, Vector<RID>({ MaterialStorage::get_singleton()->sampler_rd_get_default(RSE::CANVAS_ITEM_TEXTURE_FILTER_LINEAR, RSE::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED), get_emission_textures() })));
}

void RayScene::_hw_begin() {
	accel->begin_frame(RayAccel::Limits());
	hw_transforms.clear();
	hw_meshes.clear();
	hw_classes.clear();
	hw_usable.clear();
	hw_skipped_unbuilt = 0;
	hw_skipped_cap = 0;
}

void RayScene::_hw_instance(RID p_mesh_rid, MeshEntry &r_mesh, uint64_t p_material_candidates, uint64_t p_candidates, const Transform3D &p_transform, Instance &r_instance) {
	const uint32_t total = r_mesh.bvh_triangles.size();
	if (r_mesh.hw_generation == 0) {
		r_mesh.hw_generation = ++hw_mesh_generation;
		uint32_t opaque = 0;
		for (const Triangle &triangle : r_mesh.bvh_triangles) {
			opaque += (surface_candidates(triangle.surface, CANDIDATE_FLAGS) & p_material_candidates) == 0 ? 1u : 0u;
		}
		const bool split = p_material_candidates != 0 && p_material_candidates != ~uint64_t(0) && opaque > 0 && opaque < total;
		r_mesh.hw_split = split ? p_material_candidates : 0;
		r_mesh.hw_opaque = split ? opaque : 0;
		r_mesh.hw_order.clear();
		if (split) {
			r_mesh.hw_order.resize(total);
			uint32_t next_opaque = 0;
			uint32_t next_candidate = opaque;
			for (uint32_t i = 0; i < total; i++) {
				const bool candidate = (surface_candidates(r_mesh.bvh_triangles[i].surface, CANDIDATE_FLAGS) & p_material_candidates) != 0;
				r_mesh.hw_order[candidate ? next_candidate++ : next_opaque++] = i;
			}
		}
	}
	if (r_mesh.hw_split != 0 && r_mesh.hw_region == 0) {
		r_mesh.hw_region = hw_remap.size() + 1;
		hw_remap.push_back(r_mesh.hw_opaque);
		for (const uint32_t leaf : r_mesh.hw_order) {
			hw_remap.push_back(leaf);
		}
		hw_remap_dirty = true;
	}

	const uint32_t instance_class_value = instance_class(p_candidates, r_mesh.hw_split);
	r_instance.blas_root = r_mesh.triangle_offset;
	r_instance.pad = instance_class_value | (r_mesh.hw_region << INSTANCE_REMAP_SHIFT);

	bool usable = false;
	if (hw_meshes.size() >= accel->get_max_instances()) {
		hw_skipped_cap++;
	} else {
		const RayAccel::BlasResult result = accel->ensure_blas(p_mesh_rid, r_mesh.hw_generation, r_mesh.hw_split, r_mesh.bvh_triangles.ptr(), total, r_mesh.hw_order.is_empty() ? nullptr : r_mesh.hw_order.ptr(), r_mesh.hw_opaque);
		usable = result == RayAccel::BLAS_READY;
		hw_skipped_unbuilt += usable ? 0 : 1;
	}
	hw_transforms.push_back(p_transform);
	hw_meshes.push_back(p_mesh_rid);
	hw_classes.push_back(instance_class_value);
	hw_usable.push_back(usable ? 1 : 0);
}

void RayScene::_hw_update(const LocalVector<BuildPrimitive> &p_primitives, bool p_rebuilt) {
	if (p_rebuilt) {
		hw_order.resize(p_primitives.size());
		for (uint32_t j = 0; j < p_primitives.size(); j++) {
			hw_order[j] = p_primitives[j].index;
		}
	}

	uint32_t groups[HW_GROUP_COUNT] = {};
	uint32_t group_count = 0;
	for (const Light &light : lights) {
		if (light.caster_mask == ALL_CASTERS) {
			continue;
		}
		bool known = false;
		for (uint32_t g = 0; g < group_count; g++) {
			known = known || groups[g] == light.caster_mask;
		}
		if (!known && group_count < HW_GROUP_COUNT) {
			groups[group_count++] = light.caster_mask;
		}
	}
	const bool groups_changed = group_count != hw_group_count || memcmp(groups, hw_groups, sizeof(groups)) != 0;
	if (groups_changed) {
		memcpy(hw_groups, groups, sizeof(groups));
		hw_group_count = group_count;
		for (uint32_t g = 0; g < HW_GROUP_COUNT; g++) {
			hw_remap[g] = groups[g];
		}
		hw_remap[HW_GROUP_COUNT] = group_count;
		hw_remap_dirty = true;
	}

	uint32_t blend_instances = 0;
	for (uint32_t i = 0; i < instances.size(); i++) {
		const uint32_t end = i + 1 < instances.size() ? instances[i + 1].material_offset : materials.size();
		for (uint32_t m = instances[i].material_offset; m < end; m++) {
			if (materials[m].flags & MATERIAL_FLAG_BLEND) {
				blend_instances++;
				break;
			}
		}
	}
	if (hw_remap[HW_BLEND_INSTANCES] != blend_instances) {
		hw_remap[HW_BLEND_INSTANCES] = blend_instances;
		hw_remap_dirty = true;
	}

	if (hw_remap_dirty) {
		_ensure_buffer(hw_remap_buffer, hw_remap_capacity, hw_remap.size(), sizeof(uint32_t), "Ray Scene HW Remap");
		RD::get_singleton()->buffer_update(hw_remap_buffer, 0, hw_remap.size() * sizeof(uint32_t), hw_remap.ptr());
		hw_remap_dirty = false;
	}

	if (!p_rebuilt && !groups_changed && !accel->blas_changed_this_frame() && accel->is_ready()) {
		return;
	}
	thread_local LocalVector<RayAccel::InstanceRecord> records;
	records.clear();
	for (uint32_t j = 0; j < hw_order.size(); j++) {
		const uint32_t i = hw_order[j];
		if (i >= hw_usable.size() || !hw_usable[i]) {
			continue;
		}
		RayAccel::InstanceRecord record;
		record.mesh = hw_meshes[i];
		record.transform = hw_transforms[i];
		record.custom_index = j;
		record.mask = hw_instance_mask(instances[i].layer_mask, hw_groups, hw_group_count);
		record.instance_class = hw_classes[i];
		records.push_back(record);
	}
	if (accel->update_tlas(records, p_rebuilt || groups_changed, hw_skipped_cap, hw_skipped_unbuilt)) {
		hw_tlas_builds++;
	}
	if (!accel->is_ready()) {
		refuse_hardware("the TLAS could not be built");
	}
}
