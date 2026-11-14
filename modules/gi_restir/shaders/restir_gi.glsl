#[compute]

#version 450

#VERSION_DEFINES

#extension GL_EXT_samplerless_texture_functions : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

#ifdef RESTIR_RAYS
#define RAY_SCENE_SET 1
#include "restir_core_ray_scene_inc.glsl"
#endif
#include "restir_core_rtgi_inc.glsl"

#if defined(RESTIR_RAYS) && (defined(MODE_FINAL) || defined(MODE_SOURCE) || defined(MODE_VALIDATE))
#define RESTIR_SHADE
#define DDGI_SET 2
#define DDGI_UBO_BINDING 0
#define DDGI_SAMPLING
layout(set = 2, binding = 3) uniform utexture2DArray ddgi_probe_texture;
layout(set = 2, binding = 4) uniform texture2DArray ddgi_irradiance_texture;
layout(set = 2, binding = 5) uniform texture2DArray ddgi_distance_texture;
layout(set = 2, binding = 6) uniform sampler ddgi_linear_sampler;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 2, binding = 7) uniform texture2DArray sky_radiance;
#else
layout(set = 2, binding = 7) uniform texture2D sky_radiance;
#endif
layout(set = 2, binding = 8) uniform sampler sky_sampler;
#include "restir_core_ddgi_inc.glsl"
#endif

#include "restir_gi_inc.glsl"

#ifdef RESTIR_SHADE
#define RS_SKY_COLOR 1u
#define RS_SKY_SKY 2u
#define RS_SKY_SIGN 4u

vec3 rs_sky_radiance(vec3 p_direction) {
	if ((rf.flags & F_REAL_SKY) == 0u) {
		return min(rf.test_sky.xyz, vec3(RS_LO_CLAMP));
	}
	vec3 radiance = vec3(0.0);
	if ((rf.shade_sky_flags & RS_SKY_SKY) != 0u) {
		float sky_sign = (rf.shade_sky_flags & RS_SKY_SIGN) != 0u ? 1.0 : -1.0;
		vec4 sky_quat = vec4(rf.shade_sky, sky_sign * sqrt(max(0.0, 1.0 - dot(rf.shade_sky, rf.shade_sky))));
		vec3 sky_dir = cross(sky_quat.xyz, p_direction);
		sky_dir = p_direction + ((sky_dir * sky_quat.w) + cross(sky_quat.xyz, sky_dir)) * 2.0;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
		radiance = textureLod(sampler2DArray(sky_radiance, sky_sampler), vec3(vec3_to_oct_with_border(sky_dir, rf.shade_sky_border), 0.0), 2.0).rgb * rf.shade_sky_energy;
#else
		radiance = textureLod(sampler2D(sky_radiance, sky_sampler), vec3_to_oct_with_border(sky_dir, rf.shade_sky_border), 2.0).rgb * rf.shade_sky_energy;
#endif
	} else if ((rf.shade_sky_flags & RS_SKY_COLOR) != 0u) {
		radiance = rf.shade_sky * rf.shade_sky_energy;
	}
	return min(radiance, vec3(RS_LO_CLAMP));
}
#endif

#if defined(MODE_TEMPORAL) || defined(MODE_SPATIAL)

#ifdef MODE_TEMPORAL
bool rs_segment_hits_box(vec3 p_from, vec3 p_to, vec3 p_min, vec3 p_max) {
	vec3 delta = p_to - p_from;
	float t_enter = 0.0;
	float t_exit = 1.0;
	for (int axis = 0; axis < 3; axis++) {
		if (abs(delta[axis]) < 1e-9) {
			if (p_from[axis] < p_min[axis] || p_from[axis] > p_max[axis]) {
				return false;
			}
		} else {
			float inverse = 1.0 / delta[axis];
			float ta = (p_min[axis] - p_from[axis]) * inverse;
			float tb = (p_max[axis] - p_from[axis]) * inverse;
			t_enter = max(t_enter, min(ta, tb));
			t_exit = min(t_exit, max(ta, tb));
			if (t_enter > t_exit) {
				return false;
			}
		}
	}
	return true;
}

bool rs_dirty_hit(Receiver p_q, Reservoir p_h) {
	vec3 from = p_q.pos;
	vec3 to = from;
	if ((p_h.flags & RF_VALID) != 0u) {
		to = rs_is_sky(p_h) ? from + p_h.xs * rf.sky_distance : p_h.xs;
	}
	uint count = min(rf.dirty_count, 64u);
	for (uint i = 0u; i < count; i++) {
		vec3 lo = dirty.box_min[i].xyz - vec3(rf.dirty_margin);
		vec3 hi = dirty.box_max[i].xyz + vec3(rf.dirty_margin);
		if (rs_segment_hits_box(from, to, lo, hi)) {
			return true;
		}
	}
	return false;
}
#else
bool rs_dirty_hit(Receiver p_q, Reservoir p_h) {
	return false;
}
#endif

