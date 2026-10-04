#[vertex]

#version 450

#VERSION_DEFINES

layout(location = 0) out vec2 uv_interp;

void main() {
	vec2 base_arr[3] = vec2[](vec2(-1.0, -1.0), vec2(-1.0, 3.0), vec2(3.0, -1.0));
	gl_Position = vec4(base_arr[gl_VertexIndex], 0.0, 1.0);
	uv_interp = clamp(gl_Position.xy, vec2(0.0, 0.0), vec2(1.0, 1.0)) * 2.0;
}

#[fragment]

#version 450

#VERSION_DEFINES

layout(location = 0) in vec2 uv_interp;

layout(set = 0, binding = 0) uniform sampler2D source_depth;

layout(location = 0) out vec4 frag_color;

layout(push_constant, std430) uniform Params {
	highp mat4 inv_projection;
	float z_near;
	float z_far;
	vec2 pad;
}
params;

void main() {
	float depth = textureLod(source_depth, uv_interp, 0.0).x;
	vec4 view = params.inv_projection * vec4(uv_interp * 2.0 - 1.0, depth, 1.0);
	float linear_depth = -view.z / view.w;
	float mapped = 1.0 - clamp(log(max(linear_depth, params.z_near) / params.z_near) / log(params.z_far / params.z_near), 0.0, 1.0);
	frag_color = vec4(vec3(mapped), 1.0);
}
