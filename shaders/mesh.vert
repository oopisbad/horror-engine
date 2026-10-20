#version 450 core

// Per-object attributes; vertex layout must match Mesh's Vertex struct.
// One pipeline serves static and skinned meshes: the SKINNED define adds the
// bone UBO lookup, everything else (lighting, shadows, materials) is shared.
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;

uniform mat4 uModel;
uniform mat4 uViewProj;
uniform mat4 uLightViewProj;

#ifdef SKINNED
const int MAX_BONES = 128;
// std140 mat4 array: 64 bytes/entry. Keep MAX_BONES in sync with the renderer.
layout(std140, binding = 0) uniform BoneBlock
{
    mat4 uBones[MAX_BONES];
};
#endif

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vShadowPos;
// World-space tangent; .w carries glTF's bitangent sign (+/-1).
layout(location = 4) out vec4 vTangent;

void main()
{
    vec3 localPos = aPosition;
    vec3 localNormal = aNormal;
    vec3 localTangent = aTangent.xyz;

#ifdef SKINNED
    // Linear blend skinning of the four heaviest influences. Weights are
    // normalized by the loader; unused slots point at bone 0 with weight 0.
    mat4 skin = aWeights.x * uBones[aJoints.x]
              + aWeights.y * uBones[aJoints.y]
              + aWeights.z * uBones[aJoints.z]
              + aWeights.w * uBones[aJoints.w];
    localPos = (skin * vec4(aPosition, 1.0)).xyz;
    localNormal = mat3(skin) * aNormal;
    localTangent = mat3(skin) * aTangent.xyz;
#endif

    // uModel is the entity/root transform on top of the (skinned) model space.
    vec4 world = uModel * vec4(localPos, 1.0);
    vWorldPos = world.xyz;
    // Normal assumes uniform-ish scale; axis-stretching surfaces need a
    // normal matrix (inverse transpose) later -- noted, not needed yet.
    vNormal = normalize(mat3(uModel) * localNormal);
    // Procedural meshes leave the tangent at zero; keep it finite so the
    // fragment shader can detect "no tangent" (it only needs one when the
    // material has a normal map).
    vec3 worldTangent = mat3(uModel) * localTangent;
    vTangent = vec4(length(worldTangent) > 1e-6 ? normalize(worldTangent) : vec3(0.0),
        aTangent.w != 0.0 ? aTangent.w : 1.0);
    vUv = aUv;
    // Fragment shader divides by w; range-checking in sampleShadow eats the
    // outside-case, plus the 0..1 remap when sampling.
    vShadowPos = uLightViewProj * world;
    gl_Position = uViewProj * world;
}