void rs_fill_neighbour(Reservoir p_sample, float p_m, bool p_usable, uint p_dead_reason, bool p_dirty, Receiver p_q, Receiver p_r, Cand p_canonical, out Cand r_cand, out uint r_reason) {
	r_cand = rs_cand_none();
	r_reason = REJ_NONE;
	bool unbiased = (rf.flags & F_UNBIASED) != 0u;
	r_cand.present = true;
	r_cand.s = p_sample;
	r_cand.m = p_m;
	g_stats[PS_PRESENT] += 1.0;
	if (unbiased) {
		float a_at_c = 0.0;
		if (p_canonical.on && p_usable) {
			uint reason_c;
			uint kind_c;
			float jac_c = rs_shift_jacobian(p_canonical.s, p_r.origin, p_q.origin, reason_c, kind_c);
			if (jac_c > 0.0) {
				float phat_q = rs_phat(p_canonical.s, p_q.origin, p_q.nrm);
				if (phat_q > 0.0 && !rs_dirty_hit(p_q, p_canonical.s) && ((rf.flags & F_VIS) == 0u || rs_visible(p_q.origin, p_canonical.s))) {
					a_at_c = p_m * phat_q * jac_c;
				}
			}
		}
		r_cand.a_at_c = a_at_c;
	}
	if (!p_usable) {
		r_reason = p_dead_reason;
		g_stats[PS_ZERO_WEIGHT] += 1.0;
		return;
	}
	uint reason;
	uint kind;
	float jac = rs_shift_jacobian(p_sample, p_q.origin, p_r.origin, reason, kind);
	float phat_r = rs_phat(p_sample, p_r.origin, p_r.nrm);
	float energy = min(p_m * phat_r * p_sample.w * (jac > 0.0 ? jac : rs_jacobian_raw(p_sample, p_q.origin, p_r.origin)), 1e9);
	if (isnan(energy) || isinf(energy)) {
		energy = 0.0;
	}
	g_stats[PS_TESTED] += 1.0;
	g_stats[PS_ENERGY_ALL] += energy;
	if (!(jac > 0.0)) {
		r_reason = reason;
		g_stats[PS_ENERGY_TRUNC] += energy;
		g_stats[kind == KIND_DIST ? PS_REJ_DIST : (kind == KIND_COS ? PS_REJ_COS : PS_REJ_JAC)] += 1.0;
		return;
	}
	if (unbiased && p_dirty) {
		r_reason = REJ_DIRTY;
		return;
	}
	if (unbiased && (rf.flags & F_VIS) != 0u && !rs_visible(p_q.origin, p_sample)) {
		r_reason = REJ_VIS;
		return;
	}
	r_cand.on = true;
	r_cand.jac = jac;
	bool test_vis = (rf.flags & F_VIS) != 0u && ((rf.flags & F_VIS_REUSE) != 0u || unbiased);
	if (phat_r > 0.0 && test_vis && !rs_visible(p_r.origin, p_sample)) {
		phat_r = 0.0;
		r_reason = REJ_VIS;
		if (!unbiased) {
			r_cand.on = false;
		}
	}
	r_cand.phat_r = phat_r;
	if (unbiased) {
		r_cand.a_self = p_m * rs_phat(p_sample, p_q.origin, p_q.nrm) / jac;
	}
}

bool rs_similar(Receiver p_r, Receiver p_n, out uint r_reason) {
	r_reason = REJ_NONE;
	if (!p_n.valid) {
		r_reason = REJ_PLANE;
		return false;
	}
	if (abs(dot(p_r.nrm, p_n.pos - p_r.pos)) / p_r.depth >= rf.tol_plane) {
		r_reason = REJ_PLANE;
		return false;
	}
	if (dot(p_r.nrm, p_n.nrm) <= rf.cos_normal) {
		r_reason = REJ_NORMAL;
		return false;
	}
	if (abs(p_r.rough - p_n.rough) > rf.class_tol) {
		r_reason = REJ_CLASS;
		return false;
	}
	return true;
}

void rs_stats_store() {
	if ((rf.flags & F_PROBE_STATS) == 0u) {
		return;
	}
	uint local = gl_LocalInvocationIndex;
	for (int k = 0; k < PS_STRIDE; k++) {
		partial_ps[local * uint(PS_STRIDE) + uint(k)] = g_stats[k];
	}
	barrier();
	if (local == 0u) {
		uint groups_x = (uint(rf.trace_size.x) + 7u) / 8u;
		uint groups_y = (uint(rf.trace_size.y) + 7u) / 8u;
		uint group = gl_WorkGroupID.y * groups_x + gl_WorkGroupID.x;
		uint base = (rf.pass_index * groups_x * groups_y + group) * uint(PS_STRIDE);
		for (int k = 0; k < PS_STRIDE; k++) {
			float sum = 0.0;
			for (uint t = 0u; t < 64u; t++) {
				sum += partial_ps[t * uint(PS_STRIDE) + uint(k)];
			}
			pass_stats.data[base + uint(k)] = sum;
		}
	}
}

void rs_stats_clear() {
	for (int k = 0; k < PS_STRIDE; k++) {
		g_stats[k] = 0.0;
	}
}

Reservoir rs_finish(Reservoir p_result, int p_sel, float p_total_m, Receiver p_r, uint p_source_flag, uint p_age_inc) {
	Reservoir result = p_result;
	result.m = uint(min(p_total_m, float(rf.m_max)));
	result.depth = p_r.depth;
	result.rnormal = rs_pack_normal(p_r.nrm);
	result.epoch = rf.epoch_tag;
	if (p_sel > 0) {
		result.flags |= p_source_flag;
		result.age = min(result.age + p_age_inc, 65535u);
	}
	return result;
}

#endif

#ifdef MODE_CLEAR

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, rf.trace_size))) {
		return;
	}
	rs_store(px, rs_empty());
	imageStore(vis_state, px, uvec4(0u));
	imageStore(info_image, px, uvec4(0u));
}

#endif

#ifdef MODE_TEMPORAL

