/**************************************************************************/
/*  test_gi_compositor.cpp                                                */
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

TEST_FORCE_LINK(test_gi_compositor)

#ifdef RD_ENABLED

#include "servers/rendering/renderer_rd/environment/gi_compositor.h"

namespace TestGICompositor {

using namespace RendererRD;

static GIProviderRecord incident_record(uint32_t p_units = GI_PROVIDER_UNITS_PHYSICAL, uint32_t p_source = GI_COMPOSITOR_EXTRA_SOURCE) {
	return gi_provider_make_record(p_units, p_source, GI_PROVIDER_FLAG_SAMPLE_COUNT, GI_COMPOSITOR_SYNTHETIC_SAMPLES, 1.0f, 1.0f);
}

TEST_CASE("[GICompositor] Slot state follows the setting only for a written incident record of matching units") {
	const GIProviderRecord record = incident_record();

	CHECK(gi_compositor_slot_state(true, true, record, GI_PROVIDER_UNITS_PHYSICAL) == GI_COMPOSITOR_SLOT_ENABLED);
	CHECK(gi_compositor_slot_state(false, true, record, GI_PROVIDER_UNITS_PHYSICAL) == GI_COMPOSITOR_SLOT_DISABLED);
	CHECK(gi_compositor_slot_state(true, false, record, GI_PROVIDER_UNITS_PHYSICAL) == GI_COMPOSITOR_SLOT_NOT_AVAILABLE);
	CHECK(gi_compositor_slot_state(false, false, record, GI_PROVIDER_UNITS_PHYSICAL) == GI_COMPOSITOR_SLOT_NOT_AVAILABLE);
	CHECK(gi_compositor_slot_state(true, true, record, GI_PROVIDER_UNITS_STORAGE) == GI_COMPOSITOR_SLOT_NOT_AVAILABLE);
	CHECK(gi_compositor_slot_state(true, true, incident_record(GI_PROVIDER_UNITS_PHYSICAL, GI_PROVIDER_SOURCE_SSGI), GI_PROVIDER_UNITS_PHYSICAL) == GI_COMPOSITOR_SLOT_NOT_AVAILABLE);

	GIProviderRecord outgoing = record;
	outgoing.kind = GI_PROVIDER_KIND_OUTGOING_RADIANCE;
	CHECK(gi_compositor_slot_state(true, true, outgoing, GI_PROVIDER_UNITS_PHYSICAL) == GI_COMPOSITOR_SLOT_NOT_AVAILABLE);

	GIProviderRecord visibility = record;
	visibility.kind = GI_PROVIDER_KIND_VISIBILITY;
	CHECK(gi_compositor_slot_state(true, true, visibility, GI_PROVIDER_UNITS_PHYSICAL) == GI_COMPOSITOR_SLOT_NOT_AVAILABLE);
}

TEST_CASE("[GICompositor] Typed outputs read only their own kind") {
	GIProviderRecord record = incident_record();
	GIIncidentRadiance incident;
	GIOutgoingRadiance outgoing;
	GIVisibility visibility;

	CHECK(GIIncidentRadiance::try_from_record(record, &incident));
	CHECK(incident.get_record().source == GI_COMPOSITOR_EXTRA_SOURCE);
	CHECK_FALSE(GIOutgoingRadiance::try_from_record(record, &outgoing));
	CHECK_FALSE(GIVisibility::try_from_record(record, &visibility));
	CHECK_FALSE(GIIncidentRadiance::try_from_record(record, nullptr));

	record.kind = GI_PROVIDER_KIND_VISIBILITY;
	CHECK_FALSE(GIIncidentRadiance::try_from_record(record, &incident));
	CHECK(GIVisibility::try_from_record(record, &visibility));
	CHECK(visibility.get_kind() == GI_PROVIDER_KIND_VISIBILITY);
}

TEST_CASE("[GICompositor] A made record clamps validity and confidence and leaves its epochs unclaimed") {
	const GIProviderRecord record = gi_provider_make_record(GI_PROVIDER_UNITS_STORAGE, GI_PROVIDER_SOURCE_RTGI, GI_PROVIDER_FLAG_CONFIDENCE, 7, 1.5f, -0.5f);

	CHECK(record.kind == GI_PROVIDER_KIND_INCIDENT_RADIANCE);
	CHECK(record.units == GI_PROVIDER_UNITS_STORAGE);
	CHECK(record.sample_count == 7);
	CHECK(record.validity == 1.0f);
	CHECK(record.confidence == 0.0f);
	CHECK(record.scene_epoch == 0);
	CHECK(record.light_epoch == 0);
	CHECK(record.sky_epoch == 0);
	CHECK(record.origin_epoch == 0);
	CHECK((record.flags & (GI_PROVIDER_FLAG_SCENE_EPOCH | GI_PROVIDER_FLAG_LIGHT_EPOCH | GI_PROVIDER_FLAG_SKY_EPOCH | GI_PROVIDER_FLAG_ORIGIN_EPOCH)) == 0);
	CHECK(gi_provider_units(true) == GI_PROVIDER_UNITS_PHYSICAL);
	CHECK(gi_provider_units(false) == GI_PROVIDER_UNITS_STORAGE);
}

} // namespace TestGICompositor

#endif
