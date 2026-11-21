/**************************************************************************/
/*  test_ray_scene_cast.cpp                                               */
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

TEST_FORCE_LINK(test_ray_scene_cast)

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/environment/ray_scene.h"

namespace TestRaySceneCast {

using RendererRD::RayScene;

constexpr RSE::ShadowCastingSetting SETTINGS[] = {
	RSE::SHADOW_CASTING_SETTING_ON,
	RSE::SHADOW_CASTING_SETTING_OFF,
	RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED,
	RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY,
};

bool stops(uint32_t p_flags, bool p_shadow_ray) {
	const uint32_t skip_mask = RayScene::MATERIAL_FLAG_SKIP | (p_shadow_ray ? RayScene::MATERIAL_FLAG_SHADOW_PASS : RayScene::MATERIAL_FLAG_SHADOW_ONLY);
	return (p_flags & skip_mask) == 0;
}

TEST_CASE("[RayScene] Cast setting flag known answers") {
	CHECK(RayScene::shadow_cast_flags(RSE::SHADOW_CASTING_SETTING_ON) == 0);
	CHECK(RayScene::shadow_cast_flags(RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED) == 0);
	CHECK(RayScene::shadow_cast_flags(RSE::SHADOW_CASTING_SETTING_OFF) == RayScene::MATERIAL_FLAG_SHADOW_PASS);
	CHECK(RayScene::shadow_cast_flags(RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY) == RayScene::MATERIAL_FLAG_SHADOW_ONLY);
	CHECK(RayScene::shadow_cast_flags(RSE::SHADOW_CASTING_SETTING_ON, true) == 0);
	CHECK(RayScene::shadow_cast_flags(RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED, true) == 0);
	CHECK(RayScene::shadow_cast_flags(RSE::SHADOW_CASTING_SETTING_OFF, true) == 0);
	CHECK(RayScene::shadow_cast_flags(RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY, true) == RayScene::MATERIAL_FLAG_SHADOW_ONLY);
}

TEST_CASE("[RayScene] Shadow and bounce rays per cast setting") {
	struct Role {
		const char *name;
		RSE::ShadowCastingSetting setting;
		bool occluder;
		bool shadow_stops;
		bool bounce_stops;
	};
	const Role roles[] = {
		{ "caster, and the sun cap as ray scene only", RSE::SHADOW_CASTING_SETTING_ON, false, true, true },
		{ "double sided caster", RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED, false, true, true },
		{ "outside quiet caster, and a culled room's quiet mesh", RSE::SHADOW_CASTING_SETTING_OFF, false, false, true },
		{ "pane patch, cookie on or off", RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY, false, true, false },
		{ "ray ground: cast off with the occluder opt in", RSE::SHADOW_CASTING_SETTING_OFF, true, true, true },
		{ "caster with the occluder opt in", RSE::SHADOW_CASTING_SETTING_ON, true, true, true },
		{ "pane patch with the occluder opt in", RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY, true, true, false },
	};
	for (const Role &role : roles) {
		INFO(role.name);
		const uint32_t flags = RayScene::shadow_cast_flags(role.setting, role.occluder);
		CHECK(stops(flags, true) == role.shadow_stops);
		CHECK(stops(flags, false) == role.bounce_stops);
	}
}

TEST_CASE("[RayScene] A skipped material passes every ray whatever the cast setting") {
	for (RSE::ShadowCastingSetting setting : SETTINGS) {
		for (const bool occluder : { false, true }) {
			INFO("setting ", int(setting), " occluder ", occluder);
			const uint32_t flags = RayScene::shadow_cast_flags(setting, occluder) | RayScene::MATERIAL_FLAG_SKIP;
			CHECK_FALSE(stops(flags, true));
			CHECK_FALSE(stops(flags, false));
		}
	}
}

} // namespace TestRaySceneCast

#endif
