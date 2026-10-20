extends SceneTree

class TemporalProbe extends CompositorEffect:
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
		enabled = false
		RenderingServer.call_on_render_thread(func():
			var rd := RenderingServer.get_rendering_device()
			for rid in [mask,pipeline,shader]:
				if rid.is_valid(): rd.free_rid(rid)
			mask = RID()
			pipeline = RID()
			shader = RID())

var effect: TemporalProbe
var frames := 0
var stage := 0
var checks := 0
var failed := false

func _init() -> void:
	call_deferred("_build")

func _build() -> void:
	root.size = Vector2i(240,180)
	root.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
	var world := Node3D.new()
	root.add_child(world)
	var camera := Camera3D.new()
	camera.position.z = 6
	camera.current = true
	world.add_child(camera)
	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	environment.environment.background_mode = Environment.BG_COLOR
	environment.environment.background_color = Color.WHITE
	environment.environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world.add_child(environment)
	effect = TemporalProbe.new()
	environment.compositor = Compositor.new()
	environment.compositor.compositor_effects = [effect]

func check(condition: bool, label: String, pixel: Color) -> void:
	if not condition:
		failed = true
		push_error("NATIVE_WATER_FSR2_SMOKE_FAIL " + label + " " + str(pixel))
		quit(1)
		return
	checks += 1
	print("NATIVE_WATER_FSR2_SMOKE_PASS ",label," ",pixel)

func _process(_delta: float) -> bool:
	frames += 1
	if effect == null or frames%24 != 0:
		return false
	var image := root.get_texture().get_image()
	var pixel := image.get_pixel(image.get_width()/2,image.get_height()/2)
	match stage:
		0:
			check(minf(pixel.r,pixel.b)>0.05,"native_fsr2_accumulates_changing_pattern",pixel)
			effect.reactive = true
		1:
			check(effect.registered and is_finite(pixel.r) and is_finite(pixel.b) and maxf(pixel.r,pixel.b)>0.2,"fsr2_reactive_binding_renders_finite",pixel)
			effect.reactive = false
		2:
			check(minf(pixel.r,pixel.b)>0.05,"unregistered_mask_does_not_leak",pixel)
			effect.reset = true
		3:
			check(is_finite(pixel.r) and is_finite(pixel.b) and maxf(pixel.r,pixel.b)>0.2,"fsr2_explicit_reset_renders_finite",pixel)
			effect.shutdown()
			if not failed: print("NATIVE_WATER_FSR2_SMOKE_SUITE_PASS checks=",checks)
			quit(1 if failed else 0)
	stage += 1
	return false
