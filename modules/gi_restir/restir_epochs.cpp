/**************************************************************************/
/*  restir_epochs.cpp                                                     */
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

#include "restir_epochs.h"

void restir_plan_epochs(const RestirEpochInputs &p_inputs, RestirEpochPlan &r_plan) {
	r_plan = RestirEpochPlan();
	r_plan.light_soft_frames = p_inputs.light_soft_frames;
	if (!p_inputs.have_history) {
		r_plan.cut = true;
		r_plan.reason = RESTIR_CUT_FIRST;
		return;
	}
	if (p_inputs.have_epochs) {
		r_plan.scene_changed = p_inputs.previous.scene != p_inputs.current.scene;
		r_plan.light_changed = p_inputs.previous.light != p_inputs.current.light;
		r_plan.sky_changed = p_inputs.previous.sky != p_inputs.current.sky;
		r_plan.origin_changed = p_inputs.previous.origin != p_inputs.current.origin;
		r_plan.world_changed = p_inputs.previous.world != p_inputs.current.world;
	}
	if (r_plan.light_changed) {
		r_plan.light_soft_frames = p_inputs.m_light_frames;
	}
	if (p_inputs.history_cut) {
		r_plan.cut = true;
		r_plan.reason = RESTIR_CUT_REQUEST;
		return;
	}
	if (p_inputs.hardware_changed) {
		r_plan.cut = true;
		r_plan.reason = RESTIR_CUT_BACKEND;
		return;
	}
	if (p_inputs.fold_changed) {
		r_plan.cut = true;
		r_plan.reason = RESTIR_CUT_FOLD;
		return;
	}
	if (r_plan.origin_changed) {
		r_plan.cut = true;
		r_plan.reason = RESTIR_CUT_ORIGIN;
		return;
	}
	if (r_plan.world_changed) {
		r_plan.cut = true;
		r_plan.reason = RESTIR_CUT_WORLD;
		return;
	}
	if (!r_plan.scene_changed) {
		return;
	}
	if (!p_inputs.have_feed) {
		r_plan.cut = true;
		r_plan.reason = RESTIR_CUT_SCENE_NO_FEED;
		return;
	}
	if (p_inputs.dirty_result == RendererRD::GIEpochs::DIRTY_NONE) {
		return;
	}
	if (p_inputs.dirty_result != RendererRD::GIEpochs::DIRTY_BOXES || p_inputs.dirty_box_count > RESTIR_DIRTY_MAX) {
		r_plan.cut = true;
		r_plan.reason = RESTIR_CUT_SCENE_OVERFLOW;
		return;
	}
	bool localised = p_inputs.dirty_boxes != nullptr && p_inputs.dirty_box_count > 0;
	for (uint32_t i = 0; localised && i < p_inputs.dirty_box_count; i++) {
		const AABB &box = p_inputs.dirty_boxes[i];
		localised = box.position.is_finite() && box.size.is_finite() && box.size.x >= 0 && box.size.y >= 0 && box.size.z >= 0;
	}
	if (!localised) {
		r_plan.cut = true;
		r_plan.reason = RESTIR_CUT_SCENE_UNLOCALISED;
		return;
	}
	r_plan.dirty_count = p_inputs.dirty_box_count;
	for (uint32_t i = 0; i < p_inputs.dirty_box_count; i++) {
		const AABB &box = p_inputs.dirty_boxes[i];
		r_plan.dirty_min[i] = box.position - p_inputs.anchor;
		r_plan.dirty_max[i] = box.position + box.size - p_inputs.anchor;
	}
}
