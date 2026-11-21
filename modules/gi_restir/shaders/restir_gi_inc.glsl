#define RF_SKY 1u
#define RF_VALID 2u
#define RF_VIS_FAILED 4u
#define RF_SPATIAL 8u
#define RF_CLAMPED 16u
#define RF_BACKFACE 32u
#define RF_EMISSIVE 64u
#define RF_TEMPORAL 128u
#define RF_S_SHIFT 8u
#define RF_S_MASK 0x3Fu
#define RF_MV (1u << 14u)
#define RF_FOLD_BITS 0xFF00u

#define F_UNBIASED 1u
#define F_VIS_REUSE 2u
#define F_FRESH_LO 4u
#define F_DETERMINISTIC 8u
#define F_HISTORY 16u
#define F_VIS 32u
#define F_BLOCK_SKY 64u
#define F_PROBE_STATS 128u
#define F_PROBE_CAMERA_FIXED 256u
#define F_PROBE_PIXELS 512u
#define F_COMPAT 1024u
#define F_COMPAT_NONE 2048u
#define F_COMPAT_BINARY 4096u
#define F_COMPAT_STOP 8192u
#define F_OWN_SAMPLER 16384u
#define F_REAL_SKY 32768u
#define F_SHADE_DDGI 65536u
#define F_DEBUG_OUT 131072u
#define F_KEEP_BLOCKED 262144u
#define F_PRE_FRAME 524288u
#define F_REVAL_FRAME 1048576u
#define F_REVAL_SHARE 2097152u
#define F_KEEP_M 4194304u
#define F_REPLAY 8388608u
#define F_EVENTS 16777216u
#define F_SPATIAL_HOLD 33554432u
#define F_SKY_REREAD 67108864u
#define F_TILE_SKIP 134217728u
#define F_PACK_M 268435456u

#define REJ_NONE 0u
#define REJ_FRAME 1u
#define REJ_PLANE 2u
#define REJ_NORMAL 3u
#define REJ_CLASS 4u
#define REJ_EPOCH 5u
#define REJ_DIRTY 6u
#define REJ_JACOBIAN 7u
#define REJ_DISTANCE 8u
#define REJ_CUT 9u
#define REJ_NO_SAMPLE 10u
#define REJ_VIS 11u
#define REJ_NO_COMPAT 12u
#define REJ_BOUND 13u
#define REJ_UNTRACED 14u

#define INFO_TEMPORAL_REJECT_MASK 15u
#define INFO_SPATIAL_REJECT_SHIFT 4u
#define INFO_SEL_TEMPORAL (1u << 8u)
#define INFO_SEL_SPATIAL (1u << 9u)
#define INFO_VIS_BLOCKED (1u << 10u)
#define INFO_VIS_TESTED (1u << 11u)
#define INFO_CLAMPED (1u << 12u)
#define INFO_HAD_HISTORY (1u << 13u)
#define INFO_VALID (1u << 14u)

