/**************************************************************************/
/*  test_auto_exposure_guard.cpp                                          */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_auto_exposure_guard)

#include "servers/rendering/storage/auto_exposure_guard.h"

namespace TestAutoExposureGuard {

TEST_CASE("[AutoExposureGuard] No limit, a reset or a hold gives no factor") {
	CHECK(AutoExposureGuard::light_factor(0.0f, 1.0 / 60.0, false) == 0.0f);
	CHECK(AutoExposureGuard::light_factor(-1.0f, 1.0 / 60.0, false) == 0.0f);
	CHECK(AutoExposureGuard::light_factor(4.0f, 1.0 / 60.0, true) == 0.0f);
	CHECK(AutoExposureGuard::light_factor(4.0f, 0.0, false) == 0.0f);
	CHECK(AutoExposureGuard::limited(1.0f, 8.0f, 0.0f) == 8.0f);
}

TEST_CASE("[AutoExposureGuard] Four stops per second at 60 Hz lets the meter rise 4/60 of a stop a frame") {
	const float factor = AutoExposureGuard::light_factor(4.0f, 1.0 / 60.0, false);
	CHECK(factor == doctest::Approx(1.0473f).epsilon(0.0001));
	CHECK(AutoExposureGuard::limited(1.0f, 8.0f, factor) == doctest::Approx(1.0473f).epsilon(0.0001));
	float meter = 1.0f;
	for (int i = 0; i < 3; i++) {
		meter = AutoExposureGuard::limited(meter, 8.0f, factor);
	}
	CHECK(meter == doctest::Approx(1.1487f).epsilon(0.0001));
}

TEST_CASE("[AutoExposureGuard] A step inside the limit, a darker target and a first frame pass through") {
	const float factor = AutoExposureGuard::light_factor(4.0f, 1.0 / 60.0, false);
	CHECK(AutoExposureGuard::limited(1.0f, 1.02f, factor) == 1.02f);
	CHECK(AutoExposureGuard::limited(1.0f, 0.25f, factor) == 0.25f);
	CHECK(AutoExposureGuard::limited(0.0f, 8.0f, factor) == 8.0f);
}

TEST_CASE("[AutoExposureGuard] The limit never takes the meter under its lowest value") {
	const float factor = AutoExposureGuard::light_factor(4.0f, 1.0 / 60.0, false);
	CHECK(AutoExposureGuard::limited(0.5f, 2.0f, factor, 2.0f) == 2.0f);
	CHECK(AutoExposureGuard::limited(0.5f, 2.0f, factor, 0.25f) == doctest::Approx(0.52365f).epsilon(0.0001));
	CHECK(AutoExposureGuard::limited(0.5f, 0.3f, factor, 0.25f) == 0.3f);
}

TEST_CASE("[AutoExposureGuard] A hold keeps the stored value, and a meter with none is not held") {
	CHECK(AutoExposureGuard::held(1.0f, 8.0f, true) == 1.0f);
	CHECK(AutoExposureGuard::held(1.0f, 0.25f, true) == 1.0f);
	CHECK(AutoExposureGuard::held(1.0f, 8.0f, false) == 8.0f);
	CHECK(AutoExposureGuard::held(0.0f, 8.0f, true) == 8.0f);
}

TEST_CASE("[AutoExposureGuard] A long frame or a large limit still gives a finite factor") {
	const float factor = AutoExposureGuard::light_factor(64.0f, 10.0, false);
	CHECK(Math::is_finite(factor));
	CHECK(factor > 1.0e29f);
	CHECK(AutoExposureGuard::limited(1.0f, 8.0f, factor) == 8.0f);
}

TEST_CASE("[AutoExposureGuard] The same limit in stops per second at 30 Hz and 144 Hz") {
	for (double rate : { 30.0, 60.0, 144.0 }) {
		float meter = 1.0f;
		const float factor = AutoExposureGuard::light_factor(4.0f, 1.0 / rate, false);
		for (int i = 0; i < int(rate); i++) {
			meter = AutoExposureGuard::limited(meter, 1000.0f, factor);
		}
		CHECK(meter == doctest::Approx(16.0f).epsilon(0.001));
	}
}

} // namespace TestAutoExposureGuard
