#version 450
// Vulkan version of shaders/gl/composite.frag. Keep the two in sync.

layout(push_constant) uniform Params {
    mat4 mvp;
    vec4 color;   // brightness, contrast, saturation, opacity
    vec4 fx;      // hue (rad), blur, vignette, flags
    vec4 texel;   // 1/w, 1/h
    vec4 crop;    // visible texture rect u0, v0, u1, v1
} pc;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fragColor;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

vec3 hueRotate(vec3 c, float a)
{
    const mat3 toYiq = mat3(0.299, 0.596, 0.211,
                            0.587, -0.274, -0.523,
                            0.114, -0.322, 0.312);
    const mat3 toRgb = mat3(1.0, 1.0, 1.0,
                            0.956, -0.272, -1.106,
                            0.621, -0.647, 1.703);
    vec3 yiq = toYiq * c;
    float cs = cos(a), sn = sin(a);
    yiq.yz = vec2(yiq.y * cs - yiq.z * sn, yiq.y * sn + yiq.z * cs);
    return toRgb * yiq;
}

vec4 sampleBlur(vec2 uv, float radius)
{
    if (radius <= 0.001)
        return texture(uTex, uv);
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int x = -3; x <= 3; ++x) {
        for (int y = -3; y <= 3; ++y) {
            float w = exp(-float(x * x + y * y) / 8.0);
            acc += texture(uTex, uv + vec2(x, y) * pc.texel.xy * radius) * w;
            wsum += w;
        }
    }
    return acc / wsum;
}

void main()
{
    if (vUv.x < pc.crop.x || vUv.y < pc.crop.y || vUv.x > pc.crop.z || vUv.y > pc.crop.w) discard; // crop
    vec4 c = sampleBlur(vUv, pc.fx.y);
    vec3 rgb = c.rgb;

    rgb = (rgb - 0.5) * pc.color.y + 0.5 + pc.color.x;
    rgb = mix(vec3(dot(rgb, kLuma)), rgb, pc.color.z);
    if (abs(pc.fx.x) > 0.0001) rgb = hueRotate(rgb, pc.fx.x);

    float flags = pc.fx.w;
    if (mod(flags, 2.0) >= 1.0) rgb = vec3(dot(rgb, kLuma));
    if (flags >= 2.0) rgb = 1.0 - rgb;

    float d = distance(vUv, vec2(0.5));
    rgb *= 1.0 - pc.fx.z * smoothstep(0.25, 0.75, d);

    fragColor = vec4(clamp(rgb, 0.0, 1.0), c.a * pc.color.w);
}
