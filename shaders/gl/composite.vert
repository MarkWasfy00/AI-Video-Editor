#version 330 core
// Draws a unit quad (triangle strip, no vertex buffer) transformed by uMvp.

uniform mat4 uMvp;
out vec2 vUv;

void main()
{
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vUv = p;
    gl_Position = uMvp * vec4(p, 0.0, 1.0);
}
