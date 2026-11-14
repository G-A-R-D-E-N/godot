/**************************************************************************/
/*  gi_provider.h                                                         */
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

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <type_traits>

namespace RendererRD {

enum GIProviderKind : uint32_t {
	GI_PROVIDER_KIND_INCIDENT_RADIANCE = 0,
	GI_PROVIDER_KIND_OUTGOING_RADIANCE = 1,
	GI_PROVIDER_KIND_VISIBILITY = 2,
	GI_PROVIDER_KIND_COUNT = 3,
};

enum GIProviderUnits : uint32_t {
	GI_PROVIDER_UNITS_STORAGE = 0,
	GI_PROVIDER_UNITS_PHYSICAL = 1,
	GI_PROVIDER_UNITS_DIMENSIONLESS = 2,
};

enum GIProviderSource : uint32_t {
	GI_PROVIDER_SOURCE_NONE = 0,
	GI_PROVIDER_SOURCE_SSGI = 1,
	GI_PROVIDER_SOURCE_DDGI = 2,
	GI_PROVIDER_SOURCE_SURFEL = 3,
	GI_PROVIDER_SOURCE_SSR = 4,
	GI_PROVIDER_SOURCE_REFLECTION_PROBE = 5,
	GI_PROVIDER_SOURCE_SKY_IBL = 6,
	GI_PROVIDER_SOURCE_PATH_TRACER = 7,
	GI_PROVIDER_SOURCE_RTGI = 8,
	GI_PROVIDER_SOURCE_SPECULAR_VISIBILITY = 9,
	GI_PROVIDER_SOURCE_BACKEND_GI = 10,
	GI_PROVIDER_SOURCE_BACKEND_PT = 11,
	GI_PROVIDER_SOURCE_COUNT = 12,
};

enum GIProviderFlags : uint32_t {
	GI_PROVIDER_FLAG_SAMPLE_COUNT = 1u << 0,
	GI_PROVIDER_FLAG_CONFIDENCE = 1u << 1,
	GI_PROVIDER_FLAG_GEOMETRY_VERSION = 1u << 2,
	GI_PROVIDER_FLAG_MATERIAL_VERSION = 1u << 3,
	GI_PROVIDER_FLAG_HISTORY = 1u << 4,
	GI_PROVIDER_FLAG_SCENE_EPOCH = 1u << 5,
	GI_PROVIDER_FLAG_LIGHT_EPOCH = 1u << 6,
	GI_PROVIDER_FLAG_SKY_EPOCH = 1u << 7,
	GI_PROVIDER_FLAG_ORIGIN_EPOCH = 1u << 8,
};

struct GIProviderRecord {
	uint32_t kind;
	uint32_t units;
	uint32_t flags;
	uint32_t source;
	uint32_t sample_count;
	float validity;
	float confidence;
	uint32_t geometry_version;
	uint32_t material_version;
	uint32_t scene_epoch;
	uint32_t light_epoch;
	uint32_t sky_epoch;
	uint32_t origin_epoch;
	uint32_t pad[3];
};

static_assert(sizeof(GIProviderRecord) == 64, "GIProviderRecord must stay 16 words");
static_assert(offsetof(GIProviderRecord, kind) == 0, "kind word offset");
static_assert(offsetof(GIProviderRecord, units) == 4, "units word offset");
static_assert(offsetof(GIProviderRecord, flags) == 8, "flags word offset");
static_assert(offsetof(GIProviderRecord, source) == 12, "source word offset");
static_assert(offsetof(GIProviderRecord, sample_count) == 16, "sample_count word offset");
static_assert(offsetof(GIProviderRecord, validity) == 20, "validity word offset");
static_assert(offsetof(GIProviderRecord, confidence) == 24, "confidence word offset");
static_assert(offsetof(GIProviderRecord, geometry_version) == 28, "geometry_version word offset");
static_assert(offsetof(GIProviderRecord, material_version) == 32, "material_version word offset");
static_assert(offsetof(GIProviderRecord, scene_epoch) == 36, "scene_epoch word offset");
static_assert(offsetof(GIProviderRecord, light_epoch) == 40, "light_epoch word offset");
static_assert(offsetof(GIProviderRecord, sky_epoch) == 44, "sky_epoch word offset");
static_assert(offsetof(GIProviderRecord, origin_epoch) == 48, "origin_epoch word offset");
static_assert(offsetof(GIProviderRecord, pad) == 52, "pad word offset");

struct GIIncidentRadianceTag {
	static constexpr GIProviderKind kind = GI_PROVIDER_KIND_INCIDENT_RADIANCE;
};

struct GIOutgoingRadianceTag {
	static constexpr GIProviderKind kind = GI_PROVIDER_KIND_OUTGOING_RADIANCE;
};

struct GIVisibilityTag {
	static constexpr GIProviderKind kind = GI_PROVIDER_KIND_VISIBILITY;
};

template <typename Tag>
class GIProviderOutput {
	GIProviderRecord record;

	explicit GIProviderOutput(const GIProviderRecord &p_record) :
			record(p_record) {}

public:
	static constexpr GIProviderKind KIND = Tag::kind;

	GIProviderOutput() :
			record() {
		record.kind = Tag::kind;
	}

	static bool try_from_record(const GIProviderRecord &p_record, GIProviderOutput *r_output) {
		if (r_output == nullptr || p_record.kind != Tag::kind) {
			return false;
		}
		*r_output = GIProviderOutput(p_record);
		return true;
	}

	const GIProviderRecord &get_record() const { return record; }
	GIProviderKind get_kind() const { return Tag::kind; }
};

using GIIncidentRadiance = GIProviderOutput<GIIncidentRadianceTag>;
using GIOutgoingRadiance = GIProviderOutput<GIOutgoingRadianceTag>;
using GIVisibility = GIProviderOutput<GIVisibilityTag>;

static_assert(!std::is_convertible<GIOutgoingRadiance, GIIncidentRadiance>::value, "incident and outgoing radiance providers must not convert");
static_assert(!std::is_convertible<GIIncidentRadiance, GIOutgoingRadiance>::value, "incident and outgoing radiance providers must not convert");
static_assert(!std::is_convertible<GIVisibility, GIIncidentRadiance>::value, "visibility and incident radiance providers must not convert");
static_assert(!std::is_constructible<GIIncidentRadiance, GIOutgoingRadiance>::value, "incident radiance must not be built from outgoing radiance");

inline float gi_provider_clamp_unit(float p_value) {
	return std::min(1.0f, std::max(0.0f, p_value));
}

inline float gi_provider_confidence_from_samples(uint32_t p_sample_count, uint32_t p_target_count) {
	if (p_target_count == 0) {
		return 1.0f;
	}
	return std::min(1.0f, float(p_sample_count) / float(p_target_count));
}

inline GIProviderRecord gi_provider_make_record(uint32_t p_units, uint32_t p_source, uint32_t p_flags, uint32_t p_sample_count, float p_validity, float p_confidence) {
	GIProviderRecord record = {};
	record.units = p_units;
	record.source = p_source;
	record.flags = p_flags;
	record.sample_count = p_sample_count;
	record.validity = gi_provider_clamp_unit(p_validity);
	record.confidence = gi_provider_clamp_unit(p_confidence);
	return record;
}

inline uint32_t gi_provider_units(bool p_physical_light_units) {
	return p_physical_light_units ? GI_PROVIDER_UNITS_PHYSICAL : GI_PROVIDER_UNITS_STORAGE;
}

} // namespace RendererRD
