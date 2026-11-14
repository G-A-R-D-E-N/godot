/**************************************************************************/
/*  restir_gi.h                                                           */
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

#include "restir_epochs.h"
#include "restir_gi_core.h"
#include "restir_test.h"

#include "core/templates/hash_map.h"
#include "servers/rendering/renderer_rd/environment/gi_epochs.h"
#include "servers/rendering/renderer_rd/environment/gi_module_hooks.h"

struct RestirGiState {
	RestirGiResources resources;
	Size2i size;
	bool have_history = false;
	bool hardware = false;
	RendererRD::GIEpochs::Snapshot epochs = {};
	bool have_epochs = false;
	uint32_t light_soft_frames = 0;
	uint32_t prev_epoch_tag = 0;
	uint32_t cut_counts[RESTIR_CUT_MAX] = {};
	uint32_t dirty_frames = 0;
	uint32_t frames_since_cut = 0;
	uint32_t frames = 0;
	uint32_t history_uses = 0;
	uint32_t history_cuts = 0;
	uint32_t reval_frames = 0;
	RestirHoldRing hold;
	bool sky_reread = false;
	uint32_t sky_reread_cuts = 0;
	RestirSkyEpoch sky_epoch;
	int64_t sky_epoch_frame = -1;
	RestirRoute route;
};

struct RestirGiSummary {
	bool ran = false;
	bool history_used = false;
	bool own_sampler = false;
	bool rays = false;
	bool hardware = false;
	uint32_t mode = 0;
	uint32_t frame = 0;
	uint32_t spatial_count = 0;
	uint32_t spatial_passes = 0;
	uint32_t m_cap = 0;
	uint32_t m_max = 0;
	bool cut = false;
	uint32_t cut_reason = 0;
	uint32_t dirty_count = 0;
	bool light_cap = false;
	uint32_t light_frames_left = 0;
	bool scene_changed = false;
	bool light_changed = false;
	bool sky_changed = false;
	bool origin_changed = false;
	Size2i size;
	RestirFrameStats stats;
	bool stats_valid = false;
	bool debug_valid = false;
	uint32_t history_cap = 0;
	bool reval_active = false;
	uint32_t reval_frames = 0;
	bool events_on = false;
	bool jump_frame = false;
	uint32_t events_read = 0;
	uint32_t events_lost = 0;
	uint32_t event_capping = 0;
	uint32_t event_validate = 0;
	uint32_t reshade_budget = 0;
	bool hold_on = false;
	uint32_t hold_frames = 0;
	uint32_t hold_fill = 0;
	RestirHoldUpdate hold_update;
	String sky_reread = "off";
	String tiles = "all";
	String confidence = "frames";
	RestirRoute route;
};

class RestirGiBackend : public RendererRD::RtgiResampleBackend {
	RestirGiCore *core = nullptr;
	HashMap<RenderSceneBuffersRD *, RestirGiState> states;
	RestirGiSummary summary;
	uint32_t refusals = 0;
	uint64_t event_frames_total = 0;
	uint64_t events_read_total = 0;
	uint64_t event_validate_frames_total = 0;
	uint64_t event_capping_total = 0;
	uint64_t hold_frames_total = 0;
	uint64_t hold_dropped_total = 0;
	uint32_t hold_fill_max = 0;
	uint64_t tile_skip_frames_total = 0;
	uint64_t module_output_frames_total = 0;
	String refusal_reason;

	void _free_state(RestirGiState &r_state);

public:
	virtual bool is_active() const override;
	virtual bool wants_all_tiles() const override;
	virtual bool wants_replay() const override;
	virtual bool begin_frame(const RendererRD::RtgiFrameInputs &p_inputs) override;
	virtual void resample(const RendererRD::RtgiFrameInputs &p_inputs, RendererRD::RtgiResampleOutput &r_output) override;
	virtual void release(RenderSceneBuffersRD *p_buffers) override;
	virtual void get_status(Dictionary &r_status) const override;
	virtual const char *get_name() const override { return "restir_gi"; }

	RestirGiCore *get_core();
	~RestirGiBackend();
};
