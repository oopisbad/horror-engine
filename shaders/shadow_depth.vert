#version 450 core

layout(location = 0) in vec3 aPosition;

uniform mat4 uLightViewProj;
uniform mat4 uModel;

void main()
{
    gl_Position = uLightViewProj * uModel * vec4(aPosition, 1.0);
}
