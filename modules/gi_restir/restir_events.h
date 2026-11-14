/**************************************************************************/
/*  restir_events.h                                                       */
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
#include "core/typedefs.h"

static constexpr uint32_t RESTIR_EVENT_MAX = 64;
static constexpr uint32_t RESTIR_EVENT_BINS = 256;

enum RestirEventValidate : uint32_t {
	RESTIR_EVENT_VALIDATE_NONE = 0,
	RESTIR_EVENT_VALIDATE_EVENT = 1,
	RESTIR_EVENT_VALIDATE_ALL = 2,
};

struct RestirEventsUBO {
	uint32_t counts[4];
	float params[4];
	uint32_t gates[4];
	float sphere[RESTIR_EVENT_MAX][4];
	uint32_t since_last[RESTIR_EVENT_MAX];
};

static_assert(sizeof(RestirEventsUBO) == 1328, "RestirEventsUBO must match the std140 block in restir_gi_inc.glsl");

struct RestirEventSphere {
	Vector3 center;
	float reach = 0.0f;
	uint32_t since_last = UINT32_MAX;
};

struct RestirEventSettings {
	bool events_on = false;
	uint32_t reshade_budget = 25;
	uint32_t event_m_cap = 0;
	uint32_t jump_m_cap = 0;
	float reach_scale = 2.0f;
	float solid_angle = 0.05f;
};

struct RestirEventInputs {
	RestirEventSettings settings;
	const RestirEventSphere *events = nullptr;
	uint32_t event_count = 0;
	uint32_t events_lost = 0;
	bool directional_jump = false;
	uint32_t quiet_frames = 0;
	uint32_t trace_width = 0;
	uint32_t trace_height = 0;
	bool history = false;
};

struct RestirEventPlan {
	bool active = false;
	bool jump_frame = false;
	uint32_t event_count = 0;
	uint32_t capping = 0;
	uint32_t budget = 0;
	RestirEventValidate validate = RESTIR_EVENT_VALIDATE_NONE;
	RestirEventsUBO ubo = {};
};

uint32_t restir_event_budget(uint32_t p_width, uint32_t p_height, uint32_t p_percent);
bool restir_event_first_fire(uint32_t p_since_last, uint32_t p_quiet_frames);
float restir_event_solid_angle(float p_reach, float p_distance);
bool restir_event_caps(const RestirEventsUBO &p_ubo, const Vector3 &p_position);
bool restir_event_holds(const RestirEventsUBO &p_ubo, const Vector3 &p_sample);
uint32_t restir_event_bin(float p_key);
uint32_t restir_event_cut_bin(const uint32_t *p_bins, uint32_t p_budget);
void restir_plan_events(const RestirEventInputs &p_inputs, RestirEventPlan &r_plan);
