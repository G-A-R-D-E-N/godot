/**************************************************************************/
/*  test_renderer_opaque_sort.cpp                                         */
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

TEST_FORCE_LINK(test_renderer_opaque_sort)

#include "core/math/random_pcg.h"
#include "core/templates/local_vector.h"
#include "core/templates/sort_array.h"
#include "servers/rendering/renderer_alpha_sort.h"

namespace TestRendererOpaqueSort {

struct Owner {
	uint32_t id = 0;
};

struct Surface {
	struct {
		uint64_t sort_key1 = 0;
		uint64_t sort_key2 = 0;
	} sort;
	Owner *owner = nullptr;
};

struct Scene {
	Owner owners[8];
	Surface surfaces[32];
	LocalVector<Surface *> expected;

	Scene() {
		const uint64_t keys2[7] = { 1, 1, 2, 2, 3, 3, 5 };
		const uint64_t keys1[7] = { 5, 9, 3, 4, 1, 2, 0 };
		const uint32_t users[7] = { 0x3f, 0x14, 0xea, 0xff, 0x10, 0x45, 0xfe };
		uint32_t made = 0;
		for (int k = 0; k < 7; k++) {
			for (int o = 0; o < 8; o++) {
				if (!(users[k] & (1u << o))) {
					continue;
				}
				Surface &surface = surfaces[(made++ * 13) % 32];
				surface.sort.sort_key2 = keys2[k] << 40;
				surface.sort.sort_key1 = keys1[k] << 8;
				surface.owner = &owners[o];
				expected.push_back(&surface);
			}
		}
	}
};

bool sorts_back(const LocalVector<Surface *> &p_order, uint32_t p_seed) {
	LocalVector<Surface *> list(p_order);
	RandomPCG rng(p_seed);
	for (uint32_t i = list.size() - 1; i > 0; i--) {
		SWAP(list[i], list[rng.rand(i + 1)]);
	}
	SortArray<Surface *, SortByKeyAndOwner<Surface>> sorter;
	sorter.sort(list.ptr(), list.size());
	for (uint32_t i = 0; i < list.size(); i++) {
		if (list[i] != p_order[i]) {
			return false;
		}
	}
	return true;
}

TEST_CASE("[RendererOpaqueSort] Surfaces with equal keys sort the same whatever the input order") {
	Scene scene;
	REQUIRE(scene.expected.size() == 32);
	for (uint32_t seed = 1; seed <= 500; seed++) {
		CHECK(sorts_back(scene.expected, seed));
	}
}

TEST_CASE("[RendererOpaqueSort] Removing surfaces keeps the order of the rest") {
	Scene scene;
	LocalVector<Surface *> kept;
	for (Surface *surface : scene.expected) {
		if (surface->owner != &scene.owners[0] && surface->owner != &scene.owners[5]) {
			kept.push_back(surface);
		}
	}
	REQUIRE(kept.size() == 25);
	for (uint32_t seed = 1; seed <= 500; seed++) {
		CHECK(sorts_back(kept, seed));
	}
}

} // namespace TestRendererOpaqueSort
