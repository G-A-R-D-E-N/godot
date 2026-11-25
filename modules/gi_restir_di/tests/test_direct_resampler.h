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

#include "tests/test_macros.h"

#include "core/io/file_access.h"
#include "core/os/os.h"

#include "modules/gi_restir_di/direct_resampler.h"

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
	CHECK(offsetof(DirectResamplerRD::Params, prev_inv_projection) == 192);
	CHECK(offsetof(DirectResamplerRD::Params, camera_basis) == 256);
	CHECK(offsetof(DirectResamplerRD::Params, region_half) == 304);
	CHECK(offsetof(DirectResamplerRD::Params, floor_value) == 316);
	CHECK(offsetof(DirectResamplerRD::Params, size) == 320);
	CHECK(offsetof(DirectResamplerRD::Params, pixel_size) == 328);
	CHECK(offsetof(DirectResamplerRD::Params, cluster_shift) == 336);
	CHECK(offsetof(DirectResamplerRD::Params, z_far) == 352);
	CHECK(offsetof(DirectResamplerRD::Params, head_entries) == 360);
	CHECK(offsetof(DirectResamplerRD::Params, history_valid) == 368);
	CHECK(offsetof(DirectResamplerRD::Params, live_energy) == 372);
	CHECK(offsetof(DirectResamplerRD::Params, debug_flags) == 376);
	CHECK(offsetof(DirectResamplerRD::Params, reservoirs) == 380);
	CHECK(offsetof(DirectResamplerRD::Params, c_full) == 384);
	CHECK(offsetof(DirectResamplerRD::Params, m_temporal_cap) == 388);
	CHECK(offsetof(DirectResamplerRD::Params, dynamic_cap) == 392);
	CHECK(offsetof(DirectResamplerRD::Params, m_stored_cap) == 396);
	CHECK(offsetof(DirectResamplerRD::Params, spatial_neighbors) == 400);
	CHECK(offsetof(DirectResamplerRD::Params, spatial_radius) == 404);
	CHECK(offsetof(DirectResamplerRD::Params, counter) == 408);
	CHECK(offsetof(DirectResamplerRD::Params, head_rank) == 412);
	CHECK(offsetof(DirectResamplerRD::Params, head_swap_margin) == 416);
	CHECK(offsetof(DirectResamplerRD::Params, spatial_normal_dot) == 420);
	CHECK(offsetof(DirectResamplerRD::Params, head_swap_frames) == 424);
	CHECK(offsetof(DirectResamplerRD::Params, temporal_reuse) == 428);
	CHECK(offsetof(DirectResamplerRD::Params, head_window) == 432);
	CHECK(offsetof(DirectResamplerRD::Params, std140_pad) == 436);
	CHECK(sizeof(DirectResamplerRD::Params) == 448);
	CHECK(DirectResamplerRD::DEBUG_BYTES == (4 + 65536) * 4);
}

