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

#include "core/math/vector2.h"
#include "core/math/vector3.h"
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

} // namespace TestRaySceneAlpha

#endif
