/**************************************************************************/
/*  gi_compositor.cpp                                                     */
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

#include "gi_compositor.h"

#include "gi.h"
#include "rtgi.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/uniform_set_cache_rd.h"
#include "servers/rendering/rendering_server_globals.h"
#include "servers/rendering/storage/utilities.h"

namespace RendererRD {

GICompositor *GICompositor::singleton = nullptr;

GICompositor::GICompositor() {
	Vector<String> versions;
	versions.push_back("\n");
	compose_shader.initialize(versions);
	compose_version = compose_shader.version_create();
	compose_pipeline.create_compute_pipeline(compose_shader.version_get_shader(compose_version, 0));
	synthetic_shader.initialize(versions);
	synthetic_version = synthetic_shader.version_create();
	synthetic_pipeline.create_compute_pipeline(synthetic_shader.version_get_shader(synthetic_version, 0));
}

GICompositor::~GICompositor() {
	compose_pipeline.free();
	compose_shader.version_free(compose_version);
	synthetic_pipeline.free();
	synthetic_shader.version_free(synthetic_version);
}

void GICompositor::finalize() {
	RtgiPass::finalize();
	if (singleton != nullptr) {
		memdelete(singleton);
		singleton = nullptr;
	}
}

void GICompositor::process(const Ref<RenderSceneBuffersRD> &p_render_buffers, uint32_t p_view_count) {
	const bool setting_enabled = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/compositor/extra_provider");
	const bool debug_provider = GLOBAL_GET_CACHED(bool, "rendering/global_illumination/compositor/debug_provider");
	if (!setting_enabled && !debug_provider && !RtgiPass::is_enabled()) {
		if (p_render_buffers->has_custom_data(RB_SCOPE_GI_EXTRA)) {
			p_render_buffers->clear_context(RB_SCOPE_GI_EXTRA);
			p_render_buffers->set_custom_data(RB_SCOPE_GI_EXTRA, Ref<RenderBufferCustomDataRD>());
		}
		return;
	}
	if (singleton == nullptr) {
		singleton = memnew(GICompositor);
	}
	singleton->_process(p_render_buffers, p_view_count, setting_enabled, debug_provider);
}

Size2i GICompositor::ensure_slot_textures(const Ref<RenderSceneBuffersRD> &p_render_buffers) {
	if (!p_render_buffers->has_custom_data(RB_SCOPE_GI_EXTRA)) {
		p_render_buffers->set_custom_data(RB_SCOPE_GI_EXTRA, memnew(ViewportState));
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_GI, RB_TEX_AMBIENT)) {
		return Size2i();
	}
	Ref<ViewportState> state = p_render_buffers->get_custom_data(RB_SCOPE_GI_EXTRA);

	const Size2i size = p_render_buffers->get_texture_slice_size(RB_SCOPE_GI, RB_TEX_AMBIENT, 0);
	if (p_render_buffers->has_texture(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_RADIANCE) && p_render_buffers->get_texture_slice_size(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_RADIANCE, 0) != size) {
		p_render_buffers->clear_context(RB_SCOPE_GI_EXTRA);
		state->written = false;
	}
	if (!p_render_buffers->has_texture(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_RADIANCE)) {
		const uint32_t usage_bits = RD::TEXTURE_USAGE_SAMPLING_BIT | RD::TEXTURE_USAGE_STORAGE_BIT;
		p_render_buffers->create_texture(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_RADIANCE, RD::DATA_FORMAT_R16G16B16A16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, size);
		p_render_buffers->create_texture(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_COVERAGE, RD::DATA_FORMAT_R16_SFLOAT, usage_bits, RD::TEXTURE_SAMPLES_1, size);
	}
	return size;
}

void GICompositor::publish(const Ref<RenderSceneBuffersRD> &p_render_buffers, const GIProviderRecord &p_record) {
	if (!p_render_buffers->has_custom_data(RB_SCOPE_GI_EXTRA)) {
		return;
	}
	Ref<ViewportState> state = p_render_buffers->get_custom_data(RB_SCOPE_GI_EXTRA);
	state->record = p_record;
	state->written = true;
}

bool GICompositor::get_record(const Ref<RenderSceneBuffersRD> &p_render_buffers, GIProviderRecord &r_record) {
	if (!p_render_buffers->has_custom_data(RB_SCOPE_GI_EXTRA)) {
		return false;
	}
	Ref<ViewportState> state = p_render_buffers->get_custom_data(RB_SCOPE_GI_EXTRA);
	r_record = state->record;
	return r_record.source != GI_PROVIDER_SOURCE_NONE;
}

