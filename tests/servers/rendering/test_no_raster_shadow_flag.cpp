/**************************************************************************/
/*  test_no_raster_shadow_flag.cpp                                        */
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

TEST_FORCE_LINK(test_no_raster_shadow_flag)

#include "servers/rendering/renderer_scene_cull.h"

namespace TestNoRasterShadowFlag {

using Data = RendererSceneCull::InstanceData;

constexpr uint32_t MESH = uint32_t(RSE::INSTANCE_MESH);

TEST_CASE("[SceneCull] Both flags with casting on: ray scene only, no raster shadow") {
	const uint32_t flags = MESH | Data::FLAG_CAST_SHADOWS | Data::FLAG_RAY_SCENE_ONLY | Data::FLAG_NO_RASTER_SHADOW;
	CHECK(Data::enters_ray_scene(flags));
	CHECK_FALSE(Data::draws_in_camera(flags));
	INFO("directional cascade caster cull");
	CHECK_FALSE(Data::casts_raster_shadow(flags));
	INFO("omni, spot and area caster culls, and the lamp dirty marking on re-box, pairing and changes");
	CHECK_FALSE(Data::casts_raster_shadow(true, true));
}

TEST_CASE("[SceneCull] Ray scene only alone still casts raster shadows") {
	const uint32_t flags = MESH | Data::FLAG_CAST_SHADOWS | Data::FLAG_RAY_SCENE_ONLY;
	CHECK(Data::enters_ray_scene(flags));
	CHECK_FALSE(Data::draws_in_camera(flags));
	CHECK(Data::casts_raster_shadow(flags));
	CHECK(Data::casts_raster_shadow(true, false));
}

TEST_CASE("[SceneCull] No raster shadow alone keeps camera drawing") {
	const uint32_t flags = MESH | Data::FLAG_CAST_SHADOWS | Data::FLAG_NO_RASTER_SHADOW;
	CHECK(Data::enters_ray_scene(flags));
	CHECK(Data::draws_in_camera(flags));
	CHECK_FALSE(Data::casts_raster_shadow(flags));
}

TEST_CASE("[SceneCull] Both caster rules agree and read only their two flags") {
	for (bool casts : { false, true }) {
		for (bool no_raster : { false, true }) {
			const uint32_t flags = (casts ? uint32_t(Data::FLAG_CAST_SHADOWS) : 0u) | (no_raster ? uint32_t(Data::FLAG_NO_RASTER_SHADOW) : 0u);
			const bool expected = casts && !no_raster;
			CHECK(Data::casts_raster_shadow(casts, no_raster) == expected);
			for (uint32_t bit = 0; bit < 32; bit++) {
				const uint32_t other = 1u << bit;
				if (other == Data::FLAG_CAST_SHADOWS || other == Data::FLAG_NO_RASTER_SHADOW) {
					continue;
				}
				INFO("bit ", bit);
				CHECK(Data::casts_raster_shadow(flags | other) == expected);
			}
		}
	}
}

TEST_CASE("[SceneCull] Only the ray scene only flag leaves the camera cull and neither flag leaves the ray scene") {
	for (uint32_t bit = 0; bit < 32; bit++) {
		const uint32_t other = 1u << bit;
		if (other & Data::FLAG_BASE_TYPE_MASK) {
			continue;
		}
		INFO("bit ", bit);
		CHECK(Data::draws_in_camera(MESH | other) == (other != Data::FLAG_RAY_SCENE_ONLY));
		CHECK(Data::enters_ray_scene(MESH | other));
	}
	CHECK_FALSE(Data::enters_ray_scene(uint32_t(RSE::INSTANCE_MULTIMESH) | Data::FLAG_RAY_SCENE_ONLY));
}

} // namespace TestNoRasterShadowFlag
