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


#include "ray_scene.h"

using namespace RendererRD;

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
