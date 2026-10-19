#version 450 core

// Attribute-less fullscreen triangle: one oversized triangle covers the
// screen without a vertex buffer (gl_VertexID 0..2). The scene pass has
// already produced the HDR color + depth textures this pass consumes.
layout(location = 0) out vec2 vUv;

void main()
{
    // (0,0) (2,0) (0,2) in UV space -> clip-space (-1,-1) (3,-1) (-1,3).
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
