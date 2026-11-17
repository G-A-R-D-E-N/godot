/**************************************************************************/
/*  test_mesh_surface_buffers.cpp                                         */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mesh_surface_buffers)

#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/storage_rd/mesh_surface_buffers.h"

namespace TestMeshSurfaceBuffers {

struct Surface {
	struct LOD {
		uint32_t index_count = 0;
		RID index_buffer;
		RID index_array;
	};

	uint32_t vertex_count = 0;
	RID vertex_buffer;
	uint32_t vertex_buffer_size = 0;
	RID attribute_buffer;
	uint32_t attribute_buffer_size = 0;
	RID skin_buffer;
	uint32_t skin_buffer_size = 0;
	RID index_buffer;
	uint32_t index_buffer_size = 0;
	RID index_array;
	uint32_t index_count = 0;
	LOD *lods = nullptr;
	uint32_t lod_count = 0;
	RID blend_shape_buffer;
	uint32_t blend_shape_buffer_size = 0;

	~Surface() {
		if (lods) {
			memdelete_arr(lods);
		}
	}
};

struct Shape {
	bool attribute = false;
	bool skin = false;
	bool blend_shape = false;
	uint32_t lod_indices[2] = { 0, 0 };
	uint32_t lod_count = 0;
	bool vertex = true;
	bool index = true;
};

struct Device {
	uint32_t calls = 0;
	uint32_t fail_at = UINT32_MAX;
	uint64_t next_id = 1;
	LocalVector<RID> buffers;
	LocalVector<RID> freed;

	RID buffer_create(uint32_t p_size) {
		if (p_size == 0 || calls++ == fail_at) {
			return RID();
		}
		RID rid = RID::from_uint64(next_id++);
		buffers.push_back(rid);
		return rid;
	}

