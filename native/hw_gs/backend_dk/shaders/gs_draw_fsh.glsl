#version 460

// GS pixel pipeline for one batch: texture function, fog, alpha test, FBA. Values are kept as
// 0..255 integers in float so the GS integer rules (x * y >> 7, fog >> 8) apply directly.

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec3 vStq;
layout(location = 2) in float vFog;

// Index 0: the pixel written to the frame. Index 1: the blend coefficient As / 128 (0x80 = 1.0),
// read through DkBlendFactor_Src1Alpha.
layout(location = 0, index = 0) out vec4 outColor;
layout(location = 0, index = 1) out vec4 outBlend;

layout(binding = 0) uniform sampler2D tex;

layout(std140, binding = 0) uniform Params {
    vec4 test;   // ATST (0..7), AREF, 1 = draw only the pixels that fail the test, 1 = FBA
    vec4 texfn;  // TME, TFX, TCC, FGE
    vec4 fogcol; // FOGCOL R, G, B
    vec4 wrap;   // WMS, WMT, texture width, texture height
    vec4 region; // MINU, MAXU, MINV, MAXV
};

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

int wrap_coord(int c, int mode, int size, float lo, float hi) {
    if (mode == 0) return c & (size - 1);                 // REPEAT
    if (mode == 1) return clamp(c, 0, size - 1);          // CLAMP
    if (mode == 2) return clamp(c, int(lo), int(hi));     // REGION_CLAMP
    return clamp((c & int(lo)) | int(hi), 0, size - 1);   // REGION_REPEAT: MINU is the mask, MAXU the fix
}

vec4 fetch_texel() {
    vec2 uv = vStq.xy / vStq.z;
    int ms = int(wrap.x);
    int mt = int(wrap.y);
    if (ms == 3 || mt == 3) {
        ivec2 size = ivec2(wrap.zw);
        ivec2 ti = ivec2(floor(uv * wrap.zw));
        ti.x = wrap_coord(ti.x, ms, size.x, region.x, region.y);
        ti.y = wrap_coord(ti.y, mt, size.y, region.z, region.w);
        return texelFetch(tex, ti, 0);
    }
    if (ms == 2) uv.x = clamp(uv.x, (region.x + 0.5) / wrap.z, (region.y + 0.5) / wrap.z);
    if (mt == 2) uv.y = clamp(uv.y, (region.z + 0.5) / wrap.w, (region.w + 0.5) / wrap.w);
    return texture(tex, uv);
}

void main() {
    vec4 cv = floor(vColor * 255.0 + 0.5);
    vec3 rgb = cv.rgb;
    float a = cv.a;
    if (texfn.x > 0.5) {
        vec4 ct = floor(fetch_texel() * 255.0 + 0.5);
        int tfx = int(texfn.y);
        bool tcc = texfn.z > 0.5;
        rgb = (tfx == 1) ? ct.rgb : floor(cv.rgb * ct.rgb / 128.0);
        if (tfx >= 2) rgb += cv.a;
        if (tfx == 0) a = tcc ? floor(cv.a * ct.a / 128.0) : cv.a;
        else if (tfx == 2) a = tcc ? cv.a + ct.a : cv.a;
        else a = tcc ? ct.a : cv.a;
    }
    rgb = clamp(rgb, 0.0, 255.0);
    a = clamp(a, 0.0, 255.0);
    if (texfn.w > 0.5) {
        float f = floor(vFog * 255.0 + 0.5);
        rgb = floor((f * rgb + (255.0 - f) * fogcol.rgb) / 256.0);
    }
    bool pass = alpha_test(a, int(test.x), test.y);
    if (test.z > 0.5) pass = !pass;
    if (!pass)
        discard;
    float written_a = test.w > 0.5 ? float(int(a) | 128) : a;
    outColor = vec4(rgb, written_a) / 255.0;
    outBlend = vec4(0.0, 0.0, 0.0, a / 128.0);
}