Reservoir rs_from_candidate(ivec2 p_pixel, Receiver p_r) {
	uvec4 first = texelFetch(cand_first, p_pixel, 0);
	uvec4 second = texelFetch(cand_second, p_pixel, 0);
	uint cflags = rtgi_candidate_flags(second);
	Reservoir res = rs_empty();
	if ((rf.flags & F_REVAL_FRAME) != 0u) {
		return res;
	}
	if ((cflags & RTGI_FLAG_NO_CANDIDATE) != 0u) {
		return res;
	}
	vec3 lo = rtgi_candidate_radiance(second);
	float pdf = uintBitsToFloat(second.z);
	if (!rs_finite3(lo)) {
		g_stats[PS_LO_REFUSED] += 1.0;
		res.m = 1u;
		return res;
	}
	if (!(pdf > 0.0) || isinf(pdf)) {
		g_stats[PS_PDF_REFUSED] += 1.0;
		res.m = 1u;
		return res;
	}
	res.lo = clamp(lo, vec3(0.0), vec3(RS_LO_CLAMP));
	res.flags = RF_VALID;
	if ((cflags & RTGI_FLAG_MISS_SKY) != 0u) {
		res.flags |= RF_SKY;
		res.xs = normalize(uintBitsToFloat(first.xyz));
		res.ns = vec3(0.0, 0.0, 1.0);
	} else {
		res.xs = uintBitsToFloat(first.xyz);
		res.ns = rtgi_unpack_normal(first.w);
	}
	if ((cflags & RTGI_FLAG_BACKFACE) != 0u) {
		res.flags |= RF_BACKFACE;
	}
	if ((cflags & RTGI_FLAG_EMISSIVE) != 0u) {
		res.flags |= RF_EMISSIVE;
	}
	res.w = 1.0 / pdf;
	res.m = 1u;
	res.age = 0u;
	res.depth = p_r.depth;
	res.rnormal = rs_pack_normal(p_r.nrm);
	res.epoch = rf.epoch_tag;
	res.tag = second.w;
	return res;
}

void rs_temporal_pixel(ivec2 px) {
	Receiver r = rs_receiver_cur(px);
	if (!r.valid) {
		rs_store(px, rs_empty());
		imageStore(info_image, px, uvec4(0u));
		return;
	}
	Reservoir fresh = rs_from_candidate(px, r);
	for (int i = 0; i < MAX_CAND; i++) {
		c[i] = rs_cand_none();
	}
	c[0].s = fresh;
	c[0].m = float(fresh.m);
	c[0].jac = 1.0;
	c[0].on = fresh.m > 0u && (fresh.flags & RF_VALID) != 0u;
	c[0].phat_r = c[0].on ? rs_phat(fresh, r.origin, r.nrm) : 0.0;
	if (c[0].on) {
		float energy_c = min(c[0].m * c[0].phat_r * fresh.w, 1e9);
		g_stats[PS_ENERGY_ALL] += (isnan(energy_c) || isinf(energy_c)) ? 0.0 : energy_c;
	} else {
		g_stats[PS_NO_CANON] += 1.0;
	}

	uint reason = REJ_CUT;
	uint info = INFO_VALID;
	if ((rf.flags & F_HISTORY) != 0u) {
		reason = REJ_NONE;
		vec4 clip = rf.view_to_prev_clip * vec4(r.view, 1.0);
		ivec2 hp = ivec2(-1);
		if (clip.w > 1e-6) {
			vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
			hp = ivec2(floor(uv * vec2(rf.trace_size)));
			if (uv.x < 0.0 || uv.y < 0.0 || any(greaterThanEqual(hp, rf.trace_size))) {
				reason = REJ_FRAME;
			}
		} else {
			reason = REJ_FRAME;
		}
		if (reason == REJ_NONE) {
			Receiver q = rs_receiver_prev(hp);
			uint similar_reason;
			if (!rs_similar(r, q, similar_reason)) {
				reason = similar_reason;
			} else {
				Reservoir h = rs_load(hp);
				uint vis = imageLoad(vis_state, hp).x;
				if (h.m == 0u) {
					reason = REJ_NO_SAMPLE;
				} else if (h.epoch != rf.prev_epoch_tag) {
					reason = REJ_EPOCH;
				} else {
					info |= INFO_HAD_HISTORY;
					bool unbiased = (rf.flags & F_UNBIASED) != 0u;
					bool dirty = rs_dirty_hit(q, h);
					if (dirty && !unbiased) {
						reason = REJ_DIRTY;
					} else {
						float m_h = float(min(h.m, rf.m_temporal_cap));
						bool usable = (h.flags & RF_VALID) != 0u;
						uint dead_reason = REJ_NO_SAMPLE;
						if (!unbiased && usable && ((vis & 0xFFu) > rf.vis_fail_limit || (vis & 0x100u) != 0u)) {
							usable = false;
							dead_reason = REJ_VIS;
						}
						uint nb_reason;
						rs_fill_neighbour(h, m_h, usable, dead_reason, dirty, q, r, c[0], c[1], nb_reason);
						reason = nb_reason;
					}
				}
			}
		}
	}

	int sel;
	float total_m;
	Reservoir result = rs_combine(2, uvec2(px), sel, total_m);
	result = rs_finish(result, sel, total_m, r, RF_TEMPORAL, 1u);
	if (sel == 1) {
		info |= INFO_SEL_TEMPORAL;
	}
	info |= reason & INFO_TEMPORAL_REJECT_MASK;
	rs_store(px, result);
	imageStore(info_image, px, uvec4(info));
}

void main() {
	rs_stats_clear();
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (all(lessThan(px, rf.trace_size))) {
		rs_temporal_pixel(px);
	}
	rs_stats_store();
}

#endif

#ifdef MODE_SPATIAL

vec2 rs_spatial_offset(int p_index, uvec2 p_pixel) {
	float radius = float(rf.spatial_radius);
	float angle;
	float length_fraction;
	if ((rf.flags & F_DETERMINISTIC) != 0u) {
		angle = float(p_index) * 2.39996323 + float(rf.pass_index) * 1.3;
		length_fraction = sqrt((float(p_index) + 0.5) / float(max(rf.spatial_count, 1u)));
	} else {
		uint base = 128u + 16u * rf.pass_index;
		angle = 6.2831853 * rs_rand(p_pixel, base + uint(p_index));
		length_fraction = sqrt(rs_rand(p_pixel, base + 8u + uint(p_index)));
	}
	return vec2(cos(angle), sin(angle)) * (radius * max(length_fraction, 0.15));
}

