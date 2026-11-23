/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "restir_gi.h"
#include "restir_probe.h"
#include "restir_settings.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "servers/rendering/renderer_rd/environment/gi_module_hooks.h"

static RestirGiBackend *restir_gi_backend = nullptr;
static RestirProbe *restir_probe = nullptr;

void initialize_gi_restir_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		restir_register_settings();
		return;
	}
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	GDREGISTER_CLASS(RestirProbe);
	restir_gi_backend = memnew(RestirGiBackend);
	restir_probe = memnew(RestirProbe);
	RendererRD::GIModuleHooks::get_singleton()->register_rtgi_backend(restir_gi_backend);
	Engine::get_singleton()->add_singleton(Engine::Singleton("GiRestirProbe", restir_probe));
}

void uninitialize_gi_restir_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	RendererRD::GIModuleHooks::get_singleton()->release_all();
	RendererRD::GIModuleHooks::get_singleton()->unregister_all();
	Engine::get_singleton()->remove_singleton("GiRestirProbe");
	if (restir_probe) {
		memdelete(restir_probe);
		restir_probe = nullptr;
	}
	if (restir_gi_backend) {
		memdelete(restir_gi_backend);
		restir_gi_backend = nullptr;
	}
}