void GICompositor::_process(const Ref<RenderSceneBuffersRD> &p_render_buffers, uint32_t p_view_count, bool p_setting_enabled, bool p_debug_provider) {
	const Size2i size = ensure_slot_textures(p_render_buffers);
	Ref<ViewportState> state = p_render_buffers->get_custom_data(RB_SCOPE_GI_EXTRA);

	if (p_debug_provider) {
		RENDER_TIMESTAMP("GI Extra Synthetic");
		_run_synthetic(p_render_buffers, p_view_count, size, state.ptr());
	}

	const uint32_t lower_units = gi_provider_units(RendererSceneRenderRD::get_singleton()->is_using_physical_light_units());
	const GICompositorSlotState slot_state = gi_compositor_slot_state(p_setting_enabled, state->written, state->record, lower_units);
	state->written = false;

	const String dump_path = GLOBAL_GET("rendering/global_illumination/compositor/debug_dump_path");
	const bool dump = !dump_path.is_empty() && _ambient_can_copy(p_render_buffers);
	if (dump) {
		_dump_ambient(p_render_buffers, dump_path + ".pre");
	}
	if (slot_state == GI_COMPOSITOR_SLOT_ENABLED) {
		RENDER_TIMESTAMP("GI Extra Compose");
		_compose(p_render_buffers, p_view_count, size);
	}
	RENDER_TIMESTAMP("GI Extra End");
	if (dump) {
		_dump_ambient(p_render_buffers, dump_path);
		Ref<FileAccess> info = FileAccess::open(dump_path + ".txt", FileAccess::WRITE);
		if (info.is_valid()) {
			info->store_line(itos(size.width) + " " + itos(size.height) + " " + itos(slot_state));
		}
	}
}

void GICompositor::_run_synthetic(const Ref<RenderSceneBuffersRD> &p_render_buffers, uint32_t p_view_count, const Size2i &p_size, ViewportState *r_state) {
	SyntheticPushConstant push_constant;
	push_constant.pattern = uint32_t(CLAMP(int(GLOBAL_GET("rendering/global_illumination/compositor/debug_pattern")), 0, int(GI_COMPOSITOR_SYNTHETIC_PATTERN_COUNT) - 1));
	push_constant.coverage = float(GLOBAL_GET("rendering/global_illumination/compositor/debug_coverage"));
	push_constant.radiance = float(GLOBAL_GET("rendering/global_illumination/compositor/debug_radiance"));
	push_constant.pad = 0;

	const RID shader = synthetic_shader.version_get_shader(synthetic_version, 0);
	RD::get_singleton()->draw_command_begin_label("GI extra provider (synthetic)");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, synthetic_pipeline.get_rid());
	for (uint32_t v = 0; v < p_view_count; v++) {
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, p_render_buffers->get_texture_slice(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_RADIANCE, v, 0)));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, p_render_buffers->get_texture_slice(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_COVERAGE, v, 0)));
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 0, uniforms), 0);
		RD::get_singleton()->compute_list_set_push_constant(compute_list, &push_constant, sizeof(SyntheticPushConstant));
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_size.width, p_size.height, 1);
	}
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();

	const uint32_t units = gi_provider_units(RendererSceneRenderRD::get_singleton()->is_using_physical_light_units());
	r_state->record = gi_provider_make_record(units, GI_COMPOSITOR_EXTRA_SOURCE, GI_PROVIDER_FLAG_SAMPLE_COUNT, GI_COMPOSITOR_SYNTHETIC_SAMPLES, 1.0f, 1.0f);
	r_state->written = true;
}

void GICompositor::_compose(const Ref<RenderSceneBuffersRD> &p_render_buffers, uint32_t p_view_count, const Size2i &p_size) {
	const RID shader = compose_shader.version_get_shader(compose_version, 0);
	RD::get_singleton()->draw_command_begin_label("GI extra provider");
	RD::ComputeListID compute_list = RD::get_singleton()->compute_list_begin();
	RD::get_singleton()->compute_list_bind_compute_pipeline(compute_list, compose_pipeline.get_rid());
	for (uint32_t v = 0; v < p_view_count; v++) {
		LocalVector<RD::Uniform> uniforms;
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 0, p_render_buffers->get_texture_slice(RB_SCOPE_GI, RB_TEX_AMBIENT, v, 0)));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 1, p_render_buffers->get_texture_slice(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_RADIANCE, v, 0)));
		uniforms.push_back(RD::Uniform(RD::UNIFORM_TYPE_IMAGE, 2, p_render_buffers->get_texture_slice(RB_SCOPE_GI_EXTRA, RB_TEX_EXTRA_COVERAGE, v, 0)));
		RD::get_singleton()->compute_list_bind_uniform_set(compute_list, UniformSetCacheRD::get_singleton()->get_cache_vec(shader, 0, uniforms), 0);
		RD::get_singleton()->compute_list_dispatch_threads(compute_list, p_size.width, p_size.height, 1);
	}
	RD::get_singleton()->compute_list_end();
	RD::get_singleton()->draw_command_end_label();
}

bool GICompositor::_ambient_can_copy(const Ref<RenderSceneBuffersRD> &p_render_buffers) const {
	const RD::TextureFormat format = RD::get_singleton()->texture_get_format(p_render_buffers->get_texture_slice(RB_SCOPE_GI, RB_TEX_AMBIENT, 0, 0));
	if (format.usage_bits & RD::TEXTURE_USAGE_CAN_COPY_FROM_BIT) {
		return true;
	}
	WARN_PRINT_ONCE("GI compositor dump skipped: set compositor/debug_dump_path before the viewport's first GI frame.");
	return false;
}

void GICompositor::_dump_ambient(const Ref<RenderSceneBuffersRD> &p_render_buffers, const String &p_path) const {
	Vector<uint8_t> data = RD::get_singleton()->texture_get_data(p_render_buffers->get_texture_slice(RB_SCOPE_GI, RB_TEX_AMBIENT, 0, 0), 0);
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_buffer(data.ptr(), data.size());
	}
}

} // namespace RendererRD