#ifdef PASS_SPATIAL_COMPAT

#define COMPAT_MAX_K 64
#define COMPAT_MAX_M 8
#define COMPAT_KEY_DIM 96u
#define COMPAT_START_DIM 160u
#define COMPAT_SCALE 0.12615662

float rs_compat_score(Receiver p_r, Receiver p_q) {
	float scale = length(p_r.view) * COMPAT_SCALE;
	float hp = exp(-length(p_r.view - p_q.view) / scale);
	float c = clamp(dot(p_r.nrm, p_q.nrm), 0.0, 1.0);
	float c2 = c * c;
	float c4 = c2 * c2;
	float h = hp * (c4 * c4);
	return h < 1.17549435e-38 ? 0.0 : h;
}

float rs_compat_weight(Receiver p_r, Receiver p_q, out float r_h) {
	r_h = 0.0;
	if (!p_q.valid) {
		return 0.0;
	}
	r_h = rs_compat_score(p_r, p_q);
	if ((rf.flags & F_COMPAT_NONE) != 0u) {
		return abs(p_r.rough - p_q.rough) > rf.class_tol ? 0.0 : r_h;
	}
	uint reason;
	if (!rs_similar(p_r, p_q, reason)) {
		return 0.0;
	}
	return (rf.flags & F_COMPAT_BINARY) != 0u ? 1.0 : r_h;
}

int rs_round_away(float p_value) {
	int r = int(floor(abs(p_value) + 0.5));
	return p_value < 0.0 ? -r : r;
}

ivec2 rs_compat_offset(uint p_start, uint p_tap) {
	uint index = p_start + p_tap;
	float a = float(index * 0xC13FA9A9u) * 2.3283064365386963e-10;
	float b = float(index * 0x91E10DA6u) * 2.3283064365386963e-10;
	float u = 2.0 * a - 1.0;
	float v = 2.0 * b - 1.0;
	vec2 d = vec2(0.0);
	if (u != 0.0 || v != 0.0) {
		float radius;
		float angle;
		if (abs(u) > abs(v)) {
			radius = u;
			angle = 0.78539816 * (v / u);
		} else {
			radius = v;
			angle = 1.57079633 - 0.78539816 * (u / v);
		}
		d = vec2(radius * cos(angle), radius * sin(angle));
	}
	vec2 f = float(rf.spatial_radius) * d;
	return ivec2(rs_round_away(f.x), rs_round_away(f.y));
}

int rs_select_neighbours(Receiver p_r, ivec2 p_pixel, int p_wanted, out ivec2 r_picks[COMPAT_MAX_M]) {
	int m = clamp(p_wanted, 1, COMPAT_MAX_M);
	uint budget = min(rf.compat_k, uint(COMPAT_MAX_K));
	bool early_stop = (rf.flags & F_COMPAT_STOP) != 0u;
	uint start = uint(rs_rand(uvec2(p_pixel), COMPAT_START_DIM + rf.pass_index) * 16777216.0);
	uint taps[COMPAT_MAX_K];
	float keys[COMPAT_MAX_M];
	float scores[COMPAT_MAX_M];
	for (int i = 0; i < COMPAT_MAX_M; i++) {
		keys[i] = 0.0;
		scores[i] = 0.0;
		r_picks[i] = ivec2(0);
	}
	int listed = 0;
	int above = 0;
	float scored = 0.0;
	bool stopped = false;
	for (uint t = 0u; t < budget; t++) {
		ivec2 d = rs_compat_offset(start, t);
		uint tap_key = uint(d.x & 0xFFFF) | (uint(d.y & 0xFFFF) << 16u);
		taps[t] = tap_key;
		if (d.x == 0 && d.y == 0) {
			continue;
		}
		ivec2 np = p_pixel + d;
		if (any(lessThan(np, ivec2(0))) || any(greaterThanEqual(np, rf.trace_size))) {
			continue;
		}
		bool seen = false;
		for (uint j = 0u; j < t; j++) {
			if (taps[j] == tap_key) {
				seen = true;
				break;
			}
		}
		if (seen) {
			continue;
		}
		Receiver q = rs_receiver_cur(np);
		float h;
		float w = rs_compat_weight(p_r, q, h);
		scored += 1.0;
		if (!(w > 0.0)) {
			continue;
		}
		float u = max(rs_rand(uvec2(p_pixel), COMPAT_KEY_DIM + t), 5.9604645e-8);
		float key = log(u) / w;
		if (listed == m && !(key > keys[m - 1])) {
			if (h > 0.5) {
				above++;
			}
			if (early_stop && above > m) {
				stopped = true;
				break;
			}
			continue;
		}
		int slot = min(listed, m - 1);
		while (slot > 0 && key > keys[slot - 1]) {
			keys[slot] = keys[slot - 1];
			r_picks[slot] = r_picks[slot - 1];
			scores[slot] = scores[slot - 1];
			slot--;
		}
		keys[slot] = key;
		r_picks[slot] = d;
		scores[slot] = h;
		if (listed < m) {
			listed++;
		}
		if (h > 0.5) {
			above++;
		}
		if (early_stop && above > m) {
			stopped = true;
			break;
		}
	}
	float h_sum = 0.0;
	for (int i = 0; i < listed; i++) {
		h_sum += scores[i];
	}
	g_stats[PS_COMPAT_TAPS] += scored;
	g_stats[PS_COMPAT_STOPS] += stopped ? 1.0 : 0.0;
	g_stats[PS_COMPAT_EMPTY] += listed == 0 ? 1.0 : 0.0;
	g_stats[PS_COMPAT_HSUM] += h_sum;
	if ((rf.flags & F_PROBE_STATS) != 0u) {
		uvec4 o0 = uvec4(uint(r_picks[0].x) & 0xFFu, uint(r_picks[0].y) & 0xFFu, uint(r_picks[1].x) & 0xFFu, uint(r_picks[1].y) & 0xFFu);
		uvec2 o2 = uvec2(uint(r_picks[2].x) & 0xFFu, uint(r_picks[2].y) & 0xFFu);
		uint word_x = uint(listed) | (o0.x << 8u) | (o0.y << 16u) | (o0.z << 24u);
		uint word_y = o0.w | (o2.x << 8u) | (o2.y << 16u) | (min(uint(scored), 255u) << 24u);
		imageStore(sel_image, ivec3(p_pixel, int(rf.pass_index) - 1), uvec4(word_x, word_y, 0u, 0u));
	}
	return listed;
}

