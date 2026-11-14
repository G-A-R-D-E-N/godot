/**************************************************************************/
/*  restir_probe.h                                                        */
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

#include "core/object/object.h"
#include "restir_gi_core.h"

#include "core/os/mutex.h"
#include "core/variant/dictionary.h"
#include "core/variant/variant.h"

class RestirProbe : public Object {
	GDCLASS(RestirProbe, Object);

protected:
	static void _bind_methods();

public:
	Dictionary get_status() const;
	void configure(const Dictionary &p_options);
	Dictionary get_records() const;
	void reset_records();
	Dictionary plan_epochs(const Dictionary &p_case) const;
	Dictionary get_pixel_sums() const;
	void reset_pixel_sums();
	void request_capture();
	void request_final_capture();
	Dictionary get_final_capture() const;
	Dictionary get_capture() const;
	void run_case(const Dictionary &p_case);
	bool case_ready() const;
	Dictionary get_case_result() const;
	Dictionary read_case_file(const String &p_path) const;
	void write_case_file(const String &p_path, const Dictionary &p_case) const;
	PackedInt64Array get_fold_rows() const;
	PackedStringArray get_fold_row_fields() const;
	Dictionary get_fold_seeds() const;
	~RestirProbe();

private:
	void _run_case_render(const Dictionary &p_case);

	RestirGiCore *harness_core = nullptr;
	mutable Mutex result_mutex;
	Dictionary case_result;
	bool case_done = false;
};
