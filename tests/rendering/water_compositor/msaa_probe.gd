extends "res://harness.gd"

class ColorProbe extends Effect:
	var commit := false
	var committed := false
	var shader := RID()
	var pipeline := RID()
	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_SKY
		access_resolved_color = true
	func _render_callback(_type: int, data: RenderData) -> void:
		var rd := RenderingServer.get_rendering_device()
		if not shader.is_valid():
			var source := RDShaderSource.new()
			source.source_compute = "#version 450\nlayout(local_size_x=8,local_size_y=8) in;layout(rgba16f,set=0,binding=0) uniform image2D target;void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(any(greaterThanEqual(p,imageSize(target))))return;imageStore(target,p,vec4(1,0,0,1));}"
			var spirv := rd.shader_compile_spirv_from_source(source)
			if not spirv.compile_error_compute.is_empty():
				push_error(spirv.compile_error_compute)
				return
			shader = rd.shader_create_from_spirv(spirv)
			pipeline = rd.compute_pipeline_create(shader)
		var buffers: RenderSceneBuffersRD = data.get_render_scene_buffers()
		var uniform := RDUniform.new()
		uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		uniform.binding = 0
		uniform.add_id(buffers.get_color_layer(0))
		var uniforms: Array[RDUniform] = [uniform]
		var set := rd.uniform_set_create(uniforms,shader,0)
		var list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(list,pipeline)
		rd.compute_list_bind_uniform_set(list,set,0)
		var size := buffers.get_internal_size()
		rd.compute_list_dispatch(list,ceili(size.x/8.0),ceili(size.y/8.0),1)
		rd.compute_list_end()
		rd.free_rid(set)
		if commit and data.has_method("commit_water_color"):
			committed = data.call("commit_water_color")
	func shutdown() -> void:
		rids = [pipeline,shader]
		super()

class Inputs extends Effect:
	var shader := RID()
	var pipeline := RID()
	var sampler := RID()
	var result_buffer := RID()
	var snapshot := Vector4.ZERO
	func _init() -> void:
		effect_callback_type=EFFECT_CALLBACK_TYPE_POST_SKY
		access_resolved_color=true
		access_resolved_depth=true
	func _render_callback(_type: int,data: RenderData) -> void:
		var rd := RenderingServer.get_rendering_device()
		var buffers: RenderSceneBuffersRD=data.get_render_scene_buffers()
		if not shader.is_valid():
			var source := RDShaderSource.new()
			source.source_compute="""#version 450
layout(local_size_x=8,local_size_y=8)in;
layout(rgba16f,set=0,binding=0)uniform image2D color;
layout(set=0,binding=1)uniform sampler2D depth;
layout(std430,set=0,binding=2)buffer Result{vec4 value;}result;
void main(){ivec2 q=ivec2(gl_GlobalInvocationID.xy);ivec2 size=imageSize(color);if(any(greaterThanEqual(q,size)))return;vec4 c=imageLoad(color,q);if(all(equal(q,size/2)))result.value=vec4(c.rgb,texelFetch(depth,q,0).r);c.r*=0.5;imageStore(color,q,c);}
"""
			shader=rd.shader_create_from_spirv(rd.shader_compile_spirv_from_source(source))
			pipeline=rd.compute_pipeline_create(shader)
			sampler=rd.sampler_create(RDSamplerState.new())
			result_buffer=rd.storage_buffer_create(16)
		var color_uniform := RDUniform.new()
		color_uniform.uniform_type=RenderingDevice.UNIFORM_TYPE_IMAGE
		color_uniform.binding=0
		color_uniform.add_id(buffers.get_color_layer(0))
		var depth_uniform := RDUniform.new()
		depth_uniform.uniform_type=RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		depth_uniform.binding=1
		depth_uniform.add_id(sampler)
		depth_uniform.add_id(buffers.get_depth_layer(0))
		var result_uniform := RDUniform.new()
		result_uniform.uniform_type=RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		result_uniform.binding=2
		result_uniform.add_id(result_buffer)
		var uniforms: Array[RDUniform]=[color_uniform,depth_uniform,result_uniform]
		var set := UniformSetCacheRD.get_cache(shader,0,uniforms)
		var size := buffers.get_internal_size()
		var list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(list,pipeline)
		rd.compute_list_bind_uniform_set(list,set,0)
		rd.compute_list_dispatch(list,(size.x+7)/8,(size.y+7)/8,1)
		rd.compute_list_end()
		var bytes := rd.buffer_get_data(result_buffer)
		snapshot=Vector4(bytes.decode_float(0),bytes.decode_float(4),bytes.decode_float(8),bytes.decode_float(12))
		data.call("commit_water_color")
	func shutdown() -> void:
		rids = [shader,sampler,result_buffer]
		super()

