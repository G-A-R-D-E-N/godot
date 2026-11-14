/**************************************************************************/
/*  direct_sampler_lab_report.cpp                                         */
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

#include "direct_sampler_lab.h"

#include "modules/gi_restir_di/di_sampler_reference.h"

#include "core/io/file_access.h"
#include "core/variant/array.h"

namespace {

using Lab = DirectSamplerLab;

constexpr int64_t ID_NONE = -1;
constexpr int64_t ID_INVALID = -2;

const char *MUTANT_NAMES[Lab::MUTANT_MAX] = { "none", "index_key", "loop_ties", "k24", "no_log", "hit_strata" };
const char *MUTANT_LEGS[Lab::MUTANT_MAX] = { "", "race.permutation", "race.forced_tie", "race.edge", "race.distribution", "list" };

struct Views {
	const Lab::Row *rows = nullptr;
	const Lab::LampOut *lamps = nullptr;
	const uint32_t *counts = nullptr;
	const Lab::TableRow *table_rows = nullptr;
	const Lab::EdgeOut *edge_out = nullptr;

	explicit Views(const Lab::Outputs &p_outputs) {
		rows = reinterpret_cast<const Lab::Row *>(p_outputs.rows.ptr());
		lamps = reinterpret_cast<const Lab::LampOut *>(p_outputs.lamps.ptr());
		counts = reinterpret_cast<const uint32_t *>(p_outputs.counts.ptr());
		table_rows = reinterpret_cast<const Lab::TableRow *>(p_outputs.table_rows.ptr());
		edge_out = reinterpret_cast<const Lab::EdgeOut *>(p_outputs.edge_out.ptr());
	}
};

int64_t pick_id(const Lab::Inputs &p_inputs, const Views &p_views, uint32_t p_set, uint32_t p_pick) {
	if (p_pick == DiSamplerReference::NO_PICK) {
		return ID_NONE;
	}
	const Lab::Set &set = p_inputs.sets[p_set];
	return p_pick < set.count ? int64_t(p_views.lamps[set.first + p_pick].id) : ID_INVALID;
}

bool relative_within(float p_value, float p_reference, double p_bar, double &r_largest) {
	const double relative = Math::abs(double(p_value) - double(p_reference)) / Math::abs(double(p_reference));
	const bool within = relative <= p_bar;
	if (Math::is_finite(relative)) {
		r_largest = MAX(r_largest, relative);
	}
	return within;
}

bool store(const String &p_path, const uint8_t *p_data, uint64_t p_size) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	return p_size == 0 || file->store_buffer(p_data, p_size);
}

template <typename T>
bool store_vector(const String &p_path, const LocalVector<T> &p_data) {
	return store(p_path, reinterpret_cast<const uint8_t *>(p_data.ptr()), uint64_t(p_data.size()) * sizeof(T));
}

bool store_bytes(const String &p_path, const Vector<uint8_t> &p_data) {
	return store(p_path, p_data.ptr(), uint64_t(p_data.size()));
}

Array to_array(const LocalVector<uint32_t> &p_values) {
	Array values;
	for (const uint32_t value : p_values) {
		values.push_back(int64_t(value));
	}
	return values;
}

Dictionary evaluate_shapes(const Lab::Inputs &p_inputs, const Views &p_screen, const Views &p_hit) {
	int64_t row_mismatches = 0;
	int64_t lamp_mismatches = 0;
	for (uint32_t i = 0; i < p_inputs.rows; i++) {
		if (memcmp(&p_screen.rows[i], &p_hit.rows[i], sizeof(Lab::Row)) != 0) {
			row_mismatches++;
		}
	}
	for (uint32_t i = 0; i < p_inputs.hit.size(); i++) {
		if (memcmp(&p_screen.lamps[i], &p_hit.lamps[i], sizeof(Lab::LampOut)) != 0) {
			lamp_mismatches++;
		}
	}
	Dictionary shapes;
	shapes["rows"] = int64_t(p_inputs.rows);
	shapes["row_mismatches"] = row_mismatches;
	shapes["lamps"] = int64_t(p_inputs.hit.size());
	shapes["lamp_mismatches"] = lamp_mismatches;
	shapes["pass"] = row_mismatches == 0 && lamp_mismatches == 0;
	return shapes;
}

