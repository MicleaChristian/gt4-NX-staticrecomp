#version 460

// WritebackDk: the CPU converts GS window coordinates to NDC (inPos) and pre-scales the vertex
// colour for the batch's texture function, so the fragment stage is one multiply.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec3 inStq;  // normalised S, T and Q; Q = 1 for UV addressing
layout(location = 3) in vec2 inTest; // alpha test: ATST (0..7), AREF (0..255); ATST 1 = always

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec3 vStq;
layout(location = 2) flat out vec2 vTest;

void main() {
    gl_Position = vec4(inPos, 0.0, 1.0);
    vColor = inColor;
    vStq = inStq;
    vTest = inTest;
}
