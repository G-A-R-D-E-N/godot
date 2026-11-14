#define DI_SAMPLER_PI 3.14159265359
#define DI_SAMPLER_NO_PICK 0xFFFFFFFFu
#define DI_MIN_TARGET 5.421011e-20

const vec3 di_luminance_weights = vec3(0.2126, 0.7152, 0.0722);

uint di_hash(uint p_value) {
	p_value ^= p_value >> 16u;
	p_value *= 0x7feb352du;
	p_value ^= p_value >> 15u;
	p_value *= 0x846ca68bu;
	p_value ^= p_value >> 16u;
	return p_value;
}

float di_luminance(vec3 p_color) {
	return dot(p_color, di_luminance_weights);
}

float di_window(float p_distance, float p_inv_range, float p_decay) {
	float nd = p_distance * p_inv_range;
	nd *= nd;
	nd *= nd;
	nd = max(1.0 - nd, 0.0);
	nd *= nd;
	return nd * pow(max(p_distance, 0.0001), -p_decay);
}

float di_spot_cone(vec3 p_to_light, vec3 p_direction, float p_cos_angle, float p_cone_attenuation) {
	float spot_cos = max(dot(-p_to_light, p_direction), p_cos_angle);
	float rim = max(1e-4, (1.0 - spot_cos) / (1.0 - p_cos_angle));
	return 1.0 - pow(rim, p_cone_attenuation);
}

struct DiGeometry {
	vec3 to_light;
	float distance;
	float term;
};

DiGeometry di_geometry(vec3 p_lamp, float p_inv_radius, float p_decay, bool p_spot, vec3 p_direction, float p_cos_angle, float p_cone_attenuation, vec3 p_point) {
	DiGeometry geometry;
	vec3 rel = p_lamp - p_point;
	geometry.distance = length(rel);
	geometry.to_light = rel / max(geometry.distance, 1e-5);
	geometry.term = di_window(geometry.distance, p_inv_radius, p_decay);
	if (p_spot && geometry.term > 0.0) {
		geometry.term *= di_spot_cone(geometry.to_light, p_direction, p_cos_angle, p_cone_attenuation);
	}
	return geometry;
}

bool di_in_set(DiGeometry p_geometry, vec3 p_normal) {
	return p_geometry.term > 0.0 && dot(p_normal, p_geometry.to_light) > 0.0;
}

vec2 di_oct_encode(vec3 p_normal) {
	vec2 folded = p_normal.xy / (abs(p_normal.x) + abs(p_normal.y) + abs(p_normal.z));
	return p_normal.z >= 0.0 ? folded : (1.0 - abs(folded.yx)) * vec2(folded.x >= 0.0 ? 1.0 : -1.0, folded.y >= 0.0 ? 1.0 : -1.0);
}

vec3 di_oct_decode(vec2 p_oct) {
	vec3 normal = vec3(p_oct, 1.0 - abs(p_oct.x) - abs(p_oct.y));
	float fold = max(-normal.z, 0.0);
	normal.x += normal.x >= 0.0 ? -fold : fold;
	normal.y += normal.y >= 0.0 ? -fold : fold;
	return normalize(normal);
}

float di_surface_response(vec3 p_to_light, vec3 p_normal, vec3 p_view, float p_roughness, float p_specular, bool p_lambert) {
	float n_dot_l = dot(p_normal, p_to_light);
	if (n_dot_l <= 0.0) {
		return 0.0;
	}
	if (p_lambert) {
		return n_dot_l / DI_SAMPLER_PI;
	}
	vec3 h = normalize(p_to_light + p_view);
	float n_dot_h = max(dot(p_normal, h), 0.0);
	float n_dot_v = max(dot(p_normal, p_view), 1e-3);
	float v_dot_h = clamp(dot(p_view, h), 0.0, 1.0);
	float alpha = max(p_roughness * p_roughness, 1e-3);
	float alpha2 = alpha * alpha;
	float d = n_dot_h * n_dot_h * (alpha2 - 1.0) + 1.0;
	d = alpha2 / (DI_SAMPLER_PI * d * d);
	float k = alpha * 0.5;
	float g = 0.25 / ((n_dot_l * (1.0 - k) + k) * (n_dot_v * (1.0 - k) + k));
	float f = 0.04 + 0.96 * pow(1.0 - v_dot_h, 5.0);
	return n_dot_l * (1.0 / DI_SAMPLER_PI + d * g * f * p_specular);
}

vec3 di_screen_target(vec3 p_color, DiGeometry p_geometry, vec3 p_normal, vec3 p_view, float p_roughness, float p_specular) {
	if (p_geometry.term <= 0.0) {
		return vec3(0.0);
	}
	return p_color * (p_geometry.term * di_surface_response(p_geometry.to_light, p_normal, p_view, p_roughness, p_specular, false));
}

float di_resample_weight(vec3 p_target, float p_prior, float p_floor) {
	return di_luminance(p_target) * max(p_prior, p_floor);
}

bool di_candidate(float p_target) {
	return p_target >= DI_MIN_TARGET && !isinf(p_target);
}

float di_weight(float p_total, float p_pick_target) {
	return p_pick_target > 0.0 ? p_total / p_pick_target : 0.0;
}

