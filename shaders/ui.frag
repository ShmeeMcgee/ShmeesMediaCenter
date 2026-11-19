#version 450
// The texture times the corner's colour. The coverage atlas is swizzled to (1, 1, 1, coverage), so text, icons, shapes
// and fills come out in the vertex colour; posters and artwork come out tinted by it.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_tex;

void main()
{
	o_color = texture(u_tex, v_uv) * v_color;
}
