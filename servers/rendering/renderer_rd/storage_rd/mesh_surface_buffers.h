/**************************************************************************/
/*  mesh_surface_buffers.h                                                */
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

#include "core/os/memory.h"
#include "core/templates/rid.h"

struct MeshSurfaceBufferCheck {
	uint32_t wanted = 0;
	uint32_t made = 0;
	const char *first_missing = nullptr;

	_FORCE_INLINE_ void expect(bool p_wanted, const RID &p_buffer, const char *p_name) {
		if (!p_wanted) {
			return;
		}
		wanted++;
		if (p_buffer.is_valid()) {
			made++;
		} else if (!first_missing) {
			first_missing = p_name;
		}
	}

	_FORCE_INLINE_ bool complete() const {
		return made == wanted;
	}
};

template <typename S>
MeshSurfaceBufferCheck mesh_surface_check_buffers(const S &p_surface) {
	MeshSurfaceBufferCheck check;
	check.expect(p_surface.vertex_buffer_size > 0, p_surface.vertex_buffer, "vertex");
	check.expect(p_surface.attribute_buffer_size > 0, p_surface.attribute_buffer, "attribute");
	check.expect(p_surface.skin_buffer_size > 0, p_surface.skin_buffer, "skin");
	check.expect(p_surface.index_count > 0, p_surface.index_buffer, "index");
	check.expect(p_surface.index_count > 0, p_surface.index_array, "index array");
	for (uint32_t i = 0; i < p_surface.lod_count; i++) {
		check.expect(p_surface.lods[i].index_count > 0, p_surface.lods[i].index_buffer, "LOD index");
		check.expect(p_surface.lods[i].index_count > 0, p_surface.lods[i].index_array, "LOD index array");
	}
	check.expect(p_surface.blend_shape_buffer_size > 0, p_surface.blend_shape_buffer, "blend shape");
	return check;
}

template <typename S, typename F>
void mesh_surface_release_buffers(S &r_surface, F p_free) {
	RID *buffers[] = { &r_surface.vertex_buffer, &r_surface.attribute_buffer, &r_surface.skin_buffer, &r_surface.index_buffer, &r_surface.blend_shape_buffer };
	for (RID *buffer : buffers) {
		if (buffer->is_valid()) {
			p_free(*buffer);
			*buffer = RID();
		}
	}
	for (uint32_t i = 0; i < r_surface.lod_count; i++) {
		if (r_surface.lods[i].index_buffer.is_valid()) {
			p_free(r_surface.lods[i].index_buffer);
		}
	}
	if (r_surface.lods) {
		memdelete_arr(r_surface.lods);
		r_surface.lods = nullptr;
	}
	r_surface.lod_count = 0;
	r_surface.index_array = RID();
	r_surface.vertex_count = 0;
	r_surface.index_count = 0;
	r_surface.vertex_buffer_size = 0;
	r_surface.attribute_buffer_size = 0;
	r_surface.skin_buffer_size = 0;
	r_surface.index_buffer_size = 0;
	r_surface.blend_shape_buffer_size = 0;
}