#if defined(MODE_TEMPORAL)
#define MAX_CAND 2
#elif defined(SPATIAL_WIDE)
#define MAX_CAND 9
#else
#define MAX_CAND 4
#endif
#define RS_PI 3.14159265359
#define RS_LO_CLAMP 60000.0
#define STATS_STRIDE 16
#define PS_STRIDE 32
#define PS_TESTED 0
#define PS_REJ_DIST 1
#define PS_REJ_COS 2
#define PS_REJ_JAC 3
#define PS_ENERGY_ALL 4
#define PS_ENERGY_TRUNC 5
#define PS_PDF_REFUSED 6
#define PS_PRESENT 7
#define PS_ZERO_WEIGHT 8
#define PS_SPATIAL_COMBINES 9
#define PS_LO_REFUSED 10
#define PS_NO_CANON 11
#define PS_COMPAT_TAPS 12
#define PS_COMPAT_STOPS 13
#define PS_COMPAT_EMPTY 14
#define PS_COMPAT_HSUM 15
#define PS_REHIT_MISS 16
#define PS_REHIT_MISMATCH 17
#define PS_EVENT_CAPPED 18
#define PS_RESHADED 19
#define PS_RESHADE_OVERFLOW 20
#define PS_EDGE_STALE 21
#define PS_FOLD_REFUSED 22
#define PS_FOLD_CARRIED 23
#define PS_FOLD_RENEWED 24
#define PS_FOLD_STARTED 25
#define PS_FOLD_REFUSED_EMPTY 26
#ifdef RS_FOLD_DUMP
#define PS_FOLD_HP_EQ_PX 27
#define PS_FOLD_PHASE0 28
#define PS_FOLD_PHASE0_START 29
#define PS_FOLD_REFUSED_OFF 30
#define RS_FOLD_START_CUT 0u
#define RS_FOLD_START_FRAME 1u
#define RS_FOLD_START_PLANE 2u
#define RS_FOLD_START_NORMAL 3u
#define RS_FOLD_START_CLASS 4u
#define RS_FOLD_START_NO_SAMPLE 5u
#define RS_FOLD_START_VIS 6u
#define RS_FOLD_START_OTHER 7u
#define RS_FOLD_START_PRE 8u
#define RS_FOLD_START_S 64u
#define RS_FOLD_START_U 64u
#ifdef RS_FOLD_LINEAGE
#define RS_FOLD_LINEAGE_PIXEL_MASK 0x00FFFFFFu
#define RS_FOLD_LINEAGE_CLOCK_SHIFT 24u
#define RS_FOLD_LINEAGE_CLOCK_MASK 0xFFu
#define RS_FOLD_LINEAGE_UNKNOWN 0xFF000000u
#endif
#endif
#define PS_ROW_VALIDATE 3u
#define RS_EVENT_VALIDATE_EVENT 1u
#define RS_EVENT_VALIDATE_ALL 2u
#define RS_PASS_EVENT_COUNT 4u
#define RS_EVENT_KEY_DIM 200u
#define RS_EVENT_BINS 256u
#define KIND_NONE 0u
#define KIND_DIST 1u
#define KIND_COS 2u
#define KIND_JAC 3u

layout(set = 0, binding = 0, std140) uniform RestirFrame {
	vec4 cam_rot0;
	vec4 cam_rot1;
	vec4 cam_rot2;
	vec4 cam_to_anchor;
	vec4 prev_rot0;
	vec4 prev_rot1;
	vec4 prev_rot2;
	vec4 prev_to_anchor;
	mat4 view_to_prev_clip;
	vec4 test_sky;

	ivec2 trace_size;
	uint frame;
	uint seed;

	uint flags;
	uint spatial_count;
	uint spatial_radius;
	uint pass_index;

	uint m_temporal_cap;
	uint m_max;
	uint n_target;
	uint epoch_tag;

	uint debug_view;
	uint mode_code;
	uint vis_fail_limit;
	uint prev_epoch_tag;

	float tol_plane;
	float cos_normal;
	float jac_max;
	float d_min;

	float cos_min;
	float w_clamp;
	float origin_scale;
	float sky_distance;

	float class_tol;
	float emission_exposure;
	uint dirty_count;
	float dirty_margin;

	uint compat_k;
	float origin_floor;
	uint reval_cut_m;
	uint reval_share_denom;

	vec3 shade_sky;
	uint shade_sky_flags;

	vec2 shade_sky_border;
	float shade_sky_energy;
	float shade_bounce_energy;
	float shade_exposure;
	float reval_threshold;
	uint shade_pad1;
	uint shade_pad2;

	ivec2 screen_size;
	int trace_scale;
	uint replay_pad;

	vec4 jitter_shift;
	vec4 prev_jitter_shift;
}
rf;

#ifdef MODE_TEMPORAL
layout(set = 0, binding = 24, std140) uniform RestirDirty {
	vec4 box_min[64];
	vec4 box_max[64];
}
dirty;
#endif

#if defined(MODE_TEMPORAL) || defined(MODE_SPATIAL) || defined(MODE_FINAL) || defined(MODE_SOURCE) || defined(MODE_CAPTURE)
layout(set = 0, binding = 1) uniform texture2D geo_pos_cur;
layout(set = 0, binding = 2) uniform texture2D geo_nrm_cur;
#endif

#if defined(MODE_TEMPORAL) || defined(MODE_CAPTURE) || defined(MODE_VALIDATE)
layout(set = 0, binding = 3) uniform texture2D geo_pos_prev;
layout(set = 0, binding = 4) uniform texture2D geo_nrm_prev;
#endif

