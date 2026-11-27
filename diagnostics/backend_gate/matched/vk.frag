#version 450
layout(location=0) in vec2 local;
layout(location=1) flat in float len;
layout(location=2) flat in vec2 radius;
layout(location=0) out vec4 rgba;
layout(push_constant) uniform Params {
    vec4 color;
    vec2 screen;
    float pan;
    int mode;
} p;

void main() {
    if (p.mode != 0) {
        rgba = p.mode == 1 ? vec4(0.0) : vec4(p.color.rgb, 1.0);
        return;
    }
    // v1: distance to the union of discs along a linear radius ramp.
    float dr = radius.y - radius.x;
    float center = 0.0;
    if (abs(dr) >= len) {
        center = dr > 0.0 ? len : 0.0;
    } else {
        float slope = dr / len;
        center = clamp(local.x + abs(local.y) * slope /
                       sqrt(max(0.0001, 1.0 - slope * slope)), 0.0, len);
    }
    float r = len > 0.0 ? mix(radius.x, radius.y, center / len)
                        : max(radius.x, radius.y);
    float d = length(vec2(local.x - center, local.y));
    float a = 1.0 - smoothstep(r - 0.75, r + 0.75, d);
    rgba = vec4(0.0, 0.0, 0.0, a * p.color.a);
}
