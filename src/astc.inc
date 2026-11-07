// ASTC (LDR profile), after the Khronos Data Format Specification's ASTC chapter. A 128-bit block holds a block
// mode (weight grid size, weight range, dual plane), a partition count and pattern, colour endpoint modes,
// endpoint values in a bounded integer sequence (bits, trits or quints), and the weights read backwards from
// the block's end. Invalid blocks, and HDR endpoint modes, decode to the error colour, magenta.
// Included into gputex.cpp.

const QRgb kAstcError = qRgba(255, 0, 255, 255);

struct AstcBits {
    const uchar* p; bool reversed;
    int bit(int i) const { if(i < 0 || i > 127) return 0; if(reversed) i = 127 - i; return (p[i >> 3] >> (i & 7)) & 1; }
    int take(int at, int n) const { int v = 0; for(int k = 0; k < n; ++k) v |= bit(at + k) << k; return v; }
};

// The number of bits, and whether trits or quints come along, for each quantisation range (levels).
struct Ise { int levels, bits, trits, quints; };
const Ise kIse[21] = { {2,1,0,0},{3,0,1,0},{4,2,0,0},{5,0,0,1},{6,1,1,0},{8,3,0,0},{10,1,0,1},{12,2,1,0},{16,4,0,0},{20,2,0,1},{24,3,1,0},
                       {32,5,0,0},{40,3,0,1},{48,4,1,0},{64,6,0,0},{80,4,0,1},{96,5,1,0},{128,7,0,0},{160,5,0,1},{192,6,1,0},{256,8,0,0} };
int iseBits(int count, const Ise& q){ return count * q.bits + (q.trits ? (8 * count + 4) / 5 : q.quints ? (7 * count + 2) / 3 : 0); }

// Read `count` values of range q from the bit stream at `at`.
void iseRead(const AstcBits& b, int at, int count, const Ise& q, int* out, int* bitsOut){
    int pos = at;
    if(q.trits){
        for(int blk = 0; blk < count; blk += 5){
            int m[5] = {}, T = 0; const int n = qMin(5, count - blk);
            static const int tb[5][2] = { {0,2},{2,2},{4,1},{5,2},{7,1} };            // trit bits after each value
            for(int i = 0; i < 5; ++i){
                if(i < n){ m[i] = b.take(pos, q.bits); pos += q.bits; }
                else pos += 0;
                if(i < n){ T |= b.take(pos, tb[i][1]) << tb[i][0]; pos += tb[i][1]; }
            }
            int t[5], C;
            auto tbit = [&](int k){ return (T >> k) & 1; };
            if(((T >> 2) & 7) == 7){ C = ((T >> 5) & 7) << 2 | (T & 3); t[4] = 2; t[3] = 2; }
            else { C = T & 31; if(((T >> 5) & 3) == 3){ t[4] = 2; t[3] = tbit(7); } else { t[4] = tbit(7); t[3] = (T >> 5) & 3; } }
            if((C & 3) == 3){ t[2] = 2; t[1] = (C >> 4) & 1; t[0] = ((C >> 3) & 1) << 1 | (((C >> 2) & 1) & ~((C >> 3) & 1) & 1); }
            else if(((C >> 2) & 3) == 3){ t[2] = 2; t[1] = 2; t[0] = C & 3; }
            else { t[2] = (C >> 4) & 1; t[1] = (C >> 2) & 3; t[0] = ((C >> 1) & 1) << 1 | ((C & 1) & ~((C >> 1) & 1) & 1); }
            for(int i = 0; i < n; ++i) out[blk + i] = t[i] << q.bits | m[i];
        }
    } else if(q.quints){
        for(int blk = 0; blk < count; blk += 3){
            int m[3] = {}, Q = 0; const int n = qMin(3, count - blk);
            static const int qb[3][2] = { {0,3},{3,2},{5,2} };
            for(int i = 0; i < 3; ++i){
                if(i < n){ m[i] = b.take(pos, q.bits); pos += q.bits; Q |= b.take(pos, qb[i][1]) << qb[i][0]; pos += qb[i][1]; }
            }
            int qv[3];
            auto qbit = [&](int k){ return (Q >> k) & 1; };
            if(((Q >> 1) & 3) == 3 && ((Q >> 5) & 3) == 0){
                qv[2] = qbit(0) << 2 | (qbit(4) & ~qbit(0) & 1) << 1 | (qbit(3) & ~qbit(0) & 1); qv[1] = 4; qv[0] = 4;
            } else {
                int C;
                if(((Q >> 1) & 3) == 3){ qv[2] = 4; C = ((Q >> 3) & 3) << 3 | ((~(Q >> 5)) & 3) << 1 | qbit(0); }
                else { qv[2] = (Q >> 5) & 3; C = Q & 31; }
                if((C & 7) == 5){ qv[1] = 4; qv[0] = (C >> 3) & 3; } else { qv[1] = (C >> 3) & 3; qv[0] = C & 7; }
            }
            for(int i = 0; i < n; ++i) out[blk + i] = qv[i] << q.bits | m[i];
        }
    } else {
        for(int i = 0; i < count; ++i){ out[i] = b.take(pos, q.bits); pos += q.bits; }
    }
    *bitsOut = iseBits(count, q);
}

