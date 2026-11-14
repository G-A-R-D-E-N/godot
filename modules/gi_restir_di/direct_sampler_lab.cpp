/**************************************************************************/
/*  direct_sampler_lab.cpp                                                */
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
#include "modules/gi_restir_di/shaders/di_sampler_test.glsl.gen.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "servers/rendering/rendering_device.h"

#define DI_SAMPLER_LAB_SETTING(m_name) ("rendering/global_illumination/direct/resampled/debug/" m_name)

namespace {

using Lab = DirectSamplerLab;

constexpr uint32_t CELL_SPOT = 0x80000000u;
constexpr uint32_t LIGHT_OMNI = 1;
constexpr uint32_t LIGHT_SPOT = 2;
constexpr uint32_t BINDINGS = 15;
constexpr uint32_t BINDING_ROWS = 10;
constexpr uint32_t BINDING_LAMPS = 11;
constexpr uint32_t BINDING_COUNTS = 12;
constexpr uint32_t BINDING_TABLE_ROWS = 13;
constexpr uint32_t BINDING_EDGE_OUT = 14;

class Ka8Random {
	uint32_t state;

public:
	explicit Ka8Random(uint32_t p_seed) :
			state(p_seed) {}

	uint32_t next() {
		state += 0x9e3779b9u;
		return DiSamplerReference::hash(state);
	}
	float unit() { return float(next() >> 8u) * (1.0f / 16777216.0f); }
	float range(float p_from, float p_to) { return p_from + (p_to - p_from) * unit(); }
};

struct Lamp {
	float position[3] = {};
	float aim[3] = {};
	float color[3] = {};
	float energy = 0.0f;
	float inv_radius = 0.0f;
	float attenuation = 0.0f;
	float cos_angle = 0.0f;
	float cone_attenuation = 1.0f;
	uint32_t id = 0;
	bool spot = false;
};

enum Facing {
	FACING_ANY,
	FACING_LIT,
	FACING_AWAY,
};

float dot3(const float *p_a, const float *p_b) {
	return p_a[0] * p_b[0] + p_a[1] * p_b[1] + p_a[2] * p_b[2];
}

void random_unit(Ka8Random &r_random, float *r_out) {
	while (true) {
		const float v[3] = { r_random.range(-1.0f, 1.0f), r_random.range(-1.0f, 1.0f), r_random.range(-1.0f, 1.0f) };
		const float length_squared = dot3(v, v);
		if (length_squared > 1e-4f && length_squared <= 1.0f) {
			const float inverse = 1.0f / Math::sqrt(length_squared);
			for (int k = 0; k < 3; k++) {
				r_out[k] = v[k] * inverse;
			}
			return;
		}
	}
}

uint32_t unique_id(Ka8Random &r_random, LocalVector<uint32_t> &r_used) {
	while (true) {
		const uint32_t id = r_random.next();
		if (!r_used.has(id)) {
			r_used.push_back(id);
			return id;
		}
	}
}

Lamp make_lamp(Ka8Random &r_random, const float *p_point, const float *p_normal, Facing p_facing, uint32_t p_id) {
	Lamp lamp;
	float direction[3];
	while (true) {
		random_unit(r_random, direction);
		const float facing = dot3(direction, p_normal);
		if (p_facing == FACING_ANY) {
			break;
		}
		if (Math::abs(facing) < 0.2f) {
			continue;
		}
		if ((facing > 0.0f) != (p_facing == FACING_LIT)) {
			for (int k = 0; k < 3; k++) {
				direction[k] = -direction[k];
			}
		}
		break;
	}
	const float distance = r_random.range(0.2f, 10.0f);
	for (int k = 0; k < 3; k++) {
		lamp.position[k] = p_point[k] + direction[k] * distance;
	}
	const float range = p_facing == FACING_ANY ? r_random.range(0.5f, 20.0f) : distance * r_random.range(1.5f, 3.0f);
	lamp.inv_radius = 1.0f / range;
	lamp.attenuation = r_random.range(0.5f, 2.0f);
	lamp.energy = Math::pow(10.0f, r_random.range(-3.0f, 3.0f));
	for (int k = 0; k < 3; k++) {
		lamp.color[k] = r_random.range(0.05f, 1.0f);
	}
	lamp.aim[2] = -1.0f;
	lamp.spot = p_facing == FACING_ANY && r_random.unit() < 0.4f;
	if (lamp.spot) {
		float jitter[3];
		random_unit(r_random, jitter);
		float aim[3];
		for (int k = 0; k < 3; k++) {
			aim[k] = -direction[k] + 0.8f * jitter[k];
		}
		const float length = Math::sqrt(dot3(aim, aim));
		for (int k = 0; k < 3; k++) {
			lamp.aim[k] = length > 1e-3f ? aim[k] / length : -direction[k];
		}
		lamp.cos_angle = Math::cos(Math::deg_to_rad(r_random.range(5.0f, 80.0f)));
		lamp.cone_attenuation = r_random.range(0.5f, 4.0f);
	}
	lamp.id = p_id;
	return lamp;
}

class Builder {
	Lab::Inputs &inputs;
	LocalVector<Lamp> pool;
	LocalVector<uint32_t> pool_cells;

public:
	explicit Builder(Lab::Inputs &r_inputs) :
			inputs(r_inputs) {}

