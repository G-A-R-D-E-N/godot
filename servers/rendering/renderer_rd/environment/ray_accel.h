/**************************************************************************/
/*  ray_accel.h                                                           */
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

#include "core/math/transform_3d.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "servers/rendering/renderer_rd/environment/ray_scene.h"
#include "servers/rendering/rendering_device.h"

namespace RendererRD {

class RayAccel {
public:
	static constexpr uint32_t TLAS_MIN_CAPACITY = 1024;
	static constexpr uint32_t SOUP_BYTES_PER_TRIANGLE = 36;

	enum BlasResult {
		BLAS_READY,
		BLAS_DEFERRED,
		BLAS_FAILED,
	};

	struct Limits {
		uint32_t max_instances = 262144;
		uint32_t blas_per_frame = 64;
		uint32_t blas_triangles_per_frame = 2000000;
	};

	struct InstanceRecord {
		RID mesh;
		Transform3D transform;
		uint32_t custom_index = 0;
		uint32_t mask = 0xFF;
		uint32_t instance_class = RayScene::INSTANCE_CLASS_OPAQUE;
	};

	struct Counters {
		uint64_t frames = 0;
		uint64_t blas_built_total = 0;
		uint64_t blas_freed_total = 0;
		uint64_t blas_failed_total = 0;
		uint64_t blas_deferred_total = 0;
		uint64_t blas_split_built_total = 0;
		uint64_t tlas_builds = 0;
		uint64_t tlas_creates = 0;
		uint64_t rids_created = 0;
		uint64_t rids_freed = 0;
		uint64_t blas_record_us = 0;
		uint64_t tlas_record_us = 0;
		uint64_t tlas_last_record_us = 0;
		uint64_t soup_bytes = 0;
		uint32_t blas_built_frame = 0;
		uint32_t blas_deferred_frame = 0;
		uint32_t blas_freed_frame = 0;
		uint32_t triangles_built_frame = 0;
		uint32_t instances_in_tlas = 0;
		uint32_t instances_skipped_cap = 0;
		uint32_t instances_skipped_unbuilt = 0;
		uint32_t tlas_capacity = 0;
		uint32_t blas_count = 0;
		uint32_t blas_split = 0;
		uint32_t blas_geometries = 0;
	};

private:
	struct Blas {
		RID soup;
		RID blas;
		uint64_t generation = 0;
		uint64_t split_key = 0;
		uint32_t triangles = 0;
		uint32_t soup_bytes = 0;
		uint32_t geometries = 1;
		bool failed = false;
	};

	HashMap<RID, Blas> blases;
	RID tlas;
	uint32_t tlas_capacity = 0;
	bool tlas_built = false;
	bool blas_changed = false;
	bool cap_logged = false;
	HashSet<RID> deferred_seen;
	Limits limits;
	Counters counters;

	void _free_blas(Blas &r_blas);
	void _free_tlas();
	void _release(RID p_rid);

public:
	RayAccel() {}
	~RayAccel();

	void begin_frame(const Limits &p_limits);
	BlasResult ensure_blas(RID p_mesh, uint64_t p_generation, uint64_t p_split_key, const RayScene::Triangle *p_triangles, uint32_t p_count, const uint32_t *p_order, uint32_t p_opaque_count);
	void mesh_removed(RID p_mesh);
	void reset();
	bool update_tlas(const LocalVector<InstanceRecord> &p_records, bool p_force, uint32_t p_skipped_cap, uint32_t p_skipped_unbuilt);

	bool blas_changed_this_frame() const { return blas_changed; }
	RID get_tlas() const { return tlas; }
	bool is_ready() const { return tlas.is_valid() && tlas_built; }
	const Counters &get_counters() const { return counters; }
	uint32_t get_max_instances() const { return limits.max_instances; }
	String get_text() const;
	Dictionary get_dictionary() const;

	static BitField<RD::AccelerationStructureInstanceFlagBits> instance_flags(uint32_t p_class);
};

} // namespace RendererRD
