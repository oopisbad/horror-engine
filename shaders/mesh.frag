#version 450 core
#define MAX_POINT_LIGHTS 8

layout(location=0) in vec3 vWorldPos;
layout(location=1) in vec3 vNormal;
layout(location=2) in vec2 vUv;
layout(location=3) in vec4 vShadowPos;
layout(location=4) in vec4 vTangent;
layout(location=5) in vec2 vLightmapUv;
layout(location=0) out vec4 oColor;
layout(location=1) out vec4 oSurface;

uniform vec3 uAmbientColor;
uniform float uPointLightCount;
struct PointLight { vec3 position; vec3 color; float range; };
uniform PointLight uPointLights[MAX_POINT_LIGHTS];
uniform vec3 uSpotLightPosition;
uniform vec3 uSpotLightDirection;
uniform vec3 uSpotLightColor;
uniform float uSpotLightInnerCos;
uniform float uSpotLightOuterCos;
uniform float uSpotLightRange;
uniform float uShadowBias;
uniform int uShadows;
uniform sampler2DShadow uShadowMap;
struct LocalSpot { vec3 position; vec3 direction; vec3 color; vec3 coneRange; int shadowSlot; };
layout(binding=13) uniform sampler2DShadow uLocalShadow0;
layout(binding=14) uniform sampler2DShadow uLocalShadow1;
layout(binding=16) uniform samplerCubeShadow uPointShadow;
uniform mat4 uLocalShadowVP[2];
uniform int uPointShadowIndex;
uniform LocalSpot uLocalSpots[4];
uniform int uLocalSpotCount;

uniform vec3 uCameraPos;
uniform vec3 uAlbedo;
uniform vec3 uEmissive;
uniform float uRoughness;
uniform float uMetallic;
uniform sampler2D uBaseColorTex;
uniform int uHasBaseColorTex;
uniform sampler2D uNormalTex;
uniform int uHasNormalTex;
uniform sampler2D uMetallicRoughnessTex;
uniform int uHasMetallicRoughnessTex;
uniform sampler2D uDetailTex;
uniform float uDetailScale;
uniform sampler2D uEmissiveMap;
uniform sampler2D uOcclusionMap;
uniform int uHasEmissiveMap;
uniform int uHasOcclusionMap;
uniform int uUnlit;
uniform float uNormalScale;
uniform float uOcclusionStrength;
uniform float uOpacity;
uniform float uAlphaCutoff;
uniform float uWetness;
uniform int uReactive;
uniform float uLightmapStrength;
layout(binding=10) uniform sampler2D uLightmap;
layout(binding=17) uniform samplerCube uProbe0;
layout(binding=18) uniform samplerCube uProbe1;
struct Probe { mat4 worldToBox; vec4 originStrength; };
uniform Probe uProbes[2];
uniform int uProbeCount;
struct Decal { mat4 worldToBox; vec4 tintStrength; vec2 patternWetness; };
uniform Decal uDecals[12];
uniform int uDecalCount;

float sampleShadow(vec4 shadowPos, float NoL, vec2 depthGradient)
{
    if (uShadows == 0 || shadowPos.w <= 0.0)
        return 1.0;
    vec3 p = shadowPos.xyz / shadowPos.w * 0.5 + 0.5;
    if (any(lessThan(p, vec3(0))) || any(greaterThan(p, vec3(1))))
        return 1.0;
    // World-sized bias avoids widening the gap between far casters and their shadows.
    p.z -= min(uShadowBias, 0.0035 / max(shadowPos.w * shadowPos.w, 0.04)) * mix(1.0, 0.28, NoL);
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));

    // Nine bilinear comparison taps approximate a soft tent. Filtering the
    // individual visibility results avoids comparing interpolated depths.
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec2 offset = vec2(x,y) * texel * (4.0/3.0);
            float reference = p.z + clamp(dot(depthGradient,offset), -0.001,0.001);
            sum += texture(uShadowMap, vec3(p.xy + offset, reference));
        }
    return sum / 9.0;
}

