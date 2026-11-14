/**************************************************************************/
/*  restir_harness.h                                                      */
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

#include "restir_gi_core.h"

#include "core/variant/dictionary.h"

namespace RestirHarness {

Dictionary params_to_dict(const RestirFrameParams &p_params);
RestirFrameParams params_from_dict(const Dictionary &p_dict);
Dictionary capture_case(RestirGiCore &p_core, const RestirFrameParams &p_params, const RestirGiInputs &p_inputs, const RestirGiResources &p_resources, uint32_t p_history_index);
Dictionary capture_final(RestirGiCore &p_core, const RestirFrameParams &p_params, const RestirGiInputs &p_inputs, const RestirGiResources &p_resources);
Dictionary run_case(RestirGiCore &p_core, const Dictionary &p_case);
void write_case(const String &p_path, const Dictionary &p_case);
Dictionary read_case(const String &p_path);

} //namespace RestirHarness
