#version 460

// Placeholder fragment shader for sampling a composed RGBA8 colour target into the
// swapchain (UAM → DKSH). Not embedded yet — present today uses copyBufferToImage.

layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(tex, vUv);
}