#if defined(MODE_TEMPORAL) || defined(MODE_FINAL) || defined(MODE_CAPTURE) || defined(MODE_VALIDATE)
layout(set = 0, binding = 5) uniform utexture2D cand_first;
layout(set = 0, binding = 6) uniform utexture2D cand_second;
#endif

#if defined(MODE_TEMPORAL) || defined(MODE_SPATIAL) || defined(MODE_FINAL) || defined(MODE_PACK)
layout(set = 0, binding = 7) uniform utexture2DArray res_in;
layout(set = 0, binding = 27) uniform utexture2D rs_tiles;

bool rs_tile_traced(ivec2 p_pixel) {
	if ((rf.flags & F_TILE_SKIP) == 0u) {
		return true;
	}
	ivec2 tile = min(p_pixel / RTGI_TILE, textureSize(rs_tiles, 0) - 1);
	return (texelFetch(rs_tiles, tile, 0).r & RTGI_TILE_TRACE) != 0u;
}
#endif

#if defined(MODE_CLEAR) || defined(MODE_TEMPORAL) || defined(MODE_SPATIAL)
layout(rgba32ui, set = 0, binding = 8) uniform restrict writeonly uimage2DArray res_out;
#endif

#if defined(MODE_CLEAR) || defined(MODE_TEMPORAL) || defined(MODE_FINAL)
layout(r32ui, set = 0, binding = 9) uniform restrict uimage2D vis_state;
#endif

#if defined(MODE_CLEAR) || defined(MODE_TEMPORAL) || defined(MODE_SPATIAL) || defined(MODE_FINAL) || defined(MODE_PACK)
layout(r32ui, set = 0, binding = 10) uniform restrict uimage2D info_image;
#endif

#if defined(MODE_TEMPORAL) || defined(MODE_SPATIAL) || defined(MODE_VALIDATE)
layout(set = 0, binding = 23, std430) restrict writeonly buffer PassStatsBuffer {
	float data[];
}
pass_stats;
float g_stats[PS_STRIDE];
shared float partial_ps[64 * PS_STRIDE];
#endif

#ifdef MODE_VALIDATE
layout(rgba32ui, set = 0, binding = 8) uniform restrict uimage2DArray res_io;
#endif

#ifdef PASS_SPATIAL_COMPAT
layout(rg32ui, set = 0, binding = 25) uniform restrict writeonly uimage2DArray sel_image;
#endif

#if defined(MODE_TEMPORAL) || defined(MODE_SPATIAL) || defined(MODE_VALIDATE) || defined(MODE_SKY_EPOCH)
layout(set = 0, binding = 26, std140) uniform RestirEvents {
	uvec4 counts;
	vec4 params;
	uvec4 gates;
	vec4 sphere[64];
	uvec4 since_last[16];
	vec4 held[64];
	uvec4 hold;
}
ev;

bool rs_event_first_fire(uint p_index) {
	return p_index >= ev.counts.y || ev.since_last[p_index >> 2u][p_index & 3u] >= ev.gates.x;
}

float rs_event_solid_angle(float p_reach, float p_distance) {
	if (p_distance <= p_reach || p_distance < 1e-6) {
		return 4.0 * RS_PI;
	}
	float k = p_reach / p_distance;
	float k2 = k * k;
	return 2.0 * RS_PI * k2 / (1.0 + sqrt(1.0 - k2));
}

bool rs_event_caps(vec3 p_position) {
	uint count = min(ev.counts.x, 64u);
	for (uint i = 0u; i < count; i++) {
		if (!rs_event_first_fire(i)) {
			continue;
		}
		vec4 s = ev.sphere[i];
		float dist = length(p_position - s.xyz);
		if (dist < ev.params.x * s.w) {
			return true;
		}
		if (rs_event_solid_angle(s.w, dist) > ev.params.y) {
			return true;
		}
	}
	return false;
}

bool rs_hold_caps(vec3 p_position) {
	uint count = min(ev.hold.x, 64u);
	for (uint i = 0u; i < count; i++) {
		vec4 s = ev.held[i];
		float dist = length(p_position - s.xyz);
		if (dist < ev.params.x * s.w) {
			return true;
		}
		if (rs_event_solid_angle(s.w, dist) > ev.params.y) {
			return true;
		}
	}
	return false;
}

