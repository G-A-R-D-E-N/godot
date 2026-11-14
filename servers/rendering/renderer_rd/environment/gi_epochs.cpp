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

namespace RendererRD {

GIEpochs *GIEpochs::get_singleton() {
	static GIEpochs instance;
	return &instance;
}

uint32_t GIEpochs::next(uint32_t p_value) {
	const uint32_t value = p_value + 1;
	return value == 0 ? 1 : value;
}

void GIEpochs::bump(Source p_source) {
	if (p_source < SOURCE_COUNT) {
		epochs[p_source] = next(epochs[p_source]);
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