// Colour endpoint values to 0..255.
int unquantColour(int v, const Ise& q){
    if(!q.trits && !q.quints){ int r = 0, have = 0; while(have < 8){ r = (r << q.bits) | v; have += q.bits; } return r >> (have - 8); }
    const int m = v & ((1 << q.bits) - 1), D = v >> q.bits;
    const int A = (m & 1) ? 0x1ff : 0;
    auto bt = [&](int k){ return (m >> k) & 1; };
    int B = 0, C = 0;
    if(q.trits) switch(q.bits){
        case 1: C = 204; break;
        case 2: { const int b = bt(1); B = b << 8 | b << 4 | b << 2 | b << 1; C = 93; } break;
        case 3: { const int b = bt(1), c = bt(2); B = c << 8 | b << 7 | c << 3 | b << 2 | c << 1 | b; C = 44; } break;
        case 4: { const int b = bt(1), c = bt(2), d = bt(3); B = d << 8 | c << 7 | b << 6 | d << 2 | c << 1 | b; C = 22; } break;
        case 5: { const int b = bt(1), c = bt(2), d = bt(3), e = bt(4); B = e << 8 | d << 7 | c << 6 | b << 5 | e << 1 | d; C = 11; } break;
        case 6: { const int b = bt(1), c = bt(2), d = bt(3), e = bt(4), f = bt(5); B = f << 8 | e << 7 | d << 6 | c << 5 | b << 4 | f; C = 5; } break;
        default: return 0;
    } else switch(q.bits){
        case 1: C = 113; break;
        case 2: { const int b = bt(1); B = b << 8 | b << 3 | b << 2; C = 54; } break;
        case 3: { const int b = bt(1), c = bt(2); B = c << 8 | b << 7 | c << 2 | b << 1 | c; C = 26; } break;
        case 4: { const int b = bt(1), c = bt(2), d = bt(3); B = d << 8 | c << 7 | b << 6 | d << 1 | c; C = 13; } break;
        case 5: { const int b = bt(1), c = bt(2), d = bt(3), e = bt(4); B = e << 8 | d << 7 | c << 6 | b << 5 | e; C = 6; } break;
        default: return 0;
    }
    int T = D * C + B;
    T ^= A;
    return (A & 0x80) | (T >> 2);
}

// Weights to 0..64.
int unquantWeight(int v, const Ise& q){
    int r;
    if(!q.trits && !q.quints){ int x = 0, have = 0; while(have < 6){ x = (x << q.bits) | v; have += q.bits; } r = x >> (have - 6); }
    else if(q.bits == 0){ r = q.trits ? v * 32 : v * 16; return r; }
    else {
        const int m = v & ((1 << q.bits) - 1), D = v >> q.bits, A = (m & 1) ? 0x7f : 0;
        auto bt = [&](int k){ return (m >> k) & 1; };
        int B = 0, C = 0;
        if(q.trits) switch(q.bits){
            case 1: C = 50; break;
            case 2: { const int b = bt(1); B = b << 6 | b << 2 | b; C = 23; } break;
            case 3: { const int b = bt(1), c = bt(2); B = c << 6 | b << 5 | c << 1 | b; C = 11; } break;
            default: return 0;
        } else switch(q.bits){
            case 1: C = 28; break;
            case 2: { const int b = bt(1); B = b << 6 | b << 1; C = 13; } break;
            default: return 0;
        }
        int T = D * C + B; T ^= A; r = (A & 0x20) | (T >> 2);
    }
    if(r > 32) ++r;
    return r;
}