bool rs_event_holds(vec3 p_sample) {
	uint count = min(ev.counts.y, 64u);
	for (uint i = 0u; i < count; i++) {
		vec4 s = ev.sphere[i];
		vec3 delta = p_sample - s.xyz;
		if (dot(delta, delta) <= s.w * s.w) {
			return true;
		}
	}
	return false;
}
#endif

#if defined(MODE_TEMPORAL) && defined(RS_SKY_REREAD)
layout(set = 0, binding = 29, std430) restrict readonly buffer RestirSkyEpoch {
	float luma;
	uint epochs;
	uint jump;
	uint jumps;
	uint capped;
	uint spare0;
	uint spare1;
	uint spare2;
}
sky_epoch;
#endif

#if defined(MODE_TEMPORAL) && defined(RS_FOLD_DUMP)
layout(set = 0, binding = 32, std430) restrict buffer RestirFoldDump {
	uint s_max;
	uint n_over;
	uint texels;
	uint pad;
	uint starts[];
}
rs_fold_dump;
#endif

#if defined(MODE_TEMPORAL) && defined(RS_FOLD_DUMP) && defined(RS_FOLD_LINEAGE)
layout(rg32ui, set = 0, binding = 33) uniform restrict readonly uimage2D rs_fold_lineage_in;
layout(rg32ui, set = 0, binding = 34) uniform restrict writeonly uimage2D rs_fold_lineage_out;
layout(r32ui, set = 0, binding = 35) uniform restrict writeonly uimage2D rs_fold_refused_id;
#endif

#ifdef MODE_SKY_EPOCH
layout(set = 0, binding = 29, std430) restrict buffer RestirSkyEpoch {
	float luma;
	uint epochs;
	uint jump;
	uint jumps;
	uint capped;
	uint spare0;
	uint spare1;
	uint spare2;
}
sky_epoch;
#ifdef USE_RADIANCE_OCTMAP_ARRAY
layout(set = 0, binding = 30) uniform texture2DArray epoch_sky;
#else
layout(set = 0, binding = 30) uniform texture2D epoch_sky;
#endif
layout(set = 0, binding = 31) uniform sampler epoch_sampler;
#endif

#ifdef MODE_VALIDATE
layout(set = 0, binding = 28, std430) restrict buffer RestirEventBins {
	uint bins[];
}
event_bins;
shared uint rs_event_cut;
#endif

#ifdef MODE_FINAL
layout(rgba16f, set = 0, binding = 11) uniform restrict writeonly image2D out_radiance;
layout(r16f, set = 0, binding = 12) uniform restrict writeonly image2D out_coverage;
layout(rgba16f, set = 0, binding = 13) uniform restrict writeonly image2D out_debug;
layout(set = 0, binding = 14, std430) restrict writeonly buffer StatsBuffer {
	float data[];
}
stats;
#endif

#ifdef MODE_PACK
layout(set = 0, binding = 1) uniform texture2D geo_pos_cur;
layout(set = 0, binding = 11) uniform texture2D pk_radiance;
layout(set = 0, binding = 12) uniform texture2D pk_coverage;
layout(rgba32ui, set = 0, binding = 16) uniform restrict writeonly uimage2D pk_second;
#endif

#ifdef MODE_CAPTURE
layout(rgba32f, set = 0, binding = 17) uniform restrict writeonly image2D cap_pos;
layout(rgba32f, set = 0, binding = 18) uniform restrict writeonly image2D cap_nrm;
layout(rgba32f, set = 0, binding = 19) uniform restrict writeonly image2D cap_ppos;
layout(rgba32f, set = 0, binding = 20) uniform restrict writeonly image2D cap_pnrm;
layout(rgba32ui, set = 0, binding = 21) uniform restrict writeonly uimage2D cap_first;
layout(rgba32ui, set = 0, binding = 22) uniform restrict writeonly uimage2D cap_second;
#endif

#ifdef MODE_SOURCE
layout(rgba32ui, set = 0, binding = 15) uniform restrict writeonly uimage2D src_first;
layout(rgba32ui, set = 0, binding = 16) uniform restrict writeonly uimage2D src_second;
#endif

struct Reservoir {
	vec3 xs;
	vec3 ns;
	vec3 lo;
	uint flags;
	float w;
	uint m;
	uint age;
	float depth;
	uint rnormal;
	uint epoch;
	uint tag;
};

struct Receiver {
	vec3 view;
	vec3 true_view;
	vec3 pos;
	vec3 origin;
	vec3 nrm;
	float rough;
	float depth;
	bool valid;
};