TEST_CASE("[DirectResampler] Reservoir settings stay inside the record's fields") {
	DirectResamplerRD::Settings settings = DirectResamplerRD::clamp_settings(true, 4, 1, 0.0625f, 0);
	CHECK(settings.reservoirs == DirectResamplerRD::RESERVOIRS_OFF);
	CHECK_FALSE(settings.head_rank_visible);

	DirectResamplerRD::clamp_reservoir_settings(settings, 1, true, 2, 16.0f, 32, 20, 255, 2, 0, 0.25f, 4, false);
	CHECK(settings.reservoirs == DirectResamplerRD::RESERVOIRS_B1);
	CHECK(settings.temporal_reuse);
	CHECK(settings.spatial_neighbors == 2);
	CHECK(settings.spatial_radius == doctest::Approx(16.0f));
	CHECK(settings.c_full == 32);
	CHECK(settings.m_temporal_cap == 20);
	CHECK(settings.m_stored_cap == 255);
	CHECK(settings.dynamic_cap == 2);
	CHECK_FALSE(settings.head_rank_visible);
	CHECK(settings.head_swap_margin == doctest::Approx(0.25f));
	CHECK(settings.head_swap_frames == 4);
	CHECK_FALSE(settings.head_window);

	DirectResamplerRD::clamp_reservoir_settings(settings, 7, false, 9, 500.0f, 900, 0, 4096, -3, 1, -1.0f, 12, true);
	CHECK(settings.reservoirs == DirectResamplerRD::RESERVOIRS_U1);
	CHECK_FALSE(settings.temporal_reuse);
	CHECK(settings.spatial_neighbors == DirectResamplerRD::MAX_NEIGHBORS);
	CHECK(settings.spatial_radius == doctest::Approx(64.0f));
	CHECK(settings.c_full == DirectResamplerRD::MAX_MASS);
	CHECK(settings.m_temporal_cap == 1);
	CHECK(settings.m_stored_cap == DirectResamplerRD::MAX_MASS);
	CHECK(settings.dynamic_cap == 1);
	CHECK(settings.head_rank_visible);
	CHECK(settings.head_swap_margin == doctest::Approx(0.0f));
	CHECK(settings.head_swap_frames == DirectResamplerRD::MAX_SWAP_FRAMES);
	CHECK(settings.head_window);

	DirectResamplerRD::clamp_reservoir_settings(settings, -4, true, -1, 0.0f, 0, 20, 0, 2, 3, 9.0f, 0, false);
	CHECK(settings.reservoirs == DirectResamplerRD::RESERVOIRS_OFF);
	CHECK(settings.spatial_neighbors == 0);
	CHECK(settings.spatial_radius == doctest::Approx(1.0f));
	CHECK(settings.c_full == 1);
	CHECK(settings.m_stored_cap == 1);
	CHECK_FALSE(settings.head_rank_visible);
	CHECK(settings.head_swap_margin == doctest::Approx(4.0f));
	CHECK(settings.head_swap_frames == 1);

	for (int tail = 2; tail < 8; tail++) {
		settings = DirectResamplerRD::clamp_settings(true, 0, tail, 0.0625f, 0);
		DirectResamplerRD::clamp_reservoir_settings(settings, 2, true, 2, 16.0f, 32, 20, 255, 2, 0, 0.25f, 4, false);
		CHECK(settings.reservoirs == DirectResamplerRD::RESERVOIRS_OFF);
	}
}

TEST_CASE("[DirectResampler] The frame counter steps by one on a continuous frame and by two otherwise") {
	CHECK(DirectResamplerRD::advance_counter(1, true) == 2);
	CHECK(DirectResamplerRD::advance_counter(1, false) == 3);
	CHECK(DirectResamplerRD::advance_counter(0xffffffffu, true) == 1);
	CHECK(DirectResamplerRD::advance_counter(0xfffffffeu, false) == 1);
	CHECK(DirectResamplerRD::advance_counter(0xffffffffu, false) == 1);

	uint32_t counter = 1;
	for (int frame = 0; frame < 4096; frame++) {
		const bool continuous = (frame % 7) != 3;
		const uint32_t next = DirectResamplerRD::advance_counter(counter, continuous);
		CHECK(next != 0);
		const bool temporal_accepts = ((next - 1) & 127u) == (counter & 127u);
		CHECK(temporal_accepts == continuous);
		CHECK((next & 127u) != (counter & 127u));
		counter = next;
	}
}

TEST_CASE("[DirectResampler] Default settings are off and use the live energy arm") {
	const DirectResamplerRD::Settings settings;
	CHECK_FALSE(settings.enabled);
	CHECK(settings.live_energy);
}

#ifdef DI_RESAMPLER_LAB
TEST_CASE("[DirectResampler] The lab mass bound is the registered one at the gated settings") {
	DirectResamplerRD::Settings settings;
	settings.c_full = 8;
	settings.m_temporal_cap = 20;
	settings.m_stored_cap = 255;
	settings.spatial_neighbors = 4;
	CHECK(DirectResamplerRD::lab_mass_bound(settings) == 1076);
	settings.c_full = 255;
	CHECK(DirectResamplerRD::lab_mass_bound(settings) == 1570);
	CHECK(DirectResamplerRD::lab_settings_text(settings).ends_with(" mass_bound=1570"));
	CHECK(DirectResamplerRD::lab_settings_text(settings).contains(" stale_prior=0 no_stamp=0 mass_bound="));
	settings.no_stamp = true;
	CHECK(DirectResamplerRD::lab_settings_text(settings).contains(" no_stamp=1 mass_bound=1570"));
}
#endif

} // namespace TestDirectResampler