quint32 hash52(quint32 p){
    p ^= p >> 15; p -= p << 17; p += p << 7; p += p << 4; p ^= p >> 5; p += p << 16; p ^= p >> 7; p ^= p >> 3; p ^= p << 6; p ^= p >> 17;
    return p;
}
int selectPartition(int seed, int x, int y, int count, bool small){
    if(small){ x <<= 1; y <<= 1; }
    seed += (count - 1) * 1024;
    const quint32 r = hash52(quint32(seed));
    int s[13];
    s[1] = r & 15; s[2] = (r >> 4) & 15; s[3] = (r >> 8) & 15; s[4] = (r >> 12) & 15; s[5] = (r >> 16) & 15; s[6] = (r >> 20) & 15;
    s[7] = (r >> 24) & 15; s[8] = (r >> 28) & 15; s[9] = (r >> 18) & 15; s[10] = (r >> 22) & 15; s[11] = (r >> 26) & 15; s[12] = ((r >> 30) | (r << 2)) & 15;
    for(int i = 1; i <= 12; ++i) s[i] *= s[i];
    int sh1, sh2;
    if(seed & 1){ sh1 = (seed & 2) ? 4 : 5; sh2 = count == 3 ? 6 : 5; } else { sh1 = count == 3 ? 6 : 5; sh2 = (seed & 2) ? 4 : 5; }
    const int sh3 = (seed & 0x10) ? sh1 : sh2;
    s[1] >>= sh1; s[2] >>= sh2; s[3] >>= sh1; s[4] >>= sh2; s[5] >>= sh1; s[6] >>= sh2; s[7] >>= sh1; s[8] >>= sh2;
    s[9] >>= sh3; s[10] >>= sh3; s[11] >>= sh3; s[12] >>= sh3;
    int a = int((s[1] * x + s[2] * y + (r >> 14)) & 0x3f), b = int((s[3] * x + s[4] * y + (r >> 10)) & 0x3f);
    int c = int((s[5] * x + s[6] * y + (r >> 6)) & 0x3f), d = int((s[7] * x + s[8] * y + (r >> 2)) & 0x3f);
    if(count < 4) d = 0;
    if(count < 3) c = 0;
    if(a >= b && a >= c && a >= d) return 0;
    if(b >= c && b >= d) return 1;
    if(c >= d) return 2;
    return 3;
}

void bitTransferSigned(int& a, int& b){ b >>= 1; b |= a & 0x80; a >>= 1; a &= 0x3f; if(a & 0x20) a -= 0x40; }
void blueContract(int* e){ e[0] = (e[0] + e[2]) >> 1; e[1] = (e[1] + e[2]) >> 1; }

