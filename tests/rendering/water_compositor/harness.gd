extends SceneTree

class Effect extends CompositorEffect:
	var rids: Array = []
	func shutdown() -> void:
		enabled = false
		RenderingServer.call_on_render_thread(func():
			var rd := RenderingServer.get_rendering_device()
			for rid in rids:
				if rid.is_valid():
					rd.free_rid(rid))

var prefix := "PROBE"
var frame := 0
var stage := 0
var checks := 0
var failures := 0
var world: Node3D
var camera: Camera3D
var environment: Environment
var world_environment: WorldEnvironment

func _init() -> void:
	call_deferred("_build")

func _build() -> void:
	pass

func check(condition: bool, label: String, detail: Variant = null) -> void:
	checks += 1
	if condition:
		if detail == null:
			print(prefix, "_PASS ", label)
		else:
			print(prefix, "_PASS ", label, " ", detail)
	else:
		failures += 1
		push_error(prefix + "_FAIL " + label + ("" if detail == null else " " + str(detail)))

func advance(interval: int) -> bool:
	frame += 1
	return frame % interval == 0

func next_stage() -> void:
	stage += 1

func finish() -> void:
	if failures == 0:
		print(prefix, "_SUITE_PASS checks=", checks)
	else:
		print(prefix, "_SUITE_FAIL checks=", checks, " failed=", failures)
	quit(0 if failures == 0 else 1)

func build_scene(size: Vector2i, background: Variant = null, linear_tonemap: bool = false, with_environment: bool = true) -> void:
	root.size = size
	world = Node3D.new()
	root.add_child(world)
	camera = Camera3D.new()
	camera.position.z = 6
	camera.current = true
	world.add_child(camera)
	environment = null
	world_environment = null
	if not with_environment:
		return
	environment = Environment.new()
	environment.background_mode = Environment.BG_COLOR
	if background != null:
		environment.background_color = background
	if linear_tonemap:
		environment.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	world_environment = WorldEnvironment.new()
	world_environment.environment = environment
	world.add_child(world_environment)

func use_compositor(target, effects: Array) -> void:
	target.compositor = Compositor.new()
	target.compositor.compositor_effects = effects

func center_pixel() -> Color:
	var image := root.get_texture().get_image()
	return image.get_pixel(image.get_width()/2,image.get_height()/2)

static func free_rids(rids: Array) -> void:
	RenderingServer.call_on_render_thread(func():
		var rd := RenderingServer.get_rendering_device()
		for rid in rids:
			if rid.is_valid():
				rd.free_rid(rid))
