extends SceneTree
class Inspect extends CompositorEffect:
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
        enabled=false
        RenderingServer.call_on_render_thread(func():
            var rd := RenderingServer.get_rendering_device()
            for rid in [shader,sampler,result_buffer]:
                if rid.is_valid(): rd.free_rid(rid))

var effect := Inspect.new()
var water: MeshInstance3D
var behind: MeshInstance3D
var front: MeshInstance3D
var frame := 0
var stage := 0
var checks := 0
var failed := false
func _init() -> void: call_deferred("build")
func material(color: Color,preserve: bool) -> ShaderMaterial:
    var shader := Shader.new()
    shader.code="shader_type spatial;render_mode unshaded,cull_disabled"+(",preserve_temporal_alpha" if preserve else "")+";void fragment(){ALBEDO=vec3(%f,%f,%f);ALPHA=%f;}"%[color.r,color.g,color.b,color.a]
    var result := ShaderMaterial.new()
    result.shader=shader
    return result
func quad(scene: Node3D,z: float,color: Color) -> MeshInstance3D:
    var result := MeshInstance3D.new()
    var mesh := QuadMesh.new()
    mesh.size=Vector2(4,4)
    result.mesh=mesh
    result.material_override=material(color,false)
    scene.add_child(result)
    result.position.z=z
    return result
func build() -> void:
    root.size=Vector2i(240,180)
    root.scaling_3d_mode=Viewport.SCALING_3D_MODE_FSR2
    var scene := Node3D.new()
    root.add_child(scene)
    var camera := Camera3D.new()
    scene.add_child(camera)
    camera.position.z=6
    camera.current=true
    var env := WorldEnvironment.new()
    env.environment=Environment.new()
    env.environment.background_mode=Environment.BG_COLOR
    env.environment.background_color=Color.BLACK
    scene.add_child(env)
    behind=quad(scene,0,Color(0,1,0,0.3))
    behind.visible=false
    water=quad(scene,1,Color.BLUE)
    front=quad(scene,2,Color(1,0,0,0.4))
    front.visible=false
    camera.compositor=Compositor.new()
    camera.compositor.compositor_effects=[effect]
func check(condition: bool,label: String) -> void:
    checks+=1
    if condition: print("NATIVE_ALPHA_PASS ",label," ",effect.value)
    else:
        failed=true
        push_error("NATIVE_ALPHA_FAIL "+label+" "+str(effect.value))
func _process(_delta: float) -> bool:
    frame+=1
    if frame%20!=0: return false
    var c := effect.value
    match stage:
        0:
            check(c.a>0.99,"ordinary_transparent_alpha_one_reference")
            check(ClassDB.class_has_method("RenderDataRD","supports_water_temporal_alpha"),"explicit_capability_available")
            water.material_override=material(Color.BLUE,true)
        1:
            check(c.a<0.001 and c.b>0.99,"water_preserves_zero_alpha_without_changing_rgb")
            behind.visible=true
        2:
            check(absf(c.a-0.3)<0.002 and c.b>0.99 and c.g<0.001,"preceding_transparent_alpha_retained")
            front.visible=true
        3:
            check(absf(c.a-0.58)<0.002,"following_transparent_alpha_composes_normally")
            check(absf(c.r-0.4)<0.002 and absf(c.b-0.6)<0.002,"rgb_opacity_blending_unchanged")
            root.msaa_3d=Viewport.MSAA_4X
        4:
            check(absf(c.a-0.58)<0.002 and absf(c.r-0.4)<0.002,"msaa_preserves_other_transparent_alpha")
            water.visible=false
        5:
            check(absf(c.a-0.58)<0.002 and absf(c.g-0.18)<0.002,"ordinary_contributors_unchanged_when_water_removed")
            effect.shutdown()
            print("NATIVE_ALPHA_SUITE_","FAIL" if failed else "PASS"," checks=",checks)
            quit(1 if failed else 0)
    stage+=1
    return false
