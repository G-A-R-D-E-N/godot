/**************************************************************************/
/*  restir_settings.cpp                                                   */
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

#include "restir_settings.h"

#include "core/config/project_settings.h"

static const char *const RESTIR_PREFIX = "rendering/global_illumination/restir/";

struct RestirSetting {
	Variant::Type type;
	const char *name;
	PropertyHint hint;
	const char *hint_string;
	int64_t int_default;
	const char *text_default;
	bool internal;
	const char *description;
};

static const RestirSetting restir_table[] = {
	{ Variant::BOOL, "gi/enabled", PROPERTY_HINT_NONE, "", 0, "", false,
			"Experimental. Turns on the ReSTIR GI backend of the RTGI pass. Off keeps the plain RTGI path." },
	{ Variant::STRING, "gi/mode", PROPERTY_HINT_ENUM, "B1,B2,B2k,U1", 0, "B2", false,
			"Experimental. ReSTIR GI bias mode: B1 biased reuse, B2 biased reuse with per-reuse visibility (default), B2k as B2 but a reused sample that fails visibility, the dirty test or the history visibility state keeps its count with zero weight (tests), U1 unbiased (tests)." },
	{ Variant::INT, "gi/spatial_neighbors", PROPERTY_HINT_RANGE, "0,8,1", 3, "", false,
			"Experimental. ReSTIR GI spatial neighbours per pass. Has no effect while gi/enabled is off." },
	{ Variant::INT, "gi/spatial_radius", PROPERTY_HINT_RANGE, "1,128,1", 24, "", false,
			"Experimental. ReSTIR GI spatial reuse radius in trace pixels. Has no effect while gi/enabled is off." },
	{ Variant::INT, "gi/spatial_passes", PROPERTY_HINT_RANGE, "1,2,1", 1, "", false,
			"Experimental. ReSTIR GI spatial passes per frame. Has no effect while gi/enabled is off." },
	{ Variant::STRING, "gi/neighbor_selection", PROPERTY_HINT_ENUM, "offsets,compatibility", 0, "offsets", false,
			"Experimental. ReSTIR GI spatial neighbour selection: offsets draws random disk offsets (default), compatibility scores gi/compat_candidates taps by closeness in position and normal and keeps the best gi/spatial_neighbors. Has no effect while gi/enabled is off." },
	{ Variant::INT, "gi/compat_candidates", PROPERTY_HINT_RANGE, "1,64,1", 16, "", false,
			"Experimental. ReSTIR GI taps scored per pixel when gi/neighbor_selection is compatibility." },
	{ Variant::STRING, "gi/compat_gate", PROPERTY_HINT_ENUM, "similar,none,binary", 0, "similar", true,
			"Experimental. ReSTIR GI compatibility gate (internal): similar keeps the plane, normal and class test, none keeps only the class test, binary weights every similar tap equally." },
	{ Variant::BOOL, "gi/compat_early_stop", PROPERTY_HINT_NONE, "", 1, "", true,
			"Experimental. ReSTIR GI stops scoring taps once more than gi/spatial_neighbors taps score above 0.5 (internal)." },
	{ Variant::INT, "gi/m_temporal_cap", PROPERTY_HINT_RANGE, "1,256,1", 20, "", false,
			"Experimental. ReSTIR GI cap on the history count before temporal combine." },
	{ Variant::BOOL, "gi/m_cap_follows_rate", PROPERTY_HINT_NONE, "", 1, "", false,
			"Experimental. ReSTIR GI scales the history cap and the total cap by the share of pixels that receive a candidate each frame (RTGI ray fraction), so the history window stays the same number of frames when candidates are thinned. Off keeps the caps as set." },
	{ Variant::INT, "gi/m_max", PROPERTY_HINT_RANGE, "1,512,1", 32, "", false,
			"Experimental. ReSTIR GI cap on the total sample count after spatial reuse." },
	{ Variant::INT, "gi/m_light", PROPERTY_HINT_RANGE, "1,32,1", 2, "", false,
			"Experimental. ReSTIR GI cap on the sample count while a light change is being absorbed (lag knob, soft response to a light epoch bump)." },
	{ Variant::INT, "gi/m_light_frames", PROPERTY_HINT_RANGE, "0,64,1", 4, "", false,
			"Experimental. ReSTIR GI number of frames the m_light cap applies after a light epoch bump (lag knob). 0 turns the soft response off." },
	{ Variant::INT, "gi/n_target", PROPERTY_HINT_RANGE, "1,64,1", 8, "", false,
			"Experimental. ReSTIR GI sample count at which the output coverage reaches 1." },
	{ Variant::BOOL, "gi/visibility_reuse", PROPERTY_HINT_NONE, "", 0, "", false,
			"Experimental. ReSTIR GI visibility test per reused sample (costs extra rays)." },
	{ Variant::BOOL, "gi/fresh_lo", PROPERTY_HINT_NONE, "", 0, "", false,
			"Experimental. ReSTIR GI re-shades the selected hit each frame to remove stored-radiance lag (costs one shade per pixel)." },
	{ Variant::BOOL, "gi/blocked_keeps_coverage", PROPERTY_HINT_NONE, "", 1, "", false,
			"Experimental. ReSTIR GI keeps the output coverage of a pixel whose selected sample fails the final visibility test, with zero radiance, instead of dropping it from the coverage-weighted resolve. Removes the brightening that dropping those pixels causes." },
	{ Variant::BOOL, "gi/confidence_ramp", PROPERTY_HINT_NONE, "", 0, "", false,
			"Experimental. ReSTIR GI start-up guard: the history caps (the reservoir M cap and, with gi/denoise on, gi/denoise_history_cap) grow from gi/ramp_m_min to their set value over gi/ramp_frames frames since the last history cut, so a dark start cannot fill the long history." },
	{ Variant::INT, "gi/ramp_m_min", PROPERTY_HINT_RANGE, "1,255,1", 4, "", false,
			"Experimental. ReSTIR GI confidence ramp: the cap at a history cut." },
	{ Variant::INT, "gi/ramp_frames", PROPERTY_HINT_RANGE, "1,4096,1", 64, "", false,
			"Experimental. ReSTIR GI confidence ramp: frames since the last history cut over which the cap grows linearly to its set value." },
	{ Variant::STRING, "gi/revalidate", PROPERTY_HINT_ENUM, "off,budget,share", 0, "off", false,
			"Experimental. ReSTIR GI stored-sample re-validation: budget turns every gi/revalidate_period-th frame into a re-validation frame that ignores the incoming candidates and re-traces and re-shades the stored sample of every pixel that had a candidate; share re-traces one in gi/revalidate_share pixels every frame. Where the ray lands on the same point the stored radiance is updated (at most 10 times brighter per update); a large change cuts the reservoir count, a lost sample resets it." },
	{ Variant::INT, "gi/revalidate_period", PROPERTY_HINT_RANGE, "2,16,1", 3, "", false,
			"Experimental. ReSTIR GI re-validation: one frame in this many is a re-validation frame (budget form)." },
	{ Variant::INT, "gi/revalidate_share", PROPERTY_HINT_RANGE, "1,1024,1", 8, "", false,
			"Experimental. ReSTIR GI re-validation: one pixel in this many is re-traced each frame (share form)." },
	{ Variant::INT, "gi/revalidate_threshold", PROPERTY_HINT_RANGE, "1,1000,1", 50, "", false,
			"Experimental. ReSTIR GI re-validation: relative luminance change, in percent, above which the reservoir count is cut." },
	{ Variant::INT, "gi/revalidate_cut_m", PROPERTY_HINT_RANGE, "0,16,1", 1, "", false,
			"Experimental. ReSTIR GI re-validation: the reservoir count after a radiance cut (0 resets the reservoir)." },
	{ Variant::BOOL, "gi/trace_all_tiles", PROPERTY_HINT_NONE, "", 1, "", false,
			"Experimental. ReSTIR GI asks the RTGI pass to trace every tile while it runs. Off lets RTGI skip converged tiles, which the module does not read yet." },
	{ Variant::BOOL, "gi/replay_seed", PROPERTY_HINT_NONE, "", 0, "", false,
			"Experimental. ReSTIR GI keeps each sample's shade seed and re-shades the sample with it. RTGI's trace then picks lights by id, writes the folded pdf, and re-validation re-hits the stored point locally." },
	{ Variant::BOOL, "gi/event_reshade", PROPERTY_HINT_NONE, "", 0, "", false,
			"Experimental. ReSTIR GI light events: re-shades stored samples inside a changed lamp's reach with their stored seed, caps the history count of receivers near a lamp's first fire, and caps every receiver on a sun or moon jump or a frame with lost events. Needs gi/replay_seed, and gi/revalidate must not be budget, or the module refuses the frame. Off keeps the light epoch's gi/m_light cap." },
	{ Variant::INT, "gi/reshade_budget", PROPERTY_HINT_RANGE, "0,100,1", 25, "", false,
			"Experimental. ReSTIR GI light events: at most this percent of trace pixels is re-shaded per frame. Flagged samples past it keep their old radiance that frame." },
	{ Variant::INT, "gi/event_m_cap", PROPERTY_HINT_RANGE, "0,256,1", 0, "", false,
			"Experimental. ReSTIR GI light events: history count limit for receivers near a lamp's first fire on its event frame, in the temporal pass and on their spatial neighbours. 0 restarts them from the fresh candidates." },
	{ Variant::INT, "gi/jump_m_cap", PROPERTY_HINT_RANGE, "0,256,1", 0, "", false,
			"Experimental. ReSTIR GI light events: history count limit for every receiver on a jump frame (sun or moon jump, lost events). 0 restarts from the fresh candidates; above 0 every stored sample is re-shaded first." },
	{ Variant::FLOAT, "gi/event_reach_scale", PROPERTY_HINT_RANGE, "0,64,0.01", 0, "2.0", false,
			"Experimental. ReSTIR GI light events: a receiver nearer a first-fire sphere's centre than this many reaches is capped." },
	{ Variant::FLOAT, "gi/event_solid_angle", PROPERTY_HINT_RANGE, "0,12.566371,0.001", 0, "0.05", false,
			"Experimental. ReSTIR GI light events: a receiver that sees a first-fire sphere under more than this solid angle, in steradians, is capped." },
	{ Variant::STRING, "gi/spatial_hold", PROPERTY_HINT_ENUM, "off,uncapped", 0, "uncapped", false,
			"Experimental. ReSTIR GI light events: uncapped keeps each first-fire sphere for gi/spatial_hold_frames frames, and in the spatial pass a receiver capped by a kept sphere caps every neighbour no kept sphere caps, at gi/event_m_cap. Off caps neighbours on the event frame only. Has no effect while gi/event_reshade is off." },
	{ Variant::INT, "gi/spatial_hold_frames", PROPERTY_HINT_RANGE, "1,256,1", 40, "", false,
			"Experimental. ReSTIR GI light events: frames a first-fire sphere stays in the spatial hold, counted from its event frame." },
	{ Variant::BOOL, "gi/sky_reread", PROPERTY_HINT_NONE, "", 0, "", false,
			"Experimental. ReSTIR GI sky samples take their target weight and radiance from the live sky in the temporal, spatial and final passes instead of a stored radiance, and a sky epoch caps every receiver like a sun jump when the mean sky brightness leaves 0.8 to 1.25 times its value at the epoch's start (the cap needs gi/event_reshade). Needs the DDGI shading inputs; without them it has no effect. A change in whether it applies cuts the history." },
	{ Variant::BOOL, "gi/own_sampler", PROPERTY_HINT_NONE, "", 0, "", false,
			"Experimental. ReSTIR GI uses the module's own initial sampler instead of the RTGI candidates (interim)." },
	{ Variant::BOOL, "gi/deterministic", PROPERTY_HINT_NONE, "", 0, "", true,
			"Experimental. ReSTIR GI fixed seeds and fixed neighbour offsets for replay tests (internal)." },
	{ Variant::STRING, "gi/dump_path", PROPERTY_HINT_NONE, "", 0, "", true,
			"Experimental. ReSTIR GI writes its inputs to this folder for replay tests (internal). Empty is off." },
	{ Variant::STRING, "gi/denoise", PROPERTY_HINT_ENUM, "off,r2", 0, "r2", false,
			"Experimental. ReSTIR GI output route: r2 (default) packs the module radiance into the RTGI candidate stream so the RTGI temporal, variance and a-trous passes denoise it, off leaves the module output unused and RTGI runs on its own candidates. Has no effect while gi/enabled is off." },
	{ Variant::INT, "gi/denoise_history_cap", PROPERTY_HINT_RANGE, "1,255,1", 4, "", false,
			"Experimental. History cap of the RTGI temporal pass while gi/denoise is r2. Has no effect while gi/denoise is off." },
};

