extends "res://harness.gd"

class Inspect extends Effect:
	var shader := RID()
	var pipeline := RID()
	var sampler := RID()
	var result_buffer := RID()
	var value := Color()
	func _init() -> void:
		effect_callback_type=EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
		access_resolved_color=true
	func _render_callback(_type: int,data: RenderData) -> void:
		var rd := RenderingServer.get_rendering_device()
		if not shader.is_valid():
			var source := RDShaderSource.new()
			source.source_compute="#version 450\nlayout(local_size_x=1)in;layout(set=0,binding=0)uniform sampler2D color;layout(std430,set=0,binding=1)buffer Result{vec4 value;}result;void main(){result.value=texelFetch(color,textureSize(color,0)/2,0);}"
			shader=rd.shader_create_from_spirv(rd.shader_compile_spirv_from_source(source))
			pipeline=rd.compute_pipeline_create(shader)
			sampler=rd.sampler_create(RDSamplerState.new())
			result_buffer=rd.storage_buffer_create(16)
		var texture_uniform := RDUniform.new()
		texture_uniform.uniform_type=RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		texture_uniform.binding=0
		texture_uniform.add_id(sampler)
		texture_uniform.add_id(data.get_render_scene_buffers().get_color_layer(0))
		var output_uniform := RDUniform.new()
		output_uniform.uniform_type=RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		output_uniform.binding=1
		output_uniform.add_id(result_buffer)
		var uniforms: Array[RDUniform]=[texture_uniform,output_uniform]
		var set := UniformSetCacheRD.get_cache(shader,0,uniforms)
		var list := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(list,pipeline)
		rd.compute_list_bind_uniform_set(list,set,0)
		rd.compute_list_dispatch(list,1,1,1)
		rd.compute_list_end()
		var bytes := rd.buffer_get_data(result_buffer)
		value=Color(bytes.decode_float(0),bytes.decode_float(4),bytes.decode_float(8),bytes.decode_float(12))
	func shutdown() -> void:
		rids = [shader,sampler,result_buffer]
		super()

var effect := Inspect.new()
var water: MeshInstance3D
var behind: MeshInstance3D
var front: MeshInstance3D

func _build() -> void:
	prefix = "NATIVE_ALPHA"
	build_scene(Vector2i(240,180), Color.BLACK)
	root.scaling_3d_mode=Viewport.SCALING_3D_MODE_FSR2
	behind=quad(0,Color(0,1,0,0.3))
	behind.visible=false
	water=quad(1,Color.BLUE)
	front=quad(2,Color(1,0,0,0.4))
	front.visible=false
	use_compositor(camera,[effect])

func material(color: Color,preserve: bool) -> ShaderMaterial:
	var shader := Shader.new()
	shader.code="shader_type spatial;render_mode unshaded,cull_disabled"+(",preserve_temporal_alpha" if preserve else "")+";void fragment(){ALBEDO=vec3(%f,%f,%f);ALPHA=%f;}"%[color.r,color.g,color.b,color.a]
	var result := ShaderMaterial.new()
	result.shader=shader
	return result

func quad(z: float,color: Color) -> MeshInstance3D:
	var result := MeshInstance3D.new()
	var mesh := QuadMesh.new()
	mesh.size=Vector2(4,4)
	result.mesh=mesh
	result.material_override=material(color,false)
	world.add_child(result)
	result.position.z=z
	return result

func _process(_delta: float) -> bool:
	if not advance(20):
		return false
	var c := effect.value
	match stage:
		0:
			check(c.a>0.99,"ordinary_transparent_alpha_one_reference",c)
			check(ClassDB.class_has_method("RenderDataRD","supports_water_temporal_alpha"),"explicit_capability_available",c)
			water.material_override=material(Color.BLUE,true)
		1:
			check(c.a<0.001 and c.b>0.99,"water_preserves_zero_alpha_without_changing_rgb",c)
			behind.visible=true
		2:
			check(absf(c.a-0.3)<0.002 and c.b>0.99 and c.g<0.001,"preceding_transparent_alpha_retained",c)
			front.visible=true
		3:
			check(absf(c.a-0.58)<0.002,"following_transparent_alpha_composes_normally",c)
			check(absf(c.r-0.4)<0.002 and absf(c.b-0.6)<0.002,"rgb_opacity_blending_unchanged",c)
			root.msaa_3d=Viewport.MSAA_4X
		4:
			check(absf(c.a-0.58)<0.002 and absf(c.r-0.4)<0.002,"msaa_preserves_other_transparent_alpha",c)
			water.visible=false
		5:
			check(absf(c.a-0.58)<0.002 and absf(c.g-0.18)<0.002,"ordinary_contributors_unchanged_when_water_removed",c)
			effect.shutdown()
			finish()
	next_stage()
	return false