#endif

void rs_spatial_pixel(ivec2 px) {
	Receiver r = rs_receiver_cur(px);
	uint info = imageLoad(info_image, px).x;
	if (!r.valid) {
		rs_store(px, rs_empty());
		return;
	}
	Reservoir own = rs_load(px);
	for (int i = 0; i < MAX_CAND; i++) {
		c[i] = rs_cand_none();
	}
	c[0].s = own;
	c[0].m = float(own.m);
	c[0].jac = 1.0;
	c[0].phat_r = rs_phat(own, r.origin, r.nrm);
	c[0].on = own.m > 0u && (own.flags & RF_VALID) != 0u;
	if (c[0].on) {
		float energy_c = min(c[0].m * c[0].phat_r * own.w, 1e9);
		g_stats[PS_ENERGY_ALL] += (isnan(energy_c) || isinf(energy_c)) ? 0.0 : energy_c;
	}

	int count = 1;
	uint spatial_reason = REJ_NONE;
	int wanted = int(min(rf.spatial_count, uint(MAX_CAND - 1)));
#ifdef PASS_SPATIAL_COMPAT
	ivec2 picks[COMPAT_MAX_M];
	int picked = rs_select_neighbours(r, px, wanted, picks);
	if (picked == 0) {
		spatial_reason = REJ_NO_COMPAT;
	}
	for (int k = 0; k < picked; k++) {
		ivec2 np = px + picks[k];
		Receiver q = rs_receiver_cur(np);
		if ((rf.flags & F_COMPAT_NONE) != 0u) {
			uint similar_reason;
			if (!rs_similar(r, q, similar_reason)) {
				spatial_reason = similar_reason;
			}
		}
#else
	for (int k = 0; k < wanted; k++) {
		ivec2 np = px + ivec2(floor(rs_spatial_offset(k, uvec2(px)) + 0.5));
		if (np == px || any(lessThan(np, ivec2(0))) || any(greaterThanEqual(np, rf.trace_size))) {
			spatial_reason = REJ_FRAME;
			continue;
		}
		Receiver q = rs_receiver_cur(np);
		uint similar_reason;
		if (!rs_similar(r, q, similar_reason)) {
			spatial_reason = similar_reason;
			continue;
		}
#endif
		Reservoir n = rs_load(np);
		if (n.m == 0u) {
			spatial_reason = REJ_NO_SAMPLE;
			continue;
		}
		Cand nc;
		uint nb_reason;
		rs_fill_neighbour(n, float(min(n.m, rf.m_max)), (n.flags & RF_VALID) != 0u, REJ_NO_SAMPLE, false, q, r, c[0], nc, nb_reason);
		if (nb_reason != REJ_NONE && nb_reason != REJ_VIS) {
			spatial_reason = nb_reason;
		}
		if (nc.present) {
			c[count] = nc;
			count++;
		}
	}

	int sel;
	float total_m;
	Reservoir result;
	if (count == 1) {
		result = own;
		sel = 0;
		total_m = float(own.m);
		result.m = uint(min(total_m, float(rf.m_max)));
	} else {
		result = rs_combine(count, uvec2(px), sel, total_m);
		result = rs_finish(result, sel, total_m, r, RF_SPATIAL, 0u);
		g_stats[PS_SPATIAL_COMBINES] += 1.0;
	}
	if (sel > 0) {
		info |= INFO_SEL_SPATIAL;
	}
	info = (info & ~(15u << INFO_SPATIAL_REJECT_SHIFT)) | ((spatial_reason & 15u) << INFO_SPATIAL_REJECT_SHIFT);
	rs_store(px, result);
	imageStore(info_image, px, uvec4(info));
}

void main() {
	rs_stats_clear();
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (all(lessThan(px, rf.trace_size))) {
		rs_spatial_pixel(px);
	}
	rs_stats_store();
}

#endif

#if defined(MODE_FINAL) || defined(MODE_VALIDATE)

#ifdef RESTIR_RAYS
vec3 rs_fresh_lo(vec3 p_origin, vec3 p_dir, float p_dist, uint p_seed, out bool r_hit_here) {
	RaySceneHit hit;
	r_hit_here = false;
	float eps = 0.02 + 0.002 * p_dist;
	if (!ray_scene_trace(p_origin, p_dir, p_dist + eps, false, ray_scene_hash(p_seed + 0xa54ff53bu), hit)) {
		return vec3(0.0);
	}
	if (abs(hit.t - p_dist) > eps) {
		return vec3(0.0);
	}
	r_hit_here = true;
	RaySceneMaterial material = ray_scene_hit_material(hit);
	bool two_sided = hit.blend || bool(material.flags & RAY_SCENE_MATERIAL_TWO_SIDED);
	if (hit.backface && !two_sided) {
		return vec3(0.0);
	}
	vec3 normal = ray_scene_hit_normal(hit);
	if (hit.backface) {
		normal = -normal;
	}
	vec3 position = p_origin + p_dir * hit.t;
	float bias = 0.01 + hit.t * 0.001;
	bool ddgi_shade = (rf.flags & F_OWN_SAMPLER) == 0u && (rf.flags & F_SHADE_DDGI) != 0u;
	if (!ddgi_shade) {
		vec3 irradiance = ray_scene_direct_light(position, normal, bias, rf.sky_distance, p_seed);
		return min(material.emission * rf.emission_exposure + material.albedo * irradiance, vec3(RS_LO_CLAMP));
	}
	vec3 irradiance = ray_scene_direct_light(position, normal, bias, ddgi.max_distance, p_seed);
	if (rf.shade_bounce_energy > 0.0) {
		vec3 bounce = ddgi_sample_direction(position - ray_scene.camera_to_anchor, normal, -p_dir, normal, 0.0, hit.t * 0.5).rgb;
		irradiance += bounce * rf.shade_bounce_energy;
	}
	return min(material.emission * rf.shade_exposure + material.albedo * irradiance, vec3(RS_LO_CLAMP));
}
#endif

#endif

#ifdef MODE_FINAL

shared float partial[64 * STATS_STRIDE];

float rs_median_w(ivec2 p_pixel, out int r_count) {
	float values[9];
	int count = 0;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			ivec2 np = p_pixel + ivec2(dx, dy);
			if (any(lessThan(np, ivec2(0))) || any(greaterThanEqual(np, rf.trace_size))) {
				continue;
			}
			uvec4 b = texelFetch(res_in, ivec3(np, 1), 0);
			float w = uintBitsToFloat(b.z);
			uint m = b.w & 0xFFFFu;
			if (m > 0u && w > 0.0 && !isinf(w)) {
				values[count] = w;
				count++;
			}
		}
	}
	for (int i = 1; i < count; i++) {
		float key = values[i];
		int j = i - 1;
		while (j >= 0 && values[j] > key) {
			values[j + 1] = values[j];
			j--;
		}
		values[j + 1] = key;
	}
	r_count = count;
	return count > 0 ? values[count / 2] : 0.0;
}

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	uint local = gl_LocalInvocationIndex;
	for (int k = 0; k < STATS_STRIDE; k++) {
		partial[local * uint(STATS_STRIDE) + uint(k)] = 0.0;
	}
	bool in_range = all(lessThan(px, rf.trace_size));
	if (in_range) {
		Receiver r = rs_receiver_cur(px);
		uint info = imageLoad(info_image, px).x;
		vec3 radiance = vec3(0.0);
		float coverage = 0.0;
		vec4 debug = vec4(0.0);
		float debug_value = 0.0;
		if (r.valid) {
			Reservoir s = rs_load(px);
			float w = s.w;
			bool held = s.m > 0u && (s.flags & RF_VALID) != 0u && !isinf(w) && !isnan(w);
			bool has = held && w > 0.0;
			bool clamped = false;
			if (has && (rf.flags & F_UNBIASED) == 0u) {
				int count;
				float median = rs_median_w(px, count);
				if (count >= 3 && w > rf.w_clamp * median) {
					w = rf.w_clamp * median;
					clamped = true;
				}
			}
			bool visible = true;
			bool tested = false;
			vec3 lo = s.lo;
			if (has && (rf.flags & F_VIS) != 0u) {
				tested = true;
#ifdef RESTIR_RAYS
				if ((rf.flags & F_FRESH_LO) != 0u && !rs_is_sky(s)) {
					vec3 origin = r.origin;
					vec3 delta = s.xs - origin;
					float dist = length(delta);
					bool hit_here;
					vec3 fresh = rs_fresh_lo(origin, delta / max(dist, 1e-6), dist, uint(px.y) * 65537u + uint(px.x) + rf.frame * 6271u, hit_here);
					visible = hit_here;
					if (hit_here) {
						lo = fresh;
					}
				} else {
					visible = rs_visible(r.origin, s);
					if (visible && (rf.flags & F_FRESH_LO) != 0u) {
						lo = rs_sky_radiance(normalize(s.xs));
					}
				}
#endif
				uint old_state = imageLoad(vis_state, px).x;
				if (!visible) {
					imageStore(vis_state, px, uvec4(min((old_state & 0xFFu) + 1u, 255u) | 0x100u));
				} else {
					imageStore(vis_state, px, uvec4(0u));
				}
			}
			if (has && !visible) {
				w = 0.0;
				info |= INFO_VIS_BLOCKED;
			}
			if (tested) {
				info |= INFO_VIS_TESTED;
			}
			if (clamped) {
				info |= INFO_CLAMPED;
			}
			float cos_r = has ? max(dot(r.nrm, rs_dir_to(s, r.origin)), 0.0) : 0.0;
			radiance = lo * (cos_r * w / RS_PI);
			if (!rs_finite3(radiance)) {
				radiance = vec3(0.0);
				w = 0.0;
			}
			coverage = (held && (!(has && !visible) || (rf.flags & F_KEEP_BLOCKED) != 0u)) ? clamp(float(s.m) / float(max(rf.n_target, 1u)), 0.0, 1.0) : 0.0;

			uint view = rf.debug_view;
			vec3 colour = vec3(0.0);
			if (view == 57u) {
				debug_value = float(s.m);
				colour = rs_heat(debug_value / float(max(rf.m_max, 1u)));
			} else if (view == 58u) {
				debug_value = float(s.age);
				colour = rs_heat(debug_value / 32.0);
			} else if (view == 59u) {
				debug_value = float((info & INFO_SEL_TEMPORAL) != 0u ? 1u : 0u) + float((info & INFO_SEL_SPATIAL) != 0u ? 2u : 0u);
				colour = vec3(float((info & INFO_SEL_TEMPORAL) != 0u), float((info & INFO_SEL_SPATIAL) != 0u), 0.2);
			} else if (view == 60u) {
				uint rej_t = info & INFO_TEMPORAL_REJECT_MASK;
				debug_value = float(rej_t);
				colour = rej_t == 0u ? vec3(0.0, 0.5, 0.0) : rs_heat(float(rej_t) / 11.0);
			} else if (view == 61u) {
				debug_value = w;
				colour = rs_heat((log2(max(w, 1e-6)) + 8.0) / 16.0);
			} else if (view == 62u) {
				debug_value = !has ? 2.0 : (visible ? 1.0 : 0.0);
				colour = !has ? vec3(0.3) : (visible ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0));
			} else if (view == 63u) {
				debug_value = float(rf.mode_code);
				colour = rs_heat(float(rf.mode_code) / 7.0);
			}
			debug = vec4(colour, debug_value);

			float base[STATS_STRIDE];
			for (int k = 0; k < STATS_STRIDE; k++) {
				base[k] = 0.0;
			}
			base[0] = 1.0;
			base[1] = coverage > 0.0 ? 1.0 : 0.0;
			if (coverage > 0.0) {
				base[2] = radiance.r;
				base[3] = radiance.g;
				base[4] = radiance.b;
			}
			uvec4 cs = texelFetch(cand_second, px, 0);
			uint cflags = rtgi_candidate_flags(cs);
			if ((cflags & RTGI_FLAG_NO_CANDIDATE) == 0u) {
				vec3 clo = rtgi_candidate_radiance(cs);
				if (rs_finite3(clo)) {
					base[5] = 1.0;
					base[6] = clo.r;
					base[7] = clo.g;
					base[8] = clo.b;
				}
			}
			base[9] = float(s.m);
			base[10] = clamped ? 1.0 : 0.0;
			base[11] = (has && !visible) ? 1.0 : 0.0;
			base[12] = (info & INFO_SEL_TEMPORAL) != 0u ? 1.0 : 0.0;
			base[13] = (info & INFO_SEL_SPATIAL) != 0u ? 1.0 : 0.0;
			bool violation = !(coverage >= 0.0 && coverage <= 1.0) || !rs_finite3(radiance) || any(lessThan(radiance, vec3(0.0))) || any(greaterThan(radiance, vec3(RS_LO_CLAMP)));
			base[14] = violation ? 1.0 : 0.0;
			base[15] = has ? 1.0 : 0.0;
			for (int k = 0; k < STATS_STRIDE; k++) {
				partial[local * uint(STATS_STRIDE) + uint(k)] = base[k];
			}
			if ((rf.flags & F_PROBE_PIXELS) != 0u && (cflags & RTGI_FLAG_NO_CANDIDATE) == 0u) {
				vec3 clo = rtgi_candidate_radiance(cs);
				if (rs_finite3(clo)) {
					info |= (1u << 15u) | ((packHalf2x16(vec2(min(rs_luma(clo), 60000.0), 0.0)) & 0xFFFFu) << 16u);
				}
			}
			imageStore(info_image, px, uvec4(info));
		}
		imageStore(out_radiance, px, vec4(radiance, 1.0));
		imageStore(out_coverage, px, vec4(coverage));
		if ((rf.flags & F_DEBUG_OUT) != 0u) {
			imageStore(out_debug, px, debug);
		}
	}
	barrier();
	if (local == 0u) {
		uint groups_x = (uint(rf.trace_size.x) + 7u) / 8u;
		uint group = gl_WorkGroupID.y * groups_x + gl_WorkGroupID.x;
		for (int k = 0; k < STATS_STRIDE; k++) {
			float sum = 0.0;
			for (uint t = 0u; t < 64u; t++) {
				sum += partial[t * uint(STATS_STRIDE) + uint(k)];
			}
			stats.data[group * uint(STATS_STRIDE) + uint(k)] = sum;
		}
	}
}

