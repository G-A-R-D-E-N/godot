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

#include "restir_gi_core.h"

#include "core/config/project_settings.h"
#include "core/math/math_funcs.h"

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
			"Experimental. ReSTIR GI stops scoring taps once more than gi/spatial_neighbors taps score above gi/compat_stop_score (internal)." },
	{ Variant::INT, "gi/m_temporal_cap", PROPERTY_HINT_RANGE, "1,256,1", 20, "", false,
			"Experimental. ReSTIR GI cap on the history count before temporal combine." },
	{ Variant::BOOL, "gi/m_cap_follows_rate", PROPERTY_HINT_NONE, "", 1, "", false,
			"Experimental. ReSTIR GI scales the history cap by the share of pixels that receive a candidate each frame (RTGI ray fraction), rounded up and at least 1, and limits the total cap to that history cap plus 1, so the history window stays the same number of frames when candidates are thinned. Off keeps the caps as set." },
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
			"Experimental. ReSTIR GI stored-sample re-validation: budget turns every gi/revalidate_period-th frame into a re-validation frame that ignores the incoming candidates and re-traces and re-shades the stored sample of every pixel that had a candidate; share re-traces one in gi/revalidate_share pixels every frame. Where the ray lands on the same point the stored radiance is updated (at most gi/revalidate_growth times brighter per update); a large change cuts the reservoir count, a lost sample resets it." },
	{ Variant::INT, "gi/revalidate_period", PROPERTY_HINT_RANGE, "2,16,1", 3, "", false,
			"Experimental. ReSTIR GI re-validation: one frame in this many is a re-validation frame (budget form)." },
	{ Variant::INT, "gi/revalidate_share", PROPERTY_HINT_RANGE, "1,1024,1", 8, "", false,
			"Experimental. ReSTIR GI re-validation: one pixel in this many is re-traced each frame (share form)." },
	{ Variant::INT, "gi/revalidate_threshold", PROPERTY_HINT_RANGE, "1,1000,1", 50, "", false,
			"Experimental. ReSTIR GI re-validation: relative luminance change, in percent, above which the reservoir count is cut." },
	{ Variant::INT, "gi/revalidate_cut_m", PROPERTY_HINT_RANGE, "0,16,1", 1, "", false,
			"Experimental. ReSTIR GI re-validation: the reservoir count after a radiance cut (0 resets the reservoir)." },
	{ Variant::BOOL, "gi/trace_all_tiles", PROPERTY_HINT_NONE, "", 1, "", false,
			"Experimental. ReSTIR GI asks the RTGI pass to trace every tile while it runs (default). Off lets RTGI choose its tiles, and the module then keeps reservoirs only in the tiles RTGI traced: elsewhere it writes empty reservoirs and passes no output, so DDGI, surfels or SSGI show there. Off needs RTGI's tile texture; without it the module runs every tile and status tiles reads unavailable." },
	{ Variant::STRING, "gi/confidence", PROPERTY_HINT_ENUM, "frames,frames_and_m", 0, "frames", false,
			"Experimental. ReSTIR GI confidence source for the RTGI route (gi/denoise r2): frames (default) leaves the packed candidate's pdf word at 0, so coverage follows the RTGI history count alone; frames_and_m writes each packed pixel's reservoir sample count there, with gi/trace_all_tiles on or off, and hands RTGI gi/n_target and its history cap (gi/denoise_history_cap, or the ramped cap under gi/confidence_ramp): RTGI then keeps an average of min(count / gi/n_target, 1) per pixel, covers by the smaller of that and its history count over the cap, and skips its own restarts (the soft cut and the restart spheres), with gi/event_reshade on or off." },
	{ Variant::BOOL, "gi/replay_seed", PROPERTY_HINT_NONE, "", 0, "", false,
			"Experimental. ReSTIR GI keeps each sample's shade seed and re-shades the sample with it. RTGI's trace then picks lights by id, writes the folded pdf, and re-validation re-hits the stored point locally." },
	{ Variant::BOOL, "gi/event_reshade", PROPERTY_HINT_NONE, "", 0, "", false,
			"Experimental. ReSTIR GI light events: re-shades stored samples inside a changed lamp's reach with their stored seed, caps the history count of receivers near a lamp's first fire, and caps every receiver on a sun or moon jump or a frame with lost events. Needs gi/replay_seed, and gi/revalidate must not be budget, or the module refuses the frame. With gi/denoise r2 the RTGI temporal pass then skips its own restarts (soft cut and restart spheres), so an event restarts once. Off keeps the light epoch's gi/m_light cap." },
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
			"Experimental. ReSTIR GI sky samples take their target weight and radiance from the live sky in the temporal, spatial and final passes instead of a stored radiance, and a sky epoch caps every receiver like a sun jump when the mean sky brightness leaves gi/sky_epoch_low to gi/sky_epoch_high times its value at the epoch's start (the cap needs gi/event_reshade). Needs the DDGI shading inputs; without them it has no effect. A change in whether it applies cuts the history." },
	{ Variant::INT, "gi/fold_k", PROPERTY_HINT_RANGE, "0,64,1", 0, "", false,
			"Experimental. ReSTIR GI stored-age bound (Fold): 0 (default) is off; 1 to 3 read as 4, and other values round down to a power of two, 4 to 64. When on, no reservoir in the temporal history carries a candidate older than this many view frames, and each pixel restarts its history on a staggered frame. Applies in B1, B2 and B2k with spatial feedback off; elsewhere it has no effect. A change of the effective bound, or of the mode or spatial feedback while it is set, cuts the history." },
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
	{ Variant::FLOAT, "gi/plane_tolerance", PROPERTY_HINT_RANGE, "0.001,0.5,0.001", 0, "0.02", false,
			"Experimental. ReSTIR GI reuse limit: a neighbour or history sample is refused when its receiver lies off this receiver's plane by this share of the receiver depth or more. Lower keeps reuse on the same surface, higher reuses across steps and gives less noise." },
	{ Variant::FLOAT, "gi/normal_angle", PROPERTY_HINT_RANGE, "1,90,0.1", 0, "25", false,
			"Experimental. ReSTIR GI reuse limit: a neighbour or history sample is refused when the two receiver normals differ by this many degrees or more." },
	{ Variant::FLOAT, "gi/roughness_tolerance", PROPERTY_HINT_RANGE, "0,1,0.01", 0, "0.35", false,
			"Experimental. ReSTIR GI reuse limit: a neighbour or history sample is refused when the two receivers' roughness differs by more than this." },
	{ Variant::FLOAT, "gi/jacobian_max", PROPERTY_HINT_RANGE, "1,64,0.1", 0, "8", false,
			"Experimental. ReSTIR GI reuse limit: a reused sample is refused when moving it to this receiver changes its solid angle by more than this factor, either way." },
	{ Variant::FLOAT, "gi/min_distance", PROPERTY_HINT_RANGE, "0,1,0.001", 0, "0.03", false,
			"Experimental. ReSTIR GI reuse limit: a reused sample nearer than this distance to the receiver it came from or the receiver that takes it is refused." },
	{ Variant::FLOAT, "gi/min_cosine", PROPERTY_HINT_RANGE, "0,1,0.001", 0, "0.05", false,
			"Experimental. ReSTIR GI reuse limit: a reused sample is refused when the cosine at the sample point towards either receiver is below this (grazing reuse)." },
	{ Variant::FLOAT, "gi/weight_clamp", PROPERTY_HINT_RANGE, "1,1000,0.1", 0, "20", false,
			"Experimental. ReSTIR GI firefly limit: in the final pass of the biased modes a reservoir weight is held to this many times the median of the positive weights in its 3 by 3 block, where at least 3 pixels of the block hold one. Lower removes more bright specks and loses more energy. U1 does not use it." },
	{ Variant::FLOAT, "gi/sky_distance", PROPERTY_HINT_RANGE, "10,100000,1", 0, "1000", false,
			"Experimental. ReSTIR GI distance given to a sky sample, in world units: the length of a visibility ray towards the sky, of the segment a stored sky sample is tested with against changed-scene boxes, of the own sampler's trace, and the shading distance of a hit without DDGI." },
	{ Variant::FLOAT, "gi/dirty_margin", PROPERTY_HINT_RANGE, "0,1,0.001", 0, "0.01", false,
			"Experimental. ReSTIR GI padding added to each changed-scene box when stored samples that cross it are thrown away." },
	{ Variant::FLOAT, "gi/radiance_clamp", PROPERTY_HINT_RANGE, "1,65504,1", 0, "60000", false,
			"Experimental. ReSTIR GI tuning value: the most radiance a stored sample, a sky read or a shaded hit may carry per channel. Stored radiance is half precision, so the top of the range is the largest half." },
	{ Variant::FLOAT, "gi/revalidate_growth", PROPERTY_HINT_RANGE, "1,1000,0.1", 0, "10", false,
			"Experimental. ReSTIR GI re-validation tuning value: the stored radiance may grow by at most this factor of max(its luminance, gi/revalidate_floor) per update." },
	{ Variant::FLOAT, "gi/revalidate_floor", PROPERTY_HINT_RANGE, "0.001,10,0.001", 0, "0.1", false,
			"Experimental. ReSTIR GI re-validation tuning value: the luminance floor of the growth limit and of the change test, so a dark sample does not read a small absolute change as a large relative one." },
	{ Variant::FLOAT, "gi/compat_score_scale", PROPERTY_HINT_RANGE, "0.001,10,0.00001", 0, "0.12615662", false,
			"Experimental. ReSTIR GI compatibility selection tuning value: the position falloff length of a tap's score, as a share of the receiver's view distance. Lower scores distant taps lower. Used when gi/neighbor_selection is compatibility." },
	{ Variant::FLOAT, "gi/compat_stop_score", PROPERTY_HINT_RANGE, "0,1,0.01", 0, "0.5", true,
			"Experimental. ReSTIR GI compatibility selection tuning value (internal): a tap counts towards the early stop of gi/compat_early_stop when its score is above this." },
	{ Variant::FLOAT, "gi/sky_epoch_low", PROPERTY_HINT_RANGE, "0.01,1,0.01", 0, "0.8", false,
			"Experimental. ReSTIR GI sky epoch tuning value: a sky epoch starts when the mean sky brightness falls below this share of its value at the epoch's start. Needs gi/sky_reread." },
	{ Variant::FLOAT, "gi/sky_epoch_high", PROPERTY_HINT_RANGE, "1,100,0.01", 0, "1.25", false,
			"Experimental. ReSTIR GI sky epoch tuning value: a sky epoch starts when the mean sky brightness rises above this multiple of its value at the epoch's start. Needs gi/sky_reread." },
	{ Variant::FLOAT, "gi/sky_blur_lod", PROPERTY_HINT_RANGE, "0,8,0.1", 0, "2", false,
			"Experimental. ReSTIR GI sky tuning value: the mip level of the radiance sky texture that a sky sample reads. Higher reads a blurrier sky." },
};

