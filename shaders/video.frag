#version 450
// A video frame from its three planes: luma at full size, the two chroma planes at whatever size they have (half for
// 4:2:0), each sampled at the same coordinate. The matrix's rows turn (y, u, v, 1) into red, green and blue, with the
// range and offsets folded in, so one shader serves BT.601 and BT.709, limited and full range.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_y;
layout(set = 0, binding = 1) uniform sampler2D u_u;
layout(set = 0, binding = 2) uniform sampler2D u_v;
layout(push_constant) uniform Push { vec4 r; vec4 g; vec4 b; } pc;

void main()
{
	vec4 yuv = vec4(texture(u_y, v_uv).r, texture(u_u, v_uv).r, texture(u_v, v_uv).r, 1.0);
	o_color = vec4(dot(pc.r, yuv), dot(pc.g, yuv), dot(pc.b, yuv), 1.0) * v_color;
}
