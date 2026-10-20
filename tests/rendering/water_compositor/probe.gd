extends SceneTree

class Probe extends CompositorEffect:
	var mutex := Mutex.new()
	var requested := true
	var snapshot: Dictionary = {}
	func _init() -> void:
		effect_callback_type = EFFECT_CALLBACK_TYPE_POST_SKY
	func request() -> void:
		mutex.lock()
		requested = true
		snapshot = {}
		mutex.unlock()
	func read_snapshot() -> Dictionary:
		mutex.lock()
		var copy := snapshot.duplicate()
		mutex.unlock()
		return copy
	func _render_callback(_type: int, data: RenderData) -> void:
		mutex.lock()
		if not requested:
			mutex.unlock()
			return
		requested = false
		mutex.unlock()
		var result := {"available": data.has_method("get_water_lighting_data")}
		if result.available:
			var lighting: Dictionary = data.call("get_water_lighting_data")
			for key in ["abi_version", "directional_light_count", "omni_light_count", "spot_light_count", "area_light_count", "max_directional_lights", "directional_light_stride_bytes", "positional_light_stride_bytes", "emissive_exposure_normalization"]:
				result[key] = lighting.get(key, -1)
			var rd := RenderingServer.get_rendering_device()
			if lighting.has("directional_light_buffer"):
				var bytes := rd.buffer_get_data(lighting.directional_light_buffer)
				result["directional_buffer_size"] = bytes.size()
				result["directional_energy"] = bytes.decode_float(12)
				result["directional_direction"] = Vector3(bytes.decode_float(0),bytes.decode_float(4),bytes.decode_float(8))
				result["directional_shadow_opacity"] = bytes.decode_float(52)
				for key in ["directional_shadow_texture", "positional_shadow_texture"]:
					var rid: RID = lighting.get(key,RID())
					result[key+"_valid"] = rid.is_valid() and rd.texture_is_valid(rid)
		mutex.lock()
		snapshot = result
		mutex.unlock()

var probe: Probe
var camera: Camera3D
var sun: DirectionalLight3D
var omni: OmniLight3D
var spot: SpotLight3D
var frames := 0
var stage := 0
var checks := 0
var failed := false

func _init() -> void:
	call_deferred("_build")

func _build() -> void:
	root.size = Vector2i(240,180)
	var scene := Node3D.new()
	root.add_child(scene)
	camera = Camera3D.new()
	scene.add_child(camera)
	camera.position = Vector3(0,0,6)
	camera.current = true
	camera.attributes = CameraAttributesPractical.new()
	camera.attributes.exposure_multiplier = 2.0
	var cube := MeshInstance3D.new()
	cube.mesh = BoxMesh.new()
	scene.add_child(cube)
	sun = DirectionalLight3D.new()
	sun.light_energy = 2.0
	sun.shadow_enabled = true
	sun.rotation_degrees = Vector3(-25,30,0)
	scene.add_child(sun)
	omni = OmniLight3D.new()
	omni.omni_range = 20.0
	omni.shadow_enabled = true
	omni.position = Vector3(-2,2,3)
	scene.add_child(omni)
	spot = SpotLight3D.new()
	spot.spot_range = 20.0
	spot.shadow_enabled = true
	spot.position = Vector3(2,2,3)
	scene.add_child(spot)
	spot.look_at(Vector3.ZERO)
	var environment := WorldEnvironment.new()
	environment.environment = Environment.new()
	environment.environment.background_mode = Environment.BG_COLOR
	scene.add_child(environment)
	probe = Probe.new()
	environment.compositor = Compositor.new()
	environment.compositor.compositor_effects = [probe]

func check(condition: bool, label: String) -> void:
	if not condition:
		push_error("NATIVE_WATER_ABI_FAIL " + label)
		failed = true
		quit(1)
		return
	checks += 1
	print("NATIVE_WATER_ABI_PASS ",label)

func _process(_delta: float) -> bool:
	frames += 1
	if probe == null or frames%15 != 0:
		return false
	var s := probe.read_snapshot()
	if s.is_empty():
		check(frames < 600,"callback_timeout")
		return false
	print("NATIVE_WATER_ABI_SNAPSHOT ",stage," ",s)
	match stage:
		0:
			check(s.get("available",false),"native_method_available")
			check(s.get("abi_version",0)==1,"abi_version")
			check(s.get("directional_buffer_size",0)==8*464,"native_uniform_layout")
			check(s.get("positional_light_stride_bytes",0)==224,"native_positional_layout")
			check(s.get("directional_light_count",0)==1 and s.get("omni_light_count",0)==1 and s.get("spot_light_count",0)==1,"current_light_counts")
			check(s.get("directional_shadow_texture_valid",false) and s.get("positional_shadow_texture_valid",false),"native_shadow_atlases")
			check(absf(s.get("emissive_exposure_normalization",0.0)-2.0)<0.0001,"explicit_scene_exposure")
			check(absf(s.get("directional_energy",0.0)-4.0*PI)<0.001,"direct_energy_preexposed_once")
			check(absf(s.get("directional_shadow_opacity",0.0)-1.0)<0.001,"native_shadow_flag")
			omni.visible = false
			spot.visible = false
		1:
			check(s.get("omni_light_count",1)==0 and s.get("spot_light_count",1)==0,"removed_lights_do_not_leak")
			camera.rotation_degrees.y = 12
			camera.attributes.exposure_multiplier = 0.25
		2:
			var expected := camera.global_basis.inverse()*sun.global_basis.z
			check((s.get("directional_direction",Vector3.ZERO) as Vector3).distance_to(expected)<0.0001,"camera_relative_light_direction")
			check(absf(s.get("emissive_exposure_normalization",0.0)-0.25)<0.0001,"exposure_updates_per_view")
			check(absf(s.get("directional_energy",0.0)-0.5*PI)<0.001,"light_energy_tracks_exposure")
			sun.visible = false
		3:
			check(s.get("directional_light_count",1)==0,"removed_directional_does_not_leak")
			print("NATIVE_WATER_ABI_SUITE_PASS checks=",checks)
			quit(1 if failed else 0)
	stage += 1
	probe.call_deferred("request")
	return false
