/**************************************************************************/
/*  test_renderer_alpha_sort.cpp                                          */
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

TEST_FORCE_LINK(test_renderer_alpha_sort)

#include "core/math/random_pcg.h"
#include "core/templates/local_vector.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/renderer_alpha_sort.h"

namespace TestRendererAlphaSort {

struct Owner {
	float depth = 0.0f;
};

struct Surface {
	struct {
		uint32_t priority = 0;
	} sort;
	Owner *owner = nullptr;
	uint32_t surface_index = 0;
};

struct Scene {
	Owner owners[5];
	Surface surfaces[30];
	LocalVector<Surface *> expected;

	Scene() {
		const float depths[5] = { 9.0f, 5.0f, 5.0f, 2.0f, 5.0f };
		const uint32_t counts[5] = { 2, 8, 6, 6, 8 };
		int slot = 29;
		for (int o = 0; o < 5; o++) {
			owners[o].depth = depths[o];
			for (uint32_t s = 0; s < counts[o]; s++) {
				Surface &surface = surfaces[slot--];
				surface.owner = &owners[o];
				surface.surface_index = o == 4 ? MIN(s, 6u) : s;
				surface.sort.priority = o == 4 ? 1 : 0;
			}
		}
		for (int i = 29; i >= 0; i--) {
			expected.push_back(&surfaces[i]);
		}
		SWAP(expected[28], expected[29]);
	}
};

bool sorts_back(const LocalVector<Surface *> &p_order, uint32_t p_seed) {
	LocalVector<Surface *> list(p_order);
	RandomPCG rng(p_seed);
	for (uint32_t i = list.size() - 1; i > 0; i--) {
		SWAP(list[i], list[rng.rand(i + 1)]);
	}
	SortArray<Surface *, SortByReverseDepthAndPriority<Surface>> sorter;
	sorter.sort(list.ptr(), list.size());
	for (uint32_t i = 0; i < list.size(); i++) {
		if (list[i] != p_order[i]) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[RendererAlphaSort] Tied surfaces sort the same whatever the input order") {
	Scene scene;
	for (uint32_t seed = 1; seed <= 500; seed++) {
		CHECK(sorts_back(scene.expected, seed));
	}
}

TEST_CASE("[RendererAlphaSort] Removing surfaces keeps the order of the rest") {
	Scene scene;
	LocalVector<Surface *> kept;
	for (Surface *surface : scene.expected) {
		if (surface->owner != &scene.owners[0] && surface->owner != &scene.owners[3]) {
			kept.push_back(surface);
		}
	}
	REQUIRE(kept.size() == 22);
	for (uint32_t seed = 1; seed <= 500; seed++) {
		CHECK(sorts_back(kept, seed));
	}
}

} // namespace TestRendererAlphaSort
