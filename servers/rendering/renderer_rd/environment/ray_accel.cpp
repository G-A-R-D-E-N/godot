/**************************************************************************/
/*  ray_accel.cpp                                                         */
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

#include "ray_accel.h"

#include "core/os/os.h"

using namespace RendererRD;

BitField<RD::AccelerationStructureInstanceFlagBits> RayAccel::instance_flags(uint32_t p_class) {
	uint32_t flags = RD::ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT;
	if (p_class == RayScene::INSTANCE_CLASS_CANDIDATE) {
		flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT;
	} else if (p_class == RayScene::INSTANCE_CLASS_OPAQUE) {
		flags |= RD::ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT;
	}
	return BitField<RD::AccelerationStructureInstanceFlagBits>(flags);
}

RayAccel::~RayAccel() {
	reset();
}

void RayAccel::_release(RID p_rid) {
	RD::get_singleton()->free_rid(p_rid);
	counters.rids_freed++;
}

void RayAccel::_free_blas(Blas &r_blas) {
	if (r_blas.blas.is_valid()) {
		_release(r_blas.blas);
		r_blas.blas = RID();
	}
	if (r_blas.soup.is_valid()) {
		_release(r_blas.soup);
		r_blas.soup = RID();
	}
	counters.soup_bytes -= MIN(counters.soup_bytes, uint64_t(r_blas.soup_bytes));
	r_blas.soup_bytes = 0;
	if (r_blas.geometries > 1) {
		counters.blas_split -= MIN(counters.blas_split, 1u);
	}
	counters.blas_geometries -= MIN(counters.blas_geometries, r_blas.geometries);
	r_blas.geometries = 0;
	counters.blas_freed_total++;
	counters.blas_freed_frame++;
	blas_changed = true;
}

void RayAccel::_free_tlas() {
	if (tlas.is_valid()) {
		_release(tlas);
		tlas = RID();
	}
	tlas_capacity = 0;
	tlas_built = false;
	counters.tlas_capacity = 0;
}

void RayAccel::reset() {
	_free_tlas();
	for (KeyValue<RID, Blas> &entry : blases) {
		if (entry.value.blas.is_valid()) {
			_release(entry.value.blas);
		}
	}
	for (KeyValue<RID, Blas> &entry : blases) {
		if (entry.value.soup.is_valid()) {
			_release(entry.value.soup);
		}
	}
	blases.clear();
	counters.soup_bytes = 0;
	counters.blas_count = 0;
	counters.blas_split = 0;
	counters.blas_geometries = 0;
	counters.instances_in_tlas = 0;
	blas_changed = true;
}

void RayAccel::begin_frame(const Limits &p_limits) {
	limits = p_limits;
	limits.max_instances = MAX(limits.max_instances, 1u);
	limits.blas_per_frame = MAX(limits.blas_per_frame, 1u);
	limits.blas_triangles_per_frame = MAX(limits.blas_triangles_per_frame, 1u);
	counters.frames++;
	counters.blas_built_frame = 0;
	counters.blas_deferred_frame = 0;
	counters.blas_freed_frame = 0;
	counters.triangles_built_frame = 0;
	deferred_seen.clear();
}

