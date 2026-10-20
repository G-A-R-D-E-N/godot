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

#include "render_data_rd.h"

#include "core/object/class_db.h"
#include "servers/rendering/renderer_rd/effects/copy_effects.h"
#include "servers/rendering/renderer_rd/environment/fog.h"
#include "servers/rendering/renderer_rd/renderer_scene_render_rd.h"
#include "servers/rendering/renderer_rd/framebuffer_cache_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/rendering_server_globals.h"

void RenderDataRD::_bind_methods() {
	ClassDB::bind_static_method("RenderDataRD", D_METHOD("supports_water_temporal_alpha"), &RenderDataRD::supports_water_temporal_alpha);
	ClassDB::bind_method(D_METHOD("get_water_lighting_data"), &RenderDataRD::get_water_lighting_data);
	ClassDB::bind_method(D_METHOD("commit_water_color"), &RenderDataRD::commit_water_color);
	ClassDB::bind_method(D_METHOD("set_water_fog_mask", "mask"), &RenderDataRD::set_water_fog_mask);
	ClassDB::bind_method(D_METHOD("get_water_fog_data"), &RenderDataRD::get_water_fog_data);
	ClassDB::bind_method(D_METHOD("get_water_temporal_info"), &RenderDataRD::get_water_temporal_info);
	ClassDB::bind_method(D_METHOD("set_water_temporal_data", "view", "reactivity", "reset_history"), &RenderDataRD::set_water_temporal_data, DEFVAL(false));
}

bool RenderDataRD::commit_water_color() const {
	ERR_FAIL_COND_V(render_buffers.is_null(), false);
	if (render_buffers->get_msaa_3d() == RSE::VIEWPORT_MSAA_DISABLED) {
		return true;
	}
	RendererRD::CopyEffects *copy_effects = RendererRD::CopyEffects::get_singleton();
	FramebufferCacheRD *framebuffer_cache = FramebufferCacheRD::get_singleton();
	ERR_FAIL_NULL_V(copy_effects, false);
	ERR_FAIL_NULL_V(framebuffer_cache, false);
	const Rect2i rect(Vector2i(), render_buffers->get_internal_size());
	for (uint32_t view = 0; view < render_buffers->get_view_count(); view++) {
		RID source = render_buffers->get_internal_texture(view);
		RID target = render_buffers->get_color_msaa(view);
		ERR_FAIL_COND_V(source.is_null() || target.is_null(), false);
		RID framebuffer = framebuffer_cache->get_cache(target);
		ERR_FAIL_COND_V(framebuffer.is_null(), false);
		copy_effects->copy_to_fb_rect(source, framebuffer, rect, false, false, false, false, RID(), false, false, false, false, Rect2(), 1.0, false);
	}
	return true;
}

Dictionary RenderDataRD::get_water_lighting_data() const {
	Dictionary result;
	if (!water_lighting_ready || scene_data == nullptr) {
		return result;
	}
	RendererRD::LightStorage *light_storage = RendererRD::LightStorage::get_singleton();
	ERR_FAIL_NULL_V(light_storage, result);

	result["abi_version"] = 2;
	result["directional_light_stride_bytes"] = 528;
	result["positional_light_stride_bytes"] = 224;
	result["max_directional_lights"] = light_storage->get_max_directional_lights();
	result["directional_light_buffer"] = light_storage->get_directional_light_buffer();
	result["directional_light_count"] = directional_light_count;
	result["directional_shadow_texture"] = light_storage->directional_shadow_get_texture();
	result["omni_light_buffer"] = light_storage->get_omni_light_buffer();
	result["omni_light_count"] = water_omni_light_count;
	result["spot_light_buffer"] = light_storage->get_spot_light_buffer();
	result["spot_light_count"] = water_spot_light_count;
	result["area_light_buffer"] = light_storage->get_area_light_buffer();
	result["area_light_count"] = water_area_light_count;
	result["positional_shadow_texture"] = shadow_atlas.is_valid() && light_storage->owns_shadow_atlas(shadow_atlas) ? light_storage->shadow_atlas_get_texture(shadow_atlas) : RID();
	result["cluster_buffer"] = cluster_buffer;
	result["cluster_size"] = cluster_size;
	result["cluster_max_elements"] = cluster_max_elements;
	result["emissive_exposure_normalization"] = scene_data->get_current_exposure_normalization();
	result["camera_visible_layers"] = scene_data->camera_visible_layers;
	return result;
}

