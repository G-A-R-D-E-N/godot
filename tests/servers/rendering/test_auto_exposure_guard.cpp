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

TEST_CASE("[AutoExposureGuard] No highlight time or a reset follows the highlights at once, and no frame time freezes the shift") {
	CHECK(AutoExposureGuard::highlight_rate(0.0f, 1.0 / 60.0, false) == 1.0f);
	CHECK(AutoExposureGuard::highlight_rate(-1.0f, 1.0 / 60.0, false) == 1.0f);
	CHECK(AutoExposureGuard::highlight_rate(8.0f, 1.0 / 60.0, true) == 1.0f);
	CHECK(AutoExposureGuard::highlight_rate(0.0f, 0.0, false) == 1.0f);
	CHECK(AutoExposureGuard::highlight_rate(8.0f, 0.0, false) == 0.0f);
	CHECK(AutoExposureGuard::highlight_shift(0.5f, 2.0f, 1.0f, 1.0f, true, false) == 2.0f);
	CHECK(AutoExposureGuard::highlight_shift(2.0f, 0.5f, 1.0f, 1.0f, true, false) == 0.5f);
	CHECK(AutoExposureGuard::highlight_shift(0.5f, 2.0f, 0.0f, 0.0f, true, false) == 0.5f);
	CHECK(AutoExposureGuard::highlight_shift(2.0f, 0.5f, 0.0f, 0.0f, true, false) == 2.0f);
}

TEST_CASE("[AutoExposureGuard] The shift deepens with the highlight time and drops with the release time") {
	const float rise = AutoExposureGuard::highlight_rate(8.0f, 1.0 / 60.0, false);
	CHECK(rise == doctest::Approx(0.0020812f).epsilon(0.001).scale(0.0));
	float shift = 0.0f;
	for (int i = 0; i < 480; i++) {
		shift = AutoExposureGuard::highlight_shift(shift, 2.0f, rise, 1.0f, true, false);
	}
	CHECK(shift == doctest::Approx(2.0f * (1.0f - Math::exp(-1.0f))).epsilon(0.001));
	CHECK(AutoExposureGuard::highlight_shift(shift, 0.25f, rise, 1.0f, true, false) == 0.25f);
	const float fall = AutoExposureGuard::highlight_rate(0.3f, 1.0 / 60.0, false);
	float released = 2.0f;
	for (int i = 0; i < 18; i++) {
		released = AutoExposureGuard::highlight_shift(released, 0.0f, rise, fall, true, false);
	}
	CHECK(released == doctest::Approx(2.0f * Math::exp(-1.0f)).epsilon(0.001));
}

TEST_CASE("[AutoExposureGuard] A one-frame dip of the highlights barely moves a shift with a release time") {
	const float rise = AutoExposureGuard::highlight_rate(8.0f, 1.0 / 60.0, false);
	const float fall = AutoExposureGuard::highlight_rate(0.3f, 1.0 / 60.0, false);
	const float dipped = AutoExposureGuard::highlight_shift(1.5f, 0.0f, rise, fall, true, false);
	CHECK(dipped > 1.4f);
	CHECK(dipped < 1.5f);
	CHECK(AutoExposureGuard::highlight_shift(1.5f, 0.0f, rise, 1.0f, true, false) == 0.0f);
}

TEST_CASE("[AutoExposureGuard] A first frame, a reset or a stale state takes the highlights, and a hold keeps the shift") {
	const float rise = AutoExposureGuard::highlight_rate(8.0f, 1.0 / 60.0, false);
	CHECK(AutoExposureGuard::highlight_shift(0.0f, 1.5f, rise, 1.0f, false, false) == 1.5f);
	CHECK(AutoExposureGuard::highlight_shift(0.0f, 1.5f, rise, 1.0f, false, true) == 1.5f);
	CHECK(AutoExposureGuard::highlight_shift(1.0f, 3.0f, rise, 1.0f, true, true) == 1.0f);
	CHECK(AutoExposureGuard::highlight_shift(1.0f, 0.0f, rise, 1.0f, true, true) == 1.0f);
	CHECK(AutoExposureGuard::highlight_valid(true, true, false, false));
	CHECK_FALSE(AutoExposureGuard::highlight_valid(false, true, false, false));
	CHECK_FALSE(AutoExposureGuard::highlight_valid(true, false, false, false));
	CHECK_FALSE(AutoExposureGuard::highlight_valid(true, true, true, false));
	CHECK_FALSE(AutoExposureGuard::highlight_valid(true, true, false, true));
}

TEST_CASE("[AutoExposureGuard] The stored flag stays set on a frame with no samples and is set by any frame with samples") {
	CHECK(AutoExposureGuard::highlight_stored(true, false));
	CHECK_FALSE(AutoExposureGuard::highlight_stored(false, false));
	CHECK(AutoExposureGuard::highlight_stored(false, true));
	CHECK(AutoExposureGuard::highlight_stored(true, true));
}

