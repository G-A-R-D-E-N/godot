#[compute]

#version 450

#VERSION_DEFINES

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

#include "../../../servers/rendering/renderer_rd/shaders/light_data_inc.glsl"

#define DI_SAMPLER_HIT_ACCESSOR
#include "../../../servers/rendering/renderer_rd/shaders/environment/di_sampler_inc.glsl"

#define KA8_LEG_RECORDS 0u
#define KA8_LEG_TABLES 1u
#define KA8_LEG_EDGES 2u

#define KA8_MUTANT_NONE 0u
#define KA8_MUTANT_INDEX_KEY 1u
#define KA8_MUTANT_LOOP_TIES 2u
#define KA8_MUTANT_K24 3u
#define KA8_MUTANT_NO_LOG 4u
#define KA8_MUTANT_HIT_STRATA 5u

#define KA8_CELL_SPOT 0x80000000u
#define KA8_LIGHT_SPOT 2u
#define KA8_STRATA 32u

struct Ka8Slot {
	vec3 color;
	float energy;
	uint id;
	uint pad0;
	uint pad1;
	uint pad2;
};

struct Ka8HitLight {
	vec3 color;
	float energy;
	vec3 direction;
	uint type;
	vec3 position;
	float attenuation;
	float cos_spot_angle;
	float inv_spot_attenuation;
	float inv_radius;
	uint caster_mask;
	vec3 half_a;
	float area;
	vec3 half_b;
	uint pad;
};

struct Ka8Set {
	vec4 point;
	vec4 normal;
	uint first;
	uint count;
	uint seed_first;
	uint seed_count;
	uint seed_list;
	uint row_first;
	uint pad0;
	uint pad1;
};

struct Ka8Table {
	uint first;
	uint count;
	uint count_first;
	uint row_first;
	uint row_count;
	uint seed_count;
	uint pad0;
	uint pad1;
};

struct Ka8Weight {
	float target;
	uint id;
};

struct Ka8Edge {
	uint seed_hash;
	uint id;
	float target;
	uint pad;
};

struct Ka8Row {
	uint race_pick;
	uint race_id;
	float race_w;
	float race_key;
	uint list_pick;
	float list_w;
	uint today_pick;
	float today_w;
	uint today_count;
	uint loop_pick;
	uint pad0;
	uint pad1;
};

struct Ka8LampOut {
	float term;
	float target;
	uint id;
	uint in_set;
};

struct Ka8TableRow {
	uint pick;
	float weight;
};

struct Ka8EdgeOut {
	float exponential;
	float key;
	uint k;
	uint pad;
};

layout(set = 0, binding = 0, std430) restrict readonly buffer Ka8Omni {
	LightData data[];
}
ka8_omni;

layout(set = 0, binding = 1, std430) restrict readonly buffer Ka8Spot {
	LightData data[];
}
ka8_spot;

layout(set = 0, binding = 2, std430) restrict readonly buffer Ka8Slots {
	Ka8Slot data[];
}
ka8_slots;

layout(set = 0, binding = 3, std430) restrict readonly buffer Ka8Cells {
	uint data[];
}
ka8_cells;

layout(set = 0, binding = 4, std430) restrict readonly buffer Ka8HitLights {
	Ka8HitLight data[];
}
ka8_hit;

layout(set = 0, binding = 5, std430) restrict readonly buffer Ka8Sets {
	Ka8Set data[];
}
ka8_sets;

layout(set = 0, binding = 6, std430) restrict readonly buffer Ka8Seeds {
	uint data[];
}
ka8_seeds;

layout(set = 0, binding = 7, std430) restrict readonly buffer Ka8Tables {
	Ka8Table data[];
}
ka8_tables;

layout(set = 0, binding = 8, std430) restrict readonly buffer Ka8Weights {
	Ka8Weight data[];
}
ka8_weights;