Reservoir rs_empty() {
	Reservoir r;
	r.xs = vec3(0.0);
	r.ns = vec3(0.0, 0.0, 1.0);
	r.lo = vec3(0.0);
	r.flags = 0u;
	r.w = 0.0;
	r.m = 0u;
	r.age = 0u;
	r.depth = 0.0;
	r.rnormal = 0u;
	r.epoch = 0u;
	r.tag = 0u;
	return r;
}

float rs_luma(vec3 p_color) {
	return dot(p_color, vec3(0.2126, 0.7152, 0.0722));
}

bool rs_finite3(vec3 p_value) {
	return !(any(isnan(p_value)) || any(isinf(p_value)));
}

mat3 rs_rot(vec4 a, vec4 b, vec4 c) {
	return mat3(a.xyz, b.xyz, c.xyz);
}

vec3 rs_true_view(vec3 p_view, vec4 p_shift) {
	return vec3(p_view.xy + p_view.z * p_shift.xy + p_shift.zw, p_view.z);
}

#define RS_ORIGIN_VIEW_ULPS 4.0
#define RS_ORIGIN_ANCHOR_ULPS 4.0
#define RS_ORIGIN_RASTER 0.00390625
#define RS_FLOAT_EPSILON 1.1920929e-7

void rs_pick_gap(vec3 p_view, vec4 p_tap, float p_side, inout vec3 r_best, inout float r_gap) {
	if (p_tap.w <= 0.5) {
		return;
	}
	vec3 delta = (p_tap.xyz - p_view) * p_side;
	float gap = abs(dot(delta, normalize(p_view)));
	if (gap < r_gap) {
		r_gap = gap;
		r_best = delta;
	}
}

vec3 rs_origin_offset_vector(vec3 p_view, vec3 p_anchor_position, vec3 p_view_normal, vec3 p_world_normal, vec4 p_left, vec4 p_right, vec4 p_up, vec4 p_down) {
	vec3 best_x = vec3(0.0);
	vec3 best_y = vec3(0.0);
	float gap_x = 1e30;
	float gap_y = 1e30;
	rs_pick_gap(p_view, p_left, -1.0, best_x, gap_x);
	rs_pick_gap(p_view, p_right, 1.0, best_x, gap_x);
	rs_pick_gap(p_view, p_up, -1.0, best_y, gap_y);
	rs_pick_gap(p_view, p_down, 1.0, best_y, gap_y);
	float footprint = max(length(best_x), length(best_y));
	vec3 direction = p_world_normal;
	if (gap_x < 1e29 && gap_y < 1e29) {
		vec3 n = cross(best_x, best_y);
		float n_length = length(n);
		if (n_length > 0.0) {
			n /= n_length;
			n = dot(n, p_view) > 0.0 ? -n : n;
			if (dot(n, p_view_normal) > 0.7) {
				direction = normalize(rs_rot(rf.cam_rot0, rf.cam_rot1, rf.cam_rot2) * n);
			}
		}
	}
	float anchor_extent = max(abs(p_anchor_position.x), max(abs(p_anchor_position.y), abs(p_anchor_position.z)));
	float view_distance = length(p_view);
	float error = RS_FLOAT_EPSILON * (RS_ORIGIN_VIEW_ULPS * view_distance + RS_ORIGIN_ANCHOR_ULPS * anchor_extent) + footprint * RS_ORIGIN_RASTER;
	return direction * max(rf.origin_scale * error, rf.origin_floor * view_distance);
}