static const int RESTIR_SETTING_COUNT = sizeof(restir_table) / sizeof(restir_table[0]);

static Variant _default_value(const RestirSetting &p_setting) {
	switch (p_setting.type) {
		case Variant::BOOL:
			return p_setting.int_default != 0;
		case Variant::INT:
			return p_setting.int_default;
		case Variant::FLOAT:
			return String::utf8(p_setting.text_default).to_float();
		default:
			return String::utf8(p_setting.text_default);
	}
}

static const RestirSetting *_find_setting(const String &p_name) {
	for (int i = 0; i < RESTIR_SETTING_COUNT; i++) {
		if (p_name == restir_table[i].name) {
			return &restir_table[i];
		}
	}
	return nullptr;
}

void restir_register_settings() {
	for (int i = 0; i < RESTIR_SETTING_COUNT; i++) {
		const RestirSetting &setting = restir_table[i];
		_GLOBAL_DEF(PropertyInfo(setting.type, String(RESTIR_PREFIX) + setting.name, setting.hint, setting.hint_string), _default_value(setting), false, false, false, setting.internal);
	}
}

template <typename T>
static RestirReuseValues _reuse_values(T p_get) {
	RestirReuseValues values;
	values.plane_tolerance = p_get("gi/plane_tolerance");
	values.normal_angle = p_get("gi/normal_angle");
	values.roughness_tolerance = p_get("gi/roughness_tolerance");
	values.jacobian_max = p_get("gi/jacobian_max");
	values.min_distance = p_get("gi/min_distance");
	values.min_cosine = p_get("gi/min_cosine");
	values.weight_clamp = p_get("gi/weight_clamp");
	values.sky_distance = p_get("gi/sky_distance");
	values.dirty_margin = p_get("gi/dirty_margin");
	values.radiance_clamp = p_get("gi/radiance_clamp");
	values.revalidate_growth = p_get("gi/revalidate_growth");
	values.revalidate_floor = p_get("gi/revalidate_floor");
	values.compat_score_scale = p_get("gi/compat_score_scale");
	values.compat_stop_score = p_get("gi/compat_stop_score");
	values.sky_epoch_low = p_get("gi/sky_epoch_low");
	values.sky_epoch_high = p_get("gi/sky_epoch_high");
	values.sky_blur_lod = p_get("gi/sky_blur_lod");
	return values;
}