	uint32_t add_lamp(const Lamp &p_lamp) {
		const uint32_t slot = pool.size();
		pool.push_back(p_lamp);
		Lab::ScreenLight light = {};
		Lab::Slot data = {};
		for (int k = 0; k < 3; k++) {
			light.position[k] = p_lamp.position[k];
			light.direction[k] = p_lamp.aim[k];
			light.color[k] = p_lamp.color[k];
			data.color[k] = p_lamp.color[k];
		}
		light.inv_radius = p_lamp.inv_radius;
		light.attenuation = p_lamp.attenuation;
		light.cone_attenuation = p_lamp.cone_attenuation;
		light.cone_angle = p_lamp.cos_angle;
		light.specular_amount = 1.0f;
		light.flags = 1u | ((slot & 0xFFFFu) << 1u);
		data.energy = p_lamp.energy;
		data.id = p_lamp.id;
		if (p_lamp.spot) {
			pool_cells.push_back(CELL_SPOT | inputs.spot.size());
			inputs.spot.push_back(light);
		} else {
			pool_cells.push_back(inputs.omni.size());
			inputs.omni.push_back(light);
		}
		inputs.slots.push_back(data);
		return slot;
	}

	uint32_t add_set(const float *p_point, const float *p_normal, const LocalVector<uint32_t> &p_lamps, uint32_t p_seed_first, uint32_t p_seed_count, bool p_seed_list) {
		Lab::Set set = {};
		for (int k = 0; k < 3; k++) {
			set.point[k] = p_point[k];
			set.normal[k] = p_normal[k];
		}
		set.point[3] = 1.0f;
		set.first = inputs.hit.size();
		set.count = p_lamps.size();
		set.seed_first = p_seed_first;
		set.seed_count = p_seed_count;
		set.seed_list = p_seed_list ? 1u : 0u;
		set.row_first = inputs.rows;
		inputs.rows += p_seed_count;
		for (const uint32_t index : p_lamps) {
			const Lamp &lamp = pool[index];
			Lab::HitLight hit = {};
			for (int k = 0; k < 3; k++) {
				hit.color[k] = lamp.color[k];
				hit.direction[k] = lamp.aim[k];
				hit.position[k] = lamp.position[k];
			}
			hit.energy = lamp.energy;
			hit.type = lamp.spot ? LIGHT_SPOT : LIGHT_OMNI;
			hit.attenuation = lamp.attenuation;
			hit.cos_spot_angle = lamp.cos_angle;
			hit.inv_spot_attenuation = lamp.cone_attenuation;
			hit.inv_radius = lamp.inv_radius;
			hit.caster_mask = 0xFFFFFFFFu;
			hit.pad = lamp.id;
			inputs.hit.push_back(hit);
			inputs.cells.push_back(pool_cells[index]);
		}
		inputs.sets.push_back(set);
		return inputs.sets.size() - 1;
	}
};

uint32_t add_table(Lab::Inputs &r_inputs, const float *p_targets, const uint32_t *p_ids, uint32_t p_count, uint32_t p_seeds, uint32_t p_rows) {
	Lab::Table table = {};
	table.first = r_inputs.weights.size();
	table.count = p_count;
	table.count_first = r_inputs.counts;
	table.row_first = r_inputs.table_rows;
	table.row_count = p_rows;
	table.seed_count = p_seeds;
	r_inputs.counts += p_count + 1;
	r_inputs.table_rows += p_rows;
	for (uint32_t i = 0; i < p_count; i++) {
		const Lab::Weight weight = { p_targets[i], p_ids[i] };
		r_inputs.weights.push_back(weight);
	}
	r_inputs.tables.push_back(table);
	return r_inputs.tables.size() - 1;
}

void random_place(Ka8Random &r_random, float *r_point, float *r_normal) {
	for (int k = 0; k < 3; k++) {
		r_point[k] = r_random.range(-4.0f, 4.0f);
	}
	random_unit(r_random, r_normal);
}

template <typename T>
RID upload(const LocalVector<T> &p_data) {
	Vector<uint8_t> bytes;
	bytes.resize(MAX(int64_t(16), int64_t(p_data.size() * sizeof(T))));
	bytes.fill(0);
	if (p_data.size() > 0) {
		memcpy(bytes.ptrw(), p_data.ptr(), p_data.size() * sizeof(T));
	}
	return RD::get_singleton()->storage_buffer_create(uint32_t(bytes.size()), bytes);
}

RID zeroed(uint64_t p_bytes) {
	const uint32_t bytes = uint32_t(MAX(uint64_t(16), p_bytes));
	const RID buffer = RD::get_singleton()->storage_buffer_create(bytes);
	RD::get_singleton()->buffer_clear(buffer, 0, bytes);
	return buffer;
}

void dispatch_leg(RD *p_rd, RD::ComputeListID p_list, Lab::Leg p_leg, Lab::Mutant p_mutant, uint32_t p_index, uint32_t p_count) {
	if (p_count == 0) {
		return;
	}
	const Lab::Push push = { uint32_t(p_leg), uint32_t(p_mutant), p_index, p_count };
	p_rd->compute_list_set_push_constant(p_list, &push, sizeof(Lab::Push));
	p_rd->compute_list_dispatch(p_list, Math::division_round_up(p_count, Lab::GROUP_SIZE), 1, 1);
}

} // namespace

