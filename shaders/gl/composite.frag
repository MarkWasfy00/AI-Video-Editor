#version 330 core
// Per-layer effects: blur, brightness/contrast/saturation, hue, grayscale,
// invert, vignette, opacity. Keep in sync with shaders/vk/composite.frag.

uniform sampler2D uTex;
uniform vec4 uColor;   // brightness, contrast, saturation, opacity
uniform vec4 uFx;      // hue (rad), blur, vignette, flags
uniform vec4 uTexel;   // 1/w, 1/h
uniform vec4 uCrop;    // visible texture rect u0, v0, u1, v1

in vec2 vUv;
out vec4 fragColor;

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

vec3 hueRotate(vec3 c, float a)
{
    // Rotation around the grey axis (YIQ space).
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
            acc += texture(uTex, uv + vec2(x, y) * uTexel.xy * radius) * w;
            wsum += w;
        }
    }
    return acc / wsum;
}

void main()
{
    if (vUv.x < uCrop.x || vUv.y < uCrop.y || vUv.x > uCrop.z || vUv.y > uCrop.w) discard; // crop
    vec4 c = sampleBlur(vUv, uFx.y);
    vec3 rgb = c.rgb;

    rgb = (rgb - 0.5) * uColor.y + 0.5 + uColor.x;           // contrast, brightness
    rgb = mix(vec3(dot(rgb, kLuma)), rgb, uColor.z);           // saturation
    if (abs(uFx.x) > 0.0001) rgb = hueRotate(rgb, uFx.x);

    float flags = uFx.w;
    if (mod(flags, 2.0) >= 1.0) rgb = vec3(dot(rgb, kLuma));   // grayscale
    if (flags >= 2.0) rgb = 1.0 - rgb;                         // invert

    float d = distance(vUv, vec2(0.5));
    rgb *= 1.0 - uFx.z * smoothstep(0.25, 0.75, d);            // vignette

    fragColor = vec4(clamp(rgb, 0.0, 1.0), c.a * uColor.w);
}