Dictionary evaluate_list(const Lab::Inputs &p_inputs, const Views &p_hit) {
	int64_t compared = 0;
	int64_t pick_mismatches = 0;
	int64_t w_over_bar = 0;
	int64_t exact_rows = 0;
	int64_t exact_pick_mismatches = 0;
	double largest = 0.0;
	for (const Lab::Set &set : p_inputs.sets) {
		for (uint32_t q = 0; q < set.seed_count; q++) {
			const Lab::Row &row = p_hit.rows[set.row_first + q];
			if (row.today_count <= Lab::TODAY_EXACT_LIGHTS) {
				exact_rows++;
				exact_pick_mismatches += row.list_pick != row.today_pick ? 1 : 0;
				continue;
			}
			compared++;
			if (row.list_pick != row.today_pick) {
				pick_mismatches++;
			} else if (!relative_within(row.list_w, row.today_w, Lab::LIST_BAR_W, largest)) {
				w_over_bar++;
			}
		}
	}
	Dictionary list;
	list["rows_compared"] = compared;
	list["pick_mismatches"] = pick_mismatches;
	list["w_largest_relative"] = largest;
	list["w_over_bar"] = w_over_bar;
	list["bar_w_relative"] = Lab::LIST_BAR_W;
	list["exact_rows_printed"] = exact_rows;
	list["exact_rows_pick_mismatches_printed"] = exact_pick_mismatches;
	list["pass"] = compared > 0 && pick_mismatches == 0 && w_over_bar == 0;
	return list;
}

Dictionary evaluate_permutation(const Lab::Inputs &p_inputs, const Views &p_hit) {
	int64_t pick_mismatches = 0;
	int64_t w_over_bar = 0;
	double largest = 0.0;
	for (uint32_t b = 0; b < p_inputs.base_sets.size(); b++) {
		const uint32_t base = p_inputs.base_sets[b];
		for (uint32_t p = 0; p < Lab::PERMUTATIONS; p++) {
			const uint32_t permuted = p_inputs.permuted_sets[b * Lab::PERMUTATIONS + p];
			for (uint32_t q = 0; q < Lab::PERMUTATION_SEEDS; q++) {
				const Lab::Row &reference = p_hit.rows[p_inputs.sets[base].row_first + q];
				const Lab::Row &row = p_hit.rows[p_inputs.sets[permuted].row_first + q];
				const int64_t reference_id = pick_id(p_inputs, p_hit, base, reference.race_pick);
				const int64_t id = pick_id(p_inputs, p_hit, permuted, row.race_pick);
				if (id != reference_id || id == ID_INVALID) {
					pick_mismatches++;
				} else if (id != ID_NONE && !relative_within(row.race_w, reference.race_w, Lab::PERMUTATION_BAR_W, largest)) {
					w_over_bar++;
				}
			}
		}
	}
	Dictionary permutation;
	permutation["sets"] = int64_t(p_inputs.base_sets.size());
	permutation["orders"] = int64_t(Lab::PERMUTATIONS);
	permutation["seeds"] = int64_t(Lab::PERMUTATION_SEEDS);
	permutation["pick_mismatches"] = pick_mismatches;
	permutation["w_largest_relative"] = largest;
	permutation["w_over_bar"] = w_over_bar;
	permutation["bar_w_relative"] = Lab::PERMUTATION_BAR_W;
	permutation["pass"] = pick_mismatches == 0 && w_over_bar == 0;
	return permutation;
}

Dictionary evaluate_added(const Lab::Inputs &p_inputs, const Views &p_hit) {
	int64_t changed = 0;
	int64_t new_lamp_wins = 0;
	for (uint32_t b = 0; b < p_inputs.base_sets.size(); b++) {
		const uint32_t base = p_inputs.base_sets[b];
		const uint32_t added = p_inputs.added_sets[b];
		const int64_t new_id = int64_t(p_inputs.added_ids[b]);
		for (uint32_t q = 0; q < Lab::BASE_SEEDS; q++) {
			const int64_t before = pick_id(p_inputs, p_hit, base, p_hit.rows[p_inputs.sets[base].row_first + q].race_pick);
			const int64_t after = pick_id(p_inputs, p_hit, added, p_hit.rows[p_inputs.sets[added].row_first + q].race_pick);
			if (after == new_id) {
				new_lamp_wins++;
			} else if (after != before || after == ID_INVALID) {
				changed++;
			}
		}
	}
	Dictionary added;
	added["sets"] = int64_t(p_inputs.base_sets.size());
	added["seeds"] = int64_t(Lab::BASE_SEEDS);
	added["changed_other_than_new_lamp"] = changed;
	added["new_lamp_wins_printed"] = new_lamp_wins;
	added["pass"] = changed == 0;
	return added;
}

