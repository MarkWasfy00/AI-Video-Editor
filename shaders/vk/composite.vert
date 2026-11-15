#version 450
// Vulkan version of shaders/gl/composite.vert. Parameters come from push constants.

layout(push_constant) uniform Params {
    mat4 mvp;
    vec4 color;
    vec4 fx;
    vec4 texel;
    vec4 crop;
} pc;

layout(location = 0) out vec2 vUv;

void main()
{
    vec2 p = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));
    vUv = p;
    gl_Position = pc.mvp * vec4(p, 0.0, 1.0);
}
