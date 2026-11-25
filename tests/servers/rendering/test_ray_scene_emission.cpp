/**************************************************************************/
/*  test_ray_scene_emission.cpp                                           */
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

TEST_FORCE_LINK(test_ray_scene_emission)

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/environment/ray_scene.h"

namespace TestRaySceneEmission {

using RendererRD::RayScene;

TEST_CASE("[RayScene] a zero gi_emission is not an emission value") {
	CHECK_FALSE(RayScene::emission_has_value(Color(0.0f, 0.0f, 0.0f)));
	CHECK_FALSE(RayScene::emission_has_value(Color()));
}

TEST_CASE("[RayScene] a non-zero gi_emission is an emission value") {
	CHECK(RayScene::emission_has_value(Color(0.01f, 0.0f, 0.0f)));
	CHECK(RayScene::emission_has_value(Color(0.0f, 0.02f, 0.0f)));
	CHECK(RayScene::emission_has_value(Color(0.0f, 0.0f, 0.03f)));
	CHECK(RayScene::emission_has_value(Color(17.8f, 33.1f, 17.8f)));
}

} // namespace TestRaySceneEmission

#endif
