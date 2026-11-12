#version 460

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D tex;

void main() {
    vec4 c = vColor * texture(tex, vUv);
    if (c.a < 1.0 / 255.0)
        discard;
    outColor = c;
}
