#version 450
layout(location=0) in vec2 origin;
layout(location=1) in vec2 corner;
layout(location=2) in float chordLength;
layout(location=3) in vec2 radii;
layout(location=4) in vec2 direction;
layout(location=0) out vec2 local;
layout(location=1) flat out float len;
layout(location=2) flat out vec2 radius;
layout(push_constant) uniform Params {
    vec4 color;
    vec2 screen;
    float pan;
    int mode; // 0 coverage, 1 alpha clear, 2 resolve
} p;

void main() {
    if (p.mode != 0) {
        vec2 xy = vec2(gl_VertexIndex == 1 ? 3.0 : -1.0,
                       gl_VertexIndex == 2 ? 3.0 : -1.0);
        gl_Position = vec4(xy, 0.0, 1.0);
        local = vec2(0.0);
        len = 0.0;
        radius = vec2(0.0);
        return;
    }
    float pad = max(radii.x, radii.y) + 1.0;
    float along = corner.x * (chordLength + 2.0 * pad) - pad;
    float across = corner.y * pad;
    vec2 xy = origin + direction * along + vec2(-direction.y, direction.x) * across;
    xy.x -= p.pan;
    // A positive-height Vulkan viewport maps NDC -1 to the top.
    gl_Position = vec4(2.0 * xy / p.screen - 1.0, 0.0, 1.0);
    local = vec2(along, across);
    len = chordLength;
    radius = radii;
}