#endif

#ifdef MODE_SOURCE

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, rf.trace_size))) {
		return;
	}
	uint tag = uint(px.y * rf.trace_size.x + px.x) ^ rf.frame;
	Receiver r = rs_receiver_cur(px);
	if (!r.valid) {
		imageStore(src_first, px, uvec4(0u));
		imageStore(src_second, px, uvec4(0u, (RTGI_FLAG_NO_CANDIDATE | RTGI_FLAG_NOT_TRACED) << 16u, 0u, tag));
		return;
	}
#ifdef RESTIR_RAYS
	vec3 origin = r.origin;
	vec2 u = rtgi_rand2(uvec2(px), rf.frame, RTGI_RNG_DIRECTION, rf.seed);
	vec3 direction = rtgi_cosine_direction(r.nrm, u);
	float pdf = max(dot(direction, r.nrm), 1e-4) * 0.31830988;
	RaySceneHit hit;
	vec3 radiance;
	vec3 point;
	uint normal_packed = 0u;
	uint flags = 0u;
	if (ray_scene_trace(origin, direction, rf.sky_distance, false, ray_scene_hash(uint(px.y) * 65537u + uint(px.x) + rf.frame * 6271u + 0x510e527fu), hit)) {
		RaySceneMaterial material = ray_scene_hit_material(hit);
		point = origin + direction * hit.t;
		vec3 normal = ray_scene_hit_normal(hit);
		normal = hit.backface ? -normal : normal;
		normal_packed = rtgi_pack_normal(normal);
		bool two_sided = hit.blend || bool(material.flags & RAY_SCENE_MATERIAL_TWO_SIDED);
		if (hit.backface && !two_sided) {
			radiance = vec3(0.0);
			flags |= RTGI_FLAG_BACKFACE;
		} else {
			float bias = 0.01 + hit.t * 0.001;
			vec3 irradiance = ray_scene_direct_light(point, normal, bias, rf.sky_distance, ray_scene_hash(uint(px.y) * 65537u + uint(px.x) + rf.frame * 6271u));
			radiance = min(material.emission * rf.emission_exposure + material.albedo * irradiance, vec3(RS_LO_CLAMP));
		}
		if (dot(material.emission, vec3(1.0)) > 0.0) {
			flags |= RTGI_FLAG_EMISSIVE;
		}
	} else {
		radiance = rs_sky_radiance(direction);
		point = direction;
		flags |= RTGI_FLAG_MISS_SKY;
	}
	uvec4 first;
	uvec4 second;
	rtgi_pack_candidate(point, normal_packed, radiance, flags, pdf, tag, first, second);
	imageStore(src_first, px, first);
	imageStore(src_second, px, second);
