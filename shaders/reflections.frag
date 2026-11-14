#version 450 core

layout(location=0) in vec2 vUv;
layout(location=0) out vec4 oReflection;
layout(binding=0) uniform sampler2D uScene;
layout(binding=1) uniform sampler2D uDepth;
layout(binding=2) uniform sampler2D uSurface;
uniform mat4 uInverseVP;
uniform mat4 uVP;
uniform vec3 uEye;
uniform vec2 uPlanes;
uniform vec2 uRasterOffset;

float linearDepth(float depth)
{
    return (uPlanes.x*uPlanes.y)/(uPlanes.y-depth*(uPlanes.y-uPlanes.x));
}

vec3 unpackNormal(vec2 encoded)
{
    vec2 oct = encoded * 2.0 - 1.0;
    vec3 normal = vec3(oct, 1.0-abs(oct.x)-abs(oct.y));
    normal.xy -= clamp(-normal.z, 0.0, 1.0) * mix(vec2(-1.0), vec2(1.0), greaterThanEqual(normal.xy, vec2(0.0)));
    return normalize(normal);
}

void main()
{
    vec2 rasterUv = vUv + uRasterOffset;
    float raw = texture(uDepth, rasterUv).r;
    float depth = linearDepth(raw);
    oReflection = vec4(0,0,0,depth);
    vec4 surface = texture(uSurface, rasterUv);
    float specular = float(int(round(surface.a*255.0)) & 127)/127.0;
    // A single ray captures sharp reflections. Rougher surfaces already
    // receive filtered reflections from the probes in the forward pass.
    float smoothness = 1.0 - smoothstep(0.2, 0.42, surface.b);
    if (raw >= 1.0 || specular < 0.02 || smoothness <= 0.0)
        return;

    vec4 reconstructed = uInverseVP * vec4(vUv*2.0-1.0, raw*2.0-1.0, 1.0);
    vec3 position = reconstructed.xyz/reconstructed.w;
    vec3 normal = unpackNormal(surface.rg);
    vec3 V = normalize(uEye-position);
    vec3 ray = reflect(-V, normal);
    vec4 start = uVP * vec4(position + normal*0.045, 1.0);
    vec4 delta = uVP * vec4(ray, 0.0);
    float previousT = 0.04;
    float previousGap = -1.0;
    for (int i = 0; i < 22; ++i) {
        float stride = float(i+1)/22.0;
        float t = 0.10 + stride*stride*13.0;
        vec4 clip = start+delta*t;
        if (clip.w <= 0.05)
            return;
        vec2 uv = clip.xy/clip.w*0.5+0.5;
        if (any(lessThanEqual(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0))))
            return;
        float gap = clip.w - linearDepth(texture(uDepth, uv + uRasterOffset).r);
        if (gap > 0.0 && previousGap < 0.0) {
            float low = previousT, high = t;
            for (int j=0; j<4; ++j) {
                float middle = (low+high)*0.5;
                clip = start+delta*middle;
                uv = clip.xy/clip.w*0.5+0.5;
                gap = clip.w-linearDepth(texture(uDepth, uv + uRasterOffset).r);
                if (gap > 0.0) high = middle;
                else low = middle;
            }
            clip = start+delta*high;
            uv = clip.xy/clip.w*0.5+0.5;
            gap = clip.w-linearDepth(texture(uDepth, uv + uRasterOffset).r);
            vec3 hitNormal = unpackNormal(texture(uSurface, uv + uRasterOffset).rg);
            float hitFacing = dot(hitNormal, -ray);
            float thickness = 0.05 + clip.w*0.011;
            if (gap < thickness && length(uv-vUv) > 0.01 && hitFacing > 0.08) {
                float edge = min(min(uv.x,uv.y),min(1.0-uv.x,1.0-uv.y));
                float fresnel = 1.0-max(dot(normal,V),0.0);
                float confidence = 1.0 - smoothstep(thickness*0.5, thickness, gap);
                float weight = specular * smoothstep(0.0,0.08,edge) * (1.0-stride) * confidence
                    * smoothstep(0.08,0.38,hitFacing) * (0.25 + 0.75*fresnel*fresnel) * smoothness;
                oReflection.rgb = min(texture(uScene, uv).rgb, vec3(4.0)) * weight * 0.52;
                return;
            }
        }
        previousT = t;
        previousGap = gap;
    }
}