var color_effect: ColorProbe
var input_effect: Inputs
var inputs_started := false

func _build() -> void:
	prefix = "NATIVE_WATER_MSAA"
	build_scene(Vector2i(240,180), Color.WHITE, true)
	_cube(BoxMesh.new())
	color_effect = ColorProbe.new()
	use_compositor(world_environment, [color_effect])

func _cube(mesh: Mesh) -> void:
	var cube := MeshInstance3D.new()
	cube.mesh = mesh
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = Color.WHITE
	cube.material_override = material
	world.add_child(cube)

func _process(_delta: float) -> bool:
	if color_effect == null or not advance(20):
		return false
	if inputs_started:
		_run_inputs()
		return false
	var image := root.get_texture().get_image()
	var pixel := image.get_pixel(image.get_width()/2,image.get_height()/2)
	match stage:
		0:
			check(pixel.r>0.9 and pixel.g<0.02 and pixel.b<0.02,"non_msaa_compositor_writes",pixel)
			root.msaa_3d = Viewport.MSAA_4X
		1:
			check(pixel.r>0.9 and pixel.g>0.9 and pixel.b>0.9,"baseline_later_msaa_resolve_overwrites",pixel)
			color_effect.commit = true
		2:
			check(color_effect.committed and pixel.r>0.9 and pixel.g<0.02 and pixel.b<0.02,"native_reinjection_preserves_4x",pixel)
			root.msaa_3d = Viewport.MSAA_8X
		3:
			check(pixel.r>0.9 and pixel.g<0.02 and pixel.b<0.02,"native_reinjection_preserves_8x",pixel)
			root.size = Vector2i(400,220)
		4:
			check(image.get_width()==400 and pixel.r>0.9 and pixel.g<0.02 and pixel.b<0.02,"resize_rebuilds_msaa_targets",pixel)
			var quad := MeshInstance3D.new()
			quad.mesh = QuadMesh.new()
			quad.mesh.size = Vector2(4,4)
			quad.position.z = 2
			var material := StandardMaterial3D.new()
			material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
			material.albedo_color = Color(0,1,0,0.5)
			material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
			quad.material_override = material
			world.add_child(quad)
		5:
			check(pixel.r>0.2 and pixel.g>0.2 and pixel.b<0.05,"transparent_pass_blends_with_reinjected_color",pixel)
			color_effect.shutdown()
			_start_inputs()
	next_stage()
	return false

func _start_inputs() -> void:
	world.free()
	build_scene(Vector2i(240,180), null, false, false)
	root.msaa_3d = Viewport.MSAA_DISABLED
	var mesh := BoxMesh.new()
	mesh.size=Vector3(4,4,2)
	_cube(mesh)
	input_effect = Inputs.new()
	use_compositor(camera,[input_effect])
	stage = -1
	inputs_started = true

func _run_inputs() -> void:
	var s := input_effect.snapshot
	var pixel := center_pixel()
	check(s.x>0.99 and s.y>0.99 and s.z>0.99,"current_post_sky_color_stage_"+str(stage),s)
	check(s.w>0.005 and s.w<0.1,"current_post_sky_depth_stage_"+str(stage),s.w)
	check(pixel.r>0.7 and pixel.r<0.8 and pixel.g>0.99,"edited_input_survives_resolve_stage_"+str(stage),pixel)
	match stage:
		0: root.msaa_3d=Viewport.MSAA_4X
		1: root.msaa_3d=Viewport.MSAA_8X
		2: root.size=Vector2i(400,260)
		3:
			input_effect.shutdown()
			finish()
	next_stage()
