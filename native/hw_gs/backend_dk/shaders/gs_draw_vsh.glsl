#version 460

// WritebackDk: the CPU converts GS window coordinates to NDC and Z to [0, 1] depth.

layout(location = 0) in vec3 inPos;   // NDC x, y; depth
layout(location = 1) in vec4 inColor; // RGBAQ bytes / 255
layout(location = 2) in vec3 inStq;   // normalised S, T and Q (Q = 1 for UV addressing)
layout(location = 3) in float inFog;  // FOG byte / 255

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vStq;
layout(location = 2) out float vFog;

void main() {
    gl_Position = vec4(inPos, 1.0);
    vColor = inColor;
    vStq = inStq;
    vFog = inFog;
}
