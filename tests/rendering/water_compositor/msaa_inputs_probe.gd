extends SceneTree
class Inputs extends CompositorEffect:
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
        enabled=false
        RenderingServer.call_on_render_thread(func():
            var rd := RenderingServer.get_rendering_device()
            for rid in [shader,sampler,result_buffer]:
                if rid.is_valid(): rd.free_rid(rid))
var effect := Inputs.new()
var stage := 0
var frame := 0
var checks := 0
var failed := false
func _init() -> void: call_deferred("build")
func build() -> void:
    root.size=Vector2i(240,180)
    var scene := Node3D.new()
    root.add_child(scene)
    var camera := Camera3D.new()
    scene.add_child(camera)
    camera.position.z=6
    camera.current=true
    var cube := MeshInstance3D.new()
    var mesh := BoxMesh.new()
    mesh.size=Vector3(4,4,2)
    cube.mesh=mesh
    var material := StandardMaterial3D.new()
    material.shading_mode=BaseMaterial3D.SHADING_MODE_UNSHADED
    material.albedo_color=Color.WHITE
    cube.material_override=material
    scene.add_child(cube)
    camera.compositor=Compositor.new()
    camera.compositor.compositor_effects=[effect]
func check(ok: bool,label: String,value: Variant) -> void:
    checks+=1
    if ok: print("NATIVE_MSAA_INPUTS_PASS ",label," ",value)
    else:
        failed=true
        push_error("NATIVE_MSAA_INPUTS_FAIL "+label+" "+str(value))
func _process(_delta: float) -> bool:
    frame+=1
    if frame%20!=0: return false
    var s := effect.snapshot
    var image := root.get_texture().get_image()
    var pixel := image.get_pixel(image.get_width()/2,image.get_height()/2)
    check(s.x>0.99 and s.y>0.99 and s.z>0.99,"current_post_sky_color_stage_"+str(stage),s)
    check(s.w>0.005 and s.w<0.1,"current_post_sky_depth_stage_"+str(stage),s.w)
    check(pixel.r>0.7 and pixel.r<0.8 and pixel.g>0.99,"edited_input_survives_resolve_stage_"+str(stage),pixel)
    match stage:
        0: root.msaa_3d=Viewport.MSAA_4X
        1: root.msaa_3d=Viewport.MSAA_8X
        2: root.size=Vector2i(400,260)
        3:
            effect.shutdown()
            print("NATIVE_MSAA_INPUTS_SUITE_","FAIL" if failed else "PASS"," checks=",checks)
            quit(1 if failed else 0)
    stage+=1
    return false
