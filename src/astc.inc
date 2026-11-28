// ASTC, after the Khronos Data Format Specification's ASTC chapter. A 128-bit block holds a block mode (weight
// grid size, weight range, dual plane), a partition count and pattern, colour endpoint modes, endpoint values in
// a bounded integer sequence (bits, trits or quints), and the weights read backwards from the block's end.
// HDR endpoint modes interpolate in ASTC's logarithmic 16-bit form and come out as half floats, shown clamped in
// sRGB as BC6H is. Invalid blocks decode to the error colour, magenta.
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

// The HDR endpoint modes: 12-bit logarithmic values, widened to 16 bits. Alpha is 1.0 unless the mode carries it
// (mode 14 an LDR alpha, mode 15 an HDR one). hdrA says whether alpha is logarithmic too.
bool hdrEndpoints(int cem, const int* v, int* e0, int* e1, bool& hdrA){
    auto rgb = [](int* e, int r, int g, int b){ e[0] = r << 4; e[1] = g << 4; e[2] = b << 4; e[3] = 0x7800; };
    hdrA = true;
    switch(cem){
    case 2: {                                                                   // luminance, large range
        int y0, y1; if(v[1] >= v[0]){ y0 = v[0] << 4; y1 = v[1] << 4; } else { y0 = (v[1] << 4) + 8; y1 = (v[0] << 4) - 8; }
        rgb(e0, y0, y0, y0); rgb(e1, y1, y1, y1); return true; }
    case 3: {                                                                   // luminance, small range
        int y0, d; if(v[0] & 0x80){ y0 = ((v[1] & 0xE0) << 4) | ((v[0] & 0x7F) << 2); d = (v[1] & 0x1F) << 2; }
                   else { y0 = ((v[1] & 0xF0) << 4) | ((v[0] & 0x7F) << 1); d = (v[1] & 0x0F) << 1; }
        const int y1 = qMin(0xFFF, y0 + d); rgb(e0, y0, y0, y0); rgb(e1, y1, y1, y1); return true; }
    case 7: {                                                                   // RGB and a scale down to endpoint 0
        const int mv = ((v[0] & 0xC0) >> 6) | ((v[1] & 0x80) >> 5) | ((v[2] & 0x80) >> 4);
        int major, mode;
        if((mv & 0xC) != 0xC){ major = mv >> 2; mode = mv & 3; } else if(mv != 0xF){ major = mv & 3; mode = 4; } else { major = 0; mode = 5; }
        int r = v[0] & 0x3F, g = v[1] & 0x1F, b = v[2] & 0x1F, sc = v[3] & 0x1F;
        const int x0 = v[1] >> 6 & 1, x1 = v[1] >> 5 & 1, x2 = v[2] >> 6 & 1, x3 = v[2] >> 5 & 1, x4 = v[3] >> 7 & 1, x5 = v[3] >> 6 & 1, x6 = v[3] >> 5 & 1;
        const int oh = 1 << mode;                                               // which mode, one bit each
        if(oh & 0x30) g |= x0 << 6; if(oh & 0x3A) g |= x1 << 5;
        if(oh & 0x30) b |= x2 << 6; if(oh & 0x3A) b |= x3 << 5;
        if(oh & 0x3D) sc |= x6 << 5; if(oh & 0x2D) sc |= x5 << 6; if(oh & 0x04) sc |= x4 << 7;
        if(oh & 0x3B) r |= x4 << 6; if(oh & 0x04) r |= x3 << 6; if(oh & 0x10) r |= x5 << 7; if(oh & 0x0F) r |= x2 << 7;
        if(oh & 0x05) r |= x1 << 8; if(oh & 0x0A) r |= x0 << 8; if(oh & 0x05) r |= x0 << 9; if(oh & 0x02) r |= x6 << 9;
        if(oh & 0x01) r |= x3 << 10; if(oh & 0x02) r |= x5 << 10;
        static const int shifts[6] = {1, 1, 2, 3, 4, 5};
        r <<= shifts[mode]; g <<= shifts[mode]; b <<= shifts[mode]; sc <<= shifts[mode];
        if(mode != 5){ g = r - g; b = r - b; }
        if(major == 1) std::swap(r, g); else if(major == 2) std::swap(r, b);
        rgb(e0, qMax(0, r - sc), qMax(0, g - sc), qMax(0, b - sc)); rgb(e1, qMax(0, r), qMax(0, g), qMax(0, b));
        return true; }
    case 11: case 14: case 15: {                                                // RGB directly (and alpha)
        const int mv = ((v[1] & 0x80) >> 7) | ((v[2] & 0x80) >> 6) | ((v[3] & 0x80) >> 5), major = ((v[4] & 0x80) >> 7) | ((v[5] & 0x80) >> 6);
        if(major == 3){ e0[0] = v[0] << 8; e0[1] = v[2] << 8; e0[2] = (v[4] & 0x7F) << 9; e1[0] = v[1] << 8; e1[1] = v[3] << 8; e1[2] = (v[5] & 0x7F) << 9; }
        else {
            int a = v[0] | ((v[1] & 0x40) << 2), b0 = v[2] & 0x3F, b1 = v[3] & 0x3F, c = v[1] & 0x3F, d0 = v[4] & 0x7F, d1 = v[5] & 0x7F;
            static const int dbits[8] = {7, 6, 7, 6, 5, 6, 5, 6};
            const int x0 = v[2] >> 6 & 1, x1 = v[3] >> 6 & 1, x2 = v[4] >> 6 & 1, x3 = v[5] >> 6 & 1, x4 = v[4] >> 5 & 1, x5 = v[5] >> 5 & 1;
            const int oh = 1 << mv;
            if(oh & 0xA4) a |= x0 << 9; if(oh & 0x8) a |= x2 << 9; if(oh & 0x50) a |= x4 << 9; if(oh & 0x50) a |= x5 << 10;
            if(oh & 0xA0) a |= x1 << 10; if(oh & 0xC0) a |= x2 << 11;
            if(oh & 0x4) c |= x1 << 6; if(oh & 0xE8) c |= x3 << 6; if(oh & 0x20) c |= x2 << 7;
            if(oh & 0x5B){ b0 |= x0 << 6; b1 |= x1 << 6; } if(oh & 0x12){ b0 |= x2 << 7; b1 |= x3 << 7; }
            if(oh & 0xAF){ d0 |= x4 << 5; d1 |= x5 << 5; } if(oh & 0x5){ d0 |= x2 << 6; d1 |= x3 << 6; }
            const int db = dbits[mv]; d0 &= (1 << db) - 1; d1 &= (1 << db) - 1;
            if(d0 & (1 << (db - 1))) d0 -= 1 << db; if(d1 & (1 << (db - 1))) d1 -= 1 << db;
            const int sh = (mv >> 1) ^ 3;
            a *= 1 << sh; b0 *= 1 << sh; b1 *= 1 << sh; c *= 1 << sh; d0 *= 1 << sh; d1 *= 1 << sh;
            int r1 = a, g1 = a - b0, bl1 = a - b1, r0 = a - c, g0 = a - b0 - c - d0, bl0 = a - b1 - c - d1;
            auto cl = [](int x){ return qBound(0, x, 4095); };
            r0 = cl(r0); g0 = cl(g0); bl0 = cl(bl0); r1 = cl(r1); g1 = cl(g1); bl1 = cl(bl1);
            if(major == 1){ std::swap(r0, g0); std::swap(r1, g1); } else if(major == 2){ std::swap(r0, bl0); std::swap(r1, bl1); }
            e0[0] = r0 << 4; e0[1] = g0 << 4; e0[2] = bl0 << 4; e1[0] = r1 << 4; e1[1] = g1 << 4; e1[2] = bl1 << 4;
        }
        e0[3] = e1[3] = 0x7800;
        if(cem == 14){ e0[3] = v[6] << 8 | v[6]; e1[3] = v[7] << 8 | v[7]; hdrA = false; }
        else if(cem == 15){
            const int am = ((v[6] >> 7) & 1) | ((v[7] >> 6) & 2);
            int a6 = v[6] & 0x7F, a7 = v[7] & 0x7F, a0, a1;
            if(am == 3){ a0 = a6 << 5; a1 = a7 << 5; }
            else {
                a6 |= (a7 << (am + 1)) & 0x780;
                a7 &= 0x3F >> am; a7 ^= 32 >> am; a7 -= 32 >> am;
                a0 = a6 << (4 - am); a1 = qBound(0, a0 + a7 * (1 << (4 - am)), 0xFFF);
            }
            e0[3] = a0 << 4; e1[3] = a1 << 4;
        }
        return true; }
    default: return false;
    }
}
// ASTC's logarithmic 16-bit values into half floats.
quint16 lnsToHalf(int p){
    int m = p & 0x7FF; const int e = p >> 11 & 31;
    if(m < 512) m *= 3; else if(m >= 1536) m = m * 5 - 2048; else m = m * 4 - 512;
    return quint16(qMin(0x7BFF, e << 10 | m >> 3));
}

