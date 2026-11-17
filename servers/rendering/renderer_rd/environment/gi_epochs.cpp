/**************************************************************************/
/*  gi_epochs.cpp                                                         */
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

#include "gi_epochs.h"

#include "core/templates/hashfuncs.h"

namespace RendererRD {

GIEpochs *GIEpochs::get_singleton() {
	static GIEpochs instance;
	return &instance;
}

uint32_t GIEpochs::next(uint32_t p_value) {
	const uint32_t value = p_value + 1;
	return value == 0 ? 1 : value;
}

uint64_t GISceneTracker::add_instance(uint64_t p_signature, uint32_t p_instance_hash) {
	return p_signature + (uint64_t(p_instance_hash) | (uint64_t(1) << 32));
}

uint64_t GISceneTracker::member_key(const void *p_instance, uint64_t p_mesh_id) {
	return (uint64_t(uintptr_t(p_instance)) * 0x9E3779B97F4A7C15ull) ^ p_mesh_id;
}

uint64_t GISceneTracker::member_signature_of(const LocalVector<uint64_t> &p_members) {
	uint64_t result = 0;
	for (uint64_t member : p_members) {
		result = add_instance(result, hash_murmur3_one_64(member));
	}
	return result;
}

GISceneTracker::Change GISceneTracker::update(uint64_t p_signature, const LocalVector<uint64_t> &p_members) {
	Change change;
	const uint64_t new_member_signature = member_signature_of(p_members);
	const bool same_members = have && new_member_signature == member_signature;
	if (have && p_signature != signature) {
		change.scene = true;
		if (!members.is_empty() && !same_members) {
			bool kept = false;
			for (uint64_t member : p_members) {
				if (members.has(member)) {
					kept = true;
					break;
				}
			}
			change.world = !kept;
		}
	} else if (!have) {
		change.scene = true;
	}
	if (!same_members) {
		members.clear();
		for (uint64_t member : p_members) {
			members.insert(member);
		}
		member_signature = new_member_signature;
	}
	signature = p_signature;
	have = true;
	return change;
}

void GIEpochs::bump(Source p_source) {
	if (p_source < SOURCE_COUNT) {
		epochs[p_source] = next(epochs[p_source]);
	}
}

void GIEpochs::apply(const GISceneTracker::Change &p_change) {
	if (p_change.scene) {
		bump(SOURCE_SCENE);
	}
	if (p_change.world) {
		bump(SOURCE_WORLD);
	}
}

uint32_t GIEpochs::get(Source p_source) const {
	return p_source < SOURCE_COUNT ? epochs[p_source] : 0;
}

GIEpochs::Snapshot GIEpochs::snapshot() const {
	Snapshot result;
	result.scene = epochs[SOURCE_SCENE];
	result.light = epochs[SOURCE_LIGHT];
	result.sky = epochs[SOURCE_SKY];
	result.origin = epochs[SOURCE_ORIGIN];
	result.world = epochs[SOURCE_WORLD];
	return result;
}

void GIEpochs::fill_record(GIProviderRecord &r_record) const {
	r_record.scene_epoch = epochs[SOURCE_SCENE];
	r_record.light_epoch = epochs[SOURCE_LIGHT];
	r_record.sky_epoch = epochs[SOURCE_SKY];
	r_record.origin_epoch = epochs[SOURCE_ORIGIN];
	if (r_record.scene_epoch != 0) {
		r_record.flags |= GI_PROVIDER_FLAG_SCENE_EPOCH;
	}
	if (r_record.light_epoch != 0) {
		r_record.flags |= GI_PROVIDER_FLAG_LIGHT_EPOCH;
	}
	if (r_record.sky_epoch != 0) {
		r_record.flags |= GI_PROVIDER_FLAG_SKY_EPOCH;
	}
	if (r_record.origin_epoch != 0) {
		r_record.flags |= GI_PROVIDER_FLAG_ORIGIN_EPOCH;
	}
}

} // namespace RendererRD
