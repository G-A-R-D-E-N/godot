/**************************************************************************/
/*  test_gi_epochs.cpp                                                    */
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

TEST_FORCE_LINK(test_gi_epochs)

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/environment/gi_epochs.h"

namespace TestGIEpochs {

using namespace RendererRD;

TEST_CASE("[GIEpochs] A source that never ran stays unknown in the record") {
	GIEpochs epochs;
	GIProviderRecord record = gi_provider_make_record(GI_PROVIDER_UNITS_PHYSICAL, GI_PROVIDER_SOURCE_RTGI, GI_PROVIDER_FLAG_SAMPLE_COUNT, 4, 1.0f, 1.0f);
	epochs.fill_record(record);
	CHECK(record.flags == GI_PROVIDER_FLAG_SAMPLE_COUNT);
	CHECK(record.scene_epoch == 0);
	CHECK(record.light_epoch == 0);
	CHECK(record.sky_epoch == 0);
	CHECK(record.origin_epoch == 0);
}

TEST_CASE("[GIEpochs] A bump marks only its own source as known") {
	GIEpochs epochs;
	epochs.bump(GIEpochs::SOURCE_ORIGIN);
	epochs.bump(GIEpochs::SOURCE_ORIGIN);
	GIProviderRecord record = gi_provider_make_record(GI_PROVIDER_UNITS_PHYSICAL, GI_PROVIDER_SOURCE_RTGI, GI_PROVIDER_FLAG_SAMPLE_COUNT, 4, 1.0f, 1.0f);
	epochs.fill_record(record);
	CHECK(record.flags == (GI_PROVIDER_FLAG_SAMPLE_COUNT | GI_PROVIDER_FLAG_ORIGIN_EPOCH));
	CHECK(record.origin_epoch == 2);
	CHECK(record.scene_epoch == 0);
	CHECK(record.light_epoch == 0);
	CHECK(record.sky_epoch == 0);

	epochs.bump(GIEpochs::SOURCE_SCENE);
	epochs.bump(GIEpochs::SOURCE_LIGHT);
	epochs.bump(GIEpochs::SOURCE_SKY);
	GIProviderRecord all = gi_provider_make_record(GI_PROVIDER_UNITS_PHYSICAL, GI_PROVIDER_SOURCE_RTGI, 0, 4, 1.0f, 1.0f);
	epochs.fill_record(all);
	CHECK(all.flags == (GI_PROVIDER_FLAG_SCENE_EPOCH | GI_PROVIDER_FLAG_LIGHT_EPOCH | GI_PROVIDER_FLAG_SKY_EPOCH | GI_PROVIDER_FLAG_ORIGIN_EPOCH));
}

TEST_CASE("[GIEpochs] A word wraps past zero so a known source never reads unknown") {
	CHECK(GIEpochs::next(0) == 1);
	CHECK(GIEpochs::next(1) == 2);
	CHECK(GIEpochs::next(UINT32_MAX) == 1);
}

TEST_CASE("[GIEpochs] A snapshot holds the words and a later bump does not change it") {
	GIEpochs epochs;
	epochs.bump(GIEpochs::SOURCE_SCENE);
	epochs.bump(GIEpochs::SOURCE_SKY);
	epochs.bump(GIEpochs::SOURCE_SKY);
	const GIEpochs::Snapshot before = epochs.snapshot();
	CHECK(before.scene == 1);
	CHECK(before.light == 0);
	CHECK(before.sky == 2);
	CHECK(before.origin == 0);
	epochs.bump(GIEpochs::SOURCE_LIGHT);
	CHECK(before.light == 0);
	CHECK(epochs.get(GIEpochs::SOURCE_LIGHT) == 1);
}

TEST_CASE("[GIEpochs] A source out of range changes nothing and reads zero") {
	GIEpochs epochs;
	epochs.bump(GIEpochs::SOURCE_COUNT);
	CHECK(epochs.get(GIEpochs::SOURCE_COUNT) == 0);
	GIProviderRecord record = gi_provider_make_record(GI_PROVIDER_UNITS_PHYSICAL, GI_PROVIDER_SOURCE_RTGI, 0, 4, 1.0f, 1.0f);
	epochs.fill_record(record);
	CHECK(record.flags == 0);
}

TEST_CASE("[GIEpochs] The snapshot carries the world epoch") {
	GIEpochs epochs;
	CHECK(epochs.snapshot().world == 0);
	epochs.bump(GIEpochs::SOURCE_WORLD);
	const GIEpochs::Snapshot snapshot = epochs.snapshot();
	CHECK(snapshot.world == 1);
	CHECK(snapshot.scene == 0);
	CHECK(snapshot.sky == 0);
}

static uint64_t scene_signature(const LocalVector<uint32_t> &p_hashes) {
	uint64_t signature = 0;
	for (uint32_t hash : p_hashes) {
		signature = GISceneTracker::add_instance(signature, hash);
	}
	return signature;
}

TEST_CASE("[GIEpochs] The scene signature does not depend on instance order, and counts instances") {
	CHECK(scene_signature({ 11, 22, 33 }) == scene_signature({ 33, 11, 22 }));
	CHECK(scene_signature({}) == 0);
	CHECK(scene_signature({ 0 }) != scene_signature({}));
	CHECK(scene_signature({ 5, 5 }) != scene_signature({ 10 }));
}

TEST_CASE("[GIEpochs] The first update moves the scene epoch, an unchanged update moves nothing") {
	GIEpochs epochs;
	GISceneTracker tracker;
	epochs.apply(tracker.update(scene_signature({ 1, 2 }), { 100, 200 }));
	CHECK(epochs.get(GIEpochs::SOURCE_SCENE) == 1);
	CHECK(epochs.get(GIEpochs::SOURCE_WORLD) == 0);
	for (int i = 0; i < 3; i++) {
		const GISceneTracker::Change change = tracker.update(scene_signature({ 2, 1 }), { 200, 100 });
		CHECK_FALSE(change.scene);
		CHECK_FALSE(change.world);
		epochs.apply(change);
	}
	CHECK(epochs.get(GIEpochs::SOURCE_SCENE) == 1);
	CHECK(epochs.get(GIEpochs::SOURCE_WORLD) == 0);
}

TEST_CASE("[GIEpochs] A change moves the scene epoch by exactly one step") {
	GIEpochs epochs;
	GISceneTracker tracker;
	epochs.apply(tracker.update(scene_signature({ 1 }), { 100 }));
	const uint32_t reader = epochs.get(GIEpochs::SOURCE_SCENE);
	epochs.apply(tracker.update(scene_signature({ 1, 7 }), { 100, 700 }));
	const uint32_t stamped = epochs.get(GIEpochs::SOURCE_SCENE);
	CHECK(stamped != reader);
	CHECK(stamped == GIEpochs::next(reader));
	CHECK(epochs.get(GIEpochs::SOURCE_WORLD) == 0);
}

TEST_CASE("[GIEpochs] The world epoch moves only when no instance of the last scene is left") {
	GIEpochs epochs;
	GISceneTracker tracker;
	epochs.apply(tracker.update(scene_signature({ 1, 2, 3 }), { 100, 200, 300 }));
	GISceneTracker::Change change = tracker.update(scene_signature({ 1, 9 }), { 100, 900 });
	CHECK(change.scene);
	CHECK_FALSE(change.world);
	epochs.apply(change);
	change = tracker.update(scene_signature({ 4, 5 }), { 400, 500 });
	CHECK(change.scene);
	CHECK(change.world);
	epochs.apply(change);
	CHECK(epochs.get(GIEpochs::SOURCE_WORLD) == 1);
	change = tracker.update(0, {});
	CHECK(change.scene);
	CHECK(change.world);
	epochs.apply(change);
	CHECK(epochs.get(GIEpochs::SOURCE_WORLD) == 2);
	change = tracker.update(scene_signature({ 6 }), { 600 });
	CHECK(change.scene);
	CHECK_FALSE(change.world);
	change = tracker.update(scene_signature({ 6 }), { 600 });
	CHECK_FALSE(change.scene);
	CHECK_FALSE(change.world);
}

TEST_CASE("[GIEpochs] A moved instance is a scene change, not a world change") {
	GISceneTracker tracker;
	tracker.update(scene_signature({ 1, 2 }), { 100, 200 });
	const GISceneTracker::Change change = tracker.update(scene_signature({ 1, 3 }), { 100, 200 });
	CHECK(change.scene);
	CHECK_FALSE(change.world);
}

TEST_CASE("[GIEpochs] Members are refreshed on an unchanged scene, so a later change compares with the current set") {
	GISceneTracker tracker;
	tracker.update(scene_signature({ 1 }), { 100 });
	const GISceneTracker::Change same = tracker.update(scene_signature({ 1 }), { 200 });
	CHECK_FALSE(same.scene);
	CHECK_FALSE(same.world);
	const GISceneTracker::Change grown = tracker.update(scene_signature({ 1, 2 }), { 200, 300 });
	CHECK(grown.scene);
	CHECK_FALSE(grown.world);
}

TEST_CASE("[GIEpochs] A reused instance address with another mesh is a different member") {
	int slot = 0;
	CHECK(GISceneTracker::member_key(&slot, 7) == GISceneTracker::member_key(&slot, 7));
	CHECK(GISceneTracker::member_key(&slot, 7) != GISceneTracker::member_key(&slot, 8));
	GISceneTracker tracker;
	tracker.update(scene_signature({ 1 }), { GISceneTracker::member_key(&slot, 7) });
	const GISceneTracker::Change change = tracker.update(scene_signature({ 2 }), { GISceneTracker::member_key(&slot, 8) });
	CHECK(change.scene);
	CHECK(change.world);
}

TEST_CASE("[GIEpochs] Confidence is the sample share, full at the target and past it") {
	CHECK(gi_provider_confidence_from_samples(0, 6) == 0.0f);
	CHECK(gi_provider_confidence_from_samples(3, 6) == 0.5f);
	CHECK(gi_provider_confidence_from_samples(6, 6) == 1.0f);
	CHECK(gi_provider_confidence_from_samples(12, 6) == 1.0f);
	CHECK(gi_provider_confidence_from_samples(0, 0) == 1.0f);
}

} // namespace TestGIEpochs

#endif