#else
	imageStore(src_first, px, uvec4(0u));
	imageStore(src_second, px, uvec4(0u, (RTGI_FLAG_NO_CANDIDATE | RTGI_FLAG_NOT_TRACED) << 16u, 0u, tag));
#endif
}

#endif

#ifdef MODE_PACK

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, rf.trace_size))) {
		return;
	}
	vec3 radiance = vec3(0.0);
	uint flags = RTGI_FLAG_NO_CANDIDATE;
	if (texelFetch(geo_pos_cur, px, 0).w > 0.5) {
		uint info = imageLoad(info_image, px).x;
		if ((info & INFO_VIS_BLOCKED) != 0u) {
			flags = 0u;
		} else if (texelFetch(pk_coverage, px, 0).x > 0.0) {
			radiance = texelFetch(pk_radiance, px, 0).rgb;
			flags = 0u;
		} else {
			flags = RTGI_FLAG_THINNED;
		}
	}
	uvec4 first;
	uvec4 second;
	rtgi_pack_candidate(vec3(0.0), 0u, radiance, flags, 0.0, 0u, first, second);
	imageStore(pk_second, px, second);
}

#endif

#ifdef MODE_CAPTURE

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, rf.trace_size))) {
		return;
	}
	imageStore(cap_pos, px, texelFetch(geo_pos_cur, px, 0));
	imageStore(cap_nrm, px, texelFetch(geo_nrm_cur, px, 0));
	imageStore(cap_ppos, px, texelFetch(geo_pos_prev, px, 0));
	imageStore(cap_pnrm, px, texelFetch(geo_nrm_prev, px, 0));
	imageStore(cap_first, px, texelFetch(cand_first, px, 0));
	imageStore(cap_second, px, texelFetch(cand_second, px, 0));
}

