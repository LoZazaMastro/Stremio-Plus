// Dolby Vision GPU reshaping adapted from sainsaji/EVO-PLAYER-PS5.
// SPDX-License-Identifier: GPL-3.0-or-later
// See vendor/evo-player/NOTICE.md and LICENSE.
#pragma once

namespace evo_shader {
inline constexpr const char* dovi = R"EVO(

layout(std140) uniform DoviParams {
    vec4 ycc[3];
    vec4 off;
    vec4 lms[3];
    vec4 lohi[3];     /* lo, hi, has curve */
    vec4 piv[6];      /* inner pivots, 2 per component, 1e9 padded */
    vec4 coef[24];    /* per piece: poly {c0,c1,c2,0} or MMR {const,idx,-,order} */
    vec4 mmr[144];    /* 48 per component */
} dv;

float dv_reshape(int c, vec3 sig) {
    vec4 lh = dv.lohi[c];
    float s = sig[c];
    if (lh.z == 0.0)
        return s;
    vec4 p0 = dv.piv[c * 2];
    vec4 p1 = dv.piv[c * 2 + 1];
    int i = int(s >= p0.x) + int(s >= p0.y) + int(s >= p0.z) + int(s >= p0.w)
          + int(s >= p1.x) + int(s >= p1.y) + int(s >= p1.z);
    vec4 co = dv.coef[c * 8 + i];
    float r;
    if (co.w == 0.0) {
        r = (co.z * s + co.y) * s + co.x;
    } else {
        int m = c * 48 + int(co.y);
        vec4 sx = vec4(sig.xxy * sig.yzz, sig.x * sig.y * sig.z);
        r = co.x + dot(dv.mmr[m].xyz, sig) + dot(dv.mmr[m + 1], sx);
        if (co.w >= 2.0) {
            vec3 s2 = sig * sig;
            vec4 x2 = sx * sx;
            r += dot(dv.mmr[m + 2].xyz, s2) + dot(dv.mmr[m + 3], x2);
            if (co.w >= 3.0)
                r += dot(dv.mmr[m + 4].xyz, s2 * sig) + dot(dv.mmr[m + 5], x2 * sx);
        }
    }
    return clamp(r, lh.x, lh.y);
}

/* PQ code value -> linear light, 1.0 = 10000 nits */
vec3 pq_eotf3(vec3 x) {
    x = pow(max(x, vec3(0.0)), vec3(1.0 / 78.84375));
    x = max(x - vec3(0.8359375), vec3(0.0)) / max(vec3(18.8515625) - vec3(18.6875) * x, vec3(0.000001));
    return pow(x, vec3(1.0 / 0.1593017578125));
}

/* Linear BT.2020 light in nits for this pixel. */
vec3 dovi_nits(vec3 signal) {
    vec3 sig = clamp(signal, 0.0, 1.0);
    vec3 t = vec3(dv_reshape(0, sig), dv_reshape(1, sig), dv_reshape(2, sig)) - dv.off.xyz;
    vec3 lmsp = vec3(dot(dv.ycc[0].xyz, t), dot(dv.ycc[1].xyz, t), dot(dv.ycc[2].xyz, t));
    vec3 l = pq_eotf3(lmsp);
    vec3 rgb = vec3(dot(dv.lms[0].xyz, l), dot(dv.lms[1].xyz, l), dot(dv.lms[2].xyz, l));
    return max(rgb, vec3(0.0)) * 10000.0;
}
)EVO";
}