void DirectSamplerLab::register_settings() {
	GLOBAL_DEF(PropertyInfo(Variant::STRING, DI_SAMPLER_LAB_SETTING("ka8_request"), PROPERTY_HINT_GLOBAL_SAVE_FILE, "*.json"), String());
	GLOBAL_DEF(PropertyInfo(Variant::INT, DI_SAMPLER_LAB_SETTING("ka8_mutant"), PROPERTY_HINT_ENUM, "None,Index Key,Loop Ties,K24,No Log,Hit Strata"), 0);
}

bool DirectSamplerLab::build_inputs(Inputs &r_inputs) {
	Builder builder(r_inputs);
	Ka8Random random(0x4b413801u);
	const uint32_t base_sizes[] = { 1, 3, 5, 8, 16, 33, 64, 256 };
	for (uint32_t b = 0; b < 8; b++) {
		float point[3];
		float normal[3];
		random_place(random, point, normal);
		LocalVector<uint32_t> used;
		if (b == 3) {
			used.push_back(0u);
		}
		LocalVector<uint32_t> lamps;
		for (uint32_t i = 0; i < base_sizes[b]; i++) {
			const uint32_t id = (b == 3 && i == 5) ? 0u : unique_id(random, used);
			lamps.push_back(builder.add_lamp(make_lamp(random, point, normal, FACING_ANY, id)));
		}
		r_inputs.base_sets.push_back(builder.add_set(point, normal, lamps, 0, BASE_SEEDS, false));
		const Lamp added = make_lamp(random, point, normal, FACING_LIT, unique_id(random, used));
		LocalVector<uint32_t> grown(lamps);
		grown.insert(random.next() % (lamps.size() + 1), builder.add_lamp(added));
		r_inputs.added_sets.push_back(builder.add_set(point, normal, grown, 0, BASE_SEEDS, false));
		r_inputs.added_ids.push_back(added.id);
		for (uint32_t p = 0; p < PERMUTATIONS; p++) {
			LocalVector<uint32_t> order(lamps);
			for (uint32_t i = order.size(); i > 1; i--) {
				const uint32_t j = random.next() % i;
				SWAP(order[i - 1], order[j]);
			}
			r_inputs.permuted_sets.push_back(builder.add_set(point, normal, order, 0, PERMUTATION_SEEDS, false));
		}
	}

	{
		float point[3];
		float normal[3];
		random_place(random, point, normal);
		LocalVector<uint32_t> lamps;
		lamps.push_back(builder.add_lamp(make_lamp(random, point, normal, FACING_LIT, 0u)));
		r_inputs.id0_set = builder.add_set(point, normal, lamps, 0, ID0_SEEDS, false);
	}

	{
		float point[3];
		float normal[3];
		random_place(random, point, normal);
		LocalVector<uint32_t> used;
		const uint32_t id_a = unique_id(random, used);
		const uint32_t id_b = unique_id(random, used);
		Lamp first = make_lamp(random, point, normal, FACING_LIT, MIN(id_a, id_b));
		Lamp second = first;
		second.id = MAX(id_a, id_b);
		const uint32_t members[4] = {
			builder.add_lamp(first),
			builder.add_lamp(second),
			builder.add_lamp(make_lamp(random, point, normal, FACING_AWAY, unique_id(random, used))),
			builder.add_lamp(make_lamp(random, point, normal, FACING_AWAY, unique_id(random, used))),
		};
		r_inputs.tie_id = first.id;
		const uint32_t seed_first = r_inputs.seeds.size();
		const uint32_t hash_a = DiSamplerReference::hash(first.id);
		const uint32_t hash_b = DiSamplerReference::hash(second.id);
		for (uint32_t seed = 0; seed < TIE_SCAN_LIMIT && r_inputs.seeds.size() - seed_first < TIE_SEEDS; seed++) {
			const uint32_t seed_hash = DiSamplerReference::hash(seed);
			if ((DiSamplerReference::hash(seed_hash ^ hash_a) >> 9u) == (DiSamplerReference::hash(seed_hash ^ hash_b) >> 9u)) {
				r_inputs.seeds.push_back(seed);
			}
		}
		const uint32_t found = r_inputs.seeds.size() - seed_first;
		for (uint32_t order = 0; order < TIE_ORDERS; order++) {
			LocalVector<uint32_t> available;
			for (const uint32_t member : members) {
				available.push_back(member);
			}
			LocalVector<uint32_t> lamps;
			uint32_t code = order;
			for (uint32_t radix = 4; radix > 0; radix--) {
				const uint32_t chosen = code % radix;
				code /= radix;
				lamps.push_back(available[chosen]);
				available.remove_at(chosen);
			}
			r_inputs.tie_sets.push_back(builder.add_set(point, normal, lamps, seed_first, found, true));
		}
	}

	r_inputs.race_tables = r_inputs.tables.size();
	for (uint32_t t = 0; t < DiSamplerReference::RACE_TABLE_COUNT; t++) {
		uint32_t ids[DiSamplerReference::RACE_TABLE_LAMPS];
		for (uint32_t lamp = 0; lamp < DiSamplerReference::RACE_TABLE_LAMPS; lamp++) {
			ids[lamp] = DiSamplerReference::race_table_id(t, lamp);
		}
		add_table(r_inputs, DiSamplerReference::RACE_TABLE_WEIGHTS[t], ids, DiSamplerReference::RACE_TABLE_LAMPS, DiSamplerReference::RACE_TABLE_SEEDS, 0);
	}
	{
		const float targets[8] = { 0.0f, 0.5f, 0.0f, 1e-3f, 0.0f, 1.0f, 0.0f, 0.25f };
		LocalVector<uint32_t> used;
		uint32_t ids[8];
		for (uint32_t i = 0; i < 8; i++) {
			ids[i] = unique_id(random, used);
		}
		r_inputs.zero_table = add_table(r_inputs, targets, ids, 8, BASE_SEEDS, 0);
		r_inputs.empty_table = add_table(r_inputs, targets, ids, 0, 256, 256);
	}

	const uint32_t edge_ids[8] = { 0u, 1u, 0xFFFFFFFEu, random.next(), random.next(), random.next(), random.next(), random.next() };
	const float edge_targets[4] = { DiSamplerReference::MIN_TARGET, 1e-4f, 1.0f, 1000.0f };
	for (const uint32_t id : edge_ids) {
		for (uint32_t side = 0; side < 2; side++) {
			const uint32_t seed_hash = side == 0 ? DiSamplerReference::hash(id) : DiSamplerReference::hash_inverse(0xFFFFFFFFu) ^ DiSamplerReference::hash(id);
			for (const float target : edge_targets) {
				const Lab::Edge edge = { seed_hash, id, target, side == 0 ? 0u : DiSamplerReference::RACE_K_TOP };
				r_inputs.edges.push_back(edge);
			}
		}
	}
	return r_inputs.seeds.size() == TIE_SEEDS;
}

