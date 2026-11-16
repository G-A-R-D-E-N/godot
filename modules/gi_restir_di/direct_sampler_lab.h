/**************************************************************************/
/*  direct_sampler_lab.h                                                  */
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

#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/vector.h"
#include "core/variant/dictionary.h"

class DiSamplerTestShaderRD;

class DirectSamplerLab {
public:
	enum Mutant {
		MUTANT_NONE,
		MUTANT_INDEX_KEY,
		MUTANT_LOOP_TIES,
		MUTANT_K24,
		MUTANT_NO_LOG,
		MUTANT_HIT_STRATA,
		MUTANT_MAX,
	};

	enum Leg {
		LEG_RECORDS,
		LEG_TABLES,
		LEG_EDGES,
	};

	static constexpr uint32_t GROUP_SIZE = 128;
	static constexpr uint32_t BASE_SEEDS = 1u << 16;
	static constexpr uint32_t PERMUTATIONS = 64;
	static constexpr uint32_t PERMUTATION_SEEDS = 256;
	static constexpr uint32_t ID0_SEEDS = 1024;
	static constexpr uint32_t TIE_SEEDS = 4;
	static constexpr uint32_t TIE_ORDERS = 24;
	static constexpr uint32_t TIE_SCAN_LIMIT = 1u << 30;
	static constexpr uint32_t TODAY_EXACT_LIGHTS = 4;
	static constexpr double LIST_BAR_W = 1e-6;
	static constexpr double PERMUTATION_BAR_W = 1e-5;

	struct Set {
		float point[4];
		float normal[4];
		uint32_t first;
		uint32_t count;
		uint32_t seed_first;
		uint32_t seed_count;
		uint32_t seed_list;
		uint32_t row_first;
		uint32_t pad0;
		uint32_t pad1;
	};

	struct Slot {
		float color[3];
		float energy;
		uint32_t id;
		uint32_t pad[3];
	};

	struct ScreenLight {
		float position[3];
		float inv_radius;
		float direction[3];
		float size;
		float color[3];
		float attenuation;
		float area_width[3];
		float cone_attenuation;
		float area_height[3];
		float cone_angle;
		float specular_amount;
		float shadow_opacity;
		uint32_t sscs_index;
		uint32_t flags;
		uint32_t rest[32];
	};

	struct HitLight {
		float color[3];
		float energy;
		float direction[3];
		uint32_t type;
		float position[3];
		float attenuation;
		float cos_spot_angle;
		float inv_spot_attenuation;
		float inv_radius;
		uint32_t caster_mask;
		float half_a[3];
		float area;
		float half_b[3];
		uint32_t pad;
	};

	struct Table {
		uint32_t first;
		uint32_t count;
		uint32_t count_first;
		uint32_t row_first;
		uint32_t row_count;
		uint32_t seed_count;
		uint32_t pad0;
		uint32_t pad1;
	};

	struct Weight {
		float target;
		uint32_t id;
	};

	struct Edge {
		uint32_t seed_hash;
		uint32_t id;
		float target;
		uint32_t expected_k;
	};

	struct Row {
		uint32_t race_pick;
		uint32_t race_id;
		float race_w;
		float race_key;
		uint32_t list_pick;
		float list_w;
		uint32_t today_pick;
		float today_w;
		uint32_t today_count;
		uint32_t loop_pick;
		uint32_t pad0;
		uint32_t pad1;
	};

	struct LampOut {
		float term;
		float target;
		uint32_t id;
		uint32_t in_set;
	};

	struct TableRow {
		uint32_t pick;
		float weight;
	};

	struct EdgeOut {
		float exponential;
		float key;
		uint32_t k;
		uint32_t pad;
	};

	struct Push {
		uint32_t leg;
		uint32_t mutant;
		uint32_t index;
		uint32_t count;
	};

	static_assert(sizeof(Set) == 64);
	static_assert(sizeof(Slot) == 32);
	static_assert(sizeof(ScreenLight) == 224);
	static_assert(sizeof(HitLight) == 96);
	static_assert(sizeof(Table) == 32);
	static_assert(sizeof(Weight) == 8);
	static_assert(sizeof(Edge) == 16);
	static_assert(sizeof(Row) == 48);
	static_assert(sizeof(LampOut) == 16);
	static_assert(sizeof(TableRow) == 8);
	static_assert(sizeof(EdgeOut) == 16);
	static_assert(sizeof(Push) == 16);

	struct Inputs {
		LocalVector<ScreenLight> omni;
		LocalVector<ScreenLight> spot;
		LocalVector<Slot> slots;
		LocalVector<uint32_t> cells;
		LocalVector<HitLight> hit;
		LocalVector<Set> sets;
		LocalVector<uint32_t> seeds;
		LocalVector<Table> tables;
		LocalVector<Weight> weights;
		LocalVector<Edge> edges;
		LocalVector<uint32_t> base_sets;
		LocalVector<uint32_t> added_sets;
		LocalVector<uint32_t> added_ids;
		LocalVector<uint32_t> permuted_sets;
		LocalVector<uint32_t> tie_sets;
		uint32_t tie_id = 0;
		uint32_t id0_set = 0;
		uint32_t race_tables = 0;
		uint32_t zero_table = 0;
		uint32_t empty_table = 0;
		uint32_t rows = 0;
		uint32_t counts = 0;
		uint32_t table_rows = 0;
	};

	struct Outputs {
		Vector<uint8_t> rows;
		Vector<uint8_t> lamps;
		Vector<uint8_t> counts;
		Vector<uint8_t> table_rows;
		Vector<uint8_t> edge_out;
	};

	static void register_settings();
	static bool build_inputs(Inputs &r_inputs);

	void serve();
	void free_device_resources();

private:
	DiSamplerTestShaderRD *shader = nullptr;
	RID shader_version;
	String served_request;

	bool _ensure_shader();
	bool _dispatch(const Inputs &p_inputs, Mutant p_mutant, int p_variant, bool p_all_legs, Outputs &r_outputs);
	Dictionary _run(const String &p_request, Mutant p_mutant);
};