#if defined(MODE_TEMPORAL) || defined(MODE_SPATIAL) || defined(MODE_FINAL) || defined(MODE_SOURCE)
Receiver rs_receiver_cur(ivec2 p_pixel) {
	vec4 g = texelFetch(geo_pos_cur, p_pixel, 0);
	vec4 n = texelFetch(geo_nrm_cur, p_pixel, 0);
	Receiver r;
	r.valid = g.w > 0.5;
	r.view = g.xyz;
	r.true_view = rs_true_view(g.xyz, rf.jitter_shift);
	r.pos = rs_rot(rf.cam_rot0, rf.cam_rot1, rf.cam_rot2) * r.true_view + rf.cam_to_anchor.xyz;
	r.nrm = normalize(rs_rot(rf.cam_rot0, rf.cam_rot1, rf.cam_rot2) * normalize(n.xyz * 2.0 - 1.0));
	ivec2 limit = textureSize(geo_pos_cur, 0) - 1;
	vec3 shading_view = normalize(n.xyz * 2.0 - 1.0);
	vec4 left = texelFetch(geo_pos_cur, clamp(p_pixel + ivec2(-1, 0), ivec2(0), limit), 0);
	vec4 right = texelFetch(geo_pos_cur, clamp(p_pixel + ivec2(1, 0), ivec2(0), limit), 0);
	vec4 up = texelFetch(geo_pos_cur, clamp(p_pixel + ivec2(0, -1), ivec2(0), limit), 0);
	vec4 down = texelFetch(geo_pos_cur, clamp(p_pixel + ivec2(0, 1), ivec2(0), limit), 0);
	r.origin = r.pos + rs_origin_offset_vector(r.true_view, r.pos, shading_view, r.nrm, vec4(rs_true_view(left.xyz, rf.jitter_shift), left.w), vec4(rs_true_view(right.xyz, rf.jitter_shift), right.w), vec4(rs_true_view(up.xyz, rf.jitter_shift), up.w), vec4(rs_true_view(down.xyz, rf.jitter_shift), down.w));
	r.rough = n.w;
	r.depth = max(-g.z, 0.01);
	return r;
}
#endif

#if defined(MODE_TEMPORAL) || defined(MODE_VALIDATE)
Receiver rs_receiver_prev(ivec2 p_pixel) {
	vec4 g = texelFetch(geo_pos_prev, p_pixel, 0);
	vec4 n = texelFetch(geo_nrm_prev, p_pixel, 0);
	Receiver r;
	r.valid = g.w > 0.5;
	r.view = g.xyz;
	r.true_view = rs_true_view(g.xyz, rf.prev_jitter_shift);
	r.pos = rs_rot(rf.prev_rot0, rf.prev_rot1, rf.prev_rot2) * r.true_view + rf.prev_to_anchor.xyz;
	r.nrm = normalize(rs_rot(rf.prev_rot0, rf.prev_rot1, rf.prev_rot2) * normalize(n.xyz * 2.0 - 1.0));
	ivec2 limit = textureSize(geo_pos_prev, 0) - 1;
	vec3 shading_view = normalize(n.xyz * 2.0 - 1.0);
	vec4 left = texelFetch(geo_pos_prev, clamp(p_pixel + ivec2(-1, 0), ivec2(0), limit), 0);
	vec4 right = texelFetch(geo_pos_prev, clamp(p_pixel + ivec2(1, 0), ivec2(0), limit), 0);
	vec4 up = texelFetch(geo_pos_prev, clamp(p_pixel + ivec2(0, -1), ivec2(0), limit), 0);
	vec4 down = texelFetch(geo_pos_prev, clamp(p_pixel + ivec2(0, 1), ivec2(0), limit), 0);
	r.origin = r.pos + rs_origin_offset_vector(r.true_view, r.pos, shading_view, r.nrm, vec4(rs_true_view(left.xyz, rf.prev_jitter_shift), left.w), vec4(rs_true_view(right.xyz, rf.prev_jitter_shift), right.w), vec4(rs_true_view(up.xyz, rf.prev_jitter_shift), up.w), vec4(rs_true_view(down.xyz, rf.prev_jitter_shift), down.w));
	r.rough = n.w;
	r.depth = max(-g.z, 0.01);
	return r;
}
#endif

#if defined(MODE_TEMPORAL) || defined(MODE_SPATIAL) || defined(MODE_FINAL)
Reservoir rs_load(ivec2 p_pixel) {
	uvec4 a = texelFetch(res_in, ivec3(p_pixel, 0), 0);
	uvec4 b = texelFetch(res_in, ivec3(p_pixel, 1), 0);
	uvec4 c = texelFetch(res_in, ivec3(p_pixel, 2), 0);
	Reservoir r;
	r.xs = uintBitsToFloat(a.xyz);
	r.ns = rtgi_unpack_normal(a.w);
	vec2 rg = unpackHalf2x16(b.x);
	vec2 bz = unpackHalf2x16(b.y & 0xFFFFu);
	r.lo = vec3(rg.x, rg.y, bz.x);
	r.flags = b.y >> 16u;
	r.w = uintBitsToFloat(b.z);
	r.m = b.w & 0xFFFFu;
	r.age = b.w >> 16u;
	r.depth = uintBitsToFloat(c.x);
	r.rnormal = c.y;
	r.epoch = c.z;
	r.tag = c.w;
	return r;
}
#endif

