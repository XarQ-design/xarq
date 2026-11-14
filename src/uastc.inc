// UASTC LDR 4x4 (Basis Universal's transcodable format, a 19-mode subset of 4x4 LDR ASTC) to RGBA, after its
// public-domain specification ("UASTC LDR 4x4 Texture Specification", Binomial; CC0): the tables below are its.
// Included into gputex.cpp after astc.inc, whose integer-sequence ranges and unquantisation it shares.

const uint8_t kUastcModes[128] = { 11,0,10,3,11,15,12,7,11,18,10,5,11,14,12,9,11,0,10,4,11,16,12,8,11,18,10,6,11,2,12,13,11,0,10,3,11,17,12,7,11,18,10,5,11,14,12,9,11,0,10,4,11,1,12,8,11,18,10,6,11,2,12,13,11,0,10,3,11,19,12,7,11,18,10,5,11,14,12,9,11,0,10,4,11,16,12,8,11,18,10,6,11,2,12,13,11,0,10,3,11,17,12,7,11,18,10,5,11,14,12,9,11,0,10,4,11,1,12,8,11,18,10,6,11,2,12,13 };
const uint8_t kUastcModeBits[20] = { 4,6,5,5,5,5,5,5,5,5,3,2,3,5,5,7,6,6,4,7 };
const uint8_t kUastcWeightBits[19] = { 4, 2, 3, 2, 2, 3, 2, 2,         0,  2, 4, 2, 3, 1, 2,         4, 2, 2,     5 };
const uint8_t kUastcEndpointRange[19] = { 19, 20, 8, 7, 12, 20, 18, 12,   0,  8, 13, 13, 19, 20, 20,    20, 20, 20,  11 };
const uint8_t kUastcSubsets[19] = { 1, 1, 2, 3, 2, 1, 1, 2,         0,  2, 1, 1, 1, 1, 1,         1, 2, 1,     1 };
const uint8_t kUastcPlanes[19] = { 1, 1, 1, 1, 1, 1, 2, 1,         0,  1, 1, 2, 1, 2, 1,         1, 1, 2,     1 };
const uint8_t kUastcComps[19] = { 3, 3, 3, 3, 3, 3, 3, 3,         4,  4, 4, 4, 4, 4, 4,         2, 2, 2,     3 };
// Where each mode keeps its fields: pattern, component selector, the packed trit/quint groups, the endpoint bits
// (start, count, width) and the weights.
struct UastcLayout { int pat, patBits, comp, compBits, groups; int etq[4][2]; int ebits, ebitsCount, ebitsBits, weights; };
const UastcLayout kUastcLayout[19] = {
    {0,0, 0,0, 2,{{19,8},{27,2},{0,0},{0,0}}, 29,6,6, 65},
    {0,0, 0,0, 0,{{0,0},{0,0},{0,0},{0,0}}, 21,6,8, 69},
    {20,5, 0,0, 0,{{0,0},{0,0},{0,0},{0,0}}, 25,12,4, 73},
    {20,4, 0,0, 4,{{24,8},{32,8},{40,8},{48,5}}, 53,18,2, 89},
    {20,5, 0,0, 4,{{25,7},{32,7},{39,7},{46,7}}, 53,12,3, 89},
    {0,0, 0,0, 0,{{0,0},{0,0},{0,0},{0,0}}, 20,6,8, 68},
    {0,0, 20,2, 2,{{22,7},{29,7},{0,0},{0,0}}, 36,6,5, 66},
    {20,5, 0,0, 4,{{25,7},{32,7},{39,7},{46,7}}, 53,12,3, 89},
    {0,0, 0,0, 0,{{0,0},{0,0},{0,0},{0,0}}, 0,0,0, 0},   // 8: solid colour
    {28,5, 0,0, 0,{{0,0},{0,0},{0,0},{0,0}}, 33,16,4, 97},
    {0,0, 0,0, 2,{{20,8},{28,5},{0,0},{0,0}}, 33,8,4, 65},
    {0,0, 19,2, 2,{{21,8},{29,5},{0,0},{0,0}}, 34,8,4, 66},
    {0,0, 0,0, 2,{{20,8},{28,5},{0,0},{0,0}}, 33,8,6, 81},
    {0,0, 28,2, 0,{{0,0},{0,0},{0,0},{0,0}}, 30,8,8, 94},
    {0,0, 0,0, 0,{{0,0},{0,0},{0,0},{0,0}}, 28,8,8, 92},
    {0,0, 0,0, 0,{{0,0},{0,0},{0,0},{0,0}}, 30,4,8, 62},
    {29,5, 0,0, 0,{{0,0},{0,0},{0,0},{0,0}}, 34,8,8, 98},
    {0,0, 0,0, 0,{{0,0},{0,0},{0,0},{0,0}}, 29,4,8, 61},
    {0,0, 0,0, 0,{{0,0},{0,0},{0,0},{0,0}}, 19,6,5, 49},
};
const uint8_t kUastcPatterns2[30][16] = { { 0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1 }, { 0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1 }, { 1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0 }, { 0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1 }, { 1,1,1,1,1,1,1,0,1,1,1,0,1,1,0,0 }, { 0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1 }, { 1,1,1,0,1,1,0,0,1,0,0,0,0,0,0,0 }, { 1,1,1,1,1,1,1,0,1,1,0,0,1,0,0,0 }, { 0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1 }, { 1,1,0,0,1,0,0,0,0,0,0,0,0,0,0,0 }, { 0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1 }, { 1,1,1,1,1,1,1,1,1,1,1,0,1,0,0,0 }, { 1,1,1,0,1,0,0,0,0,0,0,0,0,0,0,0 }, { 1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,0 }, { 0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1 }, { 1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0 }, { 1,0,0,0,1,1,1,0,1,1,1,1,1,1,1,1 }, { 1,1,1,1,1,1,1,1,0,1,1,1,0,0,0,1 }, { 0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0 }, { 0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0 }, { 0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0 }, { 1,1,1,1,1,1,1,1,0,1,1,1,0,0,1,1 }, { 1,0,0,0,1,1,0,0,1,1,0,0,1,1,1,0 }, { 0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0 }, { 1,1,1,1,0,1,1,1,0,1,1,1,0,0,1,1 }, { 0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0 }, { 1,1,1,1,0,0,0,0,0,0,0,0,1,1,1,1 }, { 1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0 }, { 1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0 }, { 1,0,0,1,0,0,1,1,0,1,1,0,1,1,0,0 } };
const uint8_t kUastcPatterns3[11][16] = { { 0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2 }, { 1,1,1,1,1,1,1,1,0,0,0,0,2,2,2,2 }, { 1,1,1,1,0,0,0,0,0,0,0,0,2,2,2,2 }, { 1,1,1,1,2,2,2,2,0,0,0,0,0,0,0,0 }, { 1,1,2,0,1,1,2,0,1,1,2,0,1,1,2,0 }, { 0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2 }, { 0,2,1,1,0,2,1,1,0,2,1,1,0,2,1,1 }, { 2,0,0,0,2,0,0,0,2,1,1,1,2,1,1,1 }, { 2,0,1,2,2,0,1,2,2,0,1,2,2,0,1,2 }, { 1,1,1,1,0,0,0,0,2,2,2,2,1,1,1,1 }, { 0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2 } };
const uint8_t kUastcPatterns73[19][16] = { { 0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0 }, { 0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0 }, { 1,1,0,0,1,1,0,0,1,0,0,0,0,0,0,0 }, { 0,0,0,0,0,0,0,1,0,0,1,1,0,0,1,1 }, { 1,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1 }, { 0,1,0,0,0,1,0,0,0,1,0,0,0,1,0,0 }, { 0,0,0,1,0,0,1,1,1,1,1,1,1,1,1,1 }, { 0,1,1,1,0,0,1,1,0,0,1,1,0,0,1,1 }, { 1,1,0,0,0,0,0,0,0,0,1,1,1,1,0,0 }, { 0,1,1,1,0,1,1,1,0,0,0,0,0,0,0,0 }, { 0,0,0,0,0,0,0,0,1,1,1,0,1,1,1,0 }, { 1,1,0,0,0,0,0,0,0,0,0,0,1,1,0,0 }, { 0,1,1,1,0,0,1,1,0,0,0,0,0,0,0,0 }, { 0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1 }, { 1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,0 }, { 1,1,0,0,1,1,0,0,1,1,0,0,1,0,0,0 }, { 1,1,1,1,1,1,1,1,1,0,0,0,1,0,0,0 }, { 0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,0 }, { 1,1,1,1,0,1,1,1,0,0,0,0,0,0,0,0 } };
const uint8_t kUastcAnchors2[30][2] = { { 0, 2 }, { 0, 3 }, { 1, 0 }, { 0, 3 }, { 7, 0 }, { 0, 2 }, { 3, 0 }, { 7, 0 }, { 0, 11 }, { 2, 0 }, { 0, 7 }, { 11, 0 }, { 3, 0 }, { 8, 0 }, { 0, 4 }, { 12, 0 }, { 1, 0 }, { 8, 0 }, { 0, 1 }, { 0, 2 }, { 0, 4 }, { 8, 0 }, { 1, 0 }, { 0, 2 }, { 4, 0 }, { 0, 1 }, { 4, 0 }, { 1, 0 }, { 4, 0 }, { 1, 0 } };
const uint8_t kUastcAnchors3[11][3] = { { 0, 8, 10 }, { 8, 0, 12 }, { 4, 0, 12 }, { 8, 0, 4 }, { 3, 0, 2 }, { 0, 1, 3 }, { 0, 2, 1 }, { 1, 9, 0 }, { 1, 2, 0 }, { 4, 0, 8 }, { 0, 6, 2 } };
const uint8_t kUastcAnchors73[19][2] = { { 0, 4 }, { 0, 2 }, { 2, 0 }, { 0, 7 }, { 8, 0 }, { 0, 1 }, { 0, 3 }, { 0, 1 }, { 2, 0 }, { 0, 1 }, { 0, 8 }, { 2, 0 }, { 0, 1 }, { 0, 7 }, { 12, 0 }, { 2, 0 }, { 9, 0 }, { 0, 2 }, { 4, 0 } };
const uint8_t kUastcWeights1[2] = { 0, 64 }, kUastcWeights2[4] = { 0, 21, 43, 64 }, kUastcWeights3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
const uint8_t kUastcWeights4[16] = { 0, 4, 8, 12, 17, 21, 25, 29, 35, 39, 43, 47, 52, 56, 60, 64 };
const uint8_t kUastcWeights5[32] = { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64 };