#endif

#ifdef MODE_VALIDATE

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, rf.trace_size))) {
		return;
	}
#if defined(RESTIR_RAYS) && defined(RESTIR_SHADE)
	if ((rf.flags & F_HISTORY) == 0u) {
		return;
	}
	if ((rf.flags & F_REVAL_SHARE) != 0u) {
		if (rs_rand(uvec2(px), 40u) * float(max(rf.reval_share_denom, 1u)) >= 1.0) {
			return;
		}
	} else if ((rtgi_candidate_flags(texelFetch(cand_second, px, 0)) & RTGI_FLAG_NO_CANDIDATE) != 0u) {
		return;
	}
	uvec4 a = imageLoad(res_io, ivec3(px, 0));
	uvec4 b = imageLoad(res_io, ivec3(px, 1));
	uvec4 c = imageLoad(res_io, ivec3(px, 2));
	uint flags = b.y >> 16u;
	uint m = b.w & 0xFFFFu;
	if (m == 0u || (flags & RF_VALID) == 0u || c.z != rf.prev_epoch_tag) {
		return;
	}
	Receiver q = rs_receiver_prev(px);
	if (!q.valid) {
		return;
	}
	Reservoir h = rs_empty();
	h.xs = uintBitsToFloat(a.xyz);
	h.flags = flags;
	vec2 rg = unpackHalf2x16(b.x);
	vec2 bz = unpackHalf2x16(b.y & 0xFFFFu);
	vec3 lo_old = vec3(rg.x, rg.y, bz.x);
	vec3 fresh = vec3(0.0);
	bool landed = false;
	if (rs_is_sky(h)) {
		landed = rs_visible(q.origin, h);
		if (landed) {
			fresh = rs_sky_radiance(normalize(h.xs));
		}
	} else {
		vec3 delta = h.xs - q.origin;
		float dist = length(delta);
		if (dist < 1e-3) {
			return;
		}
		fresh = rs_fresh_lo(q.origin, delta / dist, dist, uint(px.y) * 65537u + uint(px.x) + rf.frame * 6271u, landed);
	}
	const float lo_floor = 0.1;
	float l_old = rs_luma(lo_old);
	float l_new = rs_luma(fresh);
	if (!landed) {
		b.y = b.y & 0xFFFFu;
		b.w = b.w & 0xFFFF0000u;
	} else {
		float limit = 10.0 * max(l_old, lo_floor);
		vec3 updated = l_new > limit ? fresh * (limit / l_new) : fresh;
		updated = clamp(updated, vec3(0.0), vec3(RS_LO_CLAMP));
		float change = abs(l_new - l_old) / max(max(l_old, l_new), lo_floor);
		b.x = packHalf2x16(updated.rg);
		b.y = (packHalf2x16(vec2(updated.b, 0.0)) & 0xFFFFu) | (flags << 16u);
		if (change > rf.reval_threshold) {
			b.w = (b.w & 0xFFFF0000u) | min(m, rf.reval_cut_m);
		}
	}
	imageStore(res_io, ivec3(px, 1), b);
#endif
}

#endif
