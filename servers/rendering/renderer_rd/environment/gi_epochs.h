/**************************************************************************/
/*  gi_epochs.h                                                           */
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

#include "gi_provider.h"

#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"

#include <stdint.h>

namespace RendererRD {

class GISceneTracker {
	uint64_t signature = 0;
	uint64_t member_signature = 0;
	bool have = false;
	HashSet<uint64_t> members;

public:
	struct Change {
		bool scene = false;
		bool world = false;
	};

	static uint64_t add_instance(uint64_t p_signature, uint32_t p_instance_hash);
	static uint64_t member_key(const void *p_instance, uint64_t p_mesh_id);
	static uint64_t member_signature_of(const LocalVector<uint64_t> &p_members);
	Change update(uint64_t p_signature, const LocalVector<uint64_t> &p_members);
};

class GIEpochs {
public:
	enum Source : uint32_t {
		SOURCE_SCENE = 0,
		SOURCE_LIGHT = 1,
		SOURCE_SKY = 2,
		SOURCE_ORIGIN = 3,
		SOURCE_WORLD = 4,
		SOURCE_COUNT = 5,
	};

	struct Snapshot {
		uint32_t scene;
		uint32_t light;
		uint32_t sky;
		uint32_t origin;
		uint32_t world;
	};

private:
	uint32_t epochs[SOURCE_COUNT] = {};

public:
	static GIEpochs *get_singleton();
	static uint32_t next(uint32_t p_value);

	void bump(Source p_source);
	void apply(const GISceneTracker::Change &p_change);

	uint32_t get(Source p_source) const;
	Snapshot snapshot() const;
	void fill_record(GIProviderRecord &r_record) const;
};

} // namespace RendererRD
