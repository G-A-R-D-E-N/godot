/**************************************************************************/
/*  test_rayscene_material_window.cpp                                     */
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

#include "servers/rendering/renderer_rd/environment/ray_scene.h"

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_rayscene_material_window)

TEST_CASE("[RayScene] The material window never leaves the material list") {
	CHECK_EQ(RendererRD::RayScene::material_window(0, 0, 3), 0u);
	CHECK_EQ(RendererRD::RayScene::material_window(0, 0, 0), 0u);
	CHECK_EQ(RendererRD::RayScene::material_window(4, 0, 3), 3u);
	CHECK_EQ(RendererRD::RayScene::material_window(4, 2, 3), 2u);
	CHECK_EQ(RendererRD::RayScene::material_window(6, 0, 2), 2u);
	CHECK_EQ(RendererRD::RayScene::material_window(2, 2, 3), 0u);
	CHECK_EQ(RendererRD::RayScene::material_window(9, 8, 4), 1u);
	CHECK_EQ(RendererRD::RayScene::material_window(9, 0, 0), 0u);
}