RestirReuseValues restir_reuse_defaults() {
	return _reuse_values([](const char *p_name) -> Variant {
		const RestirSetting *setting = _find_setting(p_name);
		ERR_FAIL_NULL_V(setting, Variant());
		return _default_value(*setting);
	});
}

RestirReuseValues restir_reuse_read() {
	return _reuse_values([](const char *p_name) -> Variant {
		return GLOBAL_GET(String(RESTIR_PREFIX) + p_name);
	});
}

void restir_reuse_apply(const RestirReuseValues &p_values, RestirFrameParams &r_params) {
	r_params.tol_plane = float(CLAMP(p_values.plane_tolerance, 0.001, 0.5));
	r_params.cos_normal = float(Math::cos(Math::deg_to_rad(CLAMP(p_values.normal_angle, 1.0, 90.0))));
	r_params.class_tol = float(CLAMP(p_values.roughness_tolerance, 0.0, 1.0));
	r_params.jac_max = float(CLAMP(p_values.jacobian_max, 1.0, 64.0));
	r_params.d_min = float(CLAMP(p_values.min_distance, 0.0, 1.0));
	r_params.cos_min = float(CLAMP(p_values.min_cosine, 0.0, 1.0));
	r_params.w_clamp = float(CLAMP(p_values.weight_clamp, 1.0, 1000.0));
	r_params.sky_distance = float(CLAMP(p_values.sky_distance, 10.0, 100000.0));
	r_params.dirty_margin = float(CLAMP(p_values.dirty_margin, 0.0, 1.0));
	r_params.radiance_clamp = float(CLAMP(p_values.radiance_clamp, 1.0, 65504.0));
	r_params.reval_growth = float(CLAMP(p_values.revalidate_growth, 1.0, 1000.0));
	r_params.reval_floor = float(CLAMP(p_values.revalidate_floor, 0.001, 10.0));
	r_params.compat_scale = float(CLAMP(p_values.compat_score_scale, 0.001, 10.0));
	r_params.compat_stop_score = float(CLAMP(p_values.compat_stop_score, 0.0, 1.0));
	r_params.sky_epoch_low = float(CLAMP(p_values.sky_epoch_low, 0.01, 1.0));
	r_params.sky_epoch_high = float(CLAMP(p_values.sky_epoch_high, 1.0, 100.0));
	r_params.sky_blur_lod = float(CLAMP(p_values.sky_blur_lod, 0.0, 8.0));
}

