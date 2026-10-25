highp float luminance_adapt(highp float l, highp float strength, highp float pivot) {
	if (strength >= 1.0 || !(l > 0.0) || l >= pivot) {
		return l;
	}
	return pivot * pow(l / pivot, strength);
}

highp float luminance_unadapt(highp float l, highp float strength, highp float pivot) {
	if (strength >= 1.0 || !(l > 0.0) || l >= pivot) {
		return l;
	}
	return pivot * pow(l / pivot, 1.0 / max(strength, 0.05));
}
