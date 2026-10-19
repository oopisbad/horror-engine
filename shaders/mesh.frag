#version 450 core

// Forward lighting:
//  - very low ambient floor (dark rooms stay dark)
//  - up to 8 point lights with quadratic falloff
//  - one spotlight (the flashlight) with a 2x2-PCF shadow map
// Keep MAX_POINT_LIGHTS in sync with ForwardRenderer::kMaxPointLights.

#define MAX_POINT_LIGHTS 8

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vShadowPos;
layout(location = 0) out vec4 oColor;

uniform vec3 uAmbientColor;
uniform float uPointLightCount;

struct PointLight {
    vec3 position;
    vec3 color;
    float range;
};
uniform PointLight uPointLights[MAX_POINT_LIGHTS];

uniform vec3 uSpotLightPosition;
uniform vec3 uSpotLightDirection;
uniform vec3 uSpotLightColor;
uniform float uSpotLightInnerCos;
uniform float uSpotLightOuterCos;
uniform float uSpotLightRange;

uniform float uShadowBias;
uniform sampler2D uShadowMap;

uniform vec3 uCameraPos;
uniform vec3 uAlbedo;
uniform float uRoughness;
uniform float uEmissive;
// Output is linear HDR. Exposure, tonemapping, fog, vignette, grain and the
// gamma encode all happen in post.frag.
// Texture plumbing is stubbed for now (procedural placeholders); the base
// color texture path activates with the first imported material.
uniform sampler2D uBaseColorTex;
uniform int uHasBaseColorTex;

float sampleShadow(vec4 shadowPos, float ndotl)
{
    // NDC [-1,1] -> texture/depth space [0,1] (classic remap step).
    vec3 proj = shadowPos.xyz / shadowPos.w * 0.5 + 0.5;
    if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0 || proj.z > 1.0)
        return 1.0; // outside the shadow camera's frustum
    // Slope-scaled bias: steeper N.L needs a bigger offset to avoid acne.
    float bias = max(uShadowBias * (1.0 - ndotl), uShadowBias * 0.2);
    vec2 texel = 1.0 / textureSize(uShadowMap, 0);

    float sum = 0.0;
    for (int x = -1; x <= 0; ++x) {
        for (int y = -1; y <= 0; ++y) {
            float depth = texture(uShadowMap, proj.xy + vec2(x, y) * texel).r;
            sum += (proj.z - bias > depth) ? 0.0 : 1.0;
        }
    }
    return sum * 0.25;
}

void main()
{
    vec3 albedo = uAlbedo;
    if (uHasBaseColorTex == 1)
        albedo *= texture(uBaseColorTex, vUv).rgb;

    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCameraPos - vWorldPos);

    vec3 lit = uAmbientColor * albedo;

    // ---- point lights -------------------------------------------------------
    for (int i = 0; i < MAX_POINT_LIGHTS; ++i) {
        if (float(i) >= uPointLightCount)
            break;
        vec3 L = uPointLights[i].position - vWorldPos;
        float dist = length(L);
        if (dist > uPointLights[i].range)
            continue;
        L /= dist;

        // Inverse-square with a smooth cutoff at the declared range. (The old
        // fade^2 * 4 term was a constant 4x boost near the lamp and blew the
        // whole scene out; intensity is candela-ish like the flashlight.)
        float fade = clamp(1.0 - dist / uPointLights[i].range, 0.0, 1.0);
        float atten = fade * fade / (dist * dist + 0.5);

        float ndotl = max(dot(N, L), 0.0);
        vec3 H = normalize(L + V);
        float s = pow(max(dot(N, H), 0.0), mix(64.0, 8.0, uRoughness));

        lit += uPointLights[i].color * atten * (albedo * ndotl + vec3(0.20) * s * ndotl);
    }

    // ---- spotlight (flashlight) ---------------------------------------------
    {
        vec3 L = uSpotLightPosition - vWorldPos;
        float dist = length(L);
        if (dist > 0.0001 && dist <= uSpotLightRange) {
            vec3 Ldir = L / dist;

            float spotCos = dot(-Ldir, uSpotLightDirection);
            if (spotCos >= uSpotLightOuterCos) {
                // Soft cone edge between the inner and outer angle.
                float cone = smoothstep(uSpotLightOuterCos, uSpotLightInnerCos, spotCos);

                // Candela-style inverse square. Intensities are authored in
                // candela-ish units (a 60W flashlight ~ 70cd); meters-scale
                // scenes need intensities in the tens, not single digits.
                float atten = 1.0 / (dist * dist + 0.5);
                atten *= clamp(1.0 - dist / uSpotLightRange, 0.0, 1.0);

                float ndotl = max(dot(N, Ldir), 0.0);
                vec3 H = normalize(Ldir + V);
                float s = pow(max(dot(N, H), 0.0), mix(96.0, 12.0, uRoughness));

                float shadow = sampleShadow(vShadowPos, ndotl);
                lit += uSpotLightColor * atten * cone
                    * (albedo * ndotl + vec3(0.30) * s * ndotl) * shadow;
            }
        }
    }

    // Emissive is added unshaded: a lit strip still reads as a light source
    // even when ambient is effectively zero.
    lit += albedo * uEmissive;

    // Linear HDR (negative values can come from nothing here, but clamp for
    // safety so RGBA16F doesn't carry NaNs); the post pass tonemaps.
    oColor = vec4(max(lit, vec3(0.0)), 1.0);
}