float spotVisibility(int slot, vec3 position, float facing)
{
    if (slot < 0) return 1.0;
    vec4 clip = uLocalShadowVP[slot] * vec4(position, 1.0);
    if (clip.w <= 0.0) return 1.0;
    vec3 p = clip.xyz / clip.w * 0.5 + 0.5;
    if (any(lessThan(p,vec3(0))) || any(greaterThan(p,vec3(1)))) return 1.0;
    p.z -= min(0.0014, 0.006 / max(0.04, clip.w*clip.w)) * mix(1.0,0.45,facing);
    float result = 0.0;
    for (int y = -1; y <= 1; y += 2)
        for (int x = -1; x <= 1; x += 2) {
            vec3 tap = p + vec3(vec2(x,y) * (0.8/1024.0),0);
            result += slot == 0 ? texture(uLocalShadow0,tap) : texture(uLocalShadow1,tap);
        }
    return result * 0.25;
}

vec2 packNormal(vec3 n)
{
    n /= abs(n.x)+abs(n.y)+abs(n.z);
    if (n.z < 0.0)
        n.xy = (1.0-abs(n.yx))*mix(vec2(-1.0),vec2(1.0),greaterThanEqual(n.xy,vec2(0.0)));
    return n.xy*0.5+0.5;
}

float noise(vec2 v)
{
    vec2 i = floor(v);
    vec2 f = fract(v);
    f = f*f*(3.0-2.0*f);
    vec4 j = vec4(i.x,i.x+1.0,i.x,i.x+1.0)*0.1031
        + vec4(i.y,i.y,i.y+1.0,i.y+1.0)*0.11369;
    vec4 h = fract(j);
    h = fract(h*(h+33.33)*(h+h));
    return mix(mix(h.x,h.y,f.x),mix(h.z,h.w,f.x),f.y);
}

void applyDecals(vec3 normal, inout vec3 albedo, inout float wetness)
{
    for (int i = 0; i < uDecalCount; ++i) {
        Decal decal = uDecals[i];
        vec3 p = (decal.worldToBox*vec4(vWorldPos,1.0)).xyz;
        if (any(greaterThanEqual(abs(p),vec3(0.5)))) continue;
        vec3 projector = normalize(transpose(mat3(decal.worldToBox))*vec3(0,0,1));
        float facing = smoothstep(0.2,0.7,dot(normal,projector));
        if (facing <= 0.0) continue;
        vec2 uv = p.xy + 0.5;
        float edge = 1.0-smoothstep(0.32,0.5,max(abs(p.x),abs(p.y)));
        float breakup = noise(uv*vec2(13,19))*0.65 + noise(uv*61.0)*0.35;
        int pattern = int(decal.patternWetness.x);
        if (pattern == 1)
            breakup *= smoothstep(0.35,0.80,noise(uv*vec2(24,2.5))) * uv.y;
        else if (pattern == 2)
            breakup *= smoothstep(0.35,0.85,noise(vec2(uv.x,uv.y+uv.x*0.4)*vec2(5,49)));
        float alpha = clamp(edge*facing*breakup*decal.tintStrength.a,0.0,1.0);
        albedo = mix(albedo,decal.tintStrength.rgb,alpha);
        wetness = max(wetness,alpha*decal.patternWetness.y);
    }
}

vec3 probeReflection(vec3 incidence, float roughness, vec3 F)
{
    vec3 color = vec3(0);
    float weights = 0.0;
    for (int i = 0; i < uProbeCount; ++i) {
        Probe probe = uProbes[i];
        vec3 p = (probe.worldToBox*vec4(vWorldPos,1)).xyz;
        float edge = max(abs(p.x),max(abs(p.y),abs(p.z)));
        if (edge >= 0.5 || probe.originStrength.a <= 0.0) continue;
        vec3 direction = mat3(probe.worldToBox)*incidence;
        vec3 signDir = mix(vec3(-1.0),vec3(1.0),greaterThanEqual(direction,vec3(0.0)));
        vec3 t = (signDir*0.5-p)*signDir/max(abs(direction),vec3(0.00001));
        vec3 lookup = vWorldPos + incidence*min(t.x,min(t.y,t.z)) - probe.originStrength.xyz;
        float lod = 0.7 + roughness*6.0;
        vec3 sampleColor = i == 0 ? textureLod(uProbe0,lookup,lod).rgb : textureLod(uProbe1,lookup,lod).rgb;
        float w = 1.0-smoothstep(0.32,0.5,edge);
        color += min(sampleColor,vec3(8.0))*w*probe.originStrength.a;
        weights += w;
    }
    return color * F * 0.75 / max(1.0,weights);
}