TEST_CASE("[DirectResampler] Params field order matches di_resample.glsl") {
	String base = OS::get_singleton()->get_executable_path().get_base_dir();
	String source;
	for (int i = 0; i < 6; i++) {
		const String candidate = base.path_join("modules/gi_restir_di/shaders/di_resample.glsl");
		if (FileAccess::exists(candidate)) {
			source = FileAccess::get_file_as_string(candidate);
			break;
		}
		base = base.get_base_dir();
	}
	if (source.is_empty()) {
		WARN("shader source not found from the executable path; skipping");
		return;
	}
	const int at = source.find("uniform Params");
	REQUIRE(at >= 0);
	const int open = source.find("{", at);
	const int close = source.find("}", open);
	REQUIRE(open >= 0);
	REQUIRE(close > open);

	struct Field {
		const char *name;
		size_t offset;
	};
	const Field fields[] = {
		{ "inv_projection", offsetof(DirectResamplerRD::Params, inv_projection) },
		{ "prev_view_from_view", offsetof(DirectResamplerRD::Params, prev_view_from_view) },
		{ "prev_projection", offsetof(DirectResamplerRD::Params, prev_projection) },
		{ "prev_inv_projection", offsetof(DirectResamplerRD::Params, prev_inv_projection) },
		{ "camera_basis", offsetof(DirectResamplerRD::Params, camera_basis) },
		{ "region_half", offsetof(DirectResamplerRD::Params, region_half) },
		{ "floor_value", offsetof(DirectResamplerRD::Params, floor_value) },
		{ "size", offsetof(DirectResamplerRD::Params, size) },
		{ "pixel_size", offsetof(DirectResamplerRD::Params, pixel_size) },
		{ "cluster_shift", offsetof(DirectResamplerRD::Params, cluster_shift) },
		{ "cluster_width", offsetof(DirectResamplerRD::Params, cluster_width) },
		{ "cluster_type_size", offsetof(DirectResamplerRD::Params, cluster_type_size) },
		{ "max_cluster_element_count_div_32", offsetof(DirectResamplerRD::Params, max_cluster_element_count_div_32) },
		{ "z_far", offsetof(DirectResamplerRD::Params, z_far) },
		{ "frame", offsetof(DirectResamplerRD::Params, frame) },
		{ "head_entries", offsetof(DirectResamplerRD::Params, head_entries) },
		{ "tail_samples", offsetof(DirectResamplerRD::Params, tail_samples) },
		{ "history_valid", offsetof(DirectResamplerRD::Params, history_valid) },
		{ "live_energy", offsetof(DirectResamplerRD::Params, live_energy) },
		{ "debug_flags", offsetof(DirectResamplerRD::Params, debug_flags) },
		{ "reservoirs", offsetof(DirectResamplerRD::Params, reservoirs) },
		{ "c_full", offsetof(DirectResamplerRD::Params, c_full) },
		{ "m_temporal_cap", offsetof(DirectResamplerRD::Params, m_temporal_cap) },
		{ "dynamic_cap", offsetof(DirectResamplerRD::Params, dynamic_cap) },
		{ "m_stored_cap", offsetof(DirectResamplerRD::Params, m_stored_cap) },
		{ "spatial_neighbors", offsetof(DirectResamplerRD::Params, spatial_neighbors) },
		{ "spatial_radius", offsetof(DirectResamplerRD::Params, spatial_radius) },
		{ "counter", offsetof(DirectResamplerRD::Params, counter) },
		{ "head_rank", offsetof(DirectResamplerRD::Params, head_rank) },
		{ "head_swap_margin", offsetof(DirectResamplerRD::Params, head_swap_margin) },
		{ "spatial_normal_dot", offsetof(DirectResamplerRD::Params, spatial_normal_dot) },
		{ "head_swap_frames", offsetof(DirectResamplerRD::Params, head_swap_frames) },
		{ "temporal_reuse", offsetof(DirectResamplerRD::Params, temporal_reuse) },
		{ "head_window", offsetof(DirectResamplerRD::Params, head_window) },
	};
	const int count = int(sizeof(fields) / sizeof(fields[0]));

	int index = 0;
	int cursor = open + 1;
	while (index < count) {
		const int semi = source.find(";", cursor);
		if (semi < 0 || semi > close) {
			break;
		}
		const String line = source.substr(cursor, semi - cursor).strip_edges();
		cursor = semi + 1;
		if (line.is_empty() || line.begins_with("//")) {
			continue;
		}
		const String name = line.get_slice(" ", line.get_slice_count(" ") - 1).strip_edges();
		CHECK_MESSAGE(name == fields[index].name, vformat("field %d is %s in the shader and %s in C++", index, name, fields[index].name));
		index++;
	}
	CHECK_MESSAGE(index == count, vformat("the shader block has %d fields and C++ expects %d", index, count));
}