#if defined(MODE_CLEAR) || defined(MODE_TEMPORAL) || defined(MODE_SPATIAL)
void rs_store(ivec2 p_pixel, Reservoir r) {
#ifdef RS_SKY_REREAD
	if ((r.flags & RF_SKY) != 0u) {
		r.lo = vec3(0.0);
	}
#endif
	uint lo_b = packHalf2x16(vec2(r.lo.b, 0.0)) & 0xFFFFu;
	imageStore(res_out, ivec3(p_pixel, 0), uvec4(floatBitsToUint(r.xs.x), floatBitsToUint(r.xs.y), floatBitsToUint(r.xs.z), rtgi_pack_normal(r.ns)));
	imageStore(res_out, ivec3(p_pixel, 1), uvec4(packHalf2x16(r.lo.rg), lo_b | (r.flags << 16u), floatBitsToUint(r.w), (r.m & 0xFFFFu) | (r.age << 16u)));
	imageStore(res_out, ivec3(p_pixel, 2), uvec4(floatBitsToUint(r.depth), r.rnormal, r.epoch, r.tag));
}
#endif

bool rs_is_sky(Reservoir s) {
	return (s.flags & RF_SKY) != 0u;
}

vec3 rs_dir_to(Reservoir s, vec3 p_pos) {
	return rs_is_sky(s) ? s.xs : normalize(s.xs - p_pos);
}

float rs_phat(Reservoir s, vec3 p_pos, vec3 p_nrm) {
	return rs_luma(s.lo) * max(dot(p_nrm, rs_dir_to(s, p_pos)), 0.0);
}

float rs_shift_jacobian(Reservoir s, vec3 p_xq, vec3 p_xr, out uint r_reason, out uint r_kind) {
	r_reason = REJ_NONE;
	r_kind = KIND_NONE;
	if (rs_is_sky(s)) {
		return 1.0;
	}
	vec3 dq = p_xq - s.xs;
	vec3 dr = p_xr - s.xs;
	float lq = length(dq);
	float lr = length(dr);
	if (lq < rf.d_min || lr < rf.d_min) {
		r_reason = REJ_DISTANCE;
		r_kind = KIND_DIST;
		return 0.0;
	}
	float cq = dot(s.ns, dq) / lq;
	float cr = dot(s.ns, dr) / lr;
	if (cq < rf.cos_min || cr < rf.cos_min) {
		r_reason = REJ_JACOBIAN;
		r_kind = KIND_COS;
		return 0.0;
	}
	float jac = (cr / cq) * (lq * lq) / (lr * lr);
	if (!(jac >= 1.0 / rf.jac_max && jac <= rf.jac_max)) {
		r_reason = REJ_JACOBIAN;
		r_kind = KIND_JAC;
		return 0.0;
	}
	return jac;
}

float rs_jacobian_raw(Reservoir s, vec3 p_xq, vec3 p_xr) {
	if (rs_is_sky(s)) {
		return 1.0;
	}
	vec3 dq = p_xq - s.xs;
	vec3 dr = p_xr - s.xs;
	float lq = max(length(dq), 1e-4);
	float lr = max(length(dr), 1e-4);
	float cq = max(abs(dot(s.ns, dq)) / lq, 1e-3);
	float cr = abs(dot(s.ns, dr)) / lr;
	return clamp((cr / cq) * (lq * lq) / (lr * lr), 0.0, 1000.0);
}

uint rs_pack_normal(vec3 p_normal) {
	return rtgi_pack_normal(p_normal);
}

#ifdef RESTIR_RAYS
bool rs_visible(vec3 p_origin, Reservoir s) {
	vec3 origin = p_origin;
	RaySceneHit hit;
	uvec2 px = gl_GlobalInvocationID.xy;
	uint seed = px.y * 65537u + px.x + rf.frame * 6271u;
	if (rs_is_sky(s)) {
		return !ray_scene_trace(origin, s.xs, rf.sky_distance, true, ray_scene_hash(seed + 0x3c6ef373u), hit);
	}
	vec3 delta = s.xs - origin;
	float dist = length(delta);
	float eps = 0.01 + 0.001 * dist;
	if (dist <= eps) {
		return true;
	}
	return !ray_scene_trace(origin, delta / dist, dist - eps, true, ray_scene_hash(seed + 0x1f83d9abu), hit);
}
#else
bool rs_visible(vec3 p_origin, Reservoir s) {
	return true;
}
#endif

