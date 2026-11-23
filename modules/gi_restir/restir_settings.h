/**************************************************************************/
/*  restir_settings.h                                                     */
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

#include "core/variant/dictionary.h"

struct RestirFrameParams;

struct RestirReuseValues {
	double plane_tolerance = 0.0;
	double normal_angle = 0.0;
	double roughness_tolerance = 0.0;
	double jacobian_max = 0.0;
	double min_distance = 0.0;
	double min_cosine = 0.0;
	double weight_clamp = 0.0;
	double sky_distance = 0.0;
	double dirty_margin = 0.0;
	double radiance_clamp = 0.0;
	double revalidate_growth = 0.0;
	double revalidate_floor = 0.0;
	double compat_score_scale = 0.0;
	double compat_stop_score = 0.0;
	double sky_epoch_low = 0.0;
	double sky_epoch_high = 0.0;
	double sky_blur_lod = 0.0;
};

void restir_register_settings();
bool restir_gi_enabled();
Dictionary restir_settings_snapshot();
RestirReuseValues restir_reuse_defaults();
RestirReuseValues restir_reuse_read();
void restir_reuse_apply(const RestirReuseValues &p_values, RestirFrameParams &r_params);
bool restir_setting_range(const String &p_name, double &r_min, double &r_max);
String restir_setting_invalid_choice(const String &p_name, const String &p_value);
String restir_settings_invalid_choice();
