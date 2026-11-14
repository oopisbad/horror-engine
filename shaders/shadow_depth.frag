#version 450 core
layout(location=0) in vec2 vUv;
layout(binding=3) uniform sampler2D uBaseColor;
uniform float uAlphaCutoff;
uniform float uOpacity;
uniform int uHasBaseColorTex;

void main()
{
    if (uAlphaCutoff < 0.0)
        return;
    float alpha = uOpacity;
    if (uHasBaseColorTex != 0)
        alpha *= texture(uBaseColor, vUv).a;
    if (alpha < uAlphaCutoff)
        discard;
}
