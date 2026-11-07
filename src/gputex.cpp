// GPU block-compressed textures to 8-bit RGBA. BC1–BC5 and BC7 after Microsoft's Direct3D 11 block compression
// formats; ETC1, ETC2 and EAC after the Khronos Data Format Specification; ASTC after the same specification's
// ASTC chapter (astc.inc). Everything is decoded a 4×4 (ASTC: blockW × blockH) block at a time.
#include "gputex.h"
#include <cstring>

namespace xara {
namespace gputex {
namespace {

// BC7 partition shapes (2 bits per pixel, pixel 0 lowest) and the anchor pixel of subsets 1 and 2, from the
// BC7 specification's tables.
const quint32 kBc7P2[64] = {
    0x50505050, 0x40404040, 0x54545454, 0x54505040, 0x50404000, 0x55545450, 0x55545040, 0x54504000,
    0x50400000, 0x55555450, 0x55544000, 0x54400000, 0x55555440, 0x55550000, 0x55555500, 0x55000000,
    0x55150100, 0x00004054, 0x15010000, 0x00405054, 0x00004050, 0x15050100, 0x05010000, 0x40505054,
    0x00404050, 0x05010100, 0x14141414, 0x05141450, 0x01155440, 0x00555500, 0x15014054, 0x05414150,
    0x44444444, 0x55005500, 0x11441144, 0x05055050, 0x05500550, 0x11114444, 0x41144114, 0x44111144,
    0x15055054, 0x01055040, 0x05041050, 0x05455150, 0x14414114, 0x50050550, 0x41411414, 0x00141400,
    0x00041504, 0x00105410, 0x10541000, 0x04150400, 0x50410514, 0x41051450, 0x05415014, 0x14054150,
    0x41050514, 0x41505014, 0x40011554, 0x54150140, 0x50505500, 0x00555050, 0x15151010, 0x54540404,
};
const quint32 kBc7P3[64] = {
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0, 0x5a5a5050,
    0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4, 0xa9a59450, 0x2a0a4250,
    0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454, 0x6a6a4040, 0xa4a45000, 0x1a1a0500,
    0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400, 0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200,
    0xa9a58000, 0x5090a0a8, 0xa8a09050, 0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50,
    0x500aa550, 0xaaaa4444, 0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600,
    0xaa444444, 0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
    0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44, 0x2a4a5254,
};
const uchar kBc7A2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15,
    2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15,
    2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2,
    15, 15, 15, 15, 15, 2, 2, 15,
};
const uchar kBc7A3a[64] = {
    3, 3, 15, 15, 8, 3, 15, 15,
    8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10,
    5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15,
    15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10,
    5, 10, 8, 13, 15, 12, 3, 3,
};
const uchar kBc7A3b[64] = {
    15, 8, 8, 3, 15, 15, 3, 8,
    15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8,
    3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10,
    6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 3, 15, 15, 8,
};

inline int clamp255(int v){ return v < 0 ? 0 : v > 255 ? 255 : v; }
inline quint64 le64(const uchar* p){ quint64 v = 0; for(int i = 7; i >= 0; --i) v = v << 8 | p[i]; return v; }
inline quint64 be64(const uchar* p){ quint64 v = 0; for(int i = 0; i < 8; ++i) v = v << 8 | p[i]; return v; }

// ---- BC1–BC5 ----------------------------------------------------------------------------------------------
void rgb565(quint32 c, int& r, int& g, int& b){
    r = int(c >> 11 & 31); g = int(c >> 5 & 63); b = int(c & 31);
    r = r << 3 | r >> 2; g = g << 2 | g >> 4; b = b << 3 | b >> 2;
}
// A BC1 colour block. threeColour: honour BC1's 3-colour mode; clearBlack: its index 3 is transparent.
void bc1(const uchar* s, QRgb* out, bool threeColour, bool clearBlack){
    const quint32 c0 = s[0] | s[1] << 8, c1 = s[2] | s[3] << 8;
    int r[4], g[4], b[4], a[4] = {255, 255, 255, 255};
    rgb565(c0, r[0], g[0], b[0]); rgb565(c1, r[1], g[1], b[1]);
    if(c0 > c1 || !threeColour){
        r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
        r[3] = g[3] = b[3] = 0; if(clearBlack) a[3] = 0;
    }
    const quint32 idx = s[4] | s[5] << 8 | s[6] << 16 | quint32(s[7]) << 24;
    for(int i = 0; i < 16; ++i){ const int k = int(idx >> (2 * i) & 3); out[i] = qRgba(r[k], g[k], b[k], a[k]); }
}
// A BC4 channel block into 16 values; sgn reads the endpoints as signed and maps −127..127 to 0..255.
void bc4(const uchar* s, int* v, bool sgn){
    int e0 = s[0], e1 = s[1];
    if(sgn){ e0 = qint8(s[0]); e1 = qint8(s[1]); if(e0 == -128) e0 = -127; if(e1 == -128) e1 = -127; }
    int t[8]; t[0] = e0; t[1] = e1;
    if(e0 > e1) for(int i = 1; i < 7; ++i) t[i + 1] = ((7 - i) * e0 + i * e1) / 7;
    else { for(int i = 1; i < 5; ++i) t[i + 1] = ((5 - i) * e0 + i * e1) / 5; t[6] = sgn ? -127 : 0; t[7] = sgn ? 127 : 255; }
    const quint64 idx = le64(s) >> 16;
    for(int i = 0; i < 16; ++i){ const int x = t[idx >> (3 * i) & 7]; v[i] = sgn ? (x + 127) * 255 / 254 : x; }
}

// ---- BC7 ----------------------------------------------------------------------------------------------------
struct Bits {
    const uchar* p; int at = 0;
    int take(int n){ int v = 0; for(int k = 0; k < n; ++k, ++at) v |= ((p[at >> 3] >> (at & 7)) & 1) << k; return v; }
};
const int kW2[4] = {0, 21, 43, 64}, kW3[8] = {0, 9, 18, 27, 37, 46, 55, 64}, kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
const int* weights(int bits){ return bits == 2 ? kW2 : bits == 3 ? kW3 : kW4; }
inline int interp(int e0, int e1, int w){ return ((64 - w) * e0 + w * e1 + 32) >> 6; }

bool bc7(const uchar* s, QRgb* out){
    struct M { int ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; };
    static const M modes[8] = { {3,4,0,0,4,0,1,0,3,0}, {2,6,0,0,6,0,0,1,3,0}, {3,6,0,0,5,0,0,0,2,0}, {2,6,0,0,7,0,1,0,2,0},
                                {1,0,2,1,5,6,0,0,2,3}, {1,0,2,0,7,8,0,0,2,2}, {1,0,0,0,7,7,1,0,4,0}, {2,6,0,0,5,5,1,0,2,0} };
    int mode = 0; while(mode < 8 && !(s[0] >> mode & 1)) ++mode;
    if(mode == 8){ for(int i = 0; i < 16; ++i) out[i] = 0; return false; }
    const M& m = modes[mode];
    Bits b{ s, mode + 1 };
    const int part = b.take(m.pb), rot = b.take(m.rb), isb = b.take(m.isb);
    int ep[6][4] = {};                                                   // endpoints: subset*2 + 0/1, RGBA
    for(int c = 0; c < 3; ++c) for(int e = 0; e < m.ns * 2; ++e) ep[e][c] = b.take(m.cb);
    for(int e = 0; e < m.ns * 2; ++e) ep[e][3] = m.ab ? b.take(m.ab) : 255;
    int cbits = m.cb, abits = m.ab;
    if(m.epb){ for(int e = 0; e < m.ns * 2; ++e){ const int pbit = b.take(1); for(int c = 0; c < 4; ++c) if(c < 3 || m.ab) ep[e][c] = ep[e][c] << 1 | pbit; } ++cbits; if(abits) ++abits; }
    if(m.spb){ for(int sub = 0; sub < m.ns; ++sub){ const int pbit = b.take(1); for(int e = sub * 2; e < sub * 2 + 2; ++e) for(int c = 0; c < 4; ++c) if(c < 3 || m.ab) ep[e][c] = ep[e][c] << 1 | pbit; } ++cbits; if(abits) ++abits; }
    for(int e = 0; e < m.ns * 2; ++e){
        for(int c = 0; c < 3; ++c) ep[e][c] = ep[e][c] << (8 - cbits) | ep[e][c] >> (2 * cbits - 8);
        if(abits) ep[e][3] = ep[e][3] << (8 - abits) | ep[e][3] >> (2 * abits - 8);
    }
    auto subset = [&](int i){ return m.ns == 1 ? 0 : int((m.ns == 2 ? kBc7P2[part] : kBc7P3[part]) >> (2 * i) & 3); };
    auto anchor = [&](int i){ if(i == 0) return true; if(m.ns == 2) return i == kBc7A2[part]; if(m.ns == 3) return i == kBc7A3a[part] || i == kBc7A3b[part]; return false; };
    int idx[16], idx2[16];
    for(int i = 0; i < 16; ++i) idx[i] = b.take(anchor(i) ? m.ib - 1 : m.ib);
    if(m.ib2) for(int i = 0; i < 16; ++i) idx2[i] = b.take(i == 0 ? m.ib2 - 1 : m.ib2);
    for(int i = 0; i < 16; ++i){
        const int sb = subset(i); const int* e0 = ep[sb * 2]; const int* e1 = ep[sb * 2 + 1];
        int ci = idx[i], cb = m.ib, ai = idx[i], ab = m.ib;
        if(m.ib2){ if(isb){ ci = idx2[i]; cb = m.ib2; } else { ai = idx2[i]; ab = m.ib2; } }
        int px[4];
        for(int c = 0; c < 3; ++c) px[c] = interp(e0[c], e1[c], weights(cb)[ci]);
        px[3] = interp(e0[3], e1[3], weights(ab)[ai]);
        if(rot) std::swap(px[3], px[rot - 1]);
        out[i] = qRgba(px[0], px[1], px[2], px[3]);
    }
    return true;
}

// ---- ETC1 / ETC2 / EAC ----------------------------------------------------------------------------------
const int kEtcMod[8][4] = { {2,8,-2,-8}, {5,17,-5,-17}, {9,29,-9,-29}, {13,42,-13,-42}, {18,60,-18,-60}, {24,80,-24,-80}, {33,106,-33,-106}, {47,183,-47,-183} };
const int kEtcDist[8] = {3, 6, 11, 16, 23, 32, 41, 64};
const int kEac[16][8] = { {-3,-6,-9,-15,2,5,8,14}, {-3,-7,-10,-13,2,6,9,12}, {-2,-5,-8,-13,1,4,7,12}, {-2,-4,-6,-13,1,3,5,12},
                          {-3,-6,-8,-12,2,5,7,11}, {-3,-7,-9,-11,2,6,8,10}, {-4,-7,-8,-11,3,6,7,10}, {-3,-5,-8,-11,2,4,7,10},
                          {-2,-6,-8,-10,1,5,7,9}, {-2,-5,-8,-10,1,4,7,9}, {-2,-4,-8,-10,1,3,7,9}, {-2,-5,-7,-10,1,4,6,9},
                          {-3,-4,-7,-10,2,3,6,9}, {-1,-2,-3,-10,0,1,2,9}, {-4,-6,-8,-9,3,5,7,8}, {-3,-5,-7,-9,2,4,6,8} };
inline int ext4(int v){ return v << 4 | v; }
inline int ext5(int v){ return v << 3 | v >> 2; }

// An ETC1/ETC2 colour block. etc2 enables the T, H and planar modes; punch makes it ETC2 RGB A1, whose
// "differential" bit says the block is opaque.
void etc(const uchar* s, QRgb* out, bool etc2, bool punch){
    const quint64 b = be64(s);
    const bool diff = punch ? true : (b >> 33 & 1), opaque = punch ? (b >> 33 & 1) : true, flip = b >> 32 & 1;
    auto pixIndex = [&](int i){ return int((b >> (16 + i) & 1) << 1 | (b >> i & 1)); };   // i = x * 4 + y
    auto put = [&](int x, int y, int r, int g, int bl, int a){ out[y * 4 + x] = qRgba(clamp255(r), clamp255(g), clamp255(bl), a); };
    if(diff && etc2){
        const int r = int(b >> 59 & 31), dr = int(b >> 56 & 7) - (b >> 58 & 1 ? 8 : 0);
        const int g = int(b >> 51 & 31), dg = int(b >> 48 & 7) - (b >> 50 & 1 ? 8 : 0);
        const int bb = int(b >> 43 & 31), db = int(b >> 40 & 7) - (b >> 42 & 1 ? 8 : 0);
        if(r + dr < 0 || r + dr > 31 || g + dg < 0 || g + dg > 31){
            int pr[4], pg[4], pb[4];
            if(r + dr < 0 || r + dr > 31){                                          // T mode
                const int r1 = ext4(int((b >> 59 & 3) << 2 | (b >> 56 & 3))), g1 = ext4(int(b >> 52 & 15)), b1 = ext4(int(b >> 48 & 15));
                const int r2 = ext4(int(b >> 44 & 15)), g2 = ext4(int(b >> 40 & 15)), b2 = ext4(int(b >> 36 & 15));
                const int d = kEtcDist[(b >> 34 & 3) << 1 | (b >> 32 & 1)];
                pr[0] = r1; pg[0] = g1; pb[0] = b1;
                pr[1] = r2 + d; pg[1] = g2 + d; pb[1] = b2 + d;
                pr[2] = r2; pg[2] = g2; pb[2] = b2;
                pr[3] = r2 - d; pg[3] = g2 - d; pb[3] = b2 - d;
            } else {                                                                  // H mode
                const int r1 = ext4(int(b >> 59 & 15)), g1 = ext4(int((b >> 56 & 7) << 1 | (b >> 52 & 1))), b1 = ext4(int((b >> 51 & 1) << 3 | (b >> 47 & 7)));
                const int r2 = ext4(int(b >> 43 & 15)), g2 = ext4(int(b >> 39 & 15)), b2 = ext4(int(b >> 35 & 15));
                const int c1 = r1 << 16 | g1 << 8 | b1, c2 = r2 << 16 | g2 << 8 | b2;
                const int d = kEtcDist[(b >> 34 & 1) << 2 | (b >> 32 & 1) << 1 | (c1 >= c2 ? 1 : 0)];
                pr[0] = r1 + d; pg[0] = g1 + d; pb[0] = b1 + d;
                pr[1] = r1 - d; pg[1] = g1 - d; pb[1] = b1 - d;
                pr[2] = r2 + d; pg[2] = g2 + d; pb[2] = b2 + d;
                pr[3] = r2 - d; pg[3] = g2 - d; pb[3] = b2 - d;
            }
            for(int x = 0; x < 4; ++x) for(int y = 0; y < 4; ++y){
                const int k = pixIndex(x * 4 + y);
                if(!opaque && k == 2){ out[y * 4 + x] = 0; continue; }
                put(x, y, pr[k], pg[k], pb[k], 255);
            }
            return;
        }
        if(bb + db < 0 || bb + db > 31){                                              // planar mode
            const int ro = int(b >> 57 & 63), go = int((b >> 56 & 1) << 6 | (b >> 49 & 63)), bo = int((b >> 48 & 1) << 5 | (b >> 43 & 3) << 3 | (b >> 39 & 7));
            const int rh = int((b >> 34 & 31) << 1 | (b >> 32 & 1)), gh = int(b >> 25 & 127), bh = int(b >> 19 & 63);
            const int rv = int(b >> 13 & 63), gv = int(b >> 6 & 127), bv = int(b & 63);
            auto e6 = [](int v){ return v << 2 | v >> 4; }; auto e7 = [](int v){ return v << 1 | v >> 6; };
            const int RO = e6(ro), GO = e7(go), BO = e6(bo), RH = e6(rh), GH = e7(gh), BH = e6(bh), RV = e6(rv), GV = e7(gv), BV = e6(bv);
            for(int x = 0; x < 4; ++x) for(int y = 0; y < 4; ++y)
                put(x, y, (x * (RH - RO) + y * (RV - RO) + 4 * RO + 2) >> 2, (x * (GH - GO) + y * (GV - GO) + 4 * GO + 2) >> 2,
                    (x * (BH - BO) + y * (BV - BO) + 4 * BO + 2) >> 2, 255);
            return;
        }
    }
    int base[2][3];
    if(diff){
        const int r = int(b >> 59 & 31), g = int(b >> 51 & 31), bl = int(b >> 43 & 31);
        const int dr = int(b >> 56 & 7) - (b >> 58 & 1 ? 8 : 0), dg = int(b >> 48 & 7) - (b >> 50 & 1 ? 8 : 0), db = int(b >> 40 & 7) - (b >> 42 & 1 ? 8 : 0);
        base[0][0] = ext5(r); base[0][1] = ext5(g); base[0][2] = ext5(bl);
        base[1][0] = ext5((r + dr) & 31); base[1][1] = ext5((g + dg) & 31); base[1][2] = ext5((bl + db) & 31);
    } else {
        base[0][0] = ext4(int(b >> 60 & 15)); base[1][0] = ext4(int(b >> 56 & 15));
        base[0][1] = ext4(int(b >> 52 & 15)); base[1][1] = ext4(int(b >> 48 & 15));
        base[0][2] = ext4(int(b >> 44 & 15)); base[1][2] = ext4(int(b >> 40 & 15));
    }
    const int cw[2] = { int(b >> 37 & 7), int(b >> 34 & 7) };
    for(int x = 0; x < 4; ++x) for(int y = 0; y < 4; ++y){
        const int sub = flip ? (y >= 2) : (x >= 2);
        const int k = pixIndex(x * 4 + y);
        if(!opaque && k == 2){ out[y * 4 + x] = 0; continue; }
        const int mod = (!opaque && (k == 0)) ? 0 : kEtcMod[cw[sub]][k];
        put(x, y, base[sub][0] + mod, base[sub][1] + mod, base[sub][2] + mod, 255);
    }
}
// An EAC block: 8-bit alpha (RGBA8), or 11-bit R/G (sgn: signed), into 16 values 0..255.
void eac(const uchar* s, int* v, bool elevenBit, bool sgn){
    const quint64 b = be64(s);
    const int mult = int(b >> 52 & 15), tbl = int(b >> 48 & 15);
    for(int x = 0; x < 4; ++x) for(int y = 0; y < 4; ++y){
        const int k = int(b >> (45 - 3 * (x * 4 + y)) & 7), mod = kEac[tbl][k];
        int val;
        if(!elevenBit) val = clamp255(int(s[0]) + mod * mult);
        else if(!sgn){
            int e = int(s[0]) * 8 + 4 + (mult ? mod * mult * 8 : mod);
            e = e < 0 ? 0 : e > 2047 ? 2047 : e; val = e >> 3;
        } else {
            int base = qint8(s[0]); if(base == -128) base = -127;
            int e = base * 8 + (mult ? mod * mult * 8 : mod);
            e = e < -1023 ? -1023 : e > 1023 ? 1023 : e; val = (e + 1023) * 255 / 2046;
        }
        v[y * 4 + x] = val;
    }
}

#include "astc.inc"

} // namespace

int blockBytes(Format f){
    switch(f){ case Format::BC1: case Format::BC1A: case Format::BC4: case Format::BC4S: case Format::ETC1: case Format::ETC2_RGB: case Format::ETC2_RGBA1: case Format::EAC_R11: case Format::EAC_R11S: return 8; default: return 16; }
}

qint64 dataSize(Format f, int w, int h, int bw, int bh){
    if(f != Format::ASTC){ bw = 4; bh = 4; }
    if(w < 1 || h < 1 || bw < 1 || bh < 1) return 0;
    return qint64((w + bw - 1) / bw) * ((h + bh - 1) / bh) * blockBytes(f);
}

bool decodeBlock(Format f, const uchar* s, QRgb* out, int bw, int bh){
    int a[16], c[16];
    switch(f){
    case Format::BC1:  bc1(s, out, true, false); return true;
    case Format::BC1A: bc1(s, out, true, true); return true;
    case Format::BC2:  bc1(s + 8, out, false, false); for(int i = 0; i < 16; ++i){ const int v = (s[i / 2] >> (4 * (i & 1))) & 15; out[i] = (out[i] & 0xffffff) | uint(v * 17) << 24; } return true;
    case Format::BC3:  bc1(s + 8, out, false, false); bc4(s, a, false); for(int i = 0; i < 16; ++i) out[i] = (out[i] & 0xffffff) | uint(a[i]) << 24; return true;
    case Format::BC4: case Format::BC4S: bc4(s, a, f == Format::BC4S); for(int i = 0; i < 16; ++i) out[i] = qRgba(a[i], 0, 0, 255); return true;
    case Format::BC5: case Format::BC5S: bc4(s, a, f == Format::BC5S); bc4(s + 8, c, f == Format::BC5S); for(int i = 0; i < 16; ++i) out[i] = qRgba(a[i], c[i], 0, 255); return true;
    case Format::BC7:  return bc7(s, out);
    case Format::ETC1: etc(s, out, false, false); return true;
    case Format::ETC2_RGB: etc(s, out, true, false); return true;
    case Format::ETC2_RGBA1: etc(s, out, true, true); return true;
    case Format::ETC2_RGBA8: etc(s + 8, out, true, false); eac(s, a, false, false); for(int i = 0; i < 16; ++i) out[i] = (out[i] & 0xffffff) | uint(a[i]) << 24; return true;
    case Format::EAC_R11: case Format::EAC_R11S: eac(s, a, true, f == Format::EAC_R11S); for(int i = 0; i < 16; ++i) out[i] = qRgba(a[i], 0, 0, 255); return true;
    case Format::EAC_RG11: case Format::EAC_RG11S: eac(s, a, true, f == Format::EAC_RG11S); eac(s + 8, c, true, f == Format::EAC_RG11S); for(int i = 0; i < 16; ++i) out[i] = qRgba(a[i], c[i], 0, 255); return true;
    case Format::ASTC: return astcBlock(s, out, bw, bh);
    }
    return false;
}

QImage decode(Format f, const uchar* data, qint64 size, int w, int h, int bw, int bh){
    if(f != Format::ASTC){ bw = 4; bh = 4; }
    if(w < 1 || h < 1 || w > 65536 || h > 65536 || qint64(w) * h > (qint64(1) << 28) || bw < 4 || bh < 4 || bw > 12 || bh > 12) return QImage();
    if(!data || size < dataSize(f, w, h, bw, bh)) return QImage();
    QImage img(w, h, QImage::Format_ARGB32);
    if(img.isNull()) return img;
    const int bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh, bb = blockBytes(f);
    QRgb px[144];
    for(int j = 0; j < by; ++j) for(int i = 0; i < bx; ++i){
        decodeBlock(f, data + (qint64(j) * bx + i) * bb, px, bw, bh);
        for(int y = 0; y < bh && j * bh + y < h; ++y){
            QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(j * bh + y)) + i * bw;
            for(int x = 0; x < bw && i * bw + x < w; ++x) d[x] = px[y * bw + x];
        }
    }
    return img;
}

} // namespace gputex
} // namespace xara