TEST_CASE("[AutoExposureGuard] The same highlight time at 30 Hz and 144 Hz") {
	for (double hz : { 30.0, 60.0, 144.0 }) {
		const float rise = AutoExposureGuard::highlight_rate(8.0f, 1.0 / hz, false);
		float shift = 0.0f;
		for (int i = 0; i < int(8.0 * hz); i++) {
			shift = AutoExposureGuard::highlight_shift(shift, 2.0f, rise, 1.0f, true, false);
		}
		CHECK(shift == doctest::Approx(2.0f * (1.0f - Math::exp(-1.0f))).epsilon(0.001));
	}
}

TEST_CASE("[AutoExposureGuard] No highlight or release time gives exactly the one-step highlight rule") {
	uint32_t seed = 12345u;
	auto next = [&seed](float p_low, float p_high) {
		seed = seed * 1664525u + 1013904223u;
		return p_low + (p_high - p_low) * float(seed >> 8) / float(1u << 24);
	};
	int mismatches = 0;
	for (int i = 0; i < 4000; i++) {
		const float log_mean = next(-12.0f, 6.0f);
		const float log_high = log_mean + next(-1.0f, 9.0f);
		const float previous = next(0.0f, 4.0f);
		const float old_rule = MIN(MAX(log_high - 2.5f, log_mean), log_mean + 4.0f);
		for (bool valid : { false, true }) {
			const AutoExposureGuard::HighlightStep step = AutoExposureGuard::highlight_step(log_mean, log_high, true, 2.5f, 4.0f, previous, 1.0f, 1.0f, valid, false);
			if (step.log_target != old_rule) {
				mismatches++;
			}
		}
		const AutoExposureGuard::HighlightStep off = AutoExposureGuard::highlight_step(log_mean, log_high, false, 2.5f, 4.0f, previous, 1.0f, 1.0f, true, false);
		if (off.log_target != log_mean || off.shift != 0.0f) {
			mismatches++;
		}
	}
	CHECK(mismatches == 0);
}

TEST_CASE("[AutoExposureGuard] A shift that has reached the highlights gives exactly the one-step rule") {
	const float rise = AutoExposureGuard::highlight_rate(8.0f, 1.0 / 60.0, false);
	const float log_mean = -3.1f;
	const float log_high = 1.7f;
	const float old_rule = MIN(MAX(log_high - 2.5f, log_mean), log_mean + 4.0f);
	const AutoExposureGuard::HighlightStep down = AutoExposureGuard::highlight_step(log_mean, log_high, true, 2.5f, 4.0f, 3.9f, rise, 1.0f, true, false);
	CHECK(down.log_target == old_rule);
	const float fall = AutoExposureGuard::highlight_rate(0.3f, 1.0 / 60.0, false);
	const float raw = MIN(MAX(log_high - 2.5f - log_mean, 0.0f), 4.0f);
	const AutoExposureGuard::HighlightStep caught = AutoExposureGuard::highlight_step(log_mean, log_high, true, 2.5f, 4.0f, raw, rise, fall, true, false);
	CHECK(caught.shift == raw);
	CHECK(caught.log_target == old_rule);
	const AutoExposureGuard::HighlightStep up = AutoExposureGuard::highlight_step(log_mean, log_high, true, 2.5f, 4.0f, 0.5f, rise, 1.0f, true, false);
	CHECK(up.shift > 0.5f);
	CHECK(up.shift < log_high - 2.5f - log_mean);
	CHECK(up.log_target == log_mean + up.shift);
	const AutoExposureGuard::HighlightStep held = AutoExposureGuard::highlight_step(log_mean, log_high, true, 2.5f, 4.0f, 0.5f, 1.0f, 1.0f, true, true);
	CHECK(held.shift == 0.5f);
}

TEST_CASE("[AutoExposureGuard] A stored shift above a lowered cap is cut to it, and a stored NaN takes the highlights") {
	const float rise = AutoExposureGuard::highlight_rate(8.0f, 1.0 / 60.0, false);
	const float fall = AutoExposureGuard::highlight_rate(60.0f, 1.0 / 60.0, false);
	const float log_mean = -3.1f;
	const float log_high = 1.7f;
	const float capped_rule = MIN(MAX(log_high - 2.5f, log_mean), log_mean + 0.5f);
	const AutoExposureGuard::HighlightStep lowered = AutoExposureGuard::highlight_step(log_mean, log_high, true, 2.5f, 0.5f, 3.0f, rise, fall, true, false);
	CHECK(lowered.shift == 0.5f);
	CHECK(lowered.log_target == capped_rule);
	const AutoExposureGuard::HighlightStep held = AutoExposureGuard::highlight_step(log_mean, log_high, true, 2.5f, 0.5f, 3.0f, rise, fall, true, true);
	CHECK(held.shift == 0.5f);
	const AutoExposureGuard::HighlightStep nan = AutoExposureGuard::highlight_step(log_mean, log_high, true, 2.5f, 0.5f, float(Math::NaN), rise, fall, true, false);
	CHECK(nan.shift == 0.5f);
	CHECK(nan.log_target == capped_rule);
	CHECK(AutoExposureGuard::highlight_bounded(-1.0f, 0.25f, 4.0f) == 0.25f);
	CHECK(AutoExposureGuard::highlight_bounded(1.0f, 0.25f, 4.0f) == 1.0f);
}

} // namespace TestAutoExposureGuard