static const int RESTIR_SETTING_COUNT = sizeof(restir_table) / sizeof(restir_table[0]);

void restir_register_settings() {
	for (int i = 0; i < RESTIR_SETTING_COUNT; i++) {
		const RestirSetting &setting = restir_table[i];
		Variant default_value;
		switch (setting.type) {
			case Variant::BOOL:
				default_value = setting.int_default != 0;
				break;
			case Variant::INT:
				default_value = setting.int_default;
				break;
			case Variant::FLOAT:
				default_value = String::utf8(setting.text_default).to_float();
				break;
			default:
				default_value = String::utf8(setting.text_default);
				break;
		}
		_GLOBAL_DEF(PropertyInfo(setting.type, String(RESTIR_PREFIX) + setting.name, setting.hint, setting.hint_string), default_value, false, false, false, setting.internal);
	}
}

bool restir_gi_enabled() {
	return GLOBAL_GET_CACHED(bool, "rendering/global_illumination/restir/gi/enabled");
}

Dictionary restir_settings_snapshot() {
	Dictionary snapshot;
	for (int i = 0; i < RESTIR_SETTING_COUNT; i++) {
		const RestirSetting &setting = restir_table[i];
		const String name = String(RESTIR_PREFIX) + setting.name;
		Dictionary item;
		item["value"] = ProjectSettings::get_singleton()->get_setting(name);
		item["description"] = String::utf8(setting.description);
		item["internal"] = setting.internal;
		snapshot[name] = item;
	}
	return snapshot;
}