struct DiInversion {
	float goal;
	float running;
	uint pick;
	float pick_target;
	uint last;
	float last_target;
};

DiInversion di_inversion_begin(float p_total, float p_u) {
	DiInversion inversion;
	inversion.goal = p_u * p_total;
	inversion.running = 0.0;
	inversion.pick = DI_SAMPLER_NO_PICK;
	inversion.pick_target = 0.0;
	inversion.last = DI_SAMPLER_NO_PICK;
	inversion.last_target = 0.0;
	return inversion;
}

void di_inversion_step(inout DiInversion r_inversion, float p_target, uint p_index) {
	if (!di_candidate(p_target)) {
		return;
	}
	r_inversion.running += p_target;
	r_inversion.last = p_index;
	r_inversion.last_target = p_target;
	if (r_inversion.pick == DI_SAMPLER_NO_PICK && r_inversion.running > r_inversion.goal) {
		r_inversion.pick = p_index;
		r_inversion.pick_target = p_target;
	}
}

uint di_inversion_end(inout DiInversion r_inversion) {
	if (r_inversion.pick == DI_SAMPLER_NO_PICK && r_inversion.last != DI_SAMPLER_NO_PICK) {
		r_inversion.pick = r_inversion.last;
		r_inversion.pick_target = r_inversion.last_target;
	}
	return r_inversion.pick;
}

float di_precise_divide(float p_a, float p_b) {
	precise float quotient = p_a / p_b;
	precise float residual = fma(-quotient, p_b, p_a);
	precise float corrected = fma(residual, 1.0 / p_b, quotient);
	return corrected;
}

float di_race_exponential(uint p_seed_hash, uint p_id) {
	uint k = di_hash(p_seed_hash ^ di_hash(p_id)) >> 9u;
	if (k < 4194304u) {
		return -log((float(k) + 0.5) * exp2(-23.0));
	}
	precise float v = (8388607.5 - float(k)) * exp2(-23.0);
	precise float t = v / (2.0 - v);
	precise float t2 = t * t;
	precise float series = 1.0 / 13.0;
	series = fma(series, t2, 1.0 / 11.0);
	series = fma(series, t2, 1.0 / 9.0);
	series = fma(series, t2, 1.0 / 7.0);
	series = fma(series, t2, 1.0 / 5.0);
	series = fma(series, t2, 1.0 / 3.0);
	series = fma(series, t2, 1.0);
	return 2.0 * t * series;
}

struct DiRace {
	uint seed_hash;
	float key;
	uint id;
	uint pick;
	float pick_target;
	float total;
};

DiRace di_race_begin(uint p_seed) {
	DiRace race;
	race.seed_hash = di_hash(p_seed);
	race.key = uintBitsToFloat(0x7f800000u);
	race.id = 0xFFFFFFFFu;
	race.pick = DI_SAMPLER_NO_PICK;
	race.pick_target = 0.0;
	race.total = 0.0;
	return race;
}

void di_race_step(inout DiRace r_race, float p_target, uint p_id, uint p_index) {
	if (!di_candidate(p_target)) {
		return;
	}
	r_race.total += p_target;
	float key = di_precise_divide(di_race_exponential(r_race.seed_hash, p_id), p_target);
	if (key < r_race.key || (key == r_race.key && p_id < r_race.id)) {
		r_race.key = key;
		r_race.id = p_id;
		r_race.pick = p_index;
		r_race.pick_target = p_target;
	}
}

struct DiList {
	uint seed;
	uint pick;
	float pick_target;
	float total;
};

DiList di_list_begin(uint p_seed) {
	DiList list;
	list.seed = p_seed;
	list.pick = DI_SAMPLER_NO_PICK;
	list.pick_target = 0.0;
	list.total = 0.0;
	return list;
}

void di_list_step(inout DiList r_list, float p_target, uint p_position, uint p_index) {
	if (p_target <= 0.0) {
		return;
	}
	r_list.total += p_target;
	if (float(di_hash(r_list.seed ^ (p_position * 0x9e3779b9u)) >> 8u) * r_list.total < p_target * 16777216.0) {
		r_list.pick = p_index;
		r_list.pick_target = p_target;
	}
}

#ifdef DI_SAMPLER_HIT_ACCESSOR
uint di_hit_lamp_count();
bool di_hit_lamp(uint p_index, vec3 p_point, vec3 p_normal, out float r_target, out uint r_id);

uint di_hit_pick(vec3 p_point, vec3 p_normal, uint p_seed, bool p_race, out float r_weight) {
	uint count = di_hit_lamp_count();
	if (p_race) {
		DiRace race = di_race_begin(p_seed);
		for (uint i = 0u; i < count; i++) {
			float target;
			uint id;
			if (di_hit_lamp(i, p_point, p_normal, target, id)) {
				di_race_step(race, target, id, i);
			}
		}
		r_weight = di_weight(race.total, race.pick_target);
		return race.pick;
	}
	DiList list = di_list_begin(p_seed);
	for (uint i = 0u; i < count; i++) {
		float target;
		uint id;
		if (di_hit_lamp(i, p_point, p_normal, target, id)) {
			di_list_step(list, target, i, i);
		}
	}
	r_weight = di_weight(list.total, list.pick_target);
	return list.pick;
}
#endif
