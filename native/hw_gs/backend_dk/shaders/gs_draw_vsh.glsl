#version 460

// WritebackDk: CPU already converted GS pixels → NDC in inPos.xy.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;

void main() {
    gl_Position = vec4(inPos, 0.0, 1.0);
    gl_PointSize = 1.0;
    vColor = inColor;
    vUv = inUv;
}
