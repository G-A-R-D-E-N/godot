/**************************************************************************/
/*  restir_epochs.h                                                       */
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

#include "core/math/vector3.h"
#include "servers/rendering/renderer_rd/environment/gi_epochs.h"

static constexpr uint32_t RESTIR_DIRTY_MAX = 64;

enum RestirCutReason : uint32_t {
	RESTIR_CUT_NONE = 0,
	RESTIR_CUT_FIRST = 1,
	RESTIR_CUT_REQUEST = 2,
	RESTIR_CUT_ORIGIN = 3,
	RESTIR_CUT_BACKEND = 4,
	RESTIR_CUT_SCENE_NO_FEED = 5,
	RESTIR_CUT_SCENE_OVERFLOW = 6,
	RESTIR_CUT_SCENE_UNLOCALISED = 7,
	RESTIR_CUT_FOLD = 8,
	RESTIR_CUT_MAX = 9,
};

struct RestirEpochInputs {
	bool have_history = false;
	bool have_epochs = false;
	bool history_cut = false;
	bool hardware_changed = false;
	bool fold_changed = false;
	RendererRD::GIEpochs::Snapshot previous = {};
	RendererRD::GIEpochs::Snapshot current = {};
	Vector3 anchor;
	uint32_t light_soft_frames = 0;
	uint32_t m_light_frames = 4;
};

struct RestirEpochPlan {
	bool cut = false;
	RestirCutReason reason = RESTIR_CUT_NONE;
	bool scene_changed = false;
	bool light_changed = false;
	bool sky_changed = false;
	bool origin_changed = false;
	uint32_t dirty_count = 0;
	Vector3 dirty_min[RESTIR_DIRTY_MAX];
	Vector3 dirty_max[RESTIR_DIRTY_MAX];
	uint32_t light_soft_frames = 0;
};

void restir_plan_epochs(const RestirEpochInputs &p_inputs, RestirEpochPlan &r_plan);
