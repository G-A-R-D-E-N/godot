/**************************************************************************/
/*  sky.cpp                                                               */
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

#include "sky.h"

#include "core/object/class_db.h"
#include "servers/rendering/rendering_server.h"

void Sky::set_radiance_size(RadianceSize p_size) {
	ERR_FAIL_INDEX(p_size, RADIANCE_SIZE_MAX);

	radiance_size = p_size;
	static const int size[RADIANCE_SIZE_MAX] = {
		32, 64, 128, 256, 512, 1024, 2048
	};
	RS::get_singleton()->sky_set_radiance_size(sky, size[radiance_size]);
}

Sky::RadianceSize Sky::get_radiance_size() const {
	return radiance_size;
}

void Sky::set_process_mode(ProcessMode p_mode) {
	mode = p_mode;
	RS::get_singleton()->sky_set_mode(sky, RSE::SkyMode(mode));
}

Sky::ProcessMode Sky::get_process_mode() const {
	return mode;
}

void Sky::set_radiance_update_policy(RadianceUpdatePolicy p_policy) {
	radiance_update_policy = p_policy;
	RS::get_singleton()->sky_set_radiance_update_policy(sky, RSE::SkyRadianceUpdatePolicy(radiance_update_policy));
}

Sky::RadianceUpdatePolicy Sky::get_radiance_update_policy() const {
	return radiance_update_policy;
}

void Sky::set_sh_enabled(bool p_enabled) {
	sh_enabled = p_enabled;
	RS::get_singleton()->sky_set_sh_enabled(sky, sh_enabled);
}

bool Sky::is_sh_enabled() const {
	return sh_enabled;
}

void Sky::request_radiance_update() {
	RS::get_singleton()->sky_request_radiance_update(sky);
}

int64_t Sky::get_radiance_generation() const {
	return RS::get_singleton()->sky_get_radiance_generation(sky);
}

Dictionary Sky::get_update_stats() const {
	return RS::get_singleton()->sky_get_update_stats(sky);
}

RID Sky::get_sh_buffer() const {
	return RS::get_singleton()->sky_get_sh_buffer(sky);
}

Dictionary Sky::get_sh_debug() const {
	return RS::get_singleton()->sky_debug_get_sh(sky);
}

PackedInt64Array Sky::get_layer_generations() const {
	return RS::get_singleton()->sky_debug_get_layer_generations(sky);
}

void Sky::set_material(const Ref<Material> &p_material) {
	sky_material = p_material;
	RID material_rid;
	if (sky_material.is_valid()) {
		material_rid = sky_material->get_rid();
	}
	RS::get_singleton()->sky_set_material(sky, material_rid);
}

Ref<Material> Sky::get_material() const {
	return sky_material;
}

RID Sky::get_rid() const {
	return sky;
}

void Sky::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_radiance_size", "size"), &Sky::set_radiance_size);
	ClassDB::bind_method(D_METHOD("get_radiance_size"), &Sky::get_radiance_size);

	ClassDB::bind_method(D_METHOD("set_process_mode", "mode"), &Sky::set_process_mode);
	ClassDB::bind_method(D_METHOD("get_process_mode"), &Sky::get_process_mode);

	ClassDB::bind_method(D_METHOD("set_radiance_update_policy", "policy"), &Sky::set_radiance_update_policy);
	ClassDB::bind_method(D_METHOD("get_radiance_update_policy"), &Sky::get_radiance_update_policy);

	ClassDB::bind_method(D_METHOD("set_sh_enabled", "enabled"), &Sky::set_sh_enabled);
	ClassDB::bind_method(D_METHOD("is_sh_enabled"), &Sky::is_sh_enabled);

	ClassDB::bind_method(D_METHOD("request_radiance_update"), &Sky::request_radiance_update);
	ClassDB::bind_method(D_METHOD("get_radiance_generation"), &Sky::get_radiance_generation);
	ClassDB::bind_method(D_METHOD("get_update_stats"), &Sky::get_update_stats);
	ClassDB::bind_method(D_METHOD("get_sh_buffer"), &Sky::get_sh_buffer);
	ClassDB::bind_method(D_METHOD("get_sh_debug"), &Sky::get_sh_debug);
	ClassDB::bind_method(D_METHOD("get_layer_generations"), &Sky::get_layer_generations);

	ClassDB::bind_method(D_METHOD("set_material", "material"), &Sky::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &Sky::get_material);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "sky_material", PROPERTY_HINT_RESOURCE_TYPE, "PanoramaSkyMaterial,ProceduralSkyMaterial,PhysicalSkyMaterial,ShaderMaterial", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_EDITOR_INSTANTIATE_OBJECT), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "process_mode", PROPERTY_HINT_ENUM, "Automatic,High-Quality,High-Quality Incremental,Real-Time"), "set_process_mode", "get_process_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "radiance_size", PROPERTY_HINT_ENUM, "32,64,128,256,512,1024,2048"), "set_radiance_size", "get_radiance_size");

	ADD_GROUP("Radiance Products", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "radiance_update_policy", PROPERTY_HINT_ENUM, "Automatic,Manual"), "set_radiance_update_policy", "get_radiance_update_policy");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "sh_enabled"), "set_sh_enabled", "is_sh_enabled");

	BIND_ENUM_CONSTANT(RADIANCE_SIZE_32);
	BIND_ENUM_CONSTANT(RADIANCE_SIZE_64);
	BIND_ENUM_CONSTANT(RADIANCE_SIZE_128);
	BIND_ENUM_CONSTANT(RADIANCE_SIZE_256);
	BIND_ENUM_CONSTANT(RADIANCE_SIZE_512);
	BIND_ENUM_CONSTANT(RADIANCE_SIZE_1024);
	BIND_ENUM_CONSTANT(RADIANCE_SIZE_2048);
	BIND_ENUM_CONSTANT(RADIANCE_SIZE_MAX);

	BIND_ENUM_CONSTANT(PROCESS_MODE_AUTOMATIC);
	BIND_ENUM_CONSTANT(PROCESS_MODE_QUALITY);
	BIND_ENUM_CONSTANT(PROCESS_MODE_INCREMENTAL);
	BIND_ENUM_CONSTANT(PROCESS_MODE_REALTIME);

	BIND_ENUM_CONSTANT(RADIANCE_UPDATE_POLICY_AUTOMATIC);
	BIND_ENUM_CONSTANT(RADIANCE_UPDATE_POLICY_MANUAL);
}

Sky::Sky() {
	sky = RS::get_singleton()->sky_create();
}

Sky::~Sky() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(sky);
}