	RID index_array_create(const RID &p_index_buffer, uint32_t p_index_count) {
		if (!p_index_buffer.is_valid() || p_index_count == 0 || calls++ == fail_at) {
			return RID();
		}
		return RID::from_uint64(next_id++);
	}
};

void build(Surface &r_surface, Device &r_device, const Shape &p_shape) {
	r_surface.vertex_count = 100;
	if (p_shape.vertex) {
		r_surface.vertex_buffer = r_device.buffer_create(1200);
		r_surface.vertex_buffer_size = 1200;
	}
	if (p_shape.attribute) {
		r_surface.attribute_buffer = r_device.buffer_create(800);
		r_surface.attribute_buffer_size = 800;
	}
	if (p_shape.skin) {
		r_surface.skin_buffer = r_device.buffer_create(1600);
		r_surface.skin_buffer_size = 1600;
	}
	if (p_shape.index) {
		r_surface.index_buffer = r_device.buffer_create(600);
		r_surface.index_buffer_size = 600;
		r_surface.index_count = 300;
		r_surface.index_array = r_device.index_array_create(r_surface.index_buffer, r_surface.index_count);
	}
	if (p_shape.index && p_shape.lod_count) {
		r_surface.lods = memnew_arr(Surface::LOD, p_shape.lod_count);
		r_surface.lod_count = p_shape.lod_count;
		for (uint32_t i = 0; i < p_shape.lod_count; i++) {
			const uint32_t indices = p_shape.lod_indices[i];
			r_surface.lods[i].index_buffer = r_device.buffer_create(indices * 2);
			r_surface.lods[i].index_array = r_device.index_array_create(r_surface.lods[i].index_buffer, indices);
			r_surface.lods[i].index_count = indices;
		}
	}
	if (p_shape.blend_shape) {
		r_surface.blend_shape_buffer = r_device.buffer_create(2400);
		r_surface.blend_shape_buffer_size = 2400;
	}
}

bool released(const Surface &p_surface) {
	return p_surface.vertex_buffer.is_null() && p_surface.attribute_buffer.is_null() && p_surface.skin_buffer.is_null() &&
			p_surface.index_buffer.is_null() && p_surface.index_array.is_null() && p_surface.blend_shape_buffer.is_null() &&
			p_surface.lods == nullptr && p_surface.lod_count == 0 && p_surface.vertex_count == 0 && p_surface.index_count == 0 &&
			p_surface.vertex_buffer_size == 0 && p_surface.attribute_buffer_size == 0 && p_surface.skin_buffer_size == 0 &&
			p_surface.index_buffer_size == 0 && p_surface.blend_shape_buffer_size == 0;
}

bool same_set(const LocalVector<RID> &p_a, const LocalVector<RID> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (const RID &rid : p_a) {
		uint32_t matches = 0;
		for (const RID &other : p_b) {
			matches += rid == other ? 1 : 0;
		}
		if (matches != 1) {
			return false;
		}
	}
	return true;
}

struct Case {
	const char *name;
	Shape shape;
	uint32_t fail_at;
	uint32_t wanted;
	uint32_t made;
	const char *first_missing;
};

TEST_CASE("[MeshSurfaceBuffers] A failed allocation is found, named and freed") {
	const Shape lit = { true, false, false, { 240, 90 }, 2 };
	const Shape skinned = { false, true, true, { 0, 0 }, 0 };
	const Shape empty_lod = { true, false, false, { 240, 0 }, 2 };
	const Shape vertex_only = { false, false, false, { 0, 0 }, 0, true, false };
	const Shape index_only = { false, false, false, { 0, 0 }, 0, false, true };
	const Case cases[] = {
		{ "lit, all made", lit, UINT32_MAX, 8, 8, nullptr },
		{ "lit, vertex fails", lit, 0, 8, 7, "vertex" },
		{ "lit, attribute fails", lit, 1, 8, 7, "attribute" },
		{ "lit, index fails", lit, 2, 8, 6, "index" },
		{ "lit, index array fails", lit, 3, 8, 7, "index array" },
		{ "lit, LOD 0 index fails", lit, 4, 8, 6, "LOD index" },
		{ "lit, LOD 1 index array fails", lit, 7, 8, 7, "LOD index array" },
		{ "skinned, all made", skinned, UINT32_MAX, 5, 5, nullptr },
		{ "skinned, skin fails", skinned, 1, 5, 4, "skin" },
		{ "skinned, blend shape fails", skinned, 4, 5, 4, "blend shape" },
		{ "empty LOD, all made", empty_lod, UINT32_MAX, 6, 6, nullptr },
		{ "vertex only, all made", vertex_only, UINT32_MAX, 1, 1, nullptr },
		{ "vertex only, vertex fails", vertex_only, 0, 1, 0, "vertex" },
		{ "index only, all made", index_only, UINT32_MAX, 2, 2, nullptr },
		{ "index only, index array fails", index_only, 1, 2, 1, "index array" },
	};

	for (const Case &c : cases) {
		Device device;
		device.fail_at = c.fail_at;
		Surface surface;
		build(surface, device, c.shape);

		const MeshSurfaceBufferCheck check = mesh_surface_check_buffers(surface);
		CHECK_MESSAGE(check.wanted == c.wanted, c.name);
		CHECK_MESSAGE(check.made == c.made, c.name);
		CHECK_MESSAGE(check.complete() == (c.first_missing == nullptr), c.name);
		if (c.first_missing) {
			REQUIRE_MESSAGE(check.first_missing != nullptr, c.name);
			CHECK_MESSAGE(String(check.first_missing) == String(c.first_missing), c.name);
		} else {
			CHECK_MESSAGE(check.first_missing == nullptr, c.name);
		}

		if (!check.complete()) {
			mesh_surface_release_buffers(surface, [&device](RID p_buffer) { device.freed.push_back(p_buffer); });
			CHECK_MESSAGE(released(surface), c.name);
			CHECK_MESSAGE(same_set(device.freed, device.buffers), c.name);
		}
	}
}

TEST_CASE("[MeshSurfaceBuffers] A surface that wants no buffers is complete") {
	Surface surface;
	const MeshSurfaceBufferCheck check = mesh_surface_check_buffers(surface);
	CHECK(check.wanted == 0);
	CHECK(check.made == 0);
	CHECK(check.complete());
	CHECK(check.first_missing == nullptr);
}

} // namespace TestMeshSurfaceBuffers
