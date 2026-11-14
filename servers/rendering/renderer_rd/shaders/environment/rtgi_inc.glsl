#define RTGI_TILE 8

#define RTGI_FLAG_MISS_SKY 1u
#define RTGI_FLAG_BACKFACE 2u
#define RTGI_FLAG_EMISSIVE 4u
#define RTGI_FLAG_NO_CANDIDATE 8u
#define RTGI_FLAG_NOT_TRACED 16u
#define RTGI_FLAG_GUIDED 32u
#define RTGI_FLAG_THINNED 64u

#define RTGI_TILE_TRACE 1u
#define RTGI_TILE_REASON_DDGI 2u
#define RTGI_TILE_REASON_SSGI 4u
#define RTGI_TILE_REASON_CHECK 8u
#define RTGI_TILE_REASON_FORCED 16u
#define RTGI_TILE_HAS_GEOMETRY 32u
#define RTGI_TILE_REASON_DDGI_WRONG 64u

#define RTGI_FRAME_FLAG_ORTHOGONAL 1u
#define RTGI_FRAME_FLAG_HISTORY_CUT 2u
#define RTGI_FRAME_FLAG_CAMERA_CUT 4u
#define RTGI_FRAME_FLAG_RESIZE 8u
#define RTGI_FRAME_FLAG_BACKEND_SWITCH 16u
#define RTGI_FRAME_FLAG_SSGI_VALID_TEXTURE 32u
#define RTGI_FRAME_FLAG_SOFT_CUT 64u
#define RTGI_FRAME_FLAG_RESTART_SPHERES 128u
#define RTGI_FRAME_FLAG_MODULE_OUTPUT 256u
#define RTGI_FRAME_FLAG_REPLAY 512u

#define RTGI_DDGI_WRONG_EVENTS 1u
#define RTGI_DDGI_WRONG_MEMORY 2u
#define RTGI_DDGI_WRONG_ACTIVE 4u
#define RTGI_DDGI_WRONG_READ 8u

#define RTGI_MEMORY_FRAMES 255u
#define RTGI_MEMORY_EVENTS 256u
#define RTGI_MEMORY_DISAGREE 512u

#define RTGI_DEBUG_NONE 0u
#define RTGI_DEBUG_TILE_CLASS 1u
#define RTGI_DEBUG_RAY_VALIDITY 2u
#define RTGI_DEBUG_CONTRIBUTION 3u
#define RTGI_DEBUG_HISTORY_COUNT 4u

#define RTGI_RNG_DIRECTION 0u
#define RTGI_RNG_THIN 1u
#define RTGI_RNG_CHECK 2u

uint rtgi_pcg(uint p_value) {
	uint state = p_value * 747796405u + 2891336453u;
	uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

uint rtgi_rng_state(uvec2 p_pixel, uint p_frame, uint p_dimension, uint p_seed) {
	return rtgi_pcg(p_pixel.x + rtgi_pcg(p_pixel.y + rtgi_pcg(p_frame + rtgi_pcg(p_dimension + p_seed))));
}

vec2 rtgi_rand2(uvec2 p_pixel, uint p_frame, uint p_dimension, uint p_seed) {
	uint a = rtgi_rng_state(p_pixel, p_frame, p_dimension * 2u, p_seed);
	uint b = rtgi_pcg(a + 0x9e3779b9u);
	return vec2(float(a >> 8u), float(b >> 8u)) * (1.0 / 16777216.0);
}

float rtgi_rand1(uvec2 p_pixel, uint p_frame, uint p_dimension, uint p_seed) {
	return float(rtgi_rng_state(p_pixel, p_frame, p_dimension, p_seed) >> 8u) * (1.0 / 16777216.0);
}

void rtgi_tangent_frame(vec3 p_normal, out vec3 r_tangent, out vec3 r_bitangent) {
	r_tangent = normalize(cross(abs(p_normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0), p_normal));
	r_bitangent = cross(p_normal, r_tangent);
}

vec3 rtgi_cosine_direction(vec3 p_normal, vec2 p_u) {
	float radius = sqrt(p_u.x);
	float phi = 6.2831853 * p_u.y;
	vec3 tangent;
	vec3 bitangent;
	rtgi_tangent_frame(p_normal, tangent, bitangent);
	return normalize(tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi)) + p_normal * sqrt(max(0.0, 1.0 - p_u.x)));
}

uint rtgi_pack_normal(vec3 p_normal) {
	vec3 n = p_normal / max(abs(p_normal.x) + abs(p_normal.y) + abs(p_normal.z), 1e-8);
	vec2 e = n.xy;
	if (n.z < 0.0) {
		vec2 sign_value = vec2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
		e = (1.0 - abs(e.yx)) * sign_value;
	}
	return packUnorm2x16(e * 0.5 + 0.5);
}

vec3 rtgi_unpack_normal(uint p_packed) {
	vec2 e = unpackUnorm2x16(p_packed) * 2.0 - 1.0;
	vec3 v = vec3(e.xy, 1.0 - abs(e.x) - abs(e.y));
	float t = max(-v.z, 0.0);
	v.xy += t * vec2(v.x >= 0.0 ? -1.0 : 1.0, v.y >= 0.0 ? -1.0 : 1.0);
	return normalize(v);
}

void rtgi_pack_candidate(vec3 p_point, uint p_normal_packed, vec3 p_radiance, uint p_flags, float p_pdf, uint p_tag, out uvec4 r_first, out uvec4 r_second) {
	r_first = uvec4(floatBitsToUint(p_point.x), floatBitsToUint(p_point.y), floatBitsToUint(p_point.z), p_normal_packed);
	uint rg = packHalf2x16(p_radiance.rg);
	uint b = packHalf2x16(vec2(p_radiance.b, 0.0)) & 0xFFFFu;
	r_second = uvec4(rg, b | (p_flags << 16u), floatBitsToUint(p_pdf), p_tag);
}

vec3 rtgi_candidate_radiance(uvec4 p_second) {
	vec2 rg = unpackHalf2x16(p_second.x);
	vec2 b = unpackHalf2x16(p_second.y & 0xFFFFu);
	return vec3(rg.x, rg.y, b.x);
}

uint rtgi_candidate_flags(uvec4 p_second) {
	return p_second.y >> 16u;
}

uint rtgi_bayer8(uvec2 p_pixel) {
	uint x = p_pixel.x & 7u;
	uint y = p_pixel.y & 7u;
	uint xy = x ^ y;
	uint interleaved = (xy & 1u) | ((y & 1u) << 1u) | ((xy & 2u) << 1u) | ((y & 2u) << 2u) | ((xy & 4u) << 2u) | ((y & 4u) << 3u);
	return bitfieldReverse(interleaved) >> 26u;
}

bool rtgi_thin_keeps(uvec2 p_pixel, uint p_frame, uint p_seed, float p_fraction) {
	if (p_fraction >= 1.0) {
		return true;
	}
	uint hashed = rtgi_pcg(p_frame * 2654435761u + p_seed);
	uvec2 offset = uvec2(hashed & 7u, (hashed >> 3u) & 7u);
	return rtgi_bayer8(p_pixel + offset) < uint(p_fraction * 64.0 + 0.5);
}

ivec2 rtgi_anchor_pixel(ivec2 p_trace_pixel, int p_scale, ivec2 p_screen_size) {
	ivec2 scale = ivec2(max(p_scale, 1));
	return min(p_trace_pixel * scale + scale / 2, p_screen_size - 1);
}

uint rtgi_shade_key(ivec2 p_pixel, uint p_frame) {
	return uint(p_pixel.y) * 65537u + uint(p_pixel.x) + p_frame * 6271u;
}
