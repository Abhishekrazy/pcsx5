#version 450
// Newly authored synthetic HAL shader; no guest ISA or legacy shader inputs.
layout(push_constant) uniform Draw {
    vec4 positions01;
    vec4 position2_pad;
    vec4 color;
} draw;
void main() {
    vec2 p = gl_VertexIndex == 0 ? draw.positions01.xy :
        (gl_VertexIndex == 1 ? draw.positions01.zw : draw.position2_pad.xy);
    gl_Position = vec4(p, 0.0, 1.0);
}