// One partition's endpoints from its colour values; false for HDR modes.
bool endpoints(int cem, const int* v, int* e0, int* e1){
    auto set = [](int* e, int r, int g, int b, int a){ e[0] = qBound(0, r, 255); e[1] = qBound(0, g, 255); e[2] = qBound(0, b, 255); e[3] = qBound(0, a, 255); };
    int a[8]; for(int i = 0; i < 8; ++i) a[i] = v[i];
    switch(cem){
    case 0: set(e0, a[0], a[0], a[0], 255); set(e1, a[1], a[1], a[1], 255); return true;
    case 1: { const int l0 = (a[0] >> 2) | (a[1] & 0xc0); const int l1 = qMin(255, l0 + (a[1] & 0x3f)); set(e0, l0, l0, l0, 255); set(e1, l1, l1, l1, 255); return true; }
    case 4: set(e0, a[0], a[0], a[0], a[2]); set(e1, a[1], a[1], a[1], a[3]); return true;
    case 5: bitTransferSigned(a[1], a[0]); bitTransferSigned(a[3], a[2]);
            set(e0, a[0], a[0], a[0], a[2]); set(e1, a[0] + a[1], a[0] + a[1], a[0] + a[1], a[2] + a[3]); return true;
    case 6: set(e0, (a[0] * a[3]) >> 8, (a[1] * a[3]) >> 8, (a[2] * a[3]) >> 8, 255); set(e1, a[0], a[1], a[2], 255); return true;
    case 8: {
        if(a[1] + a[3] + a[5] >= a[0] + a[2] + a[4]){ set(e0, a[0], a[2], a[4], 255); set(e1, a[1], a[3], a[5], 255); }
        else { int x[4] = {a[1], a[3], a[5], 255}, y[4] = {a[0], a[2], a[4], 255}; blueContract(x); blueContract(y); set(e0, x[0], x[1], x[2], 255); set(e1, y[0], y[1], y[2], 255); }
        return true; }
    case 9: {
        bitTransferSigned(a[1], a[0]); bitTransferSigned(a[3], a[2]); bitTransferSigned(a[5], a[4]);
        if(a[1] + a[3] + a[5] >= 0){ set(e0, a[0], a[2], a[4], 255); set(e1, a[0] + a[1], a[2] + a[3], a[4] + a[5], 255); }
        else { int x[4] = {a[0] + a[1], a[2] + a[3], a[4] + a[5], 255}, y[4] = {a[0], a[2], a[4], 255}; blueContract(x); blueContract(y);
               set(e0, x[0], x[1], x[2], 255); set(e1, y[0], y[1], y[2], 255); }
        return true; }
    case 10: set(e0, (a[0] * a[3]) >> 8, (a[1] * a[3]) >> 8, (a[2] * a[3]) >> 8, a[4]); set(e1, a[0], a[1], a[2], a[5]); return true;
    case 12: {
        if(a[1] + a[3] + a[5] >= a[0] + a[2] + a[4]){ set(e0, a[0], a[2], a[4], a[6]); set(e1, a[1], a[3], a[5], a[7]); }
        else { int x[4] = {a[1], a[3], a[5], a[7]}, y[4] = {a[0], a[2], a[4], a[6]}; blueContract(x); blueContract(y); set(e0, x[0], x[1], x[2], x[3]); set(e1, y[0], y[1], y[2], y[3]); }
        return true; }
    case 13: {
        bitTransferSigned(a[1], a[0]); bitTransferSigned(a[3], a[2]); bitTransferSigned(a[5], a[4]); bitTransferSigned(a[7], a[6]);
        if(a[1] + a[3] + a[5] >= 0){ set(e0, a[0], a[2], a[4], a[6]); set(e1, a[0] + a[1], a[2] + a[3], a[4] + a[5], a[6] + a[7]); }
        else { int x[4] = {a[0] + a[1], a[2] + a[3], a[4] + a[5], a[6] + a[7]}, y[4] = {a[0], a[2], a[4], a[6]}; blueContract(x); blueContract(y);
               set(e0, x[0], x[1], x[2], x[3]); set(e1, y[0], y[1], y[2], y[3]); }
        return true; }
    default: return false;                                                     // HDR endpoint modes
    }
}

