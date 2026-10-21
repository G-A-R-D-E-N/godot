extends "res://harness.gd"

class MaskEffect extends CompositorEffect:
	var active := false
	var registered := false
	var mask := RID()
	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_PRE_OPAQUE
	func _render_callback(_type: int, data: RenderData) -> void:
		var rd := RenderingServer.get_rendering_device()
		if not active:
			return
		var buffers: RenderSceneBuffersRD = data.get_render_scene_buffers()
		var size := buffers.get_internal_size()
		if mask.is_valid():
			var current := rd.texture_get_format(mask)
			if current.width != size.x or current.height != size.y:
				rd.free_rid(mask)
				mask = RID()
		if not mask.is_valid():
			var format := RDTextureFormat.new()
			format.width = size.x
			format.height = size.y
			format.texture_type = RenderingDevice.TEXTURE_TYPE_2D_ARRAY
			format.array_layers = 1
			format.format = RenderingDevice.DATA_FORMAT_R8_UNORM
			format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
			var bytes := PackedByteArray()
			bytes.resize(size.x*size.y)
			for y in size.y:
				for x in size.x/2:
					bytes[y*size.x+x] = 255
			mask = rd.texture_create(format,RDTextureView.new(),[bytes])
		if data.has_method("set_water_fog_mask"):
			registered = data.call("set_water_fog_mask",mask)

class FogInspect extends Effect:
	var snapshot: Dictionary = {}
	var shader := RID()
	var pipeline := RID()
	var sampler := RID()
	var sample_buffer := RID()
	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_SKY
	func _render_callback(_type: int, data: RenderData) -> void:
		if not data.has_method("get_water_fog_data"):
			return
		var s: Dictionary = data.call("get_water_fog_data")
		if s.get("volumetric_enabled",false):
			var rd := RenderingServer.get_rendering_device()
			var rid: RID = s.volumetric_light_density_texture
			s["volume_valid"] = rd.texture_is_valid(rid)
			var format := rd.texture_get_format(rid)
			s["volume_3d"] = format.texture_type == RenderingDevice.TEXTURE_TYPE_3D
			if not shader.is_valid():
				var source := RDShaderSource.new()
				source.source_compute="#version 450\nlayout(local_size_x=1)in;layout(set=0,binding=0)uniform sampler3D fog;layout(std430,set=0,binding=1)buffer Result{vec4 value;}result;void main(){result.value=textureLod(fog,vec3(0.5),0.0);}"
				shader=rd.shader_create_from_spirv(rd.shader_compile_spirv_from_source(source))
				pipeline=rd.compute_pipeline_create(shader)
				var state := RDSamplerState.new()
				state.min_filter=RenderingDevice.SAMPLER_FILTER_LINEAR
				state.mag_filter=RenderingDevice.SAMPLER_FILTER_LINEAR
				sampler=rd.sampler_create(state)
				sample_buffer=rd.storage_buffer_create(16)
			var texture_uniform := RDUniform.new()
			texture_uniform.uniform_type=RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
			texture_uniform.binding=0
			texture_uniform.add_id(sampler)
			texture_uniform.add_id(rid)
			var buffer_uniform := RDUniform.new()
			buffer_uniform.uniform_type=RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
			buffer_uniform.binding=1
			buffer_uniform.add_id(sample_buffer)
			var uniforms: Array[RDUniform]=[texture_uniform,buffer_uniform]
			var set := UniformSetCacheRD.get_cache(shader,0,uniforms)
			if set.is_valid():
				var list := rd.compute_list_begin()
				rd.compute_list_bind_compute_pipeline(list,pipeline)
				rd.compute_list_bind_uniform_set(list,set,0)
				rd.compute_list_dispatch(list,1,1,1)
				rd.compute_list_end()
				var values := rd.buffer_get_data(sample_buffer)
				s["volume_sample"]=Vector4(values.decode_float(0),values.decode_float(4),values.decode_float(8),values.decode_float(12))
		snapshot = s
	func shutdown() -> void:
		rids = [shader,sampler,sample_buffer]
		super()

var effect: MaskEffect
var inspect: FogInspect
var mobile := false
var ordinary_reference: Color
var volume_reference: Color

