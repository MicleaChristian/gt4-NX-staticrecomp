#version 460

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vStq;
layout(location = 2) flat in vec2 vTest;

// Index 0: the pixel written to the frame (alpha = As byte / 255).
// Index 1: the GS blend coefficient As / 128 (0x80 = 1.0), read through DkBlendFactor_Src1Alpha.
layout(location = 0, index = 0) out vec4 outColor;
layout(location = 0, index = 1) out vec4 outBlend;

layout(binding = 0) uniform sampler2D tex;

bool alpha_test(float a, int mode, float ref) {
    switch (mode) {
    case 0: return false;      // NEVER
    case 2: return a < ref;    // LESS
    case 3: return a <= ref;   // LEQUAL
    case 4: return a == ref;   // EQUAL
    case 5: return a >= ref;   // GEQUAL
    case 6: return a > ref;    // GREATER
    case 7: return a != ref;   // NOTEQUAL
    default: return true;      // ALWAYS
    }
}

void main() {
    vec4 c = clamp(vColor * texture(tex, vStq.xy / vStq.z), 0.0, 1.0);
    float alpha_byte = floor(c.a * 255.0 + 0.5);
    if (!alpha_test(alpha_byte, int(vTest.x), vTest.y))
        discard;
    outColor = c;
    outBlend = vec4(0.0, 0.0, 0.0, alpha_byte / 128.0);
}