Dictionary evaluate_tie(const Lab::Inputs &p_inputs, const Views &p_hit, bool p_seeds_found) {
	int64_t rows = 0;
	int64_t wrong = 0;
	for (const uint32_t set_index : p_inputs.tie_sets) {
		const Lab::Set &set = p_inputs.sets[set_index];
		for (uint32_t q = 0; q < set.seed_count; q++) {
			rows++;
			if (pick_id(p_inputs, p_hit, set_index, p_hit.rows[set.row_first + q].race_pick) != int64_t(p_inputs.tie_id)) {
				wrong++;
			}
		}
	}
	Dictionary tie;
	tie["orders"] = int64_t(p_inputs.tie_sets.size());
	tie["seeds_found"] = p_inputs.tie_sets.is_empty() ? int64_t(0) : int64_t(p_inputs.sets[p_inputs.tie_sets[0]].seed_count);
	tie["smaller_id"] = int64_t(p_inputs.tie_id);
	tie["rows"] = rows;
	tie["wrong_picks"] = wrong;
	tie["pass"] = p_seeds_found && rows > 0 && wrong == 0;
	return tie;
}

Dictionary evaluate_distribution(const Lab::Inputs &p_inputs, const Views &p_hit) {
	Array tables;
	bool pass = true;
	for (uint32_t t = 0; t < DiSamplerReference::RACE_TABLE_COUNT; t++) {
		const Lab::Table &table = p_inputs.tables[p_inputs.race_tables + t];
		const double seeds = double(table.seed_count);
		double total = 0.0;
		for (uint32_t i = 0; i < table.count; i++) {
			total += double(p_inputs.weights[table.first + i].target);
		}
		Array z;
		double largest = 0.0;
		for (uint32_t i = 0; i < table.count; i++) {
			const double share = double(p_inputs.weights[table.first + i].target) / total;
			const double standard_error = Math::sqrt(share * (1.0 - share) / seeds);
			const double value = (double(p_hit.counts[table.count_first + i]) / seeds - share) / standard_error;
			z.push_back(Math::is_finite(value) ? value : 1e300);
			largest = Math::is_finite(value) ? MAX(largest, Math::abs(value)) : 1e300;
		}
		const int64_t no_pick = p_hit.counts[table.count_first + table.count];
		Dictionary entry;
		entry["seeds"] = int64_t(table.seed_count);
		entry["z"] = z;
		entry["largest_abs_z"] = largest;
		entry["no_pick"] = no_pick;
		tables.push_back(entry);
		pass = pass && no_pick == 0 && largest <= DiSamplerReference::RACE_TABLE_BAR_SE;
	}
	Dictionary distribution;
	distribution["tables"] = tables;
	distribution["bar_standard_errors"] = DiSamplerReference::RACE_TABLE_BAR_SE;
	distribution["pass"] = pass;
	return distribution;
}

Dictionary evaluate_edge(const Lab::Inputs &p_inputs, const Views &p_hit) {
	int64_t bad_rows = 0;
	for (uint32_t e = 0; e < p_inputs.edges.size(); e++) {
		const Lab::EdgeOut &result = p_hit.edge_out[e];
		const bool good = result.k == p_inputs.edges[e].expected_k && Math::is_finite(result.exponential) && result.exponential > 0.0f && Math::is_finite(result.key);
		bad_rows += good ? 0 : 1;
	}
	int64_t zero_weight_picks = 0;
	const Lab::Table &zero = p_inputs.tables[p_inputs.zero_table];
	for (uint32_t i = 0; i < zero.count; i++) {
		if (p_inputs.weights[zero.first + i].target == 0.0f) {
			zero_weight_picks += p_hit.counts[zero.count_first + i];
		}
	}
	int64_t empty_bad_rows = 0;
	const Lab::Table &empty = p_inputs.tables[p_inputs.empty_table];
	for (uint32_t q = 0; q < empty.row_count; q++) {
		const Lab::TableRow &row = p_hit.table_rows[empty.row_first + q];
		empty_bad_rows += (row.pick == DiSamplerReference::NO_PICK && row.weight == 0.0f) ? 0 : 1;
	}
	const bool empty_counts = p_hit.counts[empty.count_first] == empty.seed_count;
	Dictionary edge;
	edge["k_rows"] = int64_t(p_inputs.edges.size());
	edge["k_bad_rows"] = bad_rows;
	edge["zero_weight_picks"] = zero_weight_picks;
	edge["empty_rows"] = int64_t(empty.row_count);
	edge["empty_bad_rows"] = empty_bad_rows;
	edge["empty_all_no_pick"] = empty_counts;
	edge["pass"] = bad_rows == 0 && zero_weight_picks == 0 && empty_bad_rows == 0 && empty_counts;
	return edge;
}