float rs_rand(uvec2 p_pixel, uint p_dimension) {
	uint frame = (rf.flags & F_DETERMINISTIC) != 0u ? 0u : rf.frame;
	uint seed = (rf.flags & F_DETERMINISTIC) != 0u ? 0x1234567u : rf.seed;
	return rtgi_rand1(p_pixel, frame, 64u + p_dimension, rtgi_pcg(seed * 0x9E3779B1u + 0x7F4A7C15u));
}

float rs_rand_frame(uvec2 p_pixel, uint p_dimension) {
	uint seed = (rf.flags & F_DETERMINISTIC) != 0u ? 0x1234567u : rf.seed;
	return rtgi_rand1(p_pixel, rf.frame, 64u + p_dimension, rtgi_pcg(seed * 0x9E3779B1u + 0x7F4A7C15u));
}

struct Cand {
	Reservoir s;
	float m;
	float jac;
	float phat_r;
	float a_self;
	float a_at_c;
	bool on;
	bool present;
	bool keep;
};

Cand rs_cand_none() {
	Cand c;
	c.s = rs_empty();
	c.m = 0.0;
	c.jac = 1.0;
	c.phat_r = 0.0;
	c.a_self = 0.0;
	c.a_at_c = 0.0;
	c.on = false;
	c.present = false;
	c.keep = false;
	return c;
}

Cand c[MAX_CAND];

Reservoir rs_combine(int p_count, uvec2 p_pixel, out int r_sel, out float r_total_m) {
	bool unbiased = (rf.flags & F_UNBIASED) != 0u;
	float total_m = 0.0;
	int neighbours = 0;
	for (int i = 0; i < p_count; i++) {
		if (unbiased ? (i == 0 || c[i].present) : (c[i].on || c[i].keep)) {
			total_m += c[i].m;
		}
		if (i > 0 && c[i].present) {
			neighbours++;
		}
	}
	float a_c0 = c[0].m * c[0].phat_r;
	float canonical_share = 0.0;
	if (unbiased && neighbours > 0) {
		for (int i = 1; i < p_count; i++) {
			if (!c[i].present) {
				continue;
			}
			float den = a_c0 + c[i].a_at_c;
			if (den > 0.0) {
				canonical_share += a_c0 / den;
			}
		}
		canonical_share /= float(neighbours);
	}
	float wsum = 0.0;
	r_sel = -1;
	for (int i = 0; i < p_count; i++) {
		if (!c[i].on) {
			continue;
		}
		float mis;
		if (!unbiased) {
			mis = total_m > 0.0 ? c[i].m / total_m : 0.0;
		} else if (neighbours == 0) {
			mis = i == 0 ? 1.0 : 0.0;
		} else if (i == 0) {
			mis = canonical_share;
		} else {
			float den = c[i].a_self + c[0].m * c[i].phat_r;
			mis = den > 0.0 ? (c[i].a_self / den) / float(neighbours) : 0.0;
		}
		float weight = mis * c[i].phat_r * c[i].s.w * c[i].jac;
		if (!(weight > 0.0) || isinf(weight)) {
			continue;
		}
		wsum += weight;
		if (rs_rand(p_pixel, uint(i) + 16u * rf.pass_index) * wsum < weight) {
			r_sel = i;
		}
	}
	r_total_m = total_m;
	if (r_sel < 0) {
		Reservoir kept = c[0].s;
		for (int i = 0; i < p_count; i++) {
			if (c[i].on) {
				kept = c[i].s;
				break;
			}
		}
		kept.w = 0.0;
		return kept;
	}
	Reservoir result = c[r_sel].s;
	result.w = c[r_sel].phat_r > 0.0 ? wsum / c[r_sel].phat_r : 0.0;
	return result;
}

vec3 rs_heat(float p_t) {
	float t = clamp(p_t, 0.0, 1.0);
	return clamp(vec3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)), 0.0, 1.0);
}
