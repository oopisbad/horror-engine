#version 450 core

layout(location=0) in vec2 vUv;
layout(location=1) in vec3 vWorld;

layout(binding=3) uniform sampler2D uBaseColor;
uniform float uAlphaCutoff;
uniform float uOpacity;
uniform int uHasBaseColorTex;
uniform vec3 uLightPosition;
uniform float uLightRange;

void main()
{
    if (uAlphaCutoff >= 0.0) {
        float alpha = uOpacity;
        if (uHasBaseColorTex != 0)
            alpha *= texture(uBaseColor, vUv).a;
        if (alpha < uAlphaCutoff)
            discard;
    }
    gl_FragDepth = min(1.0, length(vWorld - uLightPosition) / uLightRange);
}
