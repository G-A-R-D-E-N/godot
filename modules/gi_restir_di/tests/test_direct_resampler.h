/**************************************************************************/
/*  test_direct_resampler.h                                               */
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

#include "modules/gi_restir_di/direct_resampler.h"

#include "tests/test_macros.h"

#include <cstddef>

namespace TestDirectResampler {

TEST_CASE("[DirectResampler] Settings keep head and tail inside DI-1's entries") {
	DirectResamplerRD::Settings settings = DirectResamplerRD::clamp_settings(true, 4, 1, 0.0625f, 0);
	CHECK(settings.head == 4);
	CHECK(settings.tail_samples == 1);
	CHECK(settings.floor_value == doctest::Approx(0.0625f));
	CHECK_FALSE(settings.live_energy);

	settings = DirectResamplerRD::clamp_settings(true, 6, 2, 0.25f, 1);
	CHECK(settings.head == 5);
	CHECK(settings.tail_samples == 2);
	CHECK(settings.live_energy);

	settings = DirectResamplerRD::clamp_settings(true, 9, 9, 0.0f, 3);
	CHECK(settings.tail_samples == 7);
	CHECK(settings.head == 0);
	CHECK(settings.floor_value == doctest::Approx(0.0001f));
	CHECK_FALSE(settings.live_energy);

	settings = DirectResamplerRD::clamp_settings(false, -1, 0, 2.0f, 0);
	CHECK_FALSE(settings.enabled);
	CHECK(settings.head == 0);
	CHECK(settings.tail_samples == 1);
	CHECK(settings.floor_value == doctest::Approx(1.0f));

	for (int head = -2; head < 10; head++) {
		for (int tail = -2; tail < 10; tail++) {
			settings = DirectResamplerRD::clamp_settings(true, head, tail, 0.0625f, 0);
			CHECK(settings.tail_samples >= 1);
			CHECK(settings.head + settings.tail_samples + 1 <= 8);
		}
	}
}

TEST_CASE("[DirectResampler] Params match the entry pass's std140 block") {
	CHECK(offsetof(DirectResamplerRD::Params, prev_view_from_view) == 64);
	CHECK(offsetof(DirectResamplerRD::Params, prev_projection) == 128);
	CHECK(offsetof(DirectResamplerRD::Params, camera_basis) == 192);
	CHECK(offsetof(DirectResamplerRD::Params, region_half) == 240);
	CHECK(offsetof(DirectResamplerRD::Params, floor_value) == 252);
	CHECK(offsetof(DirectResamplerRD::Params, size) == 256);
	CHECK(offsetof(DirectResamplerRD::Params, pixel_size) == 264);
	CHECK(offsetof(DirectResamplerRD::Params, cluster_shift) == 272);
	CHECK(offsetof(DirectResamplerRD::Params, z_far) == 288);
	CHECK(offsetof(DirectResamplerRD::Params, head_entries) == 296);
	CHECK(offsetof(DirectResamplerRD::Params, history_valid) == 304);
	CHECK(offsetof(DirectResamplerRD::Params, live_energy) == 308);
	CHECK(offsetof(DirectResamplerRD::Params, debug_flags) == 312);
	CHECK(sizeof(DirectResamplerRD::Params) == 320);
	CHECK(DirectResamplerRD::DEBUG_BYTES == (4 + 65536) * 4);
}

} // namespace TestDirectResampler