layout(set = 0, binding = 9, std430) restrict readonly buffer Ka8Edges {
	Ka8Edge data[];
}
ka8_edges;

layout(set = 0, binding = 10, std430) restrict writeonly buffer Ka8Rows {
	Ka8Row data[];
}
ka8_rows;

layout(set = 0, binding = 11, std430) restrict writeonly buffer Ka8Lamps {
	Ka8LampOut data[];
}
ka8_lamps;

layout(set = 0, binding = 12, std430) restrict buffer Ka8Counts {
	uint data[];
}
ka8_counts;

layout(set = 0, binding = 13, std430) restrict writeonly buffer Ka8TableRows {
	Ka8TableRow data[];
}
ka8_table_rows;

layout(set = 0, binding = 14, std430) restrict writeonly buffer Ka8EdgeOuts {
	Ka8EdgeOut data[];
}
ka8_edge_out;

layout(push_constant, std430) uniform Params {
	uint leg;
	uint mutant;
	uint index;
	uint count;
}
params;

const vec3 ka8_today_luminance = vec3(0.2126, 0.7152, 0.0722);

uint ka8_first = 0u;
uint ka8_count = 0u;

bool ka8_lamp(uint p_index, vec3 p_point, vec3 p_normal, out DiGeometry r_geometry, out float r_target, out uint r_id) {
#ifdef KA8_SCREEN_SHAPE
	uint cell = ka8_cells.data[ka8_first + p_index];
	bool spot = (cell & KA8_CELL_SPOT) != 0u;
	uint record = cell & ~KA8_CELL_SPOT;
	LightData light = spot ? ka8_spot.data[record] : ka8_omni.data[record];
	Ka8Slot slot = ka8_slots.data[(light.flags >> 1) & 0xFFFFu];
	r_id = slot.id;
	r_geometry = di_geometry(light.position, light.inv_radius, light.attenuation, spot, light.direction, light.cone_angle, light.cone_attenuation, p_point);
	vec3 color = slot.color;
	float energy = slot.energy;
	bool listed = (light.flags & 1u) != 0u;
#else
	Ka8HitLight light = ka8_hit.data[ka8_first + p_index];
	bool spot = light.type == KA8_LIGHT_SPOT;
	r_id = light.pad;
	r_geometry = di_geometry(light.position, light.inv_radius, light.attenuation, spot, light.direction, light.cos_spot_angle, light.inv_spot_attenuation, p_point);
	vec3 color = light.color;
	float energy = light.energy;
	bool listed = true;
#endif
	r_target = 0.0;
	if (!listed || !di_in_set(r_geometry, p_normal)) {
		return false;
	}
	float n_dot_l = dot(p_normal, r_geometry.to_light);
	r_target = di_luminance(color * (energy * r_geometry.term * n_dot_l / DI_SAMPLER_PI));
	return true;
}

uint di_hit_lamp_count() {
	return ka8_count;
}

bool di_hit_lamp(uint p_index, vec3 p_point, vec3 p_normal, out float r_target, out uint r_id) {
	DiGeometry geometry;
	return ka8_lamp(p_index, p_point, p_normal, geometry, r_target, r_id);
}

float ka8_exponential(uint p_seed_hash, uint p_id) {
	if (params.mutant == KA8_MUTANT_K24) {
		uint k = di_hash(p_seed_hash ^ di_hash(p_id)) >> 8u;
		return -log((float(k) + 0.5) * exp2(-24.0));
	}
	return di_race_exponential(p_seed_hash, p_id);
}