bool astcBlock(const uchar* p, QRgb* out, int bw, int bh){
    const int texels = bw * bh;
    auto fail = [&]{ for(int i = 0; i < texels; ++i) out[i] = kAstcError; return false; };
    const AstcBits b{ p, false };
    const int mode = b.take(0, 11);
    if((mode & 0x1ff) == 0x1fc){                                               // void extent: one colour for the whole block
        if(mode & 0x200) return fail();                                        // an HDR constant colour
        const QRgb c = qRgba(b.take(64 + 8, 8), b.take(80 + 8, 8), b.take(96 + 8, 8), b.take(112 + 8, 8));
        for(int i = 0; i < texels; ++i) out[i] = c;
        return true;
    }
    // The block mode: weight grid size, weight range and dual plane.
    int W, H, r; bool dual = false, high = false;
    if(mode & 3){
        r = ((mode >> 4) & 1) | (mode & 3) << 1; high = (mode >> 9) & 1; dual = (mode >> 10) & 1;
        const int A = (mode >> 5) & 3, B = (mode >> 7) & 3;
        switch((mode >> 2) & 3){
            case 0: W = B + 4; H = A + 2; break;
            case 1: W = B + 8; H = A + 2; break;
            case 2: W = A + 2; H = B + 8; break;
            default: if(mode & 0x100){ W = (B & 1) + 2; H = A + 2; } else { W = A + 2; H = (B & 1) + 6; } break;
        }
    } else {
        if((mode & 15) == 0) return fail();
        r = ((mode >> 4) & 1) | ((mode >> 2) & 3) << 1;
        const int A = (mode >> 5) & 3, B = (mode >> 9) & 3;
        switch((mode >> 7) & 3){
            case 0: W = 12; H = A + 2; high = (mode >> 9) & 1; dual = (mode >> 10) & 1; break;
            case 1: W = A + 2; H = 12; high = (mode >> 9) & 1; dual = (mode >> 10) & 1; break;
            case 2: W = A + 6; H = B + 6; break;
            default:
                if((mode >> 5) & 2) return fail();                              // reserved
                high = (mode >> 9) & 1; dual = (mode >> 10) & 1;
                if((mode >> 5) & 1){ W = 10; H = 6; } else { W = 6; H = 10; }
                break;
        }
    }
    if(r < 2) return fail();
    static const int rangeLevels[2][6] = { {2, 3, 4, 5, 6, 8}, {10, 12, 16, 20, 24, 32} };
    const int levels = rangeLevels[high][r - 2];
    int wq = 0; while(kIse[wq].levels != levels) ++wq;
    const int planes = dual ? 2 : 1, nWeights = W * H * planes;
    if(W > bw || H > bh || nWeights > 64) return fail();
    const int weightBits = iseBits(nWeights, kIse[wq]);
    if(weightBits < 24 || weightBits > 96) return fail();

    const int parts = b.take(11, 2) + 1;
    if(parts == 4 && dual) return fail();
    int cem[4], colourStart, extraBits = 0, partIndex = 0;
    if(parts == 1){ cem[0] = b.take(13, 4); colourStart = 17; }
    else {
        partIndex = b.take(13, 10);
        const int field = b.take(23, 6);
        colourStart = 29;
        if((field & 3) == 0){ for(int i = 0; i < parts; ++i) cem[i] = field >> 2; }
        else {
            extraBits = 3 * parts - 4;
            const int extraAt = 128 - weightBits - extraBits - (dual ? 2 : 0);
            const int all = field | b.take(extraAt, extraBits) << 6;
            const int base = (all & 3) - 1;
            for(int i = 0; i < parts; ++i){
                const int c = (all >> (2 + i)) & 1, m = (all >> (2 + parts + 2 * i)) & 3;
                cem[i] = (base + c) << 2 | m;
            }
        }
    }
    int nValues = 0;
    for(int i = 0; i < parts; ++i) nValues += 2 * ((cem[i] >> 2) + 1);
    if(nValues > 18) return fail();
    const int colourBits = 128 - weightBits - extraBits - (dual ? 2 : 0) - colourStart;
    if(colourBits < (13 * nValues + 4) / 5) return fail();
    int cq = 20; while(cq > 0 && iseBits(nValues, kIse[cq]) > colourBits) --cq;
    if(cq < 4) return fail();
    int raw[18], used = 0;
    iseRead(b, colourStart, nValues, kIse[cq], raw, &used);
    int vals[18]; for(int i = 0; i < nValues; ++i) vals[i] = unquantColour(raw[i], kIse[cq]);
    int e[4][2][4];
    for(int i = 0, k = 0; i < parts; ++i){
        if(!endpoints(cem[i], vals + k, e[i][0], e[i][1])) return fail();
        k += 2 * ((cem[i] >> 2) + 1);
    }
    const int ccs = dual ? b.take(128 - weightBits - 2, 2) : -1;

    // Weights: stored from the block's last bit backwards.
    const AstcBits rb{ p, true };
    int wraw[64], wused = 0; iseRead(rb, 0, nWeights, kIse[wq], wraw, &wused);
    int grid[64]; for(int i = 0; i < nWeights; ++i) grid[i] = unquantWeight(wraw[i], kIse[wq]);
    const int Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1);
    const bool small = texels < 31;
    for(int t = 0; t < bh; ++t) for(int s = 0; s < bw; ++s){
        int w[2];
        const int gs = (Ds * s * (W - 1) + 32) >> 6, gt = (Dt * t * (H - 1) + 32) >> 6;
        const int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
        const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
        for(int pl = 0; pl < planes; ++pl){
            const int v0 = js + jt * W;
            auto g = [&](int idx){ return idx < W * H ? grid[idx * planes + pl] : 0; };
            const int p00 = g(v0), p01 = (js + 1 < W) ? g(v0 + 1) : 0, p10 = (jt + 1 < H) ? g(v0 + W) : 0, p11 = (js + 1 < W && jt + 1 < H) ? g(v0 + W + 1) : 0;
            w[pl] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
        }
        const int part = parts == 1 ? 0 : selectPartition(partIndex, s, t, parts, small);
        int c[4];
        for(int ch = 0; ch < 4; ++ch){
            const int wt = (dual && ch == ccs) ? w[1] : w[0];
            const int c0 = e[part][0][ch] << 8 | e[part][0][ch], c1 = e[part][1][ch] << 8 | e[part][1][ch];
            c[ch] = ((c0 * (64 - wt) + c1 * wt + 32) >> 6) >> 8;
        }
        out[t * bw + s] = qRgba(c[0], c[1], c[2], c[3]);
    }
    return true;
}