int uastcBits(const uchar* s, int at, int n){ int v = 0; for(int i = 0; i < n; ++i){ const int b = at + i; if(b < 128) v |= ((s[b >> 3] >> (b & 7)) & 1) << i; } return v; }

bool uastcBlock(const uchar* s, QRgb* out){
    const int mode = kUastcModes[s[0] & 0x7f];
    if(mode == 8){                                                             // a solid colour
        const QRgb c = qRgba(uastcBits(s, 5, 8), uastcBits(s, 13, 8), uastcBits(s, 21, 8), uastcBits(s, 29, 8));
        for(int i = 0; i < 16; ++i) out[i] = c;
        return true;
    }
    if(mode >= 19) return false;
    const UastcLayout& L = kUastcLayout[mode];
    const int subsets = kUastcSubsets[mode], planes = kUastcPlanes[mode], comps = kUastcComps[mode], wbits = kUastcWeightBits[mode];
    const Ise& q = kIse[kUastcEndpointRange[mode]];
    const int pattern = L.patBits ? uastcBits(s, L.pat, L.patBits) : 0;
    const int ccs = L.compBits ? uastcBits(s, L.comp, L.compBits) : 3;          // mode 17 (LA, two planes) always selects alpha
    // The partition: which subset each texel is in, and the texels whose weights lose their top bit.
    static const uint8_t none[16] = {}; static const uint8_t first[3] = {};
    const uint8_t* part = none; const uint8_t* anchors = first;
    if(subsets == 2 && mode == 7){ if(pattern >= 19) return false; part = kUastcPatterns73[pattern]; anchors = kUastcAnchors73[pattern]; }
    else if(subsets == 2){ if(pattern >= 30) return false; part = kUastcPatterns2[pattern]; anchors = kUastcAnchors2[pattern]; }
    else if(subsets == 3){ if(pattern >= 11) return false; part = kUastcPatterns3[pattern]; anchors = kUastcAnchors3[pattern]; }
    // Endpoints: the trits or quints of each value above its plain bits, then unquantised as ASTC's.
    const int count = comps * 2 * subsets;
    int tq[24] = {};
    if(q.trits || q.quints){
        const int per = q.trits ? 5 : 3, base = q.trits ? 3 : 5;
        for(int g = 0; g < L.groups; ++g){ int v = uastcBits(s, L.etq[g][0], L.etq[g][1]); for(int i = 0; i < per && g * per + i < 24; ++i){ tq[g * per + i] = v % base; v /= base; } }
    }
    int ep[24];
    for(int i = 0; i < count && i < 24; ++i){
        const int bits = uastcBits(s, L.ebits + i * L.ebitsBits, L.ebitsBits);
        ep[i] = unquantColour((q.trits || q.quints) ? (tq[i] << q.bits | bits) : bits, q);
    }
    // Weights, texel by texel (two a texel with two planes); anchors are one bit short.
    const uint8_t* table = wbits == 1 ? kUastcWeights1 : wbits == 2 ? kUastcWeights2 : wbits == 3 ? kUastcWeights3 : wbits == 4 ? kUastcWeights4 : kUastcWeights5;
    int w[16][2] = {}; int at = L.weights;
    for(int t = 0; t < 16; ++t) for(int p = 0; p < planes; ++p){
        const bool anchor = planes == 2 ? t == 0 : t == anchors[part[t]];
        const int n = wbits - (anchor ? 1 : 0);
        w[t][p] = table[uastcBits(s, at, n)]; at += n;
    }
    auto lerp = [](int l, int h, int wt){ l = l << 8 | l; h = h << 8 | h; return ((l * (64 - wt) + h * wt + 32) >> 6) >> 8; };
    for(int t = 0; t < 16; ++t){
        const int* e = ep + part[t] * comps * 2;
        int c[4];
        if(comps == 2){                                                         // luminance and alpha
            const int l = lerp(e[0], e[1], w[t][0]), a = lerp(e[2], e[3], w[t][planes == 2 ? 1 : 0]);
            c[0] = c[1] = c[2] = l; c[3] = a;
        } else for(int k = 0; k < 4; ++k){
            if(k == 3 && comps == 3){ c[3] = 255; continue; }
            c[k] = lerp(e[k * 2], e[k * 2 + 1], w[t][(planes == 2 && k == ccs) ? 1 : 0]);
        }
        out[t] = qRgba(c[0], c[1], c[2], c[3]);
    }
    return true;
}