Dictionary evaluate_ids(const Lab::Inputs &p_inputs, const Views &p_hit) {
	int64_t sets_with_repeats = 0;
	for (const Lab::Set &set : p_inputs.sets) {
		if (set.seed_count == 0) {
			continue;
		}
		LocalVector<uint32_t> ids;
		for (uint32_t i = 0; i < set.count; i++) {
			ids.push_back(p_hit.lamps[set.first + i].id);
		}
		ids.sort();
		for (uint32_t i = 1; i < ids.size(); i++) {
			if (ids[i] == ids[i - 1]) {
				sets_with_repeats++;
				break;
			}
		}
	}
	int64_t id0_misses = 0;
	const Lab::Set &single = p_inputs.sets[p_inputs.id0_set];
	for (uint32_t q = 0; q < single.seed_count; q++) {
		const Lab::Row &row = p_hit.rows[single.row_first + q];
		id0_misses += pick_id(p_inputs, p_hit, p_inputs.id0_set, row.race_pick) == 0 ? 0 : 1;
		id0_misses += pick_id(p_inputs, p_hit, p_inputs.id0_set, row.list_pick) == 0 ? 0 : 1;
	}
	Dictionary ids;
	ids["sets_with_repeated_ids"] = sets_with_repeats;
	ids["id0_rows"] = int64_t(single.seed_count);
	ids["id0_misses"] = id0_misses;
	ids["pass"] = sets_with_repeats == 0 && id0_misses == 0;
	return ids;
}

int64_t count_loop_mismatches(const Lab::Inputs &p_inputs, const Views &p_hit) {
	int64_t mismatches = 0;
	for (uint32_t i = 0; i < p_inputs.rows; i++) {
		mismatches += p_hit.rows[i].loop_pick == p_hit.rows[i].race_pick ? 0 : 1;
	}
	return mismatches;
}

Dictionary layouts() {
	Dictionary layout;
	layout["sets"] = "64 B: point f32x4, normal f32x4, first, count, seed_first, seed_count, seed_list, row_first, pad x2 (u32)";
	layout["hit_lights"] = "96 B, RaySceneLight: color f32x3, energy, direction f32x3, type u32, position f32x3, attenuation, cos_spot_angle, inv_spot_attenuation, inv_radius, caster_mask u32, half_a f32x3, area, half_b f32x3, id u32";
	layout["seeds"] = "u32 per seed; a set with seed_list 1 reads seeds[seed_first + q], else its seed is seed_first + q";
	layout["tables"] = "32 B: first, count, count_first, row_first, row_count, seed_count, pad x2 (u32)";
	layout["weights"] = "8 B: target f32, id u32";
	layout["edges"] = "16 B: seed_hash u32, id u32, target f32, expected_k u32";
	layout["rows"] = "48 B: race_pick u32, race_id u32, race_w f32, race_key f32, list_pick u32, list_w f32, today_pick u32, today_w f32, today_count u32, loop_pick u32, pad x2; set s owns rows [row_first, row_first + seed_count); picks are list positions in the set, 0xFFFFFFFF none";
	layout["lamps"] = "16 B per set lamp, at the set's first + position: term f32, target f32, id u32, in_set u32";
	layout["counts"] = "u32; table t owns count + 1 words at count_first, the last one counting no pick";
	layout["table_rows"] = "8 B: pick u32, weight f32";
	layout["edge_out"] = "16 B: exponential f32, key f32, k u32, pad";
	return layout;
}

} // namespace

