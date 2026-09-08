#version 450
layout(push_constant) uniform Draw {
    vec4 positions01;
    vec4 position2_pad;
    vec4 color;
} draw;
layout(location = 0) out vec4 pixel;
void main() { pixel = draw.color; }
