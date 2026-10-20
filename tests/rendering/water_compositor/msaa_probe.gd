extends SceneTree

class ColorProbe extends CompositorEffect:
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
		enabled = false
		RenderingServer.call_on_render_thread(func():
			var rd := RenderingServer.get_rendering_device()
			if pipeline.is_valid(): rd.free_rid(pipeline)
			if shader.is_valid(): rd.free_rid(shader)
			pipeline = RID()
			shader = RID())

var effect: ColorProbe
var frames := 0
var stage := 0
var scene: Node3D
var checks := 0
var failed := false

func _init() -> void:
	call_deferred("_build")

func _build() -> void:
	root.size = Vector2i(240,180)
	scene = Node3D.new()
	root.add_child(scene)
	var camera := Camera3D.new()
	camera.position.z = 6
	camera.current = true
	scene.add_child(camera)
	var cube := MeshInstance3D.new()
	cube.mesh = BoxMesh.new()
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = Color.WHITE
	cube.material_override = material
	scene.add_child(cube)
	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	environment.environment.background_mode = Environment.BG_COLOR
	environment.environment.background_color = Color.WHITE
	environment.environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	scene.add_child(environment)
	effect = ColorProbe.new()
	environment.compositor = Compositor.new()
	environment.compositor.compositor_effects = [effect]

func check(condition: bool, label: String, pixel: Color) -> void:
	if not condition:
		push_error("NATIVE_WATER_MSAA_FAIL " + label + " " + str(pixel))
		failed = true
		quit(1)
		return
	checks += 1
	print("NATIVE_WATER_MSAA_PASS ",label," ",pixel)

func _process(_delta: float) -> bool:
	frames += 1
	if effect == null or frames%20 != 0:
		return false
	var image := root.get_texture().get_image()
	var pixel := image.get_pixel(image.get_width()/2,image.get_height()/2)
	match stage:
		0:
			check(pixel.r>0.9 and pixel.g<0.02 and pixel.b<0.02,"non_msaa_compositor_writes",pixel)
			root.msaa_3d = Viewport.MSAA_4X
		1:
			check(pixel.r>0.9 and pixel.g>0.9 and pixel.b>0.9,"baseline_later_msaa_resolve_overwrites",pixel)
			effect.commit = true
		2:
			check(effect.committed and pixel.r>0.9 and pixel.g<0.02 and pixel.b<0.02,"native_reinjection_preserves_4x",pixel)
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
			scene.add_child(quad)
		5:
			check(pixel.r>0.2 and pixel.g>0.2 and pixel.b<0.05,"transparent_pass_blends_with_reinjected_color",pixel)
			effect.shutdown()
			print("NATIVE_WATER_MSAA_SUITE_PASS checks=",checks)
			quit(1 if failed else 0)
	stage += 1
	return false
