/**************************************************************************/
/*  test_ray_scene_class.cpp                                              */
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

TEST_FORCE_LINK(test_ray_scene_class)

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/environment/ray_scene.h"

namespace TestRaySceneClass {

using RendererRD::RayScene;

constexpr RSE::ShadowCastingSetting SETTINGS[] = {
	RSE::SHADOW_CASTING_SETTING_ON,
	RSE::SHADOW_CASTING_SETTING_OFF,
	RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED,
	RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY,
};

constexpr uint32_t FLAG_COMBINATIONS = 64;

constexpr uint32_t ALPHA_INDICES[] = {
	0,
	1u << RayScene::MATERIAL_ALPHA_SHIFT,
	5u << RayScene::MATERIAL_ALPHA_SHIFT,
	RayScene::MATERIAL_ALPHA_MASK,
};

uint32_t skip_mask(bool p_any, uint32_t p_skip) {
	return RayScene::MATERIAL_FLAG_SKIP | (p_any ? RayScene::MATERIAL_FLAG_SHADOW_PASS : RayScene::MATERIAL_FLAG_SHADOW_ONLY) | p_skip;
}

bool always_accepted(uint32_t p_flags) {
	if ((p_flags & RayScene::MATERIAL_ALPHA_MASK) != 0) {
		return false;
	}
	for (const bool any : { false, true }) {
		for (const uint32_t skip : { 0u, uint32_t(RayScene::MATERIAL_FLAG_BLEND) }) {
			if ((p_flags & skip_mask(any, skip)) != 0 || (p_flags & RayScene::MATERIAL_FLAG_BLEND) != 0) {
				return false;
			}
		}
	}
	return true;
}

bool caster_skips(uint32_t p_layer_mask, uint32_t p_caster_mask) {
	return p_caster_mask != RayScene::ALL_CASTERS && (p_layer_mask & p_caster_mask) == 0;
}

TEST_CASE("[RayScene] Instance class known answers") {
	CHECK(RayScene::instance_class(0, 0) == RayScene::INSTANCE_CLASS_OPAQUE);
	CHECK(RayScene::instance_class(0, 0b110) == RayScene::INSTANCE_CLASS_OPAQUE);
	CHECK(RayScene::instance_class(0b010, 0b110) == RayScene::INSTANCE_CLASS_SPLIT);
	CHECK(RayScene::instance_class(0b110, 0b110) == RayScene::INSTANCE_CLASS_SPLIT);
	CHECK(RayScene::instance_class(0b011, 0b110) == RayScene::INSTANCE_CLASS_CANDIDATE);
	CHECK(RayScene::instance_class(0b010, 0) == RayScene::INSTANCE_CLASS_CANDIDATE);
	CHECK(RayScene::instance_class(~uint64_t(0), 0b110) == RayScene::INSTANCE_CLASS_CANDIDATE);
	CHECK(RayScene::instance_class(0b010, ~uint64_t(0)) == RayScene::INSTANCE_CLASS_CANDIDATE);
	CHECK(RayScene::surface_candidates(64, RayScene::MATERIAL_FLAG_BLEND) == ~uint64_t(0));
	CHECK(RayScene::surface_candidates(64, RayScene::MATERIAL_FLAG_TWO_SIDED) == 0);
	CHECK(RayScene::surface_candidates(63, RayScene::MATERIAL_FLAG_SKIP) == uint64_t(1) << 63);
	for (const uint32_t surface : { 0u, 5u, 63u }) {
		CHECK(RayScene::surface_candidates(surface, 1u << RayScene::MATERIAL_ALPHA_SHIFT) == uint64_t(1) << surface);
		CHECK(RayScene::surface_candidates(surface, RayScene::MATERIAL_ALPHA_MASK) == uint64_t(1) << surface);
	}
	CHECK(RayScene::surface_candidates(64, 5u << RayScene::MATERIAL_ALPHA_SHIFT) == ~uint64_t(0));
	const uint32_t pending = (RayScene::MATERIAL_FLAG_TWO_SIDED | (5u << RayScene::MATERIAL_ALPHA_SHIFT)) & ~RayScene::MATERIAL_ALPHA_MASK;
	CHECK(pending == uint32_t(RayScene::MATERIAL_FLAG_TWO_SIDED));
	CHECK(RayScene::surface_candidates(5, pending) == 0);
	CHECK(RayScene::instance_class(RayScene::surface_candidates(5, pending), 0) == RayScene::INSTANCE_CLASS_OPAQUE);
}

TEST_CASE("[RayScene] A surface is opaque to hardware only when every software ray accepts it outright") {
	for (RSE::ShadowCastingSetting setting : SETTINGS) {
		for (const bool occluder : { false, true }) {
			for (const uint32_t alpha : ALPHA_INDICES) {
				for (uint32_t flags = 0; flags < FLAG_COMBINATIONS; flags++) {
					const uint32_t final_flags = flags | alpha | RayScene::shadow_cast_flags(setting, occluder);
					INFO("setting ", int(setting), " occluder ", occluder, " alpha ", alpha, " flags ", flags);
					for (const uint32_t surface : { 0u, 5u, 63u }) {
						CHECK((RayScene::surface_candidates(surface, final_flags) == 0) == always_accepted(final_flags));
					}
				}
			}
		}
	}
}

TEST_CASE("[RayScene] The opaque geometry of a split instance holds only surfaces every ray accepts") {
	const uint32_t choices[] = {
		0,
		RayScene::MATERIAL_FLAG_TWO_SIDED,
		RayScene::MATERIAL_FLAG_SKIP,
		RayScene::MATERIAL_FLAG_BLEND,
		RayScene::shadow_cast_flags(RSE::SHADOW_CASTING_SETTING_OFF),
		RayScene::shadow_cast_flags(RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY),
		5u << RayScene::MATERIAL_ALPHA_SHIFT,
		RayScene::MATERIAL_FLAG_TWO_SIDED | RayScene::MATERIAL_ALPHA_MASK,
	};
	constexpr uint32_t surfaces = 3;
	const uint32_t choice_count = std_size(choices);
	for (uint32_t combo = 0; combo < choice_count * choice_count * choice_count; combo++) {
		uint32_t flags[surfaces];
		uint64_t candidates = 0;
		uint32_t rest = combo;
		for (uint32_t s = 0; s < surfaces; s++) {
			flags[s] = choices[rest % choice_count];
			rest /= choice_count;
			candidates |= RayScene::surface_candidates(s, flags[s]);
		}
		for (uint64_t mesh_split = 0; mesh_split < (uint64_t(1) << surfaces); mesh_split++) {
			const bool split_blas = mesh_split != 0 && mesh_split != (uint64_t(1) << surfaces) - 1;
			const uint32_t instance_class = RayScene::instance_class(candidates, split_blas ? mesh_split : 0);
			INFO("combo ", combo, " split ", mesh_split);
			if (instance_class == RayScene::INSTANCE_CLASS_SPLIT) {
				CHECK(split_blas);
			}
			for (uint32_t s = 0; s < surfaces; s++) {
				const bool forced_opaque = instance_class == RayScene::INSTANCE_CLASS_OPAQUE;
				const bool in_opaque_geometry = instance_class == RayScene::INSTANCE_CLASS_SPLIT && (mesh_split & (uint64_t(1) << s)) == 0;
				if (forced_opaque || in_opaque_geometry) {
					CHECK(always_accepted(flags[s]));
				}
			}
			CHECK((instance_class == RayScene::INSTANCE_CLASS_OPAQUE) == (always_accepted(flags[0]) && always_accepted(flags[1]) && always_accepted(flags[2])));
		}
	}
}

TEST_CASE("[RayScene] Hardware instance masks give the software caster test") {
	const uint32_t groups[] = { 0x1u, 0x6u, 0xF0u, 0x100u };
	const uint32_t group_count = std_size(groups);
	for (uint32_t layer = 0; layer < 512; layer++) {
		const uint32_t mask = RayScene::hw_instance_mask(layer, groups, group_count);
		INFO("layer ", layer);
		CHECK((mask & RayScene::HW_ALL_GROUPS) != 0);
		CHECK((mask & 0xFFu) == mask);
		for (uint32_t g = 0; g < group_count; g++) {
			CHECK(((mask & (1u << g)) != 0) == !caster_skips(layer, groups[g]));
		}
	}
	const uint32_t many[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
	CHECK(RayScene::hw_instance_mask(128, many, std_size(many)) == RayScene::HW_ALL_GROUPS);
	CHECK(RayScene::hw_instance_mask(0xFFu, many, std_size(many)) == 0xFFu);
}

} // namespace TestRaySceneClass

#endif