RayAccel::BlasResult RayAccel::ensure_blas(RID p_mesh, uint64_t p_generation, uint64_t p_split_key, const RayScene::Triangle *p_triangles, uint32_t p_count, const uint32_t *p_order, uint32_t p_opaque_count) {
	Blas *existing = blases.getptr(p_mesh);
	if (existing && existing->generation == p_generation && existing->split_key == p_split_key) {
		return existing->failed ? BLAS_FAILED : BLAS_READY;
	}
	if (p_count == 0) {
		return BLAS_FAILED;
	}
	if (existing) {
		_free_blas(*existing);
		blases.erase(p_mesh);
		counters.blas_count = blases.size();
		blas_changed = true;
	}
	const bool over_count = counters.blas_built_frame >= limits.blas_per_frame;
	const bool over_triangles = counters.triangles_built_frame > 0 && uint64_t(counters.triangles_built_frame) + p_count > limits.blas_triangles_per_frame;
	if (over_count || over_triangles) {
		if (!deferred_seen.has(p_mesh)) {
			deferred_seen.insert(p_mesh);
			counters.blas_deferred_frame++;
			counters.blas_deferred_total++;
		}
		return BLAS_DEFERRED;
	}

	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	RD *rd = RD::get_singleton();
	Blas blas;
	blas.generation = p_generation;
	blas.split_key = p_split_key;
	blas.triangles = p_count;
	const bool split = p_order != nullptr && p_opaque_count > 0 && p_opaque_count < p_count;

	LocalVector<float> soup;
	soup.resize(size_t(p_count) * 9);
	for (uint32_t i = 0; i < p_count; i++) {
		const RayScene::Triangle &t = p_triangles[split ? p_order[i] : i];
		float *out = &soup[size_t(i) * 9];
		for (int k = 0; k < 3; k++) {
			out[k] = t.v0[k];
			out[3 + k] = t.v0[k] + t.e2[k];
			out[6 + k] = t.v0[k] + t.e1[k];
		}
	}
	const uint32_t bytes = p_count * SOUP_BYTES_PER_TRIANGLE;
	blas.soup = rd->vertex_buffer_create(bytes, Span<uint8_t>(reinterpret_cast<const uint8_t *>(soup.ptr()), bytes), BitField<RD::BufferCreationBits>(RD::BUFFER_CREATION_DEVICE_ADDRESS_BIT | RD::BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT));
	if (blas.soup.is_valid()) {
		counters.rids_created++;
		blas.soup_bytes = bytes;
		counters.soup_bytes += bytes;
		RD::AccelerationStructureGeometry geometries[2];
		uint32_t geometry_count = 1;
		geometries[0].flags = BitField<RD::AccelerationStructureGeometryFlagBits>(RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT);
		geometries[0].vertex_buffer = blas.soup;
		geometries[0].vertex_offset = 0;
		geometries[0].vertex_stride = 3 * sizeof(float);
		geometries[0].vertex_count = (split ? p_opaque_count : p_count) * 3;
		geometries[0].vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		if (split) {
			geometry_count = 2;
			geometries[1].flags = BitField<RD::AccelerationStructureGeometryFlagBits>(0);
			geometries[1].vertex_buffer = blas.soup;
			geometries[1].vertex_offset = p_opaque_count * SOUP_BYTES_PER_TRIANGLE;
			geometries[1].vertex_stride = 3 * sizeof(float);
			geometries[1].vertex_count = (p_count - p_opaque_count) * 3;
			geometries[1].vertex_format = RD::DATA_FORMAT_R32G32B32_SFLOAT;
		}
		blas.geometries = geometry_count;
		blas.blas = rd->blas_create(Span<RD::AccelerationStructureGeometry>(geometries, geometry_count), BitField<RD::AccelerationStructureFlagBits>(RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT));
		if (blas.blas.is_valid()) {
			counters.rids_created++;
			if (rd->blas_build(blas.blas) != OK) {
				_release(blas.blas);
				blas.blas = RID();
			}
		}
	}
	if (blas.blas.is_null()) {
		blas.failed = true;
		blas.geometries = 0;
		counters.blas_failed_total++;
		if (blas.soup.is_valid()) {
			_release(blas.soup);
			blas.soup = RID();
			counters.soup_bytes -= MIN(counters.soup_bytes, uint64_t(blas.soup_bytes));
			blas.soup_bytes = 0;
		}
		blases.insert(p_mesh, blas);
		counters.blas_count = blases.size();
		return BLAS_FAILED;
	}
	rd->set_resource_name(blas.soup, "Ray Accel Soup");
	rd->set_resource_name(blas.blas, "Ray Accel BLAS");

	blases.insert(p_mesh, blas);
	counters.blas_count = blases.size();
	counters.blas_geometries += blas.geometries;
	if (split) {
		counters.blas_split++;
		counters.blas_split_built_total++;
	}
	counters.blas_built_total++;
	counters.blas_built_frame++;
	counters.triangles_built_frame += p_count;
	counters.blas_record_us += OS::get_singleton()->get_ticks_usec() - start;
	blas_changed = true;
	return BLAS_READY;
}

void RayAccel::mesh_removed(RID p_mesh) {
	Blas *blas = blases.getptr(p_mesh);
	if (!blas) {
		return;
	}
	_free_blas(*blas);
	blases.erase(p_mesh);
	counters.blas_count = blases.size();
	blas_changed = true;
}