// A block as its parts: endpoint modes and ISE-coded endpoint values, the partition seed, and the ISE-coded weight
// grid. Basis Universal's UASTC HDR 6x6 intermediate format builds these without packing them into 128 bits.
struct AstcLogical {
    bool solid = false; quint16 solidHalf[4] = {};                              // an HDR void-extent block
    int parts = 1, cem[4] = {}, seed = 0, endRange = 20, values[18] = {};
    int weightRange = 0, W = 0, H = 0, ccs = -1; bool dual = false; int weights[72] = {};
};

// The texels of a block's parts; false (and the error colour) if its endpoint modes can't be decoded.
bool astcEvaluate(const AstcLogical& L, QRgb* out, int bw, int bh){
    const int texels = bw * bh;
    auto fail = [&]{ for(int i = 0; i < texels; ++i) out[i] = kAstcError; return false; };
    if(L.solid){
        const QRgb c = qRgba(linearToSrgb8(halfToFloat(L.solidHalf[0])), linearToSrgb8(halfToFloat(L.solidHalf[1])), linearToSrgb8(halfToFloat(L.solidHalf[2])),
                             qBound(0, int(halfToFloat(L.solidHalf[3]) * 255 + 0.5f), 255));
        for(int i = 0; i < texels; ++i) out[i] = c;
        return true;
    }
    const int parts = L.parts, planes = L.dual ? 2 : 1, W = L.W, H = L.H, nWeights = W * H * planes;
    if(parts < 1 || parts > 4 || W < 2 || H < 2 || W > bw || H > bh || nWeights > 72 || L.endRange < 0 || L.endRange > 20 || L.weightRange < 0 || L.weightRange > 11) return fail();
    int vals[18], nValues = 0;
    for(int i = 0; i < parts; ++i) nValues += 2 * ((L.cem[i] >> 2) + 1);
    if(nValues > 18) return fail();
    for(int i = 0; i < nValues; ++i) vals[i] = unquantColour(L.values[i], kIse[L.endRange]);
    int e[4][2][4]; bool hdrRgb[4] = {}, hdrA[4] = {};                           // endpoints as 16-bit values
    for(int i = 0, k = 0; i < parts; ++i){
        if(endpoints(L.cem[i], vals + k, e[i][0], e[i][1])){ for(int j = 0; j < 2; ++j) for(int ch = 0; ch < 4; ++ch) e[i][j][ch] = e[i][j][ch] << 8 | e[i][j][ch]; }
        else if(hdrEndpoints(L.cem[i], vals + k, e[i][0], e[i][1], hdrA[i])) hdrRgb[i] = true;
        else return fail();
        k += 2 * ((L.cem[i] >> 2) + 1);
    }
    const bool dual = L.dual; const int ccs = L.ccs;
    int grid[72]; for(int i = 0; i < nWeights; ++i) grid[i] = unquantWeight(L.weights[i], kIse[L.weightRange]);
    const int Ds = (1024 + bw / 2) / (bw - 1), Dt = (1024 + bh / 2) / (bh - 1);
    const bool small = texels < 31;
    const int partIndex = L.seed;
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
            const int v = (e[part][0][ch] * (64 - wt) + e[part][1][ch] * wt + 32) >> 6;
            if(!(ch < 3 ? hdrRgb[part] : hdrA[part])) c[ch] = v >> 8;
            else { const float f = halfToFloat(lnsToHalf(v)); c[ch] = ch < 3 ? linearToSrgb8(f) : qBound(0, int(f * 255 + 0.5f), 255); }
        }
        out[t * bw + s] = qRgba(c[0], c[1], c[2], c[3]);
    }
    return true;
}

