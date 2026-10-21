extends "res://harness.gd"

class TemporalProbe extends Effect:
	var reactive := false
	var reset := false
	var registered := false
	var phase := 0
	var shader := RID()
	var pipeline := RID()
	var mask := RID()
	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
		access_resolved_color = true
		needs_motion_vectors = true
	func _render_callback(_type: int, data: RenderData) -> void:
		var rd := RenderingServer.get_rendering_device()
		var buffers: RenderSceneBuffersRD = data.get_render_scene_buffers()
		var size := buffers.get_internal_size()
		if not shader.is_valid():
			var source := RDShaderSource.new()
			source.source_compute = "#version 450\nlayout(local_size_x=8,local_size_y=8) in;layout(rgba16f,set=0,binding=0) uniform image2D target;layout(push_constant,std430) uniform P{uint phase;}p;void main(){ivec2 q=ivec2(gl_GlobalInvocationID.xy);if(any(greaterThanEqual(q,imageSize(target))))return;float r=float((uint(q.x+q.y)+p.phase)%2u);imageStore(target,q,vec4(r,0,1-r,0));}"
			var spirv := rd.shader_compile_spirv_from_source(source)
			shader = rd.shader_create_from_spirv(spirv)
			pipeline = rd.compute_pipeline_create(shader)
			var format := RDTextureFormat.new()
			format.width = size.x
			format.height = size.y
			format.format = RenderingDevice.DATA_FORMAT_R8_UNORM
			format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
			var bytes := PackedByteArray()
			bytes.resize(size.x*size.y)
			bytes.fill(255)
			mask = rd.texture_create(format,RDTextureView.new(),[bytes])
		var uniform := RDUniform.new()
		uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		uniform.binding = 0
		uniform.add_id(buffers.get_color_layer(0))
		var uniforms: Array[RDUniform] = [uniform]
		var set := rd.uniform_set_create(uniforms,shader,0)
		var list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(list,pipeline)
		rd.compute_list_bind_uniform_set(list,set,0)
		rd.compute_list_set_push_constant(list,PackedInt32Array([phase%2]).to_byte_array(),4)
		rd.compute_list_dispatch(list,ceili(size.x/8.0),ceili(size.y/8.0),1)
		rd.compute_list_end()
		rd.free_rid(set)
		phase += 1
		if reactive or reset:
			if data.has_method("set_water_temporal_data"):
				registered = data.call("set_water_temporal_data",0,mask if reactive else RID(),reset)
	func shutdown() -> void:
		rids = [mask,pipeline,shader]
		super()

var effect: TemporalProbe
var fsr2 := false

func _build() -> void:
	fsr2 = OS.get_cmdline_user_args().has("fsr2")
	prefix = "NATIVE_WATER_FSR2_SMOKE" if fsr2 else "NATIVE_WATER_TEMPORAL"
	build_scene(Vector2i(240,180), Color.WHITE, true)
	if fsr2:
		root.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
	else:
		root.use_taa = true
	effect = TemporalProbe.new()
	use_compositor(world_environment, [effect])

func _process(_delta: float) -> bool:
	if effect == null or not advance(24):
		return false
	var pixel := center_pixel()
	match stage:
		0:
			check(minf(pixel.r,pixel.b)>0.05,"native_fsr2_accumulates_changing_pattern" if fsr2 else "native_taa_accumulates_changing_pattern",pixel)
			effect.reactive = true
		1:
			if fsr2:
				check(effect.registered and is_finite(pixel.r) and is_finite(pixel.b) and maxf(pixel.r,pixel.b)>0.2,"fsr2_reactive_binding_renders_finite",pixel)
			else:
				check(effect.registered and maxf(pixel.r,pixel.b)>0.98 and minf(pixel.r,pixel.b)<0.02,"reactivity_is_consumed_by_taa",pixel)
			effect.reactive = false
		2:
			check(minf(pixel.r,pixel.b)>0.05,"unregistered_mask_does_not_leak",pixel)
			effect.reset = true
		3:
			if fsr2:
				check(is_finite(pixel.r) and is_finite(pixel.b) and maxf(pixel.r,pixel.b)>0.2,"fsr2_explicit_reset_renders_finite",pixel)
			else:
				check(maxf(pixel.r,pixel.b)>0.98 and minf(pixel.r,pixel.b)<0.02,"explicit_taa_history_reset",pixel)
			effect.shutdown()
			finish()
	next_stage()
	return false
