#define RS_FOLD_C1 0x68E31DA4u
#define RS_FOLD_C2 0x2C1B3C6Du
#define RS_FOLD_DIM_RENEW 0u
#define RS_FOLD_DIM_START 1u
#define RS_FOLD_PSI_R(m_pixel, m_v) rtgi_rng_state(m_pixel, m_v, RS_FOLD_DIM_RENEW, RS_FOLD_C2)
#define RS_FOLD_PSI_S(m_pixel, m_v) rtgi_rng_state(m_pixel, m_v, RS_FOLD_DIM_START, RS_FOLD_C2)

uint rs_fold_isqrt(uint p_x) {
	uint s = uint(sqrt(float(p_x)));
	if ((s + 1u) * (s + 1u) <= p_x) {
		s++;
	}
	if (s * s > p_x) {
		s--;
	}
	return s;
}

uint rs_fold_draw(uint p_psi, uint p_k) {
	uint quarter = (p_k * p_k) >> 2u;
	uint x = p_psi % (3u * quarter);
	if (x < quarter) {
		return rs_fold_isqrt(x);
	}
	return (p_k >> 1u) + (x - quarter) / p_k;
}

uint rs_fold_phi(uvec2 p_pixel) {
	return rtgi_pcg(p_pixel.x + rtgi_pcg(p_pixel.y + RS_FOLD_C1));
}
