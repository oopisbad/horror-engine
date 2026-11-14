#version 450 core

// Composite pass: linear HDR scene -> display.
// Order matters: fog in linear space, then exposure, tonemapping, and only
// then the stylized effects (vignette/grain) so their strengths are
// perceptually meaningful and independent of scene brightness.

// Explicit location must match post.vert's output (Mesa rejects a mix of
// explicit and implicit link-by-name varyings).
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oColor;

uniform sampler2D uScene;
uniform sampler2D uDepth;

uniform float uNear;
uniform float uFar;

uniform float uExposure;
uniform float uTime;
uniform float uVignette;
uniform float uGrain;

uniform vec3 uFogColor;
uniform float uFogDensity;
layout(binding=2) uniform sampler2D uBloom;
layout(binding=7) uniform sampler2D uVolume;
uniform float uBloomStrength;
uniform int uHasVolume;
layout(binding=12) uniform sampler2D uReflection;
uniform int uHasReflections;
uniform vec2 uRayScale;
uniform vec2 uRasterOffset;

// Bilateral reconstruction across depth edges keeps flashlight haze behind walls.
vec3 upsampleDepthAware(sampler2D source, float distanceToScene)
{
    vec2 size = vec2(textureSize(source, 0));
    vec2 p = vUv * size - 0.5;
    ivec2 base = ivec2(floor(p));
    vec2 f = fract(p);
    vec3 total = vec3(0.0);
    float totalWeight = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec4 sampleValue = texelFetch(source,
                clamp(base + ivec2(x,y), ivec2(0), ivec2(size)-1), 0);
            float delta = abs(distanceToScene - sampleValue.a);
            float weight = (x == 0 ? 1.0-f.x : f.x) * (y == 0 ? 1.0-f.y : f.y);
            float relative = delta / (0.08 + distanceToScene * 0.06);
            weight /= 1.0 + relative * relative * relative;
            total += sampleValue.rgb * weight;
            totalWeight += weight;
        }
    }
    return total / max(totalWeight, 0.04);
}

// Narkowicz's ACES filmic approximation: cheap, keeps highlight detail
// instead of clipping the flashlight's inverse-square falloff to white.
vec3 acesToneMap(vec3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Hash-based noise; deterministic per pixel/time, no texture needed.
float hash(vec2 p)
{
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main()
{
    vec3 color = texture(uScene, vUv).rgb;

    // ---- depth fog ---------------------------------------------------------
    // Exponential-squared fog driven by the scene depth buffer: far walls and
    // the corridor end sink into black instead of a gray haze.
    float depth = texture(uDepth, vUv + uRasterOffset).r;
    float linearDepth = (2.0 * uNear * uFar)
        / (uFar + uNear - (depth * 2.0 - 1.0) * (uFar - uNear));
    vec2 slope = (vUv * 2.0 - 1.0) * uRayScale;
    float distanceToScene = linearDepth * sqrt(1.0 + dot(slope, slope));
    if (uHasReflections != 0)
        color += upsampleDepthAware(uReflection, linearDepth);
    float density = distanceToScene * uFogDensity;
    float fogAmount = 1.0 - exp(-density * density);
    color = mix(color, uFogColor, fogAmount);
    if (uHasVolume != 0)
        color += upsampleDepthAware(uVolume, distanceToScene);
    if (uBloomStrength > 0.0)
        color += texture(uBloom, vUv).rgb * uBloomStrength;

    // ---- exposure + tonemap ------------------------------------------------
    color *= uExposure;
    color = acesToneMap(color);

    // ---- vignette ----------------------------------------------------------
    // Normalized distance from center; quadratic darkening toward the edges.
    vec2 centered = vUv - vec2(0.5);
    float vignette = 1.0 - uVignette * dot(centered, centered) * 1.6;
    color *= clamp(vignette, 0.0, 1.0);

    // ---- gamma encode ------------------------------------------------------
    // The swapchain is not sRGB, so encode explicitly (matches the old
    // forward-shader path; a future HDR swapchain would move this).
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));

    // ---- film grain --------------------------------------------------------
    // Applied after gamma: a fixed linear amplitude would be amplified by the
    // encode in near-black areas and turn the shadows into TV static.
    // Luminance-weighted so highlights stay clean.
    float noise = hash(floor(gl_FragCoord.xy) + fract(uTime) * 97.0) - 0.5;
    float luma = dot(color, vec3(0.299, 0.587, 0.114));
    color += noise * uGrain * (1.0 - luma * 0.7);

    oColor = vec4(color, 1.0);
}