bool astcBlock(const uchar* p, QRgb* out, int bw, int bh){
    const int texels = bw * bh;
    auto fail = [&]{ for(int i = 0; i < texels; ++i) out[i] = kAstcError; return false; };
    const AstcBits b{ p, false };
    const int mode = b.take(0, 11);
    if((mode & 0x1ff) == 0x1fc){                                               // void extent: one colour for the whole block
        QRgb c;
        if(mode & 0x200){                                                      // an HDR constant colour, in half floats
            auto ch = [&](int at){ return halfToFloat(quint16(b.take(at, 16))); };
            c = qRgba(linearToSrgb8(ch(64)), linearToSrgb8(ch(80)), linearToSrgb8(ch(96)), qBound(0, int(ch(112) * 255 + 0.5f), 255));
        } else c = qRgba(b.take(64 + 8, 8), b.take(80 + 8, 8), b.take(96 + 8, 8), b.take(112 + 8, 8));
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
    AstcLogical L;
    L.parts = parts; for(int i = 0; i < parts; ++i) L.cem[i] = cem[i];
    L.seed = partIndex; L.endRange = cq; L.weightRange = wq; L.W = W; L.H = H; L.dual = dual;
    int used = 0; iseRead(b, colourStart, nValues, kIse[cq], L.values, &used);
    L.ccs = dual ? b.take(128 - weightBits - 2, 2) : -1;
    // Weights: stored from the block's last bit backwards.
    const AstcBits rb{ p, true };
    int wused = 0; iseRead(rb, 0, nWeights, kIse[wq], L.weights, &wused);
    return astcEvaluate(L, out, bw, bh);
}
