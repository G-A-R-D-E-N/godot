/**************************************************************************/
/*  rtgi_restarts.h                                                       */
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

#include "core/math/transform_3d.h"
#include "core/math/vector3.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "servers/rendering/renderer_rd/environment/ddgi_light_tracker.h"

namespace RendererRD {

class RtgiRestarts {
public:
	static constexpr uint32_t MAX_SPHERES = 16;
	static constexpr float INNER_BOX_FRACTION = 0.875f;
	static constexpr float ENERGY_SHARE = 0.2f;

	struct Selection {
		float spheres[MAX_SPHERES][4] = {};
		uint32_t count = 0;
		uint32_t dropped = 0;
		uint32_t repeats = 0;
		uint32_t crossings = 0;
		uint32_t behind = 0;
	};

	struct DirectionalSample {
		RID light;
		Vector3 direction;
		float energy = 0.0f;
	};

	struct DirectionalRecord {
		RID light;
		Vector3 direction;
		float energy = 0.0f;
		uint64_t last_jump = UINT64_MAX;
	};

	struct DirectionalRead {
		uint32_t jumps = 0;
		uint32_t repeats = 0;
	};

	static void select(const LocalVector<DDGILightTracker::Event> &p_events, const Transform3D &p_world_to_view, const Vector3 &p_camera_origin, const Vector3 &p_region_half, uint32_t p_quiet_frames, Selection &r_selection);
	static DirectionalRead read_directionals(const LocalVector<DirectionalSample> &p_samples, LocalVector<DirectionalRecord> &r_records, uint64_t p_frame, float p_limit_degrees, float p_floor, uint32_t p_quiet_frames);
};

} // namespace RendererRD
