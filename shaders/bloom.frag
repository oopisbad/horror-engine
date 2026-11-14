#version 450 core
layout(location=0) in vec2 vUv;
layout(location=0) out vec3 oColor;
layout(binding=0) uniform sampler2D uSource;
uniform int uMode; // 0: first downsample, 1: later downsample, 2: upsample

void main()
{
    vec2 texel = 1.0 / vec2(textureSize(uSource, 0));
    if (uMode == 2) {
        vec3 sum = texture(uSource, vUv).rgb * 4.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                if (x != 0 || y != 0)
                    sum += texture(uSource, vUv + vec2(x, y) * texel).rgb
                        * ((x == 0 || y == 0) ? 2.0 : 1.0);
        oColor = sum * (0.65 / 16.0);
        return;
    }

    vec3 sum = vec3(0.0);
    for (int y = -1; y <= 1; y += 2) {
        for (int x = -1; x <= 1; x += 2) {
            vec3 c = texture(uSource, vUv + vec2(x, y) * texel * 0.65).rgb;
            if (uMode == 0) {
                float peak = max(c.r, max(c.g, c.b));
                c *= smoothstep(0.55, 1.3, peak) * min(1.0, 8.0 / max(peak, 0.001));
            }
            sum += c;
        }
    }
    oColor = sum * 0.25;
}
