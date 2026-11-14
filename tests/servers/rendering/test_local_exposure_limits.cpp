/**************************************************************************/
/*  test_local_exposure_limits.cpp                                        */
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

TEST_FORCE_LINK(test_local_exposure_limits)

#include "servers/rendering/storage/local_exposure_limits.h"

namespace TestLocalExposureLimits {

TEST_CASE("[LocalExposureLimits] A pivot six stops from middle gray is kept") {
	for (float stops = -6.0f; stops <= 6.0f; stops += 0.25f) {
		CHECK(LocalExposureLimits::pivot(stops) == stops);
	}
	CHECK(LocalExposureLimits::pivot(-8.0f) == -8.0f);
	CHECK(LocalExposureLimits::pivot(8.0f) == 8.0f);
}

TEST_CASE("[LocalExposureLimits] A pivot past the range stops at its end") {
	CHECK(LocalExposureLimits::pivot(-9.0f) == -LocalExposureLimits::MAX_PIVOT_STOPS);
	CHECK(LocalExposureLimits::pivot(12.0f) == LocalExposureLimits::MAX_PIVOT_STOPS);
}

TEST_CASE("[LocalExposureLimits] Strength, contrast and shift stay inside their ranges") {
	CHECK(LocalExposureLimits::strength(-0.5f) == 0.0f);
	CHECK(LocalExposureLimits::strength(0.4f) == 0.4f);
	CHECK(LocalExposureLimits::strength(3.0f) == 1.0f);
	CHECK(LocalExposureLimits::contrast(0.0f) == LocalExposureLimits::MIN_CONTRAST);
	CHECK(LocalExposureLimits::contrast(0.65f) == 0.65f);
	CHECK(LocalExposureLimits::contrast(1.5f) == 1.0f);
	CHECK(LocalExposureLimits::shift(-1.0f) == 0.0f);
	CHECK(LocalExposureLimits::shift(2.5f) == 2.5f);
	CHECK(LocalExposureLimits::shift(9.0f) == LocalExposureLimits::MAX_SHIFT_STOPS);
}

} // namespace TestLocalExposureLimits