void ka8_race_step(inout DiRace r_race, float p_target, uint p_id, uint p_index) {
	if (params.mutant == KA8_MUTANT_NONE) {
		di_race_step(r_race, p_target, p_id, p_index);
		return;
	}
	if (!di_candidate(p_target)) {
		return;
	}
	r_race.total += p_target;
	float key;
	if (params.mutant == KA8_MUTANT_NO_LOG) {
		uint k = di_hash(r_race.seed_hash ^ di_hash(p_id)) >> 9u;
		key = di_precise_divide((float(k) + 0.5) * exp2(-23.0), p_target);
	} else {
		key = di_precise_divide(ka8_exponential(r_race.seed_hash, params.mutant == KA8_MUTANT_INDEX_KEY ? p_index : p_id), p_target);
	}
	bool wins = params.mutant == KA8_MUTANT_LOOP_TIES ? key < r_race.key : (key < r_race.key || (key == r_race.key && p_id < r_race.id));
	if (wins) {
		r_race.key = key;
		r_race.id = p_id;
		r_race.pick = p_index;
		r_race.pick_target = p_target;
	}
}

DiRace ka8_race(vec3 p_point, vec3 p_normal, uint p_seed) {
	DiRace race = di_race_begin(p_seed);
	for (uint i = 0u; i < ka8_count; i++) {
		float target;
		uint id;
		if (di_hit_lamp(i, p_point, p_normal, target, id)) {
			ka8_race_step(race, target, id, i);
		}
	}
	return race;
}

uint ka8_strata_pick(vec3 p_point, vec3 p_normal, uint p_seed, out float r_weight) {
	uint total_count = 0u;
	for (uint i = 0u; i < ka8_count; i++) {
		float target;
		uint id;
		if (di_hit_lamp(i, p_point, p_normal, target, id)) {
			total_count++;
		}
	}
	DiList list = di_list_begin(p_seed);
	float pick_target = 0.0;
	uint position = 0u;
	for (uint i = 0u; i < ka8_count; i++) {
		float target;
		uint id;
		if (!di_hit_lamp(i, p_point, p_normal, target, id)) {
			continue;
		}
		uint stratum = ((position + 1u) * KA8_STRATA - 1u) / total_count;
		uint stratum_first = (stratum * total_count) / KA8_STRATA;
		uint stratum_size = ((stratum + 1u) * total_count) / KA8_STRATA - stratum_first;
		uint drawn = stratum_first + di_hash(p_seed ^ di_hash(stratum + 0x3c6ef372u)) % stratum_size;
		if (position == drawn) {
			di_list_step(list, target * float(stratum_size), stratum, i);
			if (list.pick == i) {
				pick_target = target;
			}
		}
		position++;
	}
	r_weight = pick_target > 0.0 ? list.total / pick_target : 0.0;
	return list.pick;
}

uint ka8_today_hash(uint p_value) {
	p_value ^= p_value >> 16u;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15u;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16u;
	return p_value;
}

float ka8_today_attenuation(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(p_distance, 0.0001), -p_decay);
}

uint ka8_today(vec3 p_point, vec3 p_normal, uint p_seed, out float r_weight, out uint r_count) {
	uint chosen = DI_SAMPLER_NO_PICK;
	float chosen_weight = 0.0;
	float total_weight = 0.0;
	uint local_count = 0u;
	for (uint i = 0u; i < ka8_count; i++) {
		Ka8HitLight light = ka8_hit.data[ka8_first + i];
		vec3 relative = light.position - p_point;
		float distance = length(relative);
		vec3 to_light = relative / max(distance, 1e-4);
		float attenuation = ka8_today_attenuation(distance, light.inv_radius, light.attenuation);
		if (light.type == KA8_LIGHT_SPOT) {
			float spot_cos = max(dot(-to_light, light.direction), light.cos_spot_angle);
			float spot_rim = max(1e-4, (1.0 - spot_cos) / (1.0 - light.cos_spot_angle));
			attenuation *= 1.0 - pow(spot_rim, light.inv_spot_attenuation);
		}
		float n_dot_l = dot(p_normal, to_light);
		if (n_dot_l <= 0.0 || attenuation <= 0.0) {
			continue;
		}
		vec3 light_value = light.color * (light.energy * attenuation * n_dot_l / 3.14159265359);
		float weight = dot(light_value, ka8_today_luminance);
		if (weight <= 0.0) {
			continue;
		}
		total_weight += weight;
		local_count++;
		if (float(ka8_today_hash(p_seed ^ (i * 0x9e3779b9u)) >> 8u) * total_weight < weight * 16777216.0) {
			chosen = i;
			chosen_weight = weight;
		}
	}
	r_count = local_count;
	r_weight = chosen_weight > 0.0 ? total_weight / chosen_weight : 0.0;
	return chosen;
}