bool RenderDataRD::set_water_fog_mask(RID p_mask) {
	if (p_mask.is_null()) {
		water_fog_mask = RID();
		return true;
	}
	ERR_FAIL_COND_V(render_buffers.is_null() || !RD::get_singleton()->texture_is_valid(p_mask), false);
	RD::TextureFormat format = RD::get_singleton()->texture_get_format(p_mask);
	const Size2i size = render_buffers->get_internal_size();
	ERR_FAIL_COND_V(format.texture_type != RD::TEXTURE_TYPE_2D_ARRAY || format.width != uint32_t(size.x) || format.height != uint32_t(size.y) || format.array_layers != render_buffers->get_view_count(), false);
	ERR_FAIL_COND_V(!(format.usage_bits & RD::TEXTURE_USAGE_SAMPLING_BIT), false);
	ERR_FAIL_COND_V(format.format != RD::DATA_FORMAT_R8_UNORM && format.format != RD::DATA_FORMAT_R16_SFLOAT && format.format != RD::DATA_FORMAT_R32_SFLOAT, false);
	water_fog_mask = p_mask;
	return true;
}

Dictionary RenderDataRD::get_water_fog_data() const {
	Dictionary result;
	result["abi_version"] = 1;
	result["water_fog_mask"] = water_fog_mask;
	result["ownership_active"] = water_fog_mask.is_valid() && RD::get_singleton()->texture_is_valid(water_fog_mask);
	result["fog_enabled"] = false;
	result["volumetric_enabled"] = false;
	result["volumetric_requested"] = false;
	result["volumetric_light_density_texture"] = RID();
	RendererSceneRenderRD *renderer = RendererSceneRenderRD::get_singleton();
	if (renderer == nullptr || environment.is_null()) {
		return result;
	}
	result["fog_enabled"] = renderer->environment_get_fog_enabled(environment);
	result["volumetric_requested"] = renderer->environment_get_volumetric_fog_enabled(environment);
	result["fog_mode"] = int(renderer->environment_get_fog_mode(environment));
	result["fog_density"] = renderer->environment_get_fog_density(environment);
	result["fog_height"] = renderer->environment_get_fog_height(environment);
	result["fog_height_density"] = renderer->environment_get_fog_height_density(environment);
	float end = renderer->environment_get_fog_depth_end(environment);
	if (end <= 0.0f && scene_data) {
		end = scene_data->z_far;
	}
	result["fog_depth_end"] = end;
	result["fog_depth_begin"] = MIN(renderer->environment_get_fog_depth_begin(environment), end - 0.001f);
	result["fog_depth_curve"] = renderer->environment_get_fog_depth_curve(environment);
	const float exposure_normalization = renderer->is_using_physical_light_units() && camera_attributes.is_valid() ? RSG::camera_attributes->camera_attributes_get_exposure_normalization_factor(camera_attributes) : 1.0f;
	result["fog_light_color_energy"] = renderer->environment_get_fog_light_color(environment).srgb_to_linear() * (renderer->environment_get_fog_light_energy(environment) * exposure_normalization);
	result["fog_sun_scatter"] = renderer->environment_get_fog_sun_scatter(environment);
	result["fog_aerial_perspective"] = renderer->environment_get_fog_aerial_perspective(environment);
	result["camera_height_world"] = scene_data ? scene_data->cam_transform.origin.y : 0.0;
	if (scene_data) {
		Projection correction;
		correction.set_depth_correction(scene_data->flip_y);
		result["fog_projection"] = correction * scene_data->cam_projection;
		result["camera_z_near"] = scene_data->z_near;
		result["camera_z_far"] = scene_data->z_far;
	}
	result["radiance_texture"] = RID();
	result["radiance_is_array"] = false;
	result["radiance_max_roughness_lod"] = renderer->get_roughness_layers() - 1;
	RID sky_rid = renderer->environment_get_sky(environment);
	if (sky_rid.is_valid() && scene_data) {
		RID radiance = renderer->get_sky()->sky_get_radiance_texture_rd(sky_rid);
		result["radiance_texture"] = radiance;
		if (radiance.is_valid() && RD::get_singleton()->texture_is_valid(radiance)) {
			RD::TextureFormat radiance_format = RD::get_singleton()->texture_get_format(radiance);
			result["radiance_is_array"] = radiance_format.texture_type == RD::TEXTURE_TYPE_2D_ARRAY;
			result["radiance_layers"] = radiance_format.array_layers;
			result["radiance_mipmaps"] = radiance_format.mipmaps;
		}
		result["radiance_inverse_xform"] = renderer->environment_get_sky_orientation(environment).inverse() * scene_data->cam_transform.basis;
		result["radiance_border_size"] = scene_data->radiance_border_size;
		result["radiance_pixel_size"] = scene_data->radiance_pixel_size;
	}
	result["fog_sky_affect"] = renderer->environment_get_fog_sky_affect(environment);
	result["fog_use_legacy_blending"] = renderer->fog_use_legacy_blending_get();
	result["volumetric_sky_affect"] = renderer->environment_get_volumetric_fog_sky_affect(environment);
	if (water_lighting_ready && render_buffers.is_valid() && render_buffers->has_custom_data(RB_SCOPE_FOG)) {
		Ref<RendererRD::Fog::VolumetricFog> fog = render_buffers->get_custom_data(RB_SCOPE_FOG);
		if (fog.is_valid()) {
			result["volumetric_enabled"] = true;
			result["volumetric_light_density_texture"] = fog->light_density_map;
			result["volumetric_length"] = fog->length;
			result["volumetric_spread"] = fog->spread;
			result["volumetric_size"] = Vector3i(fog->width, fog->height, fog->depth);
		}
	}
	return result;
}