func _build() -> void:
	mobile = RenderingServer.get_current_rendering_method() == "mobile"
	prefix = "NATIVE_WATER_FOG_MOBILE" if mobile else "NATIVE_WATER_FOG"
	build_scene(Vector2i(240,180), Color.WHITE, true)
	for x in [-1.25,1.25]:
		var cube := MeshInstance3D.new()
		cube.mesh = BoxMesh.new()
		cube.mesh.size = Vector3(1.5,1.5,1.5)
		cube.position.x = x
		var material := StandardMaterial3D.new()
		material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		material.albedo_color = Color.WHITE
		cube.material_override = material
		world.add_child(cube)
	environment.fog_enabled = true
	environment.fog_light_color = Color.RED
	environment.fog_light_energy = 1
	environment.fog_density = 0.5
	environment.fog_sky_affect = 1.0
	effect = MaskEffect.new()
	inspect = FogInspect.new()
	if mobile:
		inspect.effect_callback_type = CompositorEffect.EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	use_compositor(world_environment, [effect,inspect])

func _process(_delta: float) -> bool:
	if effect == null or not advance(24):
		return false
	if mobile:
		_mobile()
		return false
	var image := root.get_texture().get_image()
	var left := image.get_pixel(image.get_width()*0.32,image.get_height()*0.5)
	var right := image.get_pixel(image.get_width()*0.68,image.get_height()*0.5)
	var sky_left := image.get_pixel(image.get_width()*0.2,image.get_height()*0.1)
	var sky_right := image.get_pixel(image.get_width()*0.8,image.get_height()*0.1)
	var s := inspect.snapshot
	match stage:
		0:
			ordinary_reference = right
			check(left.g<0.5 and right.g<0.5,"native_distance_fog_baseline",right)
			check(s.get("fog_enabled",false) and absf(s.get("fog_density",0.0)-0.5)<0.0001,"native_parameters_exposed",s)
			effect.active = true
		1:
			check(effect.registered and s.get("ownership_active",false),"registered_current_view_mask",s)
			check(left.g>0.98 and right.g<0.5,"only_masked_opaque_ray_defers_fog",Vector2(left.g,right.g))
			check(right.is_equal_approx(ordinary_reference),"unmasked_opaque_bitwise_color",right)
			check(sky_left.g>0.98 and sky_right.g<0.02,"only_masked_sky_ray_defers_fog",Vector2(sky_left.g,sky_right.g))
			effect.active = false
		2:
			check(not s.get("ownership_active",true) and left.g<0.5,"ownership_does_not_leak_next_frame",left)
			environment.fog_enabled = false
			environment.volumetric_fog_enabled = true
			environment.volumetric_fog_density = 0.25
			environment.volumetric_fog_albedo = Color.BLACK
			environment.volumetric_fog_emission = Color.GREEN
			environment.volumetric_fog_emission_energy = 0.25
			environment.volumetric_fog_length = 12
			environment.volumetric_fog_temporal_reprojection_enabled = false
		3:
			volume_reference = right
			check(s.get("volume_valid",false) and s.get("volume_3d",false),"unintegrated_native_volume_exposed",s)
			var volume_sample: Vector4=s.get("volume_sample",Vector4.ZERO)
			check(volume_sample.y>0.0 and volume_sample.w>0.0,"native_froxel_sampler3d_dispatch",volume_sample)
			check(absf(s.get("volumetric_length",0.0)-12.0)<0.0001,"native_froxel_mapping_exposed",s)
			check(right.r<0.8 and right.g>right.r,"native_volumetric_baseline",right)
			effect.active = true
		4:
			check(left.r>0.98 and left.b>0.98 and right.r<0.8,"only_masked_opaque_ray_defers_volumetric",Vector2(left.r,right.r))
			check(right.is_equal_approx(volume_reference),"unmasked_volumetric_bitwise_color",right)
			check(sky_left.r>0.98 and sky_right.r<0.8,"only_masked_sky_ray_defers_volumetric",Vector2(sky_left.r,sky_right.r))
			environment.sky = Sky.new()
			environment.sky.sky_material = ProceduralSkyMaterial.new()
			environment.background_mode = Environment.BG_SKY
			environment.fog_enabled = true
			environment.fog_aerial_perspective = 1.0
		5:
			check((s.get("radiance_texture",RID()) as RID).is_valid(),"native_aerial_radiance_exposed",s)
			check(s.get("radiance_layers",0)>0 and s.get("radiance_pixel_size",0.0)>0.0,"native_radiance_sampling_metadata",s)
			check(s.has("fog_projection") and s.get("camera_z_far",0.0)>0.0,"unjittered_fog_projection_exposed",s)
			effect.enabled = false
			inspect.shutdown()
			free_rids([effect.mask])
			finish()
	next_stage()
	return false

func _mobile() -> void:
	if stage == 0:
		effect.active = true
	else:
		check(effect.registered and not inspect.snapshot.get("ownership_active",true),"mobile_ownership_inactive",inspect.snapshot)
		effect.enabled = false
		inspect.shutdown()
		free_rids([effect.mask])
		finish()
	next_stage()
