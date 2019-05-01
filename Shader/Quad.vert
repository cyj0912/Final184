#version 450

layout(location = 0) out vec2 outUV;

void main()
{
	// Draws a fullscreen quad with only 3 vertices
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUV * 2 - 1, 0, 1);
}