bool RayAccel::update_tlas(const LocalVector<InstanceRecord> &p_records, bool p_force, uint32_t p_skipped_cap, uint32_t p_skipped_unbuilt) {
	counters.instances_skipped_cap = p_skipped_cap;
	counters.instances_skipped_unbuilt = p_skipped_unbuilt;
	if (p_skipped_cap > 0 && !cap_logged) {
		cap_logged = true;
		WARN_PRINT(vformat("Ray scene: %d instances were left out of the hardware scene, over the cap of %d.", p_skipped_cap, limits.max_instances));
	}

	uint32_t wanted = TLAS_MIN_CAPACITY;
	while (wanted < p_records.size()) {
		wanted <<= 1;
	}
	wanted = MIN(wanted, MAX(limits.max_instances, uint32_t(p_records.size())));

	bool needs_build = p_force || blas_changed || !tlas_built;
	if (tlas.is_null() || wanted > tlas_capacity) {
		_free_tlas();
		RD *rd = RD::get_singleton();
		tlas = rd->tlas_create(wanted, BitField<RD::AccelerationStructureFlagBits>(RD::ACCELERATION_STRUCTURE_RAY_QUERY_ONLY_BIT | RD::ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT));
		if (tlas.is_null()) {
			return false;
		}
		counters.rids_created++;
		rd->set_resource_name(tlas, "Ray Accel TLAS");
		tlas_capacity = wanted;
		counters.tlas_capacity = wanted;
		counters.tlas_creates++;
		needs_build = true;
	}
	if (!needs_build) {
		return false;
	}

	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	thread_local LocalVector<RD::AccelerationStructureInstance> instances;
	instances.clear();
	for (const InstanceRecord &record : p_records) {
		const Blas *blas = blases.getptr(record.mesh);
		if (!blas || blas->failed) {
			continue;
		}
		RD::AccelerationStructureInstance instance;
		instance.transform = record.transform;
		instance.id = record.custom_index;
		instance.mask = record.mask;
		instance.hit_sbt_range = 0;
		instance.flags = instance_flags(record.instance_class);
		instance.blas = blas->blas;
		instances.push_back(instance);
	}
	counters.instances_in_tlas = instances.size();
	const Error err = RD::get_singleton()->tlas_build(tlas, Span<RD::AccelerationStructureInstance>(instances.ptr(), instances.size()));
	if (err != OK) {
		tlas_built = false;
		return false;
	}
	tlas_built = true;
	blas_changed = false;
	counters.tlas_builds++;
	counters.tlas_last_record_us = OS::get_singleton()->get_ticks_usec() - start;
	counters.tlas_record_us += counters.tlas_last_record_us;
	return true;
}

String RayAccel::get_text() const {
	return vformat("RAY_HW blas %d (%.2f MiB soup, built %d, deferred %d this frame) tlas %d/%d instances (left out: %d over cap, %d unbuilt), %d tlas builds",
			counters.blas_count, double(counters.soup_bytes) / (1024.0 * 1024.0), counters.blas_built_frame, counters.blas_deferred_frame,
			counters.instances_in_tlas, counters.tlas_capacity, counters.instances_skipped_cap, counters.instances_skipped_unbuilt, counters.tlas_builds);
}

Dictionary RayAccel::get_dictionary() const {
	Dictionary d;
	d["frames"] = String::num_uint64(counters.frames);
	d["blas_count"] = int64_t(counters.blas_count);
	d["blas_split"] = int64_t(counters.blas_split);
	d["blas_geometries"] = int64_t(counters.blas_geometries);
	d["blas_built_total"] = String::num_uint64(counters.blas_built_total);
	d["blas_split_built_total"] = String::num_uint64(counters.blas_split_built_total);
	d["blas_freed_total"] = String::num_uint64(counters.blas_freed_total);
	d["blas_failed_total"] = String::num_uint64(counters.blas_failed_total);
	d["blas_deferred_total"] = String::num_uint64(counters.blas_deferred_total);
	d["blas_built_frame"] = int64_t(counters.blas_built_frame);
	d["blas_deferred_frame"] = int64_t(counters.blas_deferred_frame);
	d["tlas_builds"] = String::num_uint64(counters.tlas_builds);
	d["tlas_creates"] = String::num_uint64(counters.tlas_creates);
	d["tlas_capacity"] = int64_t(counters.tlas_capacity);
	d["instances_in_tlas"] = int64_t(counters.instances_in_tlas);
	d["instances_skipped_cap"] = int64_t(counters.instances_skipped_cap);
	d["instances_skipped_unbuilt"] = int64_t(counters.instances_skipped_unbuilt);
	d["rids_created"] = String::num_uint64(counters.rids_created);
	d["rids_freed"] = String::num_uint64(counters.rids_freed);
	d["soup_bytes"] = String::num_uint64(counters.soup_bytes);
	d["blas_record_us"] = String::num_uint64(counters.blas_record_us);
	d["tlas_record_us"] = String::num_uint64(counters.tlas_record_us);
	d["tlas_last_record_us"] = String::num_uint64(counters.tlas_last_record_us);
	d["tlas_built"] = tlas_built;
	return d;
}
