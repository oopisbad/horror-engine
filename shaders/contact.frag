#version 450 core
layout(location=0) in vec2 vUv;
layout(location=0) out vec4 oContact;
layout(binding=0) uniform sampler2D uDepth;
layout(binding=1) uniform sampler2D uSurface;
uniform mat4 uInverseProjection, uView;
uniform vec2 uRasterOffset;

vec3 viewPosition(vec2 uv, float depth)
{
    vec4 point = uInverseProjection * vec4(uv*2.0-1.0, depth*2.0-1.0, 1);
    return point.xyz / point.w;
}

void main()
{
    float depth = texture(uDepth, vUv+uRasterOffset).r;
    vec3 p = viewPosition(vUv, depth);
    oContact = vec4(0, 0, 0, -p.z);
    if (depth >= 1.0) return;
    vec2 oct = texture(uSurface,vUv+uRasterOffset).rg*2.0-1.0;
    vec3 n = vec3(oct, 1.0-abs(oct.x)-abs(oct.y));
    n.xy -= clamp(-n.z,0.0,1.0)*mix(vec2(-1),vec2(1),greaterThanEqual(n.xy,vec2(0)));
    n = normalize(mat3(uView)*n);
    const float radius = 0.48;
    vec2 footprint = radius / (-2.0*p.z*vec2(uInverseProjection[0][0],uInverseProjection[1][1]));
    float total = 0.0;
    for (int i=0; i<8; ++i) {
        float a = float(i)*2.399963;
        vec2 uv = vUv + vec2(cos(a),sin(a))*footprint*(float(i)+1.0)/8.0;
        if (any(lessThan(uv,vec2(0))) || any(greaterThan(uv,vec2(1)))) continue;
        vec3 delta = viewPosition(uv,texture(uDepth,uv+uRasterOffset).r)-p;
        float d = length(delta);
        total += max(dot(n,delta)/max(d,0.001)-0.08,0.0)*(1.0-smoothstep(0.05,radius,d));
    }
    oContact.r = min(0.8,total*0.26);
}