bool DirectSamplerLab::_ensure_shader() {
	if (shader != nullptr) {
		return shader_version.is_valid() && shader->version_is_valid(shader_version);
	}
	shader = memnew(DiSamplerTestShaderRD);
	Vector<String> modes;
	modes.push_back("\n#define KA8_SCREEN_SHAPE\n");
	modes.push_back("\n");
	shader->initialize(modes);
	shader_version = shader->version_create();
	return shader_version.is_valid() && shader->version_is_valid(shader_version);
}

bool DirectSamplerLab::_dispatch(const Inputs &p_inputs, Mutant p_mutant, int p_variant, bool p_all_legs, Outputs &r_outputs) {
	RD *rd = RD::get_singleton();
	const RID shader_rid = shader->version_get_shader(shader_version, p_variant);
	ERR_FAIL_COND_V(shader_rid.is_null(), false);
	const RID buffers[BINDINGS] = {
		upload(p_inputs.omni),
		upload(p_inputs.spot),
		upload(p_inputs.slots),
		upload(p_inputs.cells),
		upload(p_inputs.hit),
		upload(p_inputs.sets),
		upload(p_inputs.seeds),
		upload(p_inputs.tables),
		upload(p_inputs.weights),
		upload(p_inputs.edges),
		zeroed(uint64_t(p_inputs.rows) * sizeof(Row)),
		zeroed(uint64_t(p_inputs.hit.size()) * sizeof(LampOut)),
		zeroed(uint64_t(p_inputs.counts) * sizeof(uint32_t)),
		zeroed(uint64_t(p_inputs.table_rows) * sizeof(TableRow)),
		zeroed(uint64_t(p_inputs.edges.size()) * sizeof(EdgeOut)),
	};
	LocalVector<RD::Uniform> uniforms;
	for (uint32_t binding = 0; binding < BINDINGS; binding++) {
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_STORAGE_BUFFER, binding, buffers[binding]));
	}
	const RID uniform_set = rd->uniform_set_create(uniforms, shader_rid, 0);
	const RID pipeline = rd->compute_pipeline_create(shader_rid);
	const bool ready = uniform_set.is_valid() && pipeline.is_valid();
	if (ready) {
		RD::ComputeListID list = rd->compute_list_begin();
		rd->compute_list_bind_compute_pipeline(list, pipeline);
		rd->compute_list_bind_uniform_set(list, uniform_set, 0);
		for (uint32_t s = 0; s < p_inputs.sets.size(); s++) {
			dispatch_leg(rd, list, LEG_RECORDS, p_mutant, s, p_inputs.sets[s].seed_count);
		}
		if (p_all_legs) {
			for (uint32_t t = 0; t < p_inputs.tables.size(); t++) {
				dispatch_leg(rd, list, LEG_TABLES, p_mutant, t, p_inputs.tables[t].seed_count);
			}
			dispatch_leg(rd, list, LEG_EDGES, p_mutant, 0, p_inputs.edges.size());
		}
		rd->compute_list_end();
		r_outputs.rows = rd->buffer_get_data(buffers[BINDING_ROWS]);
		r_outputs.lamps = rd->buffer_get_data(buffers[BINDING_LAMPS]);
		r_outputs.counts = rd->buffer_get_data(buffers[BINDING_COUNTS]);
		r_outputs.table_rows = rd->buffer_get_data(buffers[BINDING_TABLE_ROWS]);
		r_outputs.edge_out = rd->buffer_get_data(buffers[BINDING_EDGE_OUT]);
	}
	if (uniform_set.is_valid()) {
		rd->free_rid(uniform_set);
	}
	if (pipeline.is_valid()) {
		rd->free_rid(pipeline);
	}
	for (const RID &buffer : buffers) {
		if (buffer.is_valid()) {
			rd->free_rid(buffer);
		}
	}
	return ready && uint64_t(r_outputs.rows.size()) >= uint64_t(p_inputs.rows) * sizeof(Row) && uint64_t(r_outputs.lamps.size()) >= uint64_t(p_inputs.hit.size()) * sizeof(LampOut);
}

