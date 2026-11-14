#version 450 core

layout(location=0) in vec2 vUv;
layout(location=0) out vec4 oHistory;
layout(binding=0) uniform sampler2D uCurrent;
layout(binding=1) uniform sampler2D uDepth;
layout(binding=2) uniform sampler2D uSurface;
layout(binding=3) uniform sampler2D uHistory;

uniform mat4 uInverseVP;
uniform mat4 uPreviousVP;
uniform vec2 uPlanes;
uniform vec2 uRasterOffset;
uniform int uHistoryValid;
uniform int uFixedView;
uniform float uLightingChange;

vec4 reprojectHistory(vec2 uv, float depth, ivec2 size)
{
    vec4 p = uInverseVP * vec4(uv*2.0-1.0, depth*2.0-1.0, 1.0);
    vec4 previous = uPreviousVP * vec4(p.xyz / p.w, 1.0);
    if (previous.w <= 0.0)
        return vec4(0.0);
    vec2 oldUv = previous.xy / previous.w * 0.5 + 0.5;
    if (any(lessThanEqual(oldUv, vec2(0.0))) || any(greaterThanEqual(oldUv, vec2(1.0))))
        return vec4(0.0);

    // Keep every bilinear tap on the same surface to avoid dragging
    // moving silhouettes across static geometry.
    float depthTolerance = max(0.035, previous.w * 0.018);
    float oldDepth = texelFetch(uHistory, ivec2(oldUv*vec2(size)), 0).a;
    if (oldDepth <= 0.0 || abs(oldDepth - previous.w) > depthTolerance)
        return vec4(0.0);

    vec2 samplePosition = oldUv * vec2(size) - 0.5;
    ivec2 base = ivec2(floor(samplePosition));
    vec2 f = fract(samplePosition);
    vec3 history = vec3(0.0);
    float weightSum = 0.0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) {
            vec4 tap = texelFetch(uHistory, clamp(base+ivec2(x,y),ivec2(0),size-1),0);
            if (tap.a <= 0.0 || abs(tap.a-previous.w) > depthTolerance) continue;
            float weight = (x == 0 ? 1.0-f.x : f.x) * (y == 0 ? 1.0-f.y : f.y);
            history += tap.rgb*weight;
            weightSum += weight;
        }
    if (weightSum < 0.25)
        return vec4(0.0);
    float movement = length((oldUv-uv)*vec2(size));
    return vec4(history/weightSum, clamp(0.88 - movement*0.018, 0.06, 0.88));
}

void main()
{
    ivec2 size = textureSize(uCurrent, 0);
    // Reconstruct the latest color at the stable output pixel center.
    // The sub-half-pixel offset leaves the nearest depth and surface texel unchanged.
    vec2 rasterUv = vUv + uRasterOffset;
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(uDepth, pixel, 0).r;
    float viewDepth = (uPlanes.x * uPlanes.y)
        / (uPlanes.y - depth * (uPlanes.y - uPlanes.x));
    vec3 current = texture(uCurrent, rasterUv).rgb;
    bool reactive = texelFetch(uSurface, pixel, 0).a >= 0.5;
    oHistory = vec4(current, reactive ? -viewDepth : viewDepth);
    if (uHistoryValid == 0 || reactive)
        return;

    vec4 history;
    if (uFixedView != 0) {
        // Jitter can swap which side of a static edge supplies the depth.
        // Both sides contribute to the same output pixel when the view is fixed.
        history = texelFetch(uHistory, pixel, 0);
        if (history.a <= 0.0)
            return;
        history.a = 0.88;
    } else {
        if (depth >= 1.0)
            return;
        history = reprojectHistory(vUv, depth, size);
        if (history.a <= 0.0)
            return;
    }

    vec3 minimum = current, maximum = current;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec3 sampleColor = texelFetch(uCurrent, clamp(pixel+ivec2(x,y), ivec2(0), size-1), 0).rgb;
            minimum = min(minimum, sampleColor);
            maximum = max(maximum, sampleColor);
        }
    float amount = clamp(history.a - uLightingChange, 0.06, 0.88);
    oHistory.rgb = mix(current, clamp(history.rgb, minimum, maximum), amount);
}
