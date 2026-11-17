/**************************************************************************/
/*  test_ray_scene_alpha.cpp                                              */
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

TEST_FORCE_LINK(test_ray_scene_alpha)

#ifdef RD_ENABLED

#include "core/io/file_access.h"
#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/environment/ray_scene.h"
#include "servers/rendering/renderer_rd/environment/ray_scene_alpha.h"

namespace TestRaySceneAlpha {

using RendererRD::RayScene;

constexpr float HASH_RANGE = 16777216.0f;

struct Uv {
	float x = 0.0f;
	float y = 0.0f;
};

struct CoverageSample {
	float constant = 0.0f;
	bool left_half = false;

	float sample(const Uv &p_uv) const {
		if (left_half) {
			return Math::fract(p_uv.x) < 0.5f ? 1.0f : 0.0f;
		}
		return constant;
	}
};

float from_bits(uint32_t p_bits) {
	float value;
	memcpy(&value, &p_bits, sizeof(float));
	return value;
}

float half_to_float(uint32_t p_half) {
	const uint32_t sign = (p_half & 0x8000u) << 16;
	const uint32_t exponent = (p_half >> 10) & 0x1Fu;
	const uint32_t mantissa = p_half & 0x3FFu;
	if (exponent == 0) {
		const float magnitude = float(mantissa) * 0x1p-24f;
		return sign != 0 ? -magnitude : magnitude;
	}
	if (exponent == 31) {
		return from_bits(sign | 0x7F800000u | (mantissa << 13));
	}
	return from_bits(sign | ((exponent + 112u) << 23) | (mantissa << 13));
}

uint32_t ray_scene_hash(uint32_t p_value) {
	p_value ^= p_value >> 16u;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15u;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16u;
	return p_value;
}

bool stochastic_passes(float p_coverage, uint32_t p_seed, uint32_t p_triangle, uint32_t p_instance) {
	return p_coverage > float(ray_scene_hash(ray_scene_hash(p_seed ^ (p_triangle * 0x9e3779b9u)) + p_instance) >> 8u) / HASH_RANGE;
}

double stochastic_pass_probability(float p_coverage) {
	return Math::ceil(double(p_coverage) * double(HASH_RANGE)) / double(HASH_RANGE);
}

RayAlphaStore::TableEntry make_entry(uint32_t p_layer, uint32_t p_state, float p_scale_x, float p_scale_y, float p_offset_x, float p_offset_y) {
	RayAlphaStore::TableEntry entry = {};
	entry.layer = p_layer;
	entry.state = p_state;
	entry.scale[0] = p_scale_x;
	entry.scale[1] = p_scale_y;
	entry.offset[0] = p_offset_x;
	entry.offset[1] = p_offset_y;
	return entry;
}

Uv interpolate_uv(const LocalVector<RayScene::TriangleUv> &p_uvs, uint32_t p_triangle, float p_u, float p_v, const RayAlphaStore::TableEntry &p_entry) {
	const RayScene::TriangleUv &record = p_uvs[p_triangle];
	const float w0 = 1.0f - p_u - p_v;
	Uv uv;
	uv.x = half_to_float(record.uv[0] & 0xFFFFu) * w0 + half_to_float(record.uv[1] & 0xFFFFu) * p_u + half_to_float(record.uv[2] & 0xFFFFu) * p_v;
	uv.y = half_to_float(record.uv[0] >> 16) * w0 + half_to_float(record.uv[1] >> 16) * p_u + half_to_float(record.uv[2] >> 16) * p_v;
	uv.x = uv.x * p_entry.scale[0] + p_entry.offset[0];
	uv.y = uv.y * p_entry.scale[1] + p_entry.offset[1];
	return uv;
}

bool alpha_passes(uint32_t p_flags, uint32_t p_triangle, uint32_t p_instance, float p_u, float p_v, uint32_t p_seed, const LocalVector<RayAlphaStore::TableEntry> &p_table, const LocalVector<RayScene::TriangleUv> &p_uvs, const CoverageSample &p_coverage, bool p_threshold) {
	const uint32_t index = p_flags >> RayScene::MATERIAL_ALPHA_SHIFT;
	if (index == 0) {
		return true;
	}
	const RayAlphaStore::TableEntry &entry = p_table[index];
	if (entry.state != RayAlphaStore::LAYER_READY) {
		return true;
	}
	const float coverage = p_coverage.sample(interpolate_uv(p_uvs, p_triangle, p_u, p_v, entry));
	if (p_threshold) {
		return coverage >= 0.5f;
	}
	return stochastic_passes(coverage, p_seed, p_triangle, p_instance);
}

bool moller_trumbore(const Vector3 &p_origin, const Vector3 &p_direction, const Vector3 &p_v0, const Vector3 &p_e1, const Vector3 &p_e2, real_t &r_t, real_t &r_u, real_t &r_v) {
	const Vector3 p = p_direction.cross(p_e2);
	const real_t det = p_e1.dot(p);
	if (Math::abs(det) < real_t(1e-14)) {
		return false;
	}
	const real_t inv_det = real_t(1.0) / det;
	const Vector3 s = p_origin - p_v0;
	r_u = s.dot(p) * inv_det;
	const Vector3 q = s.cross(p_e1);
	r_v = p_direction.dot(q) * inv_det;
	r_t = p_e2.dot(q) * inv_det;
	return r_u >= 0 && r_v >= 0 && r_u + r_v <= 1 && r_t > 0;
}

Vector2 vertex_barycentrics(const Vector3 &p_point, const Vector3 &p_a, const Vector3 &p_b, const Vector3 &p_c) {
	const Vector3 ab = p_b - p_a;
	const Vector3 ac = p_c - p_a;
	const Vector3 ap = p_point - p_a;
	const Vector3 normal = ab.cross(ac);
	const real_t area = normal.dot(normal);
	return Vector2(ap.cross(ac).dot(normal) / area, ab.cross(ap).dot(normal) / area);
}

float fill_texel(const LocalVector<uint8_t> &p_source, uint32_t p_source_width, uint32_t p_source_height, uint32_t p_layer_size, uint32_t p_cutoff, uint32_t p_x, uint32_t p_y) {
	const uint32_t x0 = (p_x * p_source_width) / p_layer_size;
	const uint32_t x1 = ((p_x + 1u) * p_source_width) / p_layer_size;
	const uint32_t y0 = (p_y * p_source_height) / p_layer_size;
	const uint32_t y1 = ((p_y + 1u) * p_source_height) / p_layer_size;
	uint32_t count = 0;
	for (uint32_t y = y0; y < y1; y++) {
		for (uint32_t x = x0; x < x1; x++) {
			const float alpha = float(p_source[y * p_source_width + x]) / 255.0f;
			count += uint32_t(Math::round(alpha * 255.0f)) >= p_cutoff ? 1u : 0u;
		}
	}
	return float(count) / float((x1 - x0) * (y1 - y0));
}

TEST_CASE("[RayScene] Half float known answers") {
	CHECK(RayScene::float_to_half(0.0f) == 0x0000u);
	CHECK(RayScene::float_to_half(-0.0f) == 0x8000u);
	CHECK(RayScene::float_to_half(1.0f) == 0x3C00u);
	CHECK(RayScene::float_to_half(-2.0f) == 0xC000u);
	CHECK(RayScene::float_to_half(0.5f) == 0x3800u);
	CHECK(RayScene::float_to_half(-3.0f) == 0xC200u);
	CHECK(RayScene::float_to_half(65504.0f) == 0x7BFFu);
	CHECK(RayScene::float_to_half(65519.0f) == 0x7BFFu);
	CHECK(RayScene::float_to_half(65520.0f) == 0x7BFFu);
	CHECK(RayScene::float_to_half(1e9f) == 0x7BFFu);
	CHECK(RayScene::float_to_half(-1e9f) == 0xFBFFu);
	CHECK(RayScene::float_to_half(from_bits(0x7F7FFFFFu)) == 0x7BFFu);
	CHECK(RayScene::float_to_half(from_bits(0x7F800000u)) == 0x7C00u);
	CHECK(RayScene::float_to_half(from_bits(0xFF800000u)) == 0xFC00u);
	CHECK(RayScene::float_to_half(from_bits(0x7FC00000u)) == 0x7E00u);
	CHECK(RayScene::float_to_half(from_bits(0x7F800001u)) == 0x7E00u);
	CHECK(RayScene::float_to_half(from_bits(0xFFC00000u)) == 0xFE00u);
	CHECK(RayScene::float_to_half(0x1p-24f) == 0x0001u);
	CHECK(RayScene::float_to_half(-0x1p-24f) == 0x8001u);
	CHECK(RayScene::float_to_half(0x1p-25f) == 0x0000u);
	CHECK(RayScene::float_to_half(-0x1p-25f) == 0x8000u);
	CHECK(RayScene::float_to_half(from_bits(0x33000001u)) == 0x0001u);
	CHECK(RayScene::float_to_half(0x1.8p-24f) == 0x0002u);
	CHECK(RayScene::float_to_half(0x1.4p-23f) == 0x0002u);
	CHECK(RayScene::float_to_half(0x1.ffcp-15f) == 0x0400u);
	CHECK(RayScene::float_to_half(0x1p-14f) == 0x0400u);
	CHECK(RayScene::float_to_half(0x1.002p0f) == 0x3C00u);
	CHECK(RayScene::float_to_half(0x1.006p0f) == 0x3C02u);
	CHECK(RayScene::float_to_half(0x1.002002p0f) == 0x3C01u);
	CHECK(RayScene::float_to_half(0x1.fffp0f) == 0x4000u);
	CHECK(RayScene::pack_half2(1.0f, -2.0f) == 0xC0003C00u);
	CHECK(RayScene::pack_half2(0.5f, 0.0f) == 0x00003800u);
}

TEST_CASE("[RayScene] Every half float survives a round trip through float") {
	uint32_t mismatches = 0;
	uint32_t first_mismatch = 0;
	for (uint32_t half = 0; half < 0x10000u; half++) {
		if (((half >> 10) & 0x1Fu) == 31 && (half & 0x3FFu) != 0) {
			continue;
		}
		if (RayScene::float_to_half(half_to_float(half)) != half) {
			first_mismatch = mismatches == 0 ? half : first_mismatch;
			mismatches++;
		}
	}
	INFO("first mismatch ", first_mismatch);
	CHECK(mismatches == 0);
}

TEST_CASE("[RayScene] Half float UV guard known answers") {
	CHECK_FALSE(RayScene::alpha_uv_unsafe(0.0f, 1.0f, 256));
	CHECK_FALSE(RayScene::alpha_uv_unsafe(1.0f, 1.0f, 256));
	CHECK_FALSE(RayScene::alpha_uv_unsafe(3.0f, 1.0f, 256));
	CHECK_FALSE(RayScene::alpha_uv_unsafe(4.0f, 1.0f, 256));
	CHECK_FALSE(RayScene::alpha_uv_unsafe(7.5f, 1.0f, 256));
	CHECK(RayScene::alpha_uv_unsafe(8.0f, 1.0f, 256));
	CHECK(RayScene::alpha_uv_unsafe(16.0f, 1.0f, 256));
	CHECK(RayScene::alpha_uv_unsafe(65503.0f, 1.0f, 256));
	CHECK(RayScene::alpha_uv_unsafe(65504.0f, 1.0f, 256));
	CHECK_FALSE(RayScene::alpha_uv_unsafe(65503.0f, 0.0f, 256));
	CHECK(RayScene::alpha_uv_unsafe(65504.0f, 0.0f, 256));
	CHECK_FALSE(RayScene::alpha_uv_unsafe(3.5f, 2.0f, 256));
	CHECK(RayScene::alpha_uv_unsafe(4.0f, 2.0f, 256));
	CHECK_FALSE(RayScene::alpha_uv_unsafe(1.5f, 4.0f, 256));
	CHECK(RayScene::alpha_uv_unsafe(2.0f, 4.0f, 256));
	CHECK_FALSE(RayScene::alpha_uv_unsafe(3.5f, 1.0f, 512));
	CHECK(RayScene::alpha_uv_unsafe(4.0f, 1.0f, 512));
	CHECK(RayScene::alpha_uv_unsafe(7.5f, 1.0f, 512));
}

TEST_CASE("[RayScene] Alpha store keys, entries and clamps need no device") {
	RayAlphaStore store;
	const RID grate = RID::from_uint64(0x100000001ull);
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	CHECK_FALSE(store.is_configured());
	CHECK(store.acquire(grate, 128, one, zero) == 0);

	store.configure(2, 256, 1u << 20);
	CHECK(store.is_configured());
	CHECK(store.get_max_layers() == 16);
	CHECK(store.get_layer_size() == 256);
	CHECK(store.get_entry_count() == 1);
	CHECK(store.get_entry(0).state == RayAlphaStore::LAYER_EMPTY);
	CHECK(store.get_layer_texture().is_null());
	CHECK(store.get_table_buffer().is_null());

	CHECK(store.acquire(RID(), 128, one, zero) == 0);
	CHECK(store.acquire(grate, 0, one, zero) == 0);
	CHECK(store.acquire(grate, 256, one, zero) == 0);
	CHECK(store.get_entry_count() == 1);

	const uint32_t first = store.acquire(grate, 128, one, zero);
	CHECK(first == 1);
	CHECK(store.acquire(grate, 128, one, zero) == first);
	CHECK(store.get_entry_count() == 2);
	CHECK(store.get_entry(first).layer == 0);

	const uint32_t scaled = store.acquire(grate, 128, Vector2(2.0f, 2.0f), zero);
	CHECK(scaled == 2);
	CHECK(store.get_entry(scaled).layer == 0);
	CHECK(store.get_entry(scaled).scale[0] == 2.0f);
	const uint32_t shifted = store.acquire(grate, 128, one, Vector2(0.5f, 0.0f));
	CHECK(shifted == 3);
	CHECK(store.get_entry(shifted).layer == 0);
	CHECK(store.get_entry(shifted).offset[0] == 0.5f);
	CHECK(store.acquire(grate, 128, Vector2(2.0f, 2.0f), zero) == scaled);

	const uint32_t harder = store.acquire(grate, 200, one, zero);
	CHECK(harder == 4);
	CHECK(store.get_entry(harder).layer == 1);
	const uint32_t hardest = store.acquire(grate, 255, one, zero);
	CHECK(hardest == 5);
	CHECK(store.get_entry(hardest).layer == 2);
	const uint32_t other = store.acquire(RID::from_uint64(0x100000002ull), 128, one, zero);
	CHECK(other == 6);
	CHECK(store.get_entry(other).layer == 3);

	for (uint32_t layer = 4; layer < 16; layer++) {
		const uint32_t index = store.acquire(RID::from_uint64(1000 + layer), 128, one, zero);
		INFO("layer ", layer);
		CHECK(index != 0);
		CHECK(store.get_entry(index).layer == layer);
	}
	CHECK(store.get_stats().layers_refused == 0);
	CHECK(store.acquire(RID::from_uint64(5000), 128, one, zero) == 0);
	CHECK(store.get_stats().layers_refused == 1);
	const uint32_t full_but_known = store.acquire(grate, 128, Vector2(3.0f, 3.0f), zero);
	CHECK(full_but_known != 0);
	CHECK(store.get_entry(full_but_known).layer == 0);

	for (uint32_t index = 1; index < store.get_entry_count(); index++) {
		INFO("entry ", index);
		CHECK(store.get_entry(index).state == RayAlphaStore::LAYER_EMPTY);
	}

	const uint32_t entries = store.get_entry_count();
	store.configure(16, 256, 1u << 10);
	CHECK(store.get_entry_count() == entries);
	store.configure(2, 1024, 1u << 20);
	CHECK(store.get_layer_size() == 512);
	CHECK(store.get_entry_count() == 1);
	CHECK(store.acquire(grate, 128, one, zero) == 1);
}

LocalVector<uint64_t> live_ids;

bool test_texture_live(RID p_texture) {
	for (const uint64_t id : live_ids) {
		if (id == p_texture.get_id()) {
			return true;
		}
	}
	return false;
}

RID make_live(uint64_t p_id) {
	live_ids.push_back(p_id);
	return RID::from_uint64(p_id);
}

void kill_texture(uint64_t p_id) {
	for (uint32_t i = 0; i < live_ids.size(); i++) {
		if (live_ids[i] == p_id) {
			live_ids.remove_at_unordered(i);
			return;
		}
	}
}

uint32_t live_entry_count(const RayAlphaStore &p_store) {
	uint32_t count = 0;
	for (uint32_t i = 1; i < p_store.get_entry_count(); i++) {
		count += p_store.get_entry(i).layer != RayAlphaStore::ENTRY_RETIRED ? 1 : 0;
	}
	return count;
}

bool holds_layer(const RayAlphaStore &p_store, RID p_source, uint32_t p_cutoff) {
	for (uint32_t i = 0; i < p_store.get_layer_count(); i++) {
		if (p_store.get_layer_source(i) == p_source && p_store.get_layer_cutoff(i) == p_cutoff) {
			return true;
		}
	}
	return false;
}

TEST_CASE("[RayScene] Alpha store keys layers by the whole RID") {
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	const uint64_t slot = 7;
	const uint64_t validator = 0x1234;
	const RID first = RID::from_uint64((validator << 32) | slot);
	const RID second = RID::from_uint64(((validator + (1ull << 23)) << 32) | slot);
	CHECK(first != second);
	CHECK(((first.get_id() << 9) | 128u) == ((second.get_id() << 9) | 128u));

	const uint32_t a = store.acquire(first, 128, one, zero);
	const uint32_t b = store.acquire(second, 128, one, zero);
	REQUIRE(a != 0);
	REQUIRE(b != 0);
	CHECK(store.get_entry(a).layer != store.get_entry(b).layer);
	CHECK(store.get_layer_source(store.get_entry(a).layer) == first);
	CHECK(store.get_layer_source(store.get_entry(b).layer) == second);
	CHECK(store.get_live_layer_count() == 2);
}

TEST_CASE("[RayScene] Alpha store refuses a texture that is already freed") {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	store.begin_update(1);
	const RID dead = RID::from_uint64(0x500000001ull);
	CHECK(store.acquire(dead, 128, Vector2(1.0f, 1.0f), Vector2()) == 0);
	CHECK(store.get_layer_count() == 0);
	CHECK(store.get_entry_count() == 1);
	CHECK(store.get_stats().dead_sources_refused == 1);
	CHECK(store.get_stats().layers_refused == 0);
	store.begin_update(2);
	CHECK(store.get_stats().dead_sources_refused == 0);
}

TEST_CASE("[RayScene] Alpha store releases the layers of freed textures") {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	store.begin_update(1);
	RID textures[4];
	uint32_t indices[4];
	for (uint32_t i = 0; i < 4; i++) {
		textures[i] = make_live(0x100000010ull + i);
		indices[i] = store.acquire(textures[i], 128, one, zero);
		REQUIRE(indices[i] == i + 1);
		REQUIRE(store.get_entry(indices[i]).layer == i);
	}
	CHECK(store.get_live_layer_count() == 4);

	kill_texture(textures[1].get_id());
	kill_texture(textures[3].get_id());
	store.begin_update(2);
	CHECK(store.get_live_layer_count() == 2);
	CHECK(store.get_layer_count() == 4);
	CHECK(store.get_stats().layers_released_total == 2);
	CHECK(store.get_layer_source(1).is_null());
	CHECK(store.get_layer_source(3).is_null());
	CHECK(store.get_layer_generation(1) == 1);
	CHECK(store.get_layer_generation(3) == 1);
	CHECK(store.get_layer_generation(0) == 0);
	for (const uint32_t i : { 1u, 3u }) {
		INFO("texture ", i);
		CHECK(store.get_entry(indices[i]).layer == RayAlphaStore::ENTRY_RETIRED);
		CHECK(store.get_entry(indices[i]).state == RayAlphaStore::LAYER_EMPTY);
	}
	CHECK(store.get_entry(indices[0]).layer == 0);
	CHECK(store.get_entry(indices[2]).layer == 2);
	CHECK(store.get_free_entry_count() == 2);

	const RID fresh = make_live(0x100000020ull);
	const uint32_t index = store.acquire(fresh, 128, one, zero);
	REQUIRE(index != 0);
	CHECK((index == indices[1] || index == indices[3]));
	const uint32_t layer = store.get_entry(index).layer;
	CHECK((layer == 1 || layer == 3));
	CHECK(store.get_layer_source(layer) == fresh);
	CHECK(store.get_layer_generation(layer) == 1);
	CHECK(store.get_layer_state(layer) == RayAlphaStore::LAYER_EMPTY);
	CHECK(store.get_entry(index).state == RayAlphaStore::LAYER_EMPTY);
	CHECK(store.get_entry_generation(index) == store.get_layer_generation(layer));
	CHECK(store.get_layer_count() == 4);
	CHECK(store.get_entry_count() == 5);
	CHECK(store.get_free_entry_count() == 1);
}

TEST_CASE("[RayScene] Alpha store resets the old texture's entry before a new texture reuses it") {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	store.begin_update(1);
	const RID old = make_live(0x100000001ull);
	const uint32_t old_index = store.acquire(old, 128, one, zero);
	REQUIRE(old_index != 0);
	const uint32_t layer = store.get_entry(old_index).layer;
	store.commit_fill(layer, 42);
	store.sync_entries();
	CHECK(store.get_entry(old_index).state == RayAlphaStore::LAYER_READY);

	kill_texture(old.get_id());
	store.begin_update(2);
	CHECK(store.get_entry(old_index).layer == RayAlphaStore::ENTRY_RETIRED);
	const RID next = make_live(0x200000001ull);
	const uint32_t index = store.acquire(next, 128, one, zero);
	REQUIRE(index != 0);
	CHECK(store.get_entry(index).layer == layer);
	CHECK(store.get_layer_source(layer) == next);
	CHECK(store.get_layer_state(layer) == RayAlphaStore::LAYER_EMPTY);
	CHECK(store.get_entry(index).state == RayAlphaStore::LAYER_EMPTY);
	CHECK(store.acquire(next, 128, one, zero) == index);
	store.sync_entries();
	CHECK(store.get_entry(index).state == RayAlphaStore::LAYER_EMPTY);

	store.commit_fill(layer, 43);
	store.sync_entries();
	CHECK(store.get_entry(index).state == RayAlphaStore::LAYER_READY);
	CHECK(index == old_index);
	CHECK(store.get_entry_generation(index) == store.get_layer_generation(layer));
	CHECK(store.get_entry_count() == 2);
}

TEST_CASE("[RayScene] Alpha store evicts the least recently used layer at the cap") {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	store.begin_update(1);
	RID textures[16];
	for (uint32_t i = 0; i < 16; i++) {
		textures[i] = make_live(0x100000000ull | (100 + i));
		REQUIRE(store.acquire(textures[i], 128, one, zero) != 0);
	}
	CHECK(store.get_live_layer_count() == 16);

	store.begin_update(2);
	for (uint32_t i = 0; i < 10; i++) {
		CHECK(store.acquire(textures[i], 128, one, zero) != 0);
	}
	const RID extra = make_live(0x100000000ull | 500);
	CHECK(store.acquire(extra, 128, one, zero) == 0);
	CHECK(store.get_stats().layers_refused == 1);
	CHECK(store.get_stats().layers_evicted_total == 0);

	store.begin_update(3);
	for (uint32_t i = 0; i < 10; i++) {
		CHECK(store.acquire(textures[i], 128, one, zero) != 0);
	}
	const uint32_t got = store.acquire(extra, 128, one, zero);
	REQUIRE(got != 0);
	CHECK(store.get_stats().layers_evicted_total == 1);
	CHECK(store.get_entry(got).layer == 10);
	CHECK(store.get_layer_source(10) == extra);

	store.commit_fill(11, 7);
	store.commit_fill(12, 7);
	const RID extra2 = make_live(0x100000000ull | 501);
	const uint32_t got2 = store.acquire(extra2, 128, one, zero);
	REQUIRE(got2 != 0);
	CHECK(store.get_entry(got2).layer == 11);
	CHECK(store.get_stats().layers_evicted_total == 2);
	for (uint32_t i = 0; i < 10; i++) {
		INFO("texture ", i);
		CHECK(store.get_layer_source(i) == textures[i]);
	}
	CHECK(store.get_layer_source(12) == textures[12]);
	CHECK(store.get_stats().layers_refused == 0);
}

TEST_CASE("[RayScene] Alpha store keeps layers used earlier in the same frame") {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	store.begin_update(1);
	for (uint32_t i = 0; i < 16; i++) {
		REQUIRE(store.acquire(make_live(0x100000000ull | (100 + i)), 128, one, zero) != 0);
	}
	LocalVector<RID> set_a;
	LocalVector<RID> set_b;
	LocalVector<RID> set_c;
	for (uint32_t i = 0; i < 8; i++) {
		set_a.push_back(make_live(0x100000000ull | (200 + i)));
		set_b.push_back(make_live(0x100000000ull | (300 + i)));
		set_c.push_back(make_live(0x100000000ull | (400 + i)));
	}

	store.begin_update(5);
	for (const RID &texture : set_a) {
		CHECK(store.acquire(texture, 128, one, zero) != 0);
	}
	store.begin_update(5);
	for (const RID &texture : set_b) {
		CHECK(store.acquire(texture, 128, one, zero) != 0);
	}
	CHECK(store.get_stats().layers_evicted_total == 16);
	store.begin_update(5);
	for (const RID &texture : set_c) {
		CHECK(store.acquire(texture, 128, one, zero) == 0);
	}
	CHECK(store.get_stats().layers_refused == 8);
	for (uint32_t i = 0; i < 8; i++) {
		INFO("texture ", i);
		CHECK(holds_layer(store, set_a[i], 128));
		CHECK(holds_layer(store, set_b[i], 128));
		CHECK_FALSE(holds_layer(store, set_c[i], 128));
	}
}

TEST_CASE("[RayScene] Alpha store ages out entries no material uses") {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	const Vector2 one(1.0f, 1.0f);
	const RID moving = make_live(0x100000001ull);
	uint32_t refused = 0;
	uint32_t worst_live = 0;
	for (uint64_t f = 1; f <= 1000; f++) {
		store.begin_update(f);
		refused += store.acquire(moving, 128, one, Vector2(float(f) * 0.001f, 0.0f)) == 0 ? 1 : 0;
		worst_live = MAX(worst_live, live_entry_count(store));
	}
	CHECK(refused == 0);
	CHECK(worst_live == 2);
	CHECK(store.get_entry_count() == 3);
	CHECK(store.get_live_layer_count() == 1);
	CHECK(store.get_stats().entries_retired_total == 998);

	const float nan = float(Math::NaN);
	const float inf = float(Math::INF);
	const uint32_t before = store.get_entry_count();
	CHECK(store.acquire(moving, 128, one, Vector2(nan, 0.0f)) == 0);
	CHECK(store.acquire(moving, 128, Vector2(nan, 1.0f), Vector2()) == 0);
	CHECK(store.acquire(moving, 128, one, Vector2(0.0f, inf)) == 0);
	CHECK(store.acquire(moving, 128, Vector2(1.0f, -inf), Vector2()) == 0);
	CHECK(store.get_entry_count() == before);
}

TEST_CASE("[RayScene] Alpha change serial stays still when nothing a ray reads changes") {
	live_ids.clear();
	RayAlphaStore store;
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	const RID grate = make_live(0x100000001ull);
	store.begin_update(1);
	CHECK(store.acquire(grate, 128, one, zero) == 0);
	store.sync_entries();
	store.release();
	CHECK(store.get_change_serial() == 0);

	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	store.begin_update(1);
	CHECK(store.get_change_serial() == 0);
	const uint32_t index = store.acquire(grate, 128, one, zero);
	REQUIRE(index != 0);
	const uint32_t layer = store.get_entry(index).layer;
	store.commit_fill(layer, 42);
	store.sync_entries();
	store.begin_update(2);
	CHECK(store.acquire(grate, 128, one, zero) == index);
	store.sync_entries();
	const uint64_t settled = store.get_change_serial();
	CHECK(settled != 0);

	for (uint64_t f = 3; f <= 10; f++) {
		INFO("frame ", f);
		store.begin_update(f);
		CHECK(store.acquire(grate, 128, one, zero) == index);
		store.commit_fill(layer, 42);
		store.sync_entries();
		CHECK(store.get_change_serial() == settled);
	}
	store.configure(16, 256, 1u << 10);
	CHECK(store.get_change_serial() == settled);
	CHECK(store.get_stats().entries_retired_total == 0);
	CHECK(store.get_layer_state(layer) == RayAlphaStore::LAYER_READY);
}

TEST_CASE("[RayScene] Alpha change serial moves when a layer becomes ready and once more on the next frame") {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	const RID grate = make_live(0x100000001ull);
	store.begin_update(1);
	const uint32_t index = store.acquire(grate, 128, one, zero);
	REQUIRE(index != 0);
	const uint32_t layer = store.get_entry(index).layer;
	store.sync_entries();
	store.begin_update(2);
	CHECK(store.acquire(grate, 128, one, zero) == index);
	store.sync_entries();
	const uint64_t waiting = store.get_change_serial();

	store.commit_fill(layer, 42);
	store.sync_entries();
	CHECK(store.get_entry(index).state == RayAlphaStore::LAYER_READY);
	const uint64_t ready = store.get_change_serial();
	CHECK(ready != waiting);

	store.begin_update(3);
	const uint64_t next_frame = store.get_change_serial();
	CHECK(next_frame != ready);
	CHECK(store.acquire(grate, 128, one, zero) == index);
	store.sync_entries();
	CHECK(store.get_change_serial() == next_frame);

	store.begin_update(4);
	CHECK(store.acquire(grate, 128, one, zero) == index);
	store.sync_entries();
	CHECK(store.get_change_serial() == next_frame);
}

TEST_CASE("[RayScene] Alpha change serial takes the next-frame move only in a later frame") {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	const RID grate = make_live(0x100000001ull);
	store.begin_update(5);
	const uint32_t index = store.acquire(grate, 128, one, zero);
	REQUIRE(index != 0);
	const uint32_t layer = store.get_entry(index).layer;
	store.commit_fill(layer, 42);
	store.sync_entries();
	CHECK(store.get_entry(index).state == RayAlphaStore::LAYER_READY);
	const uint64_t filled = store.get_change_serial();

	store.begin_update(5);
	CHECK(store.acquire(grate, 128, one, zero) == index);
	store.sync_entries();
	CHECK(store.get_change_serial() == filled);

	store.begin_update(6);
	const uint64_t next_frame = store.get_change_serial();
	CHECK(next_frame != filled);
	CHECK(store.acquire(grate, 128, one, zero) == index);
	store.sync_entries();
	store.begin_update(6);
	CHECK(store.get_change_serial() == next_frame);
	store.begin_update(7);
	CHECK(store.acquire(grate, 128, one, zero) == index);
	store.sync_entries();
	CHECK(store.get_change_serial() == next_frame);
}

TEST_CASE("[RayScene] Alpha change serial moves when a ready layer is refilled") {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	const RID grate = make_live(0x100000001ull);
	store.begin_update(1);
	const uint32_t index = store.acquire(grate, 128, one, zero);
	REQUIRE(index != 0);
	const uint32_t layer = store.get_entry(index).layer;
	store.commit_fill(layer, 42);
	store.sync_entries();
	store.begin_update(2);
	CHECK(store.acquire(grate, 128, one, zero) == index);
	store.sync_entries();
	const uint64_t settled = store.get_change_serial();

	store.commit_fill(layer, 43);
	store.sync_entries();
	CHECK(store.get_layer_state(layer) == RayAlphaStore::LAYER_READY);
	CHECK(store.get_entry(index).state == RayAlphaStore::LAYER_READY);
	const uint64_t refilled = store.get_change_serial();
	CHECK(refilled != settled);

	store.begin_update(3);
	CHECK(store.acquire(grate, 128, one, zero) == index);
	store.sync_entries();
	const uint64_t after = store.get_change_serial();
	store.begin_update(4);
	CHECK(store.acquire(grate, 128, one, zero) == index);
	store.commit_fill(layer, 43);
	store.sync_entries();
	CHECK(store.get_change_serial() == after);
}

TEST_CASE("[RayScene] Alpha change serial moves when a layer is evicted, released or dropped") {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	const Vector2 one(1.0f, 1.0f);
	const Vector2 zero(0.0f, 0.0f);
	RID textures[16];
	store.begin_update(1);
	for (uint32_t i = 0; i < 16; i++) {
		textures[i] = make_live(0x100000000ull | (100 + i));
		REQUIRE(store.acquire(textures[i], 128, one, zero) != 0);
		store.commit_fill(i, 7);
	}
	store.sync_entries();
	for (uint64_t f = 2; f <= 3; f++) {
		store.begin_update(f);
		for (uint32_t i = 0; i < 16; i++) {
			if (f == 3 && i == 15) {
				continue;
			}
			REQUIRE(store.acquire(textures[i], 128, one, zero) != 0);
		}
		store.sync_entries();
	}
	uint64_t serial = store.get_change_serial();

	store.begin_update(4);
	CHECK(store.get_stats().entries_retired_total == 1);
	CHECK(store.get_change_serial() != serial);
	for (uint32_t i = 0; i < 15; i++) {
		REQUIRE(store.acquire(textures[i], 128, one, zero) != 0);
	}
	serial = store.get_change_serial();
	const RID extra = make_live(0x100000000ull | 500);
	const uint32_t got = store.acquire(extra, 128, one, zero);
	REQUIRE(got != 0);
	CHECK(store.get_stats().layers_evicted_total == 1);
	CHECK(store.get_entry(got).layer == 15);
	CHECK(store.get_change_serial() != serial);
	store.sync_entries();

	serial = store.get_change_serial();
	kill_texture(textures[0].get_id());
	store.begin_update(5);
	CHECK(store.get_stats().layers_released_total == 1);
	CHECK(store.get_layer_source(0).is_null());
	CHECK(store.get_change_serial() != serial);

	serial = store.get_change_serial();
	store.configure(16, 512, 1u << 20);
	CHECK(store.get_entry_count() == 1);
	CHECK(store.get_change_serial() != serial);

	serial = store.get_change_serial();
	store.release();
	CHECK(store.get_change_serial() != serial);
	serial = store.get_change_serial();
	store.release();
	store.begin_update(6);
	CHECK(store.get_change_serial() == serial);
}

struct StreamMaterial {
	uint32_t texture = 0;
	uint32_t cutoff = 0;
	Vector2 offset;
};

void run_stream_cycles(bool p_kill) {
	live_ids.clear();
	RayAlphaStore store;
	store.configure(16, 256, 1u << 20);
	store.set_liveness(&test_texture_live);
	const Vector2 one(1.0f, 1.0f);
	const uint32_t texture_count = 18;
	const uint32_t first_in_phase[3] = { 0, 6, 10 };
	const uint32_t end_in_phase[3] = { 10, 18, 18 };
	uint64_t ids[texture_count];
	bool dead[texture_count];
	uint64_t next_validator = 1;
	for (uint32_t t = 0; t < texture_count; t++) {
		ids[t] = make_live((next_validator++ << 32) | uint64_t(t + 1)).get_id();
		dead[t] = false;
	}
	LocalVector<StreamMaterial> materials;
	for (uint32_t t = 0; t < 6; t++) {
		for (const uint32_t cutoff : { 96u, 192u }) {
			for (const float offset : { 0.0f, 0.25f }) {
				materials.push_back({ t, cutoff, Vector2(offset, 0.0f) });
			}
		}
	}
	for (uint32_t t = 6; t < texture_count; t++) {
		materials.push_back({ t, 128u, Vector2() });
	}
	LocalVector<uint32_t> fill_generation;
	fill_generation.resize(16);
	for (uint32_t &g : fill_generation) {
		g = 0xFFFFFFFF;
	}
	HashSet<uint64_t> used_before;
	uint32_t wrong_entry = 0;
	uint32_t dead_layers = 0;
	uint32_t stale_entries = 0;
	uint32_t refusals_with_room = 0;
	uint32_t over_cap = 0;
	uint32_t worst_entries = 0;
	uint64_t frame = 1;
	for (uint32_t cycle = 0; cycle < 200; cycle++) {
		for (uint32_t phase = 0; phase < 3; phase++) {
			const uint32_t previous = (phase + 2) % 3;
			for (uint32_t t = 0; t < texture_count; t++) {
				const bool was = t >= first_in_phase[previous] && t < end_in_phase[previous];
				const bool is = t >= first_in_phase[phase] && t < end_in_phase[phase];
				if (was && !is && p_kill && (cycle & 1) && (t & 1) == 0) {
					kill_texture(ids[t]);
					dead[t] = true;
				}
				if (is && dead[t]) {
					ids[t] = make_live((next_validator++ << 32) | uint64_t(t + 1)).get_id();
					dead[t] = false;
				}
			}
			for (uint32_t step = 0; step < 3; step++, frame++) {
				store.begin_update(frame);
				for (uint32_t i = 0; i < store.get_layer_count(); i++) {
					const RID source = store.get_layer_source(i);
					dead_layers += source.is_valid() && !test_texture_live(source) ? 1 : 0;
				}
				HashSet<uint64_t> used_now;
				for (const StreamMaterial &m : materials) {
					if (m.texture < first_in_phase[phase] || m.texture >= end_in_phase[phase]) {
						continue;
					}
					const RID source = RID::from_uint64(ids[m.texture]);
					used_now.insert((uint64_t(m.texture) << 8) | m.cutoff);
					const uint32_t index = store.acquire(source, m.cutoff, one, m.offset);
					if (index == 0) {
						continue;
					}
					const RayAlphaStore::TableEntry &entry = store.get_entry(index);
					if (entry.layer >= store.get_layer_count() || store.get_layer_source(entry.layer) != source || store.get_layer_cutoff(entry.layer) != m.cutoff || store.get_entry_generation(index) != store.get_layer_generation(entry.layer) || entry.scale[0] != 1.0f || entry.offset[0] != m.offset.x) {
						wrong_entry++;
					}
				}
				HashSet<uint64_t> protected_pairs(used_now);
				for (const uint64_t key : used_before) {
					protected_pairs.insert(key);
				}
				if (protected_pairs.size() <= 16 && store.get_stats().layers_refused != 0) {
					refusals_with_room++;
				}
				used_before = used_now;
				for (uint32_t i = 0; i < store.get_layer_count(); i++) {
					if (store.get_layer_source(i).is_valid() && store.get_layer_state(i) != RayAlphaStore::LAYER_READY) {
						store.commit_fill(i, frame);
						fill_generation[i] = store.get_layer_generation(i);
					}
				}
				store.sync_entries();
				for (uint32_t i = 1; i < store.get_entry_count(); i++) {
					const RayAlphaStore::TableEntry &entry = store.get_entry(i);
					if (entry.layer == RayAlphaStore::ENTRY_RETIRED) {
						stale_entries += entry.state != RayAlphaStore::LAYER_EMPTY ? 1 : 0;
						continue;
					}
					if (entry.layer >= store.get_layer_count() || store.get_layer_source(entry.layer).is_null() || store.get_entry_generation(i) != store.get_layer_generation(entry.layer) || (entry.state == RayAlphaStore::LAYER_READY && fill_generation[entry.layer] != store.get_layer_generation(entry.layer))) {
						stale_entries++;
					}
				}
				over_cap += store.get_live_layer_count() > 16 || store.get_layer_count() > 16 ? 1 : 0;
				worst_entries = MAX(worst_entries, store.get_entry_count());
			}
		}
	}
	CHECK(frame == 1801);
	CHECK(wrong_entry == 0);
	CHECK(dead_layers == 0);
	CHECK(stale_entries == 0);
	CHECK(refusals_with_room == 0);
	CHECK(over_cap == 0);
	CHECK(worst_entries <= 37);
	CHECK(store.get_stats().layers_evicted_total > 0);
	if (p_kill) {
		CHECK(store.get_stats().layers_released_total > 0);
	} else {
		CHECK(store.get_stats().layers_released_total == 0);
	}
}

TEST_CASE("[RayScene] Alpha store streams cells out and in 200 times with textures freed") {
	run_stream_cycles(true);
}

TEST_CASE("[RayScene] Alpha store streams cells out and in 200 times with textures kept alive") {
	run_stream_cycles(false);
}

TEST_CASE("[RayScene] Alpha accept rule known answers") {
	LocalVector<RayAlphaStore::TableEntry> table;
	table.push_back(make_entry(0, RayAlphaStore::LAYER_EMPTY, 0.0f, 0.0f, 0.0f, 0.0f));
	table.push_back(make_entry(0, RayAlphaStore::LAYER_READY, 1.0f, 1.0f, 0.0f, 0.0f));
	table.push_back(make_entry(1, RayAlphaStore::LAYER_PENDING, 1.0f, 1.0f, 0.0f, 0.0f));
	table.push_back(make_entry(2, RayAlphaStore::LAYER_READY, 2.0f, 4.0f, 0.5f, -1.0f));
	LocalVector<RayScene::TriangleUv> uvs;
	RayScene::TriangleUv record;
	record.uv[0] = RayScene::pack_half2(0.5f, 0.25f);
	record.uv[1] = RayScene::pack_half2(1.0f, 0.0f);
	record.uv[2] = RayScene::pack_half2(0.0f, 1.0f);
	for (uint32_t triangle = 0; triangle < 16; triangle++) {
		uvs.push_back(record);
	}

	const Uv plain = interpolate_uv(uvs, 0, 0.25f, 0.5f, table[1]);
	CHECK(plain.x == 0.375f);
	CHECK(plain.y == 0.5625f);
	const Uv moved = interpolate_uv(uvs, 0, 0.25f, 0.5f, table[3]);
	CHECK(moved.x == 1.25f);
	CHECK(moved.y == 1.25f);

	CoverageSample none;
	CoverageSample full;
	full.constant = 1.0f;
	CoverageSample half;
	half.constant = 0.5f;
	CoverageSample under_half;
	under_half.constant = 0.49999997f;
	CoverageSample left;
	left.left_half = true;

	const uint32_t ready = 1u << RayScene::MATERIAL_ALPHA_SHIFT;
	const uint32_t pending = 2u << RayScene::MATERIAL_ALPHA_SHIFT;
	const uint32_t moved_ready = 3u << RayScene::MATERIAL_ALPHA_SHIFT;
	for (const bool threshold : { false, true }) {
		CHECK(alpha_passes(RayScene::MATERIAL_FLAG_TWO_SIDED, 0, 0, 0.25f, 0.5f, 7, table, uvs, none, threshold));
		CHECK(alpha_passes(pending | RayScene::MATERIAL_FLAG_TWO_SIDED, 0, 0, 0.25f, 0.5f, 7, table, uvs, none, threshold));
	}
	CHECK(alpha_passes(ready, 0, 0, 0.25f, 0.5f, 7, table, uvs, half, true));
	CHECK_FALSE(alpha_passes(ready, 0, 0, 0.25f, 0.5f, 7, table, uvs, under_half, true));
	CHECK(alpha_passes(ready, 0, 0, 0.25f, 0.5f, 7, table, uvs, left, true));
	CHECK_FALSE(alpha_passes(ready, 0, 0, 0.75f, 0.125f, 7, table, uvs, left, true));
	CHECK(alpha_passes(moved_ready, 0, 0, 0.25f, 0.5f, 7, table, uvs, left, true));

	uint32_t full_misses = 0;
	uint32_t none_hits = 0;
	uint32_t partial_hits = 0;
	constexpr uint32_t seeds = 65536;
	for (uint32_t seed = 0; seed < seeds; seed++) {
		full_misses += alpha_passes(ready, seed & 15u, seed >> 4, 0.25f, 0.5f, seed, table, uvs, full, false) ? 0u : 1u;
		none_hits += alpha_passes(ready, seed & 15u, seed >> 4, 0.25f, 0.5f, seed, table, uvs, none, false) ? 1u : 0u;
		partial_hits += stochastic_passes(0.3f, seed, 7, 3) ? 1u : 0u;
	}
	CHECK(full_misses == 0);
	CHECK(none_hits == 0);
	CHECK(Math::abs(double(partial_hits) / double(seeds) - 0.3) < 0.01);
	CHECK(ray_scene_hash(0) == 0u);
	CHECK(ray_scene_hash(1) == 1753845952u);
	CHECK(ray_scene_hash(0x12345678u) == 0xF5E71C96u);
}

TEST_CASE("[RayScene] Hardware barycentrics swapped give the software hit coordinates") {
	const Vector3 v0(0.3, -0.2, 1.1);
	const Vector3 e1(1.7, 0.4, -0.3);
	const Vector3 e2(-0.2, 1.3, 0.6);
	const Vector2 uv0(0.1, 0.9);
	const Vector2 uv1(0.8, 0.2);
	const Vector2 uv2(0.4, 0.6);
	const Vector2 points[] = { Vector2(0.1, 0.2), Vector2(0.6, 0.3), Vector2(0.05, 0.9), Vector2(0.33, 0.33), Vector2(0.7, 0.1) };
	const Vector3 directions[] = { Vector3(0.1, -0.3, -1.0), Vector3(-0.6, 0.2, -0.7), Vector3(0.0, 0.0, 1.0) };
	for (const Vector2 &point : points) {
		for (const Vector3 &direction_raw : directions) {
			const Vector3 direction = direction_raw.normalized();
			const Vector3 target = v0 + e1 * point.x + e2 * point.y;
			const Vector3 origin = target - direction * 3.0;
			real_t t = 0;
			real_t u = 0;
			real_t v = 0;
			INFO("point ", point.x, " ", point.y, " direction ", direction.x, " ", direction.y, " ", direction.z);
			REQUIRE(moller_trumbore(origin, direction, v0, e1, e2, t, u, v));
			CHECK(Math::abs(u - point.x) < 1e-5);
			CHECK(Math::abs(v - point.y) < 1e-5);
			const Vector3 hit = origin + direction * t;
			const Vector2 hardware = vertex_barycentrics(hit, v0, v0 + e2, v0 + e1);
			CHECK(Math::abs(hardware.y - u) < 1e-5);
			CHECK(Math::abs(hardware.x - v) < 1e-5);
			if (Math::abs(point.x - point.y) > 0.05) {
				CHECK(Math::abs(hardware.x - u) > 1e-3);
			}
			const Vector2 shader_uv = uv0 * (1.0 - hardware.y - hardware.x) + uv1 * hardware.y + uv2 * hardware.x;
			const Vector2 vertex_uv = uv0 * (1.0 - hardware.x - hardware.y) + uv2 * hardware.x + uv1 * hardware.y;
			const Vector2 software_uv = uv0 * (1.0 - u - v) + uv1 * u + uv2 * v;
			CHECK(shader_uv.distance_to(vertex_uv) < 1e-5);
			CHECK(shader_uv.distance_to(software_uv) < 1e-5);
		}
	}
}

TEST_CASE("[RayScene] Alpha fill keeps a grate's open share in stochastic mode") {
	constexpr uint32_t source_size = 1024;
	constexpr uint32_t layer_size = 256;
	constexpr uint32_t period = 10;
	constexpr uint32_t bar = 4;
	constexpr uint32_t cutoff = 128;
	LocalVector<uint8_t> source;
	source.resize(source_size * source_size);
	uint64_t open_texels = 0;
	for (uint32_t y = 0; y < source_size; y++) {
		for (uint32_t x = 0; x < source_size; x++) {
			const uint8_t alpha = (x % period) < bar || (y % period) < bar ? uint8_t(255) : uint8_t(0);
			source[y * source_size + x] = alpha;
			open_texels += uint32_t(Math::round(float(alpha) / 255.0f * 255.0f)) >= cutoff ? 0u : 1u;
		}
	}
	const double mip0_open = double(open_texels) / double(source_size * source_size);
	CHECK(mip0_open > 0.3);
	CHECK(mip0_open < 0.4);

	double stochastic_open = 0.0;
	double stored_open = 0.0;
	uint32_t threshold_open = 0;
	uint32_t sampled_open = 0;
	for (uint32_t y = 0; y < layer_size; y++) {
		for (uint32_t x = 0; x < layer_size; x++) {
			const float coverage = fill_texel(source, source_size, source_size, layer_size, cutoff, x, y);
			const float stored = Math::round(coverage * 255.0f) / 255.0f;
			stochastic_open += 1.0 - stochastic_pass_probability(coverage);
			stored_open += 1.0 - stochastic_pass_probability(stored);
			threshold_open += coverage >= 0.5f ? 0u : 1u;
			sampled_open += stochastic_passes(coverage, y * layer_size + x, 0, 0) ? 0u : 1u;
		}
	}
	const double texels = double(layer_size * layer_size);
	stochastic_open /= texels;
	stored_open /= texels;
	CHECK(Math::abs(stochastic_open - mip0_open) < 1e-6);
	CHECK(Math::abs(stored_open - mip0_open) <= 0.5 / 255.0);
	CHECK(Math::abs(double(sampled_open) / texels - mip0_open) < 0.01);
	CHECK(Math::abs(double(threshold_open) / texels - mip0_open) > 0.1);
}

struct GratePoint {
	String fixture;
	String texture;
	Vector2 corner[3];
	double bary_u = 0.0;
	double bary_v = 0.0;
	float threshold = 0.5f;
	float opacity = 1.0f;
	bool ranges = false;
	Vector2 range = Vector2(0.0f, 1.0f);
	Vector2 scale = Vector2(1.0f, 1.0f);
	Vector2 offset;
	float cast_alpha = 0.0f;
};

Vector2 parse_pair(const String &p_text) {
	const PackedStringArray parts = p_text.strip_edges().trim_prefix("(").trim_suffix(")").split(",");
	return parts.size() == 2 ? Vector2(parts[0].to_float(), parts[1].to_float()) : Vector2();
}

uint32_t grate_cutoff(const GratePoint &p_point) {
	float cutoff = 0.0f;
	if (p_point.ranges) {
		cutoff = (p_point.threshold - float(p_point.range.x)) / float(p_point.range.y - p_point.range.x);
	} else {
		cutoff = p_point.threshold / p_point.opacity;
	}
	cutoff = CLAMP(cutoff, 0.0f, 1.0f);
	return uint32_t(Math::ceil(cutoff * 255.0f));
}

float sample_linear_repeat(const LocalVector<float> &p_layer, uint32_t p_size, const Uv &p_uv) {
	const float x = p_uv.x * float(p_size) - 0.5f;
	const float y = p_uv.y * float(p_size) - 0.5f;
	const float x_floor = Math::floor(x);
	const float y_floor = Math::floor(y);
	const float ax = x - x_floor;
	const float ay = y - y_floor;
	const int64_t size = int64_t(p_size);
	const int64_t x0 = int64_t(x_floor);
	const int64_t y0 = int64_t(y_floor);
	const auto at = [&](int64_t p_x, int64_t p_y) {
		return p_layer[uint32_t(((p_y % size) + size) % size) * p_size + uint32_t(((p_x % size) + size) % size)];
	};
	return at(x0, y0) * (1.0f - ax) * (1.0f - ay) + at(x0 + 1, y0) * ax * (1.0f - ay) + at(x0, y0 + 1) * (1.0f - ax) * ay + at(x0 + 1, y0 + 1) * ax * ay;
}

TEST_CASE("[RayScene] Alpha store keeps Vault111Cryo's grate footprint open: 256 box fill and linear sample, against a 512 nearest cast") {
	const String dir = OS::get_singleton()->get_environment("GARDEN_RAY_SCENE_GRATE_DIR");
	if (dir.is_empty()) {
		MESSAGE("SKIP: GARDEN_RAY_SCENE_GRATE_DIR is not set; the grate known-answer test needs converted game data, so nothing was checked.");
		return;
	}
	const Ref<FileAccess> table = FileAccess::open(dir.path_join("grate_points.tsv"), FileAccess::READ);
	REQUIRE(table.is_valid());
	const PackedStringArray header = table->get_line().split("\t");
	HashMap<String, int> column;
	for (int i = 0; i < header.size(); i++) {
		column[header[i]] = i;
	}
	for (const char *name : { "fixture", "kind", "tex", "thr", "opa", "uv_scale", "uv_offset", "alpha_nn", "t0", "t1", "t2", "bu", "bv", "ranges", "rng" }) {
		REQUIRE(column.has(name));
	}
	LocalVector<GratePoint> points;
	uint32_t other = 0;
	while (!table->eof_reached()) {
		const String line = table->get_line();
		if (line.strip_edges().is_empty()) {
			continue;
		}
		const PackedStringArray f = line.split("\t");
		REQUIRE(f.size() == header.size());
		if (f[column["kind"]] != "grate") {
			other++;
			continue;
		}
		GratePoint point;
		point.fixture = f[column["fixture"]];
		point.texture = f[column["tex"]];
		point.corner[0] = parse_pair(f[column["t0"]]);
		point.corner[1] = parse_pair(f[column["t1"]]);
		point.corner[2] = parse_pair(f[column["t2"]]);
		point.bary_u = f[column["bu"]].to_float();
		point.bary_v = f[column["bv"]].to_float();
		point.threshold = f[column["thr"]].is_empty() || f[column["thr"]] == "None" ? 0.5f : f[column["thr"]].to_float();
		point.opacity = f[column["opa"]].is_empty() || f[column["opa"]] == "None" ? 1.0f : f[column["opa"]].to_float();
		point.ranges = f[column["ranges"]] == "True";
		if (point.ranges) {
			point.range = parse_pair(f[column["rng"]]);
			REQUIRE(Math::abs(point.range.y - point.range.x) > 1e-6);
		}
		point.scale = parse_pair(f[column["uv_scale"]]);
		point.offset = parse_pair(f[column["uv_offset"]]);
		point.cast_alpha = f[column["alpha_nn"]].to_float();
		points.push_back(point);
	}
	REQUIRE(other == 0);
	REQUIRE(points.size() == 5500);

	constexpr uint32_t source_size = 512;
	constexpr uint32_t layer_size = 256;
	HashMap<String, LocalVector<uint8_t>> sources;
	HashMap<String, LocalVector<float>> layers;
	for (const GratePoint &point : points) {
		const uint32_t cutoff = grate_cutoff(point);
		REQUIRE(cutoff > 0);
		const String layer_key = point.texture + "|" + itos(cutoff);
		if (layers.has(layer_key)) {
			continue;
		}
		if (!sources.has(point.texture)) {
			const Ref<FileAccess> plane = FileAccess::open(dir.path_join(point.texture + ".alpha0.512x512.r8"), FileAccess::READ);
			REQUIRE(plane.is_valid());
			REQUIRE(plane->get_length() == source_size * source_size);
			LocalVector<uint8_t> loaded;
			loaded.resize(source_size * source_size);
			plane->get_buffer(loaded.ptr(), loaded.size());
			sources[point.texture] = loaded;
		}
		const LocalVector<uint8_t> &source = sources[point.texture];
		LocalVector<float> layer;
		layer.resize(layer_size * layer_size);
		for (uint32_t y = 0; y < layer_size; y++) {
			for (uint32_t x = 0; x < layer_size; x++) {
				layer[y * layer_size + x] = Math::round(fill_texel(source, source_size, source_size, layer_size, cutoff, x, y) * 255.0f) / 255.0f;
			}
		}
		layers[layer_key] = layer;
	}

	LocalVector<String> order;
	HashMap<String, double> engine_open;
	HashMap<String, double> threshold_open;
	HashMap<String, uint32_t> cast_open;
	HashMap<String, uint32_t> count;
	uint32_t cast_mismatches = 0;
	LocalVector<RayScene::TriangleUv> uvs;
	uvs.resize(1);
	for (const GratePoint &point : points) {
		if (!count.has(point.fixture)) {
			order.push_back(point.fixture);
			engine_open[point.fixture] = 0.0;
			threshold_open[point.fixture] = 0.0;
			cast_open[point.fixture] = 0;
			count[point.fixture] = 0;
		}
		for (uint32_t i = 0; i < 3; i++) {
			uvs[0].uv[i] = RayScene::pack_half2(point.corner[i].x, point.corner[i].y);
		}
		const RayAlphaStore::TableEntry entry = make_entry(0, RayAlphaStore::LAYER_READY, point.scale.x, point.scale.y, point.offset.x, point.offset.y);
		const float coverage = sample_linear_repeat(layers[point.texture + "|" + itos(grate_cutoff(point))], layer_size, interpolate_uv(uvs, 0, float(point.bary_u), float(point.bary_v), entry));
		engine_open[point.fixture] += 1.0 - stochastic_pass_probability(coverage);
		threshold_open[point.fixture] += coverage >= 0.5f ? 0.0 : 1.0;

		const double w0 = 1.0 - point.bary_u - point.bary_v;
		const double u = (w0 * point.corner[0].x + point.bary_u * point.corner[1].x + point.bary_v * point.corner[2].x) * point.scale.x + point.offset.x;
		const double v = (w0 * point.corner[0].y + point.bary_u * point.corner[1].y + point.bary_v * point.corner[2].y) * point.scale.y + point.offset.y;
		const int64_t size = int64_t(source_size);
		const int64_t tx = ((int64_t(Math::floor(u * double(source_size))) % size) + size) % size;
		const int64_t ty = ((int64_t(Math::floor(v * double(source_size))) % size) + size) % size;
		const float nearest = float(sources[point.texture][uint32_t(ty) * source_size + uint32_t(tx)]) / 255.0f;
		cast_mismatches += nearest == point.cast_alpha ? 0u : 1u;
		cast_open[point.fixture] += nearest * point.opacity < point.threshold ? 1u : 0u;
		count[point.fixture]++;
	}
	CHECK(cast_mismatches == 0);
	REQUIRE(order.size() == 22);

	double engine_mean = 0.0;
	double cast_mean = 0.0;
	double threshold_mean = 0.0;
	for (const String &fixture : order) {
		REQUIRE(count[fixture] == 250);
		const double engine = engine_open[fixture] / double(count[fixture]);
		const double threshold = threshold_open[fixture] / double(count[fixture]);
		const double cast = double(cast_open[fixture]) / double(count[fixture]);
		MESSAGE(vformat("%s: engine stochastic open %.4f, threshold mode %.4f, cast %.4f", fixture, engine, threshold, cast));
		engine_mean += engine;
		cast_mean += cast;
		threshold_mean += threshold;
	}
	engine_mean /= double(order.size());
	cast_mean /= double(order.size());
	threshold_mean /= double(order.size());
	MESSAGE(vformat("grate footprint mean over %d fixtures: engine stochastic %.4f, threshold mode %.4f, cast %.4f", int(order.size()), engine_mean, threshold_mean, cast_mean));
	CHECK(Math::abs(cast_mean - 0.3438) < 0.0005);
	CHECK(engine_mean >= 0.308);
	CHECK(engine_mean <= 0.368);
	CHECK(Math::abs(engine_mean - cast_mean) < 0.01);
	CHECK(Math::abs(engine_mean - 0.3469) < 0.001);
}

} // namespace TestRaySceneAlpha

#endif
