#version 450 core
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 oVolume;
layout(binding=0) uniform sampler2D uDepth;
layout(binding=15) uniform sampler2DShadow uShadowMap;

uniform mat4 uInvViewProj;
uniform vec2 uRasterOffset;
uniform mat4 uLightViewProj;
uniform vec3 uEye;
uniform vec3 uLightPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec2 uCone;
uniform float uRange;
uniform int uSteps;
uniform int uFlashlightShadows;
struct LocalSpot { vec3 position; vec3 direction; vec3 color; vec3 coneRange; int shadowSlot; };
uniform LocalSpot uLocalSpots[4];
uniform int uLocalSpotCount;
uniform mat4 uLocalShadowVP[2];
layout(binding=13) uniform sampler2DShadow uLocalShadow0;
layout(binding=14) uniform sampler2DShadow uLocalShadow1;

vec3 scatteredLight(vec3 p)
{
    vec3 light = vec3(0);
    for (int k=0; k<uLocalSpotCount; ++k) {
        LocalSpot spot = uLocalSpots[k];
        vec3 ray = p - spot.position;
        float d = max(length(ray),0.001);
        if (d >= spot.coneRange.z) continue;
        float cone = smoothstep(spot.coneRange.x,max(spot.coneRange.x+0.0001,spot.coneRange.y),dot(ray/d,spot.direction));
        if (cone == 0.0) continue;
        float visibility = 1.0;
        if (spot.shadowSlot >= 0) {
            vec4 clip = uLocalShadowVP[spot.shadowSlot]*vec4(p,1);
            vec3 uv = clip.xyz/clip.w*0.5+0.5;
            uv.z -= 0.00015;
            if (uv.z > 0.0) visibility = spot.shadowSlot==0 ? texture(uLocalShadow0,uv) : texture(uLocalShadow1,uv);
        }
        float falloff = 1.0-d/spot.coneRange.z;
        light += spot.color * (cone*visibility*falloff*falloff/(1.0+d*d));
    }
    return light;
}

struct Fog { mat4 worldToBox; vec4 colorDensity; };
uniform Fog uFog[4];
uniform int uFogCount;

vec2 boxSegment(Fog fog, vec3 origin, vec3 direction, float limit)
{
    vec3 p = (fog.worldToBox * vec4(origin,1)).xyz;
    vec3 d = mat3(fog.worldToBox) * direction;
    vec2 bounds = vec2(0,limit);
    for (int axis = 0; axis < 3; ++axis) {
        if (abs(d[axis]) < 0.00001) {
            if (abs(p[axis]) > 0.5) return vec2(0);
            continue;
        }
        vec2 t = (vec2(-0.5,0.5) - p[axis]) / d[axis];
        bounds = vec2(max(bounds.x,min(t.x,t.y)),min(bounds.y,max(t.x,t.y)));
    }
    return bounds;
}

void main()
{
    float depth = texture(uDepth, vUv + uRasterOffset).r;
    vec4 world = uInvViewProj * vec4(vUv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec3 ray = world.xyz / world.w - uEye;
    float distanceToScene = length(ray);
    vec3 dir = ray / max(distanceToScene, 0.0001);
    float farT = min(distanceToScene, 16.0);
    // Use the same midpoint rule for neighboring rays. Square the interval
    // boundaries to concentrate samples near the flashlight, where falloff
    // is steepest. A pixel-dependent offset here becomes a visible grid
    // when the quarter-resolution haze is enlarged.
    float invSteps = 1.0 / float(uSteps);
    vec4 clipOrigin = uLightViewProj * vec4(uEye, 1.0);
    vec4 clipDirection = uLightViewProj * vec4(dir, 0.0);
    vec3 toLightOrigin = uEye - uLightPos;
    vec3 sum = vec3(0.0);
    for (int i = 0; i < (uFlashlightShadows != 0 ? uSteps : 0); ++i) {
        float a = float(i) * invSteps;
        float b = a + invSteps;
        float t = farT * 0.5 * (a*a + b*b);
        float width = farT * invSteps * (a + b);
        vec3 p = uEye + dir * t;
        vec3 delta = toLightOrigin + dir * t;
        float dist2 = dot(delta, delta);
        float cone = smoothstep(uCone.x, uCone.y, dot(delta, uLightDir) * inversesqrt(max(dist2, 0.0001)));
        if (cone <= 0.0 || dist2 > uRange * uRange)
            continue;
        vec4 shadow = clipOrigin + clipDirection * t;
        if (shadow.w <= 0.0)
            continue;
        vec3 sc = shadow.xyz / shadow.w * 0.5 + 0.5;
        float visibility = sc.z <= 0.0 ? 1.0 : texture(uShadowMap, vec3(sc.xy, sc.z - 0.00022));
        float dust = 0.75 + 0.25 * sin(p.y * 2.3 + sin(p.z * 0.83) * 1.4);
        sum += uLightColor * (width * dust * cone * visibility * exp2(-0.17 * t) / (1.0 + dist2));
    }
    vec3 haze = sum * 0.0045;
    // Integrate each box over its clipped segment. Thin layers are resolved
    // analytically, so quarter resolution does not need a dithering pattern.
    for (int i = 0; i < uFogCount; ++i) {
        vec2 segment = boxSegment(uFog[i], uEye, dir, min(distanceToScene, 45.0));
        float span = segment.y-segment.x;
        if (span <= 0.0) continue;
        float coverage = 1.0-exp(-span*uFog[i].colorDensity.a);
        vec3 tint = uFog[i].colorDensity.rgb;
        haze += tint * coverage * 0.35;
        if (uLocalSpotCount > 0) {
            int steps = clamp(uSteps, 2, 12);
            float stride = span / float(steps);
            float scatter = 1.0-exp(-stride*uFog[i].colorDensity.a);
            float throughput = 1.0;
            for (int j=0; j<steps; ++j) {
                vec3 p = uEye + dir*(segment.x+(float(j)+0.5)*stride);
                haze += tint * scatteredLight(p) * (scatter*throughput);
                throughput *= 1.0-scatter;
            }
        }
    }
    // Alpha carries eye-to-surface distance for depth-aware upsampling.
    oVolume = vec4(haze, distanceToScene);
}