Dictionary RenderDataRD::get_water_temporal_info() const {
	Dictionary result;
	result["abi_version"] = 1;
	if (render_buffers.is_null() || scene_data == nullptr) {
		return result;
	}
	result["taa_active"] = render_buffers->get_use_taa();
	result["preserve_temporal_alpha"] = true;
	result["fsr2_active"] = render_buffers->get_scaling_3d_mode() == RSE::VIEWPORT_SCALING_3D_MODE_FSR2;
	result["jitter"] = scene_data->taa_jitter;
	Array projections;
	Projection correction;
	correction.set_depth_correction(scene_data->flip_y);
	for (uint32_t view = 0; view < scene_data->view_count; view++) {
		projections.push_back(correction * scene_data->view_projection[view]);
	}
	result["unjittered_projections"] = projections;
	return result;
}

bool RenderDataRD::set_water_temporal_data(uint32_t p_view, RID p_reactivity, bool p_reset_history) {
	ERR_FAIL_COND_V(render_buffers.is_null(), false);
	ERR_FAIL_UNSIGNED_INDEX_V(p_view, render_buffers->get_view_count(), false);
	if (p_reactivity.is_valid()) {
		ERR_FAIL_COND_V(!RD::get_singleton()->texture_is_valid(p_reactivity), false);
		RD::TextureFormat format = RD::get_singleton()->texture_get_format(p_reactivity);
		Size2i size = render_buffers->get_internal_size();
		ERR_FAIL_COND_V(format.texture_type != RD::TEXTURE_TYPE_2D || format.width != uint32_t(size.x) || format.height != uint32_t(size.y), false);
		ERR_FAIL_COND_V(!(format.usage_bits & RD::TEXTURE_USAGE_SAMPLING_BIT), false);
		ERR_FAIL_COND_V(format.format != RD::DATA_FORMAT_R8_UNORM && format.format != RD::DATA_FORMAT_R16_SFLOAT && format.format != RD::DATA_FORMAT_R32_SFLOAT, false);
	}
	water_temporal_reactivity[p_view] = p_reactivity;
	water_reset_history |= p_reset_history;
	return true;
}
