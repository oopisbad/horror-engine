#version 450 core

layout(location = 0) in vec3 aPosition;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in uvec4 aJoints;
layout(location = 5) in vec4 aWeights;

uniform mat4 uLightViewProj;
uniform mat4 uModel;

#ifdef SKINNED
const int MAX_BONES = 128;
layout(std140, binding = 0) uniform BoneBlock
{
    mat4 uBones[MAX_BONES];
};
#endif

void main()
{
    vec3 localPos = aPosition;
#ifdef SKINNED
    // Shadow casters must deform exactly like the lit mesh or the shadows
    // detach from the character.
    mat4 skin = aWeights.x * uBones[aJoints.x]
              + aWeights.y * uBones[aJoints.y]
              + aWeights.z * uBones[aJoints.z]
              + aWeights.w * uBones[aJoints.w];
    localPos = (skin * vec4(aPosition, 1.0)).xyz;
#endif
    gl_Position = uLightViewProj * uModel * vec4(localPos, 1.0);
}