// GGX distribution, Schlick Fresnel and the fast correlated Smith visibility.
// The light strengths are calibrated to the corridor's existing artistic units.
vec3 brdf(vec3 N, vec3 V, vec3 L, vec3 albedo, float roughness, float metallic, float NoL)
{
    vec3 H = V + L;
    H *= inversesqrt(max(dot(H,H), 0.00001));
    float NoV = max(dot(N,V), 0.001);
    float NoH = max(dot(N,H), 0.0);
    float VoH = clamp(dot(V,H), 0.0, 1.0);
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NoH * NoH * (a2 - 1.0) + 1.0;
    float D = a2 / max(d * d, 0.000001);
    float vis = 0.5 / max(mix(2.0 * NoV * NoL, NoV + NoL, a), 0.001);
    float grazing = 1.0 - VoH;
    float g2 = grazing * grazing;
    vec3 F = mix(mix(vec3(0.04), albedo, metallic), vec3(1.0), g2 * g2 * grazing);
    return ((1.0 - F) * (1.0 - metallic) * albedo + min(D * vis, 12.0) * F) * NoL;
}

void main()
{
    // Compute derivatives before per-pixel discard or light cone branches.
    vec3 projected = vShadowPos.xyz * (0.5 / max(abs(vShadowPos.w), 0.01));
    vec3 dx = dFdx(projected), dy = dFdy(projected);
    float jacobian = dx.x*dy.y - dx.y*dy.x;
    vec2 depthGradient = vec2(0.0);
    if (abs(jacobian) > 1e-10)
        depthGradient = vec2(dy.y*dx.z-dx.y*dy.z, dx.x*dy.z-dy.x*dx.z) / jacobian;

    vec4 base = vec4(uAlbedo, uOpacity);
    if (uHasBaseColorTex != 0)
        base *= texture(uBaseColorTex, vUv);
    if (base.a < uAlphaCutoff)
        discard;
    vec3 albedo = base.rgb;
    if (uUnlit != 0) {
        oColor = vec4(albedo, 1.0);
        oSurface = vec4(0.5,0.5,1.0,uReactive != 0 ? 128.0/255.0 : 0.0);
        return;
    }
    float roughness = uRoughness;
    float metallic = uMetallic;
    if (uHasMetallicRoughnessTex != 0) {
        vec2 mr = texture(uMetallicRoughnessTex, vUv).gb;
        roughness *= mr.x;
        metallic *= mr.y;
    }

    vec3 Ng = normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    vec3 N = Ng;
    if (uHasNormalTex != 0) {
        vec3 T = vTangent.xyz - N * dot(N, vTangent.xyz);
        if (dot(T,T) > 1e-8) {
            T = normalize(T);
            vec3 B = cross(N,T) * vTangent.w;
            vec3 detailNormal = texture(uNormalTex, vUv).xyz * 2.0 - 1.0;
            detailNormal.xy *= uNormalScale;
            N = normalize(mat3(T,B,N) * detailNormal);
        }
    }
    if (uDetailScale > 0.0) {
        // Dominant-axis projection gives the corridor consistent world-space texel density.
        vec3 axis = abs(Ng);
        bool xFace = axis.x > max(axis.y, axis.z);
        bool yFace = !xFace && axis.y > axis.z;
        vec2 uv = (xFace ? vWorldPos.zy : (yFace ? vWorldPos.xz : vWorldPos.xy)) * uDetailScale;
        vec3 detail = texture(uDetailTex, uv).rgb;
        albedo *= mix(0.46, 1.28, detail.r);
        roughness *= mix(0.75, 1.15, detail.r);
        vec2 slope = (detail.gb - 0.5) * 0.33;
        vec3 bump = xFace ? vec3(0,slope.y,slope.x) : (yFace ? vec3(slope.x,0,slope.y) : vec3(slope,0));
        N = normalize(N + bump);
    }

    float wetness = uWetness;
    if (uDecalCount > 0)
        applyDecals(Ng, albedo, wetness);
    if (wetness > 0.0) {
        float patches = smoothstep(0.16, 0.73, noise(vWorldPos.xz * 2.7));
        wetness *= mix(0.48, 1.0, patches);
        roughness = mix(roughness, 0.13, wetness);
        albedo *= mix(1.0, 0.69, wetness);
    }
    roughness = clamp(roughness, 0.12, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 lit = uAmbientColor * albedo * mix(0.6, 1.25, N.y * 0.5 + 0.5);
    if (uHasOcclusionMap != 0)
        lit *= mix(1.0, texture(uOcclusionMap, vUv).r, uOcclusionStrength);
    if (uLightmapStrength > 0.0)
        lit += texture(uLightmap, vLightmapUv).rgb * albedo * (1.0-metallic) * uLightmapStrength;
    float NoV = clamp(dot(N,V),0.0,1.0);
    float grazing = 1.0-NoV;
    float g2 = grazing*grazing;
    vec3 reflectance = mix(vec3(0.04+0.03*wetness),albedo,metallic);
    vec3 fresnel = mix(reflectance,vec3(1.0),g2*g2*grazing) * (1.0-0.77*roughness);
    if (uProbeCount > 0)
        lit += probeReflection(reflect(-V,N),roughness,fresnel);

    for (int i = 0; i < int(uPointLightCount); ++i) {
        vec3 delta = uPointLights[i].position - vWorldPos;
        float d2 = dot(delta, delta);
        float range = uPointLights[i].range;
        if (d2 >= range * range || d2 < 0.00001)
            continue;
        float invD = inversesqrt(d2);
        vec3 L = delta * invD;
        float NoL = dot(N,L);
        if (NoL <= 0.0)
            continue;
        float fade = 1.0 - d2 * invD / range;
        float visibility = i == uPointShadowIndex
            ? texture(uPointShadow, vec4(-delta, max(0.0, sqrt(d2)-mix(0.043,0.020,NoL))/range))
            : 1.0;
        lit += uPointLights[i].color * (fade * fade * visibility / (d2 + 0.5))
            * brdf(N,V,L,albedo,roughness,metallic,NoL);
    }

    for (int i = 0; i < uLocalSpotCount; ++i) {
        LocalSpot spot = uLocalSpots[i];
        vec3 offset = spot.position - vWorldPos;
        float distance2 = dot(offset,offset);
        if (distance2 <= 0.00001 || distance2 >= spot.coneRange.z * spot.coneRange.z)
            continue;
        vec3 L = offset * inversesqrt(distance2);
        float NoL = dot(N,L);
        float angle = dot(-L,spot.direction);
        if (NoL > 0.0 && angle > spot.coneRange.x) {
            float cone = clamp((angle-spot.coneRange.x) / max(spot.coneRange.y-spot.coneRange.x,0.0001),0.0,1.0);
            float fade = 1.0-sqrt(distance2)/spot.coneRange.z;
            float visibility = spotVisibility(spot.shadowSlot, vWorldPos, max(dot(Ng,L),0.0));
            lit += spot.color * (cone*cone*fade*fade*visibility/(distance2+0.5))
                * brdf(N,V,L,albedo,roughness,metallic,NoL);
        }
    }

    vec3 delta = uSpotLightPosition - vWorldPos;
    float d2 = dot(delta,delta);
    if (d2 > 0.00001 && d2 < uSpotLightRange * uSpotLightRange) {
        float invD = inversesqrt(d2);
        vec3 L = delta * invD;
        float NoL = dot(N,L);
        float angle = dot(-L, uSpotLightDirection);
        if (NoL > 0.0 && angle > uSpotLightOuterCos) {
            float cone = smoothstep(uSpotLightOuterCos, uSpotLightInnerCos, angle);
            float falloff = (1.0 - d2 * invD / uSpotLightRange) / (d2 + 0.5);
            lit += uSpotLightColor * (cone * falloff * sampleShadow(vShadowPos, max(dot(Ng,L), 0.0), depthGradient))
                * brdf(N,V,L,albedo,roughness,metallic,NoL);
        }
    }
    vec3 emissive = uEmissive;
    if (uHasEmissiveMap != 0)
        emissive *= texture(uEmissiveMap, vUv).rgb;
    oColor = vec4(max(lit + emissive, vec3(0)), 1.0);
    float specular = clamp(metallic + wetness*0.9,0.0,1.0);
    float packedReflectance = (round(specular*127.0) + (uReactive != 0 ? 128.0 : 0.0))/255.0;
    oSurface = vec4(packNormal(N),roughness,packedReflectance);
}