void ka8_run_record(uint p_row) {
	Ka8Set record_set = ka8_sets.data[params.index];
	ka8_first = record_set.first;
	ka8_count = record_set.count;
	vec3 point = record_set.point.xyz;
	vec3 normal = record_set.normal.xyz;
	uint seed = record_set.seed_list != 0u ? ka8_seeds.data[record_set.seed_first + p_row] : record_set.seed_first + p_row;

	DiRace race = ka8_race(point, normal, seed);
	Ka8Row row;
	row.loop_pick = race.pick;
	row.race_id = race.id;
	row.race_key = race.key;
	if (params.mutant == KA8_MUTANT_NONE) {
		row.race_pick = di_hit_pick(point, normal, seed, true, row.race_w);
	} else {
		row.race_pick = race.pick;
		row.race_w = di_weight(race.total, race.pick_target);
	}
	if (params.mutant == KA8_MUTANT_HIT_STRATA) {
		row.list_pick = ka8_strata_pick(point, normal, seed, row.list_w);
	} else {
		row.list_pick = di_hit_pick(point, normal, seed, false, row.list_w);
	}
	row.today_pick = ka8_today(point, normal, seed, row.today_w, row.today_count);
	row.pad0 = 0u;
	row.pad1 = 0u;
	ka8_rows.data[record_set.row_first + p_row] = row;

	if (p_row == 0u) {
		for (uint i = 0u; i < ka8_count; i++) {
			DiGeometry geometry;
			Ka8LampOut lamp;
			lamp.in_set = ka8_lamp(i, point, normal, geometry, lamp.target, lamp.id) ? 1u : 0u;
			lamp.term = geometry.term;
			ka8_lamps.data[ka8_first + i] = lamp;
		}
	}
}

void ka8_run_table(uint p_seed) {
	Ka8Table table = ka8_tables.data[params.index];
	DiRace race = di_race_begin(p_seed);
	for (uint i = 0u; i < table.count; i++) {
		Ka8Weight entry = ka8_weights.data[table.first + i];
		ka8_race_step(race, entry.target, entry.id, i);
	}
	uint slot = race.pick == DI_SAMPLER_NO_PICK ? table.count : race.pick;
	atomicAdd(ka8_counts.data[table.count_first + slot], 1u);
	if (p_seed < table.row_count) {
		Ka8TableRow row;
		row.pick = race.pick;
		row.weight = di_weight(race.total, race.pick_target);
		ka8_table_rows.data[table.row_first + p_seed] = row;
	}
}

void ka8_run_edge(uint p_row) {
	Ka8Edge entry = ka8_edges.data[p_row];
	Ka8EdgeOut result;
	result.exponential = ka8_exponential(entry.seed_hash, entry.id);
	result.key = di_precise_divide(result.exponential, entry.target);
	result.k = di_hash(entry.seed_hash ^ di_hash(entry.id)) >> 9u;
	result.pad = 0u;
	ka8_edge_out.data[p_row] = result;
}

void main() {
	uint invocation = gl_GlobalInvocationID.x;
	if (invocation >= params.count) {
		return;
	}
	if (params.leg == KA8_LEG_RECORDS) {
		ka8_run_record(invocation);
	} else if (params.leg == KA8_LEG_TABLES) {
		ka8_run_table(invocation);
	} else {
		ka8_run_edge(invocation);
	}
}
