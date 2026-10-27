/**************************************************************************/
/*  renderer_alpha_sort.h                                                 */
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

#include "core/typedefs.h"

template <typename T>
struct SortByKeyAndOwner {
	_FORCE_INLINE_ bool operator()(const T *A, const T *B) const {
		if (A->sort.sort_key2 != B->sort.sort_key2) {
			return A->sort.sort_key2 < B->sort.sort_key2;
		}
		if (A->sort.sort_key1 != B->sort.sort_key1) {
			return A->sort.sort_key1 < B->sort.sort_key1;
		}
		return (uintptr_t)A->owner < (uintptr_t)B->owner;
	}
};

template <typename T>
struct SortByReverseDepthAndPriority {
	_FORCE_INLINE_ bool operator()(const T *A, const T *B) const {
		if (A->sort.priority != B->sort.priority) {
			return A->sort.priority < B->sort.priority;
		}
		if (A->owner->depth != B->owner->depth) {
			return A->owner->depth > B->owner->depth;
		}
		if (A->owner != B->owner) {
			return (uintptr_t)A->owner < (uintptr_t)B->owner;
		}
		if (A->surface_index != B->surface_index) {
			return A->surface_index > B->surface_index;
		}
		for (const T *s = A->next; s; s = s->next) {
			if (s == B) {
				return true;
			}
		}
		return false;
	}
};