void DirectSamplerLab::free_device_resources() {
	if (shader == nullptr) {
		return;
	}
	if (shader_version.is_valid()) {
		shader->version_free(shader_version);
		shader_version = RID();
	}
	memdelete(shader);
	shader = nullptr;
}

void DirectSamplerLab::serve() {
	const String request = GLOBAL_GET(DI_SAMPLER_LAB_SETTING("ka8_request"));
	if (request.is_empty() || request == served_request || RD::get_singleton() == nullptr) {
		return;
	}
	served_request = request;
	const Mutant mutant = Mutant(CLAMP(int(GLOBAL_GET(DI_SAMPLER_LAB_SETTING("ka8_mutant"))), 0, int(MUTANT_MAX) - 1));
	const String dir = request.get_base_dir();
	if (!dir.is_empty()) {
		DirAccess::make_dir_recursive_absolute(dir);
	}
	const Dictionary report = _run(request, mutant);
	Ref<FileAccess> json = FileAccess::open(request, FileAccess::WRITE);
	if (json.is_null()) {
		ERR_PRINT(vformat("The KA8 report %s could not be opened.", request));
		return;
	}
	json->store_string(JSON::stringify(report, "\t", true, true));
	json.unref();
	Ref<FileAccess> done = FileAccess::open(request + ".done", FileAccess::WRITE);
	if (done.is_valid()) {
		done->store_string(bool(report.get("ok", false)) ? "ok\n" : "failed\n");
	}
}