Dictionary DirectSamplerLab::_run(const String &p_request, Mutant p_mutant) {
	Dictionary report;
	report["ok"] = false;
	report["test"] = "KA8";
	report["mutant"] = int(p_mutant);
	report["mutant_name"] = MUTANT_NAMES[p_mutant];
	report["must_fail"] = MUTANT_LEGS[p_mutant];
	if (!_ensure_shader()) {
		report["error"] = "the KA8 test shader is not valid";
		return report;
	}
	Inputs inputs;
	const bool seeds_found = build_inputs(inputs);
	Outputs screen;
	Outputs hit;
	if (!_dispatch(inputs, p_mutant, 0, false, screen) || !_dispatch(inputs, p_mutant, 1, true, hit)) {
		report["error"] = "dispatch or readback failed";
		return report;
	}
	const Views screen_view(screen);
	const Views hit_view(hit);

	const Dictionary shapes = evaluate_shapes(inputs, screen_view, hit_view);
	report["shapes"] = shapes;
	report["list"] = evaluate_list(inputs, hit_view);
	Dictionary race;
	race["permutation"] = evaluate_permutation(inputs, hit_view);
	race["add_one"] = evaluate_added(inputs, hit_view);
	race["forced_tie"] = evaluate_tie(inputs, hit_view, seeds_found);
	race["distribution"] = evaluate_distribution(inputs, hit_view);
	race["edge"] = evaluate_edge(inputs, hit_view);
	race["gi_replay"] = "not run: ReSTIR GI's replay-flag race is not in this engine tree";
	race["loop_mismatches_printed"] = count_loop_mismatches(inputs, hit_view);
	report["race"] = race;
	report["ids"] = evaluate_ids(inputs, hit_view);

	Dictionary sets;
	sets["base"] = to_array(inputs.base_sets);
	sets["add_one"] = to_array(inputs.added_sets);
	sets["add_one_ids"] = to_array(inputs.added_ids);
	sets["permuted"] = to_array(inputs.permuted_sets);
	sets["tie"] = to_array(inputs.tie_sets);
	sets["id0"] = int64_t(inputs.id0_set);
	sets["race_tables_first"] = int64_t(inputs.race_tables);
	sets["zero_table"] = int64_t(inputs.zero_table);
	sets["empty_table"] = int64_t(inputs.empty_table);
	report["sets"] = sets;

	bool stored = true;
	Dictionary files;
	const bool screen_differs = !bool(shapes["pass"]);
	struct Named {
		const char *key;
		const char *suffix;
	};
	const Named names[] = {
		{ "sets", ".sets.bin" },
		{ "hit_lights", ".hit_lights.bin" },
		{ "seeds", ".seeds.bin" },
		{ "tables", ".tables.bin" },
		{ "weights", ".weights.bin" },
		{ "edges", ".edges.bin" },
		{ "rows", ".hit.rows.bin" },
		{ "lamps", ".hit.lamps.bin" },
		{ "counts", ".counts.bin" },
		{ "table_rows", ".table_rows.bin" },
		{ "edge_out", ".edge_out.bin" },
	};
	for (const Named &name : names) {
		files[name.key] = (p_request + name.suffix).get_file();
	}
	stored = store_vector(p_request + ".sets.bin", inputs.sets) && stored;
	stored = store_vector(p_request + ".hit_lights.bin", inputs.hit) && stored;
	stored = store_vector(p_request + ".seeds.bin", inputs.seeds) && stored;
	stored = store_vector(p_request + ".tables.bin", inputs.tables) && stored;
	stored = store_vector(p_request + ".weights.bin", inputs.weights) && stored;
	stored = store_vector(p_request + ".edges.bin", inputs.edges) && stored;
	stored = store(p_request + ".hit.rows.bin", hit.rows.ptr(), uint64_t(inputs.rows) * sizeof(Row)) && stored;
	stored = store(p_request + ".hit.lamps.bin", hit.lamps.ptr(), uint64_t(inputs.hit.size()) * sizeof(LampOut)) && stored;
	stored = store(p_request + ".counts.bin", hit.counts.ptr(), uint64_t(inputs.counts) * sizeof(uint32_t)) && stored;
	stored = store(p_request + ".table_rows.bin", hit.table_rows.ptr(), uint64_t(inputs.table_rows) * sizeof(TableRow)) && stored;
	stored = store(p_request + ".edge_out.bin", hit.edge_out.ptr(), uint64_t(inputs.edges.size()) * sizeof(EdgeOut)) && stored;
	if (screen_differs) {
		files["screen_rows"] = (p_request + ".screen.rows.bin").get_file();
		files["screen_lamps"] = (p_request + ".screen.lamps.bin").get_file();
		stored = store_bytes(p_request + ".screen.rows.bin", screen.rows) && stored;
		stored = store_bytes(p_request + ".screen.lamps.bin", screen.lamps) && stored;
	}
	report["files"] = files;
	report["layouts"] = layouts();
	report["ok"] = stored;
	return report;
}