bool restir_setting_range(const String &p_name, double &r_min, double &r_max) {
	const RestirSetting *setting = _find_setting(p_name);
	if (!setting || setting->hint != PROPERTY_HINT_RANGE) {
		return false;
	}
	const Vector<String> parts = String::utf8(setting->hint_string).split(",");
	if (parts.size() < 2) {
		return false;
	}
	r_min = parts[0].to_float();
	r_max = parts[1].to_float();
	return true;
}

String restir_setting_invalid_choice(const String &p_name, const String &p_value) {
	const RestirSetting *setting = _find_setting(p_name);
	if (!setting || setting->hint != PROPERTY_HINT_ENUM) {
		return String();
	}
	if (String::utf8(setting->hint_string).split(",").has(p_value)) {
		return String();
	}
	return String(RESTIR_PREFIX) + setting->name + "=" + p_value + " (not one of " + setting->hint_string + ")";
}

String restir_settings_invalid_choice() {
	for (int i = 0; i < RESTIR_SETTING_COUNT; i++) {
		const RestirSetting &setting = restir_table[i];
		if (setting.hint != PROPERTY_HINT_ENUM) {
			continue;
		}
		const String bad = restir_setting_invalid_choice(setting.name, GLOBAL_GET(String(RESTIR_PREFIX) + setting.name));
		if (!bad.is_empty()) {
			return bad;
		}
	}
	return String();
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
