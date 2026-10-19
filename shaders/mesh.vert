#version 450 core

// Per-object attributes; vertex layout must match Mesh's Vertex struct.
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat4 uLightViewProj;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vShadowPos;

void main()
{
    vec4 world = uModel * vec4(aPosition, 1.0);
    vWorldPos = world.xyz;
    // Normal assumes uniform-ish scale; axis-stretching surfaces need a
    // normal matrix (inverse transpose) later -- noted, not needed yet.
    vNormal = normalize(mat3(uModel) * aNormal);
    vUv = aUv;
    // Fragment shader divides by w; range-checking in sampleShadow eats the
    // outside-case, plus the 0..1 remap when sampling.
    vShadowPos = uLightViewProj * world;
    gl_Position = uViewProj * world;
}
