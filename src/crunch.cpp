// Crunch (.crn): DXT textures shrunk by vector quantisation and Huffman coding, after crn_decomp.h by Rich
// Geldreich (public domain), and Unity's revision of it (zlib licence) with its ETC formats. Palettes of colour endpoints (two RGB565 colours), colour selectors (2 bits a
// texel), and for alpha, alpha endpoints and 3-bit selectors, each delta coded. The image is walked in chunks of
// 2 x 2 blocks, row by row and back again; each chunk names how its blocks share up to four endpoint "tiles",
// then delta-coded palette indices for its tiles and blocks. The result is plain DXT1/DXT5/DXN/DXT5A blocks,
// which gputex decodes.
#include "textures.h"
#include "gputex.h"
#include <QVector>
#include <cstring>

namespace xara {
namespace textures {
namespace {

// Bits most significant first, as Crunch packs them.
struct Codec {
    const uchar* p = nullptr; const uchar* end = nullptr; quint64 buf = 0; int count = 0;
    int past = 0;                                     // bytes asked for beyond the end
    void start(const uchar* d, qint64 n){ p = d; end = d + qMax<qint64>(0, n); buf = 0; count = 0; past = 0; }
    // Whole bytes left unread: a level decoded the right way ends within a byte or two of its data.
    qint64 left() const { return (end - p) + count / 8; }
    quint32 get(int n){
        if(n <= 0) return 0;
        while(count < n){ quint64 c = 0; if(p < end) c = *p++; else ++past; count += 8; buf |= c << (64 - count); }
        const quint32 v = quint32(buf >> (64 - n)); buf <<= n; count -= n; return v;
    }
    quint32 bits(int n){ return n > 16 ? (get(n - 16) << 16) | get(16) : get(n); }
};

// Canonical Huffman, codes counted up by length and read a bit at a time.
struct Model {
    int counts[17] = {}; QVector<int> symbols; bool valid = false;
    bool build(const QVector<int>& lengths){
        std::fill(counts, counts + 17, 0); symbols.clear(); valid = false;
        int used = 0; for(int l : lengths){ if(l < 0 || l > 16) return false; if(l){ ++counts[l]; ++used; } }
        if(!used) return false;
        int offs[17] = {}; for(int l = 1; l < 16; ++l) offs[l + 1] = offs[l] + counts[l];
        symbols.resize(used); for(int s = 0; s < lengths.size(); ++s) if(lengths[s]) symbols[offs[lengths[s]]++] = s;
        return valid = true;
    }
    int decode(Codec& c) const {
        if(!valid) return -1;
        int code = 0, first = 0, index = 0;
        for(int l = 1; l <= 16; ++l){
            code |= int(c.get(1));
            if(code - first < counts[l]) return symbols[index + code - first];
            index += counts[l]; first += counts[l]; first <<= 1; code <<= 1;
        }
        return -1;
    }
};

bool readModel(Codec& c, Model& m){
    static const int order[21] = {17, 18, 19, 20, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15, 16};
    const int total = int(c.bits(14));                 // 8192 symbols at most
    if(!total){ m = Model(); return true; }
    const int codes = int(c.bits(5)); if(codes < 1 || codes > 21) return false;
    QVector<int> small(21, 0); for(int i = 0; i < codes; ++i) small[order[i]] = int(c.bits(3));
    Model lm; if(!lm.build(small)) return false;
    QVector<int> lengths(total, 0); int ofs = 0;
    while(ofs < total){
        const int s = lm.decode(c); if(s < 0) return false;
        const int left = total - ofs;
        if(s <= 16) lengths[ofs++] = s;
        else if(s == 17 || s == 18){ const int run = s == 17 ? int(c.bits(3)) + 3 : int(c.bits(7)) + 11; if(run > left) return false; ofs += run; }
        else { const int run = s == 19 ? int(c.bits(2)) + 3 : int(c.bits(6)) + 7; if(!ofs || run > left || !lengths[ofs - 1]) return false;
               const int prev = lengths[ofs - 1]; for(int i = 0; i < run; ++i) lengths[ofs++] = prev; }
    }
    return m.build(lengths);
}

quint32 be(const uchar* p, int n){ quint32 v = 0; for(int i = 0; i < n; ++i) v = v << 8 | p[i]; return v; }

struct Header {
    int w = 0, h = 0, levels = 0, format = 0; quint32 dataSize = 0;
    struct Palette { quint32 ofs = 0, size = 0; int num = 0; } cEnd, cSel, aEnd, aSel;
    quint32 tablesOfs = 0, tablesSize = 0, levelOfs = 0, levelEnd = 0;
};
struct Tables { Model ref, endDm[2], selDm[2]; };

// The blocks of the biggest level as the original Crunch codes them: chunks of 2 x 2 blocks, walked back and forth.
bool classicLevel(const uchar* p, const Header& hd, const Tables& t, QByteArray& blocks, Codec& c){
    const int format = hd.format;
    QVector<quint32> colourEnds, colourSels; QVector<quint16> alphaEnds, alphaSels;
    if(hd.cEnd.num){
        c.start(p + hd.cEnd.ofs, hd.cEnd.size); Model dm[2];
        if(!readModel(c, dm[0]) || !readModel(c, dm[1])) return false;
        quint32 a = 0, b = 0, cc = 0, d = 0, e = 0, f = 0;
        for(int i = 0; i < hd.cEnd.num; ++i){
            const int da = dm[0].decode(c), db = dm[1].decode(c), dc = dm[0].decode(c), dd = dm[0].decode(c), de = dm[1].decode(c), df = dm[0].decode(c);
            if((da | db | dc | dd | de | df) < 0) return false;
            a = (a + da) & 31; b = (b + db) & 63; cc = (cc + dc) & 31; d = (d + dd) & 31; e = (e + de) & 63; f = (f + df) & 31;
            colourEnds << (cc | b << 5 | a << 11 | f << 16 | e << 21 | d << 27);
        }
        c.start(p + hd.cSel.ofs, hd.cSel.size); Model sm;
        if(!readModel(c, sm)) return false;
        static const int fromLinear[4] = {0, 2, 3, 1};
        int cur[16] = {};
        for(int i = 0; i < hd.cSel.num; ++i){
            for(int j = 0; j < 8; ++j){ const int s = sm.decode(c); if(s < 0 || s >= 49) return false; cur[j * 2] = (cur[j * 2] + s % 7 - 3) & 3; cur[j * 2 + 1] = (cur[j * 2 + 1] + s / 7 - 3) & 3; }
            quint32 v = 0; for(int k = 0; k < 16; ++k) v |= quint32(fromLinear[cur[k]]) << (2 * k);
            colourSels << v;
        }
    }
    if(hd.aEnd.num){
        c.start(p + hd.aEnd.ofs, hd.aEnd.size); Model dm;
        if(!readModel(c, dm)) return false;
        quint32 a = 0, b = 0;
        for(int i = 0; i < hd.aEnd.num; ++i){ const int sa = dm.decode(c), sb = dm.decode(c); if(sa < 0 || sb < 0) return false; a = (a + sa) & 255; b = (b + sb) & 255; alphaEnds << quint16(a | b << 8); }
        c.start(p + hd.aSel.ofs, hd.aSel.size); Model sm;
        if(!readModel(c, sm)) return false;
        static const int fromLinear[8] = {0, 2, 3, 4, 5, 6, 7, 1};
        int cur[16] = {};
        for(int i = 0; i < hd.aSel.num; ++i){
            for(int j = 0; j < 8; ++j){ const int s = sm.decode(c); if(s < 0 || s >= 225) return false; cur[j * 2] = (cur[j * 2] + s % 15 - 7) & 7; cur[j * 2 + 1] = (cur[j * 2 + 1] + s / 15 - 7) & 7; }
            quint64 v = 0; for(int k = 0; k < 16; ++k) v |= quint64(fromLinear[cur[k]]) << (3 * k);
            alphaSels << quint16(v) << quint16(v >> 16) << quint16(v >> 32);
        }
    }
    c.start(p + hd.levelOfs, hd.levelEnd - hd.levelOfs);
    const int bx = qMax(1, (hd.w + 3) / 4), by = qMax(1, (hd.h + 3) / 4), cx = (bx + 1) / 2, cy = (by + 1) / 2;
    const bool dxt1 = format == 0, dxt5a = format == 9, dxn = format == 7 || format == 8;
    if((dxt1 || !dxt5a) && !dxn && (colourEnds.isEmpty() || colourSels.isEmpty()) ) return false;
    if(!dxt1 && (alphaEnds.isEmpty() || alphaSels.isEmpty())) return false;
    const int blockBytes = (dxt1 || dxt5a) ? 8 : 16;
    blocks = QByteArray(int(qint64(bx) * by * blockBytes), '\0');
    uchar* out = reinterpret_cast<uchar*>(blocks.data());
    static const int tiles[8][4] = { {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 1, 2}, {1, 2, 0, 0}, {0, 1, 0, 2}, {1, 0, 2, 0}, {0, 1, 2, 3} };
    static const int tileCount[8] = {1, 2, 2, 3, 3, 3, 3, 4};
    auto put32 = [](uchar* d, quint32 v){ d[0] = uchar(v); d[1] = uchar(v >> 8); d[2] = uchar(v >> 16); d[3] = uchar(v >> 24); };
    auto putAlpha = [&](uchar* d, quint16 ends, int sel){ d[0] = uchar(ends); d[1] = uchar(ends >> 8); for(int k = 0; k < 3; ++k){ const quint16 s = alphaSels[sel * 3 + k]; d[2 + 2 * k] = uchar(s); d[3 + 2 * k] = uchar(s >> 8); } };
    auto step = [](quint32& index, int delta, int size){ index += quint32(delta); if(int(index) >= size) index -= quint32(size); };
    quint32 chunkBits = 1, prevCE = 0, prevCS = 0, prevAE = 0, prevAS = 0, prevAE1 = 0, prevAS1 = 0;
    const int nCE = colourEnds.size(), nCS = colourSels.size(), nAE = alphaEnds.size(), nAS = hd.aSel.num;
    for(int y = 0; y < cy; ++y){
        const bool skipBottom = y == cy - 1 && (by & 1);
        for(int k = 0; k < cx; ++k){
            const int x = (y & 1) ? cx - 1 - k : k;
            if(chunkBits == 1){ const int s = t.ref.decode(c); if(s < 0) return false; chunkBits = quint32(s) | 512; }
            const int enc = int(chunkBits & 7); chunkBits >>= 3;
            const bool skipRight = (bx & 1) && x == cx - 1;
            quint32 ce[4] = {}, ae[4] = {}, ae1[4] = {};
            if(!dxt1){
                for(int i = 0; i < tileCount[enc]; ++i){ const int d = t.endDm[1].decode(c); if(d < 0) return false; step(prevAE, d, nAE); ae[i] = alphaEnds[int(prevAE)]; }
                if(dxn) for(int i = 0; i < tileCount[enc]; ++i){ const int d = t.endDm[1].decode(c); if(d < 0) return false; step(prevAE1, d, nAE); ae1[i] = alphaEnds[int(prevAE1)]; }
            }
            if(!dxn && !dxt5a) for(int i = 0; i < tileCount[enc]; ++i){ const int d = t.endDm[0].decode(c); if(d < 0) return false; step(prevCE, d, nCE); ce[i] = colourEnds[int(prevCE)]; }
            for(int yy = 0; yy < 2; ++yy) for(int xx = 0; xx < 2; ++xx){
                int as = 0, as1 = 0, cs = 0;
                if(!dxt1){ const int d = t.selDm[1].decode(c); if(d < 0) return false; step(prevAS, d, nAS); as = int(prevAS); }
                if(dxn){ const int d = t.selDm[1].decode(c); if(d < 0) return false; step(prevAS1, d, nAS); as1 = int(prevAS1); }
                if(!dxn && !dxt5a){ const int d = t.selDm[0].decode(c); if(d < 0) return false; step(prevCS, d, nCS); cs = int(prevCS); }
                if((xx && skipRight) || (yy && skipBottom)) continue;
                const int tt = tiles[enc][xx + yy * 2];
                uchar* blk = out + (qint64(y * 2 + yy) * bx + (x * 2 + xx)) * blockBytes;
                if(dxt1){ put32(blk, ce[tt]); put32(blk + 4, colourSels[cs]); }
                else if(dxt5a) putAlpha(blk, quint16(ae[tt]), as);
                else if(dxn){ putAlpha(blk, quint16(ae[tt]), as); putAlpha(blk + 8, quint16(ae1[tt]), as1); }
                else { putAlpha(blk, quint16(ae[tt]), as); put32(blk + 8, ce[tt]); put32(blk + 12, colourSels[cs]); }
            }
        }
    }
    return true;
}

// The blocks as Unity's Crunch codes them (its DXT and ETC formats): blocks row by row, each pair of rows sharing
// endpoint references, palette indices sent directly for selectors, and selectors coded as XORed bit planes.
bool unityLevel(const uchar* p, const Header& hd, const Tables& t, QByteArray& blocks, Codec& c){
    const int format = hd.format;
    const bool etc = format >= 10, subblocks = format >= 10 && format <= 12;
    QVector<quint32> colourEnds, colourSels; QVector<quint16> alphaEnds; QByteArray alphaSels;
    if(hd.cEnd.num){
        c.start(p + hd.cEnd.ofs, hd.cEnd.size); Model dm[2];
        if(!readModel(c, dm[0]) || (!etc && !readModel(c, dm[1]))) return false;
        quint32 a = 0, b = 0, cc = 0, d = 0, e = 0, f = 0;
        for(int i = 0; i < hd.cEnd.num; ++i){
            if(etc){
                for(int k = 0; k < 32; k += 8){ const int v = dm[0].decode(c); if(v < 0) return false; a += quint32(v) << k; }
                a &= 0x1F1F1F1F;
                // ETC1S keeps one colour and intensity: written straight out as a differential block with no delta.
                colourEnds << (subblocks ? a : (a & 0x07000000) << 5 | (a & 0x07000000) << 2 | 0x02000000 | (a & 0x001F1F1F) << 3);
            } else {
                const int da = dm[0].decode(c), db = dm[1].decode(c), dc = dm[0].decode(c), dd = dm[0].decode(c), de = dm[1].decode(c), df = dm[0].decode(c);
                if((da | db | dc | dd | de | df) < 0) return false;
                a = (a + da) & 31; b = (b + db) & 63; cc = (cc + dc) & 31; d = (d + dd) & 31; e = (e + de) & 63; f = (f + df) & 31;
                colourEnds << (cc | b << 5 | a << 11 | f << 16 | e << 21 | d << 27);
            }
        }
        c.start(p + hd.cSel.ofs, hd.cSel.size); Model sm;
        if(!readModel(c, sm)) return false;
        colourSels.fill(0, hd.cSel.num << (subblocks ? 1 : 0));
        quint32 s = 0;
        for(int i = 0; i < hd.cSel.num; ++i){
            for(int j = 0; j < 32; j += 4){ const int v = sm.decode(c); if(v < 0) return false; s ^= quint32(v) << j; }
            if(!etc){ colourSels[i] = ((s ^ s << 1) & 0xAAAAAAAA) | (s >> 1 & 0x55555555); continue; }
            // ETC keeps each texel's index as two bit planes, column by column; a block split side by side and one
            // split top and bottom see the same selectors transposed.
            const quint32 sel = (~s & 0xAAAAAAAA) | (~(s ^ s >> 1) & 0x55555555);
            quint32 tt = 8;
            for(int hh = 0; hh < 4; ++hh, tt -= 15) for(int ww = 0; ww < 4; ++ww, tt += 4){
                if(subblocks){ const quint32 s0 = sel >> (ww << 3 | hh << 1); colourSels[i << 1] |= ((s0 >> 1 & 1) | (s0 & 1) << 16) << (tt & 15); }
                const quint32 s1 = sel >> (hh << 3 | ww << 1);
                colourSels[subblocks ? i << 1 | 1 : i] |= ((s1 >> 1 & 1) | (s1 & 1) << 16) << (tt & 15);
            }
        }
    }
    if(hd.aEnd.num){
        c.start(p + hd.aEnd.ofs, hd.aEnd.size); Model dm;
        if(!readModel(c, dm)) return false;
        quint32 a = 0, b = 0;
        for(int i = 0; i < hd.aEnd.num; ++i){ const int sa = dm.decode(c), sb = dm.decode(c); if(sa < 0 || sb < 0) return false; a = (a + sa) & 255; b = (b + sb) & 255; alphaEnds << quint16(a | b << 8); }
        c.start(p + hd.aSel.ofs, hd.aSel.size); Model sm;
        if(!readModel(c, sm)) return false;
        if(!etc){
            // DXT5 alpha: 3-bit indices, two texels a symbol, XORed into 24-bit halves.
            static const int fromLinear[8] = {0, 2, 3, 4, 5, 6, 7, 1};
            alphaSels.fill('\0', hd.aSel.num * 6);
            quint32 lin[2] = {0, 0};
            for(int i = 0; i < hd.aSel.num; ++i){
                quint32 half[2] = {0, 0};
                for(int k = 0; k < 2; ++k) for(int j = 0; j < 24; j += 6){
                    const int v = sm.decode(c); if(v < 0) return false; lin[k] ^= quint32(v) << j;
                    const quint32 g = lin[k] >> j & 0x3F; half[k] |= quint32(fromLinear[g & 7] | fromLinear[g >> 3] << 3) << j;
                }
                uchar* d = reinterpret_cast<uchar*>(alphaSels.data()) + i * 6;
                for(int k = 0; k < 3; ++k){ d[k] = uchar(half[0] >> (8 * k)); d[3 + k] = uchar(half[1] >> (8 * k)); }
            }
        } else {
            // EAC alpha: 3-bit indices most significant first, column by column; ETC2A keeps a transposed copy too.
            const int per = format == 12 ? 12 : 6;
            alphaSels.fill('\0', hd.aSel.num * per);
            uchar lin[8] = {};
            for(int i = 0; i < hd.aSel.num; ++i){
                uchar* d = reinterpret_cast<uchar*>(alphaSels.data()) + i * per;
                quint32 group = 0;
                for(int px = 0; px < 16; ++px){
                    if(px & 1) group >>= 3; else { const int v = sm.decode(c); if(v < 0) return false; lin[px >> 1] ^= uchar(v); group = lin[px >> 1]; }
                    uchar sv = uchar(group & 7); if(sv <= 3) sv = uchar(3 - sv);
                    auto place = [&](uchar* base, int bitPos){ const int byteOfs = bitPos >> 3, bitOfs = bitPos & 7;
                        base[byteOfs] = uchar(base[byteOfs] | (sv << (8 - bitOfs))); if(bitOfs < 3) base[byteOfs - 1] = uchar(base[byteOfs - 1] | (sv >> bitOfs)); };
                    const int straight = 3 * (px + 1), turned = straight + 9 * ((px & 3) - (px >> 2));
                    if(format == 12){ place(d, straight); place(d + 6, turned); } else place(d, turned);
                }
            }
        }
    }
    c.start(p + hd.levelOfs, hd.levelEnd - hd.levelOfs);
    const int bx = qMax(1, (hd.w + 3) / 4), by = qMax(1, (hd.h + 3) / 4), width = (bx + 1) & ~1, height = (by + 1) & ~1;
    const bool hasColour = format != 7 && format != 8 && format != 9, alpha0 = format != 0 && format != 10 && format != 11 && format != 13, alpha1 = format == 7 || format == 8;
    if(hasColour && (colourEnds.isEmpty() || colourSels.isEmpty())) return false;
    if(alpha0 && (alphaEnds.isEmpty() || alphaSels.isEmpty())) return false;
    const int blockBytes = (hasColour && alpha0) || alpha1 ? 16 : 8;
    blocks = QByteArray(int(qint64(bx) * by * blockBytes), '\0');
    uchar* out = reinterpret_cast<uchar*>(blocks.data());
    const int nCE = colourEnds.size(), nAE = alphaEnds.size(), nCS = hd.cSel.num, nAS = hd.aSel.num;
    auto put32 = [](uchar* d, quint32 v){ d[0] = uchar(v); d[1] = uchar(v >> 8); d[2] = uchar(v >> 16); d[3] = uchar(v >> 24); };
    auto step = [&](quint32& index, const Model& m, int size){ const int d = m.decode(c); if(d < 0) return false; index += quint32(d); if(index >= quint32(size)) index -= quint32(size); return index < quint32(size); };
    auto pick = [&](const Model& m, int size, int& v){ v = m.decode(c); return v >= 0 && v < size; };
    auto putAlpha = [&](uchar* d, int end, int sel, int variant){ d[0] = uchar(alphaEnds[end]); d[1] = uchar(alphaEnds[end] >> 8);
        std::memcpy(d + 2, alphaSels.constData() + sel * (format == 12 ? 12 : 6) + variant * 6, 6); };
    struct Buf { int ref = 0; quint32 ce = 0, a0 = 0, a1 = 0; };
    QVector<Buf> buf(width * 2);
    quint32 ce = 0, a0 = 0, a1 = 0, diagCe = 0, diagA0 = 0; int group = 0;
    for(int y = 0; y < height; ++y) for(int x = 0; x < width; ++x){
        const bool visible = y < by && x < bx;
        uchar* blk = visible ? out + (qint64(y) * bx + x) * blockBytes : nullptr;
        if(!etc || format == 13 || format == 14){
            // The DXT layout (ETC1S and ETC2AS too): each pair of rows shares two-bit endpoint references.
            Buf& b = buf[x];
            if(!(y & 1) && !(x & 1)){ group = t.ref.decode(c); if(group < 0) return false; }
            int ref;
            if(y & 1) ref = b.ref; else { ref = group & 3; group >>= 2; b.ref = group & 3; group >>= 2; }
            if(ref == 0){
                if(hasColour && !step(ce, t.endDm[0], nCE)) return false;
                if(alpha0 && !step(a0, t.endDm[1], nAE)) return false;
                if(alpha1 && !step(a1, t.endDm[1], nAE)) return false;
                b.ce = ce; b.a0 = a0; b.a1 = a1;
            } else if(ref == 1){ b.ce = ce; b.a0 = a0; b.a1 = a1; }
            else { ce = b.ce; a0 = b.a0; a1 = b.a1; }
            if(ce >= quint32(qMax(1, nCE)) || a0 >= quint32(qMax(1, nAE)) || a1 >= quint32(qMax(1, nAE))) return false;
            int cs = 0, as0 = 0, as1 = 0;
            if(hasColour && !pick(t.selDm[0], nCS, cs)) return false;
            if(alpha0 && !pick(t.selDm[1], nAS, as0)) return false;
            if(alpha1 && !pick(t.selDm[1], nAS, as1)) return false;
            if(!blk) continue;
            if(alpha0) putAlpha(blk, int(a0), as0, 0);
            if(alpha1) putAlpha(blk + 8, int(a1), as1, 0);
            if(hasColour){ uchar* cb = blk + (alpha0 ? 8 : 0); put32(cb, colourEnds[int(ce)]); put32(cb + 4, colourSels[cs]); }
        } else {
            // ETC1 / ETC2 / ETC2A: each block's two halves can take their own endpoints, and a block can borrow the
            // one diagonally above.
            Buf& b = buf[x << 1];
            int ref;
            if(y & 1) ref = b.ref; else { group = t.ref.decode(c); if(group < 0) return false; ref = (group & 3) | (group >> 2 & 12); b.ref = (group >> 2 & 3) | (group >> 4 & 12); }
            const bool withAlpha = format == 12;
            switch(ref & 3){
            case 0: if(!step(ce, t.endDm[0], nCE)) return false; if(withAlpha && !step(a0, t.endDm[1], nAE)) return false; b.ce = ce; b.a0 = a0; break;
            case 1: b.ce = ce; b.a0 = a0; break;
            case 3: ce = diagCe; a0 = diagA0; b.ce = ce; b.a0 = a0; break;
            default: ce = b.ce; a0 = b.a0; break;
            }
            ref >>= 2;
            if(ce >= quint32(nCE) || (withAlpha && a0 >= quint32(nAE))) return false;
            const quint32 e0 = colourEnds[int(ce)];
            int cs = 0, as0 = 0;
            if(!pick(t.selDm[0], nCS, cs)) return false;
            if(withAlpha && !pick(t.selDm[1], nAS, as0)) return false;
            if(ref && !step(ce, t.endDm[0], nCE)) return false;
            const quint32 e1 = colourEnds[int(ce)];
            diagCe = buf[x << 1 | 1].ce; diagA0 = buf[x << 1 | 1].a0;
            buf[x << 1 | 1].ce = ce; buf[x << 1 | 1].a0 = a0;
            if(!blk) continue;
            const int flip = (ref >> 1) ^ 1; bool diff = true;
            auto ch = [](quint32 v, int c){ return int(v >> (8 * c) & 255); };
            for(int k = 0; diff && k < 3; ++k) diff = ch(e0, k) + 3 >= ch(e1, k) && ch(e1, k) + 4 >= ch(e0, k);
            uchar* cb = blk + (withAlpha ? 8 : 0);
            for(int k = 0; k < 3; ++k) cb[k] = uchar(diff ? (ch(e0, k) << 3 | ((ch(e1, k) - ch(e0, k)) & 7)) : ((ch(e0, k) << 3 & 0xF0) | ch(e1, k) >> 1));
            cb[3] = uchar(ch(e0, 3) << 5 | ch(e1, 3) << 2 | int(diff) << 1 | flip);
            put32(cb + 4, colourSels[cs << 1 | flip]);
            if(withAlpha) putAlpha(blk, int(a0), as0, flip);
        }
    }
    return true;
}

} // namespace

QImage decodeCrn(const uchar* p, qint64 n, QString* note){
    if(n < 74 || be(p, 2) != (('H' << 8) | 'x')) return {};
    Header hd;
    const int headerSize = int(be(p + 2, 2)); hd.dataSize = be(p + 6, 4);
    hd.w = int(be(p + 12, 2)); hd.h = int(be(p + 14, 2)); hd.levels = p[16]; hd.format = p[18];
    const int w = hd.w, h = hd.h, format = hd.format;
    if(w < 1 || h < 1 || hd.levels < 1 || headerSize < 62 + 4 * hd.levels || headerSize > n || qint64(w) * h > (qint64(1) << 28)) return {};
    if(hd.dataSize > quint64(n)) return {};
    auto palette = [&](int at){ return Header::Palette{be(p + at, 3), be(p + at + 3, 3), int(be(p + at + 6, 2))}; };
    hd.cEnd = palette(33); hd.cSel = palette(41); hd.aEnd = palette(49); hd.aSel = palette(57);
    hd.tablesSize = be(p + 65, 2); hd.tablesOfs = be(p + 67, 3);
    auto inside = [&](quint32 o, quint32 s){ return quint64(o) + s <= quint64(hd.dataSize); };
    if(!inside(hd.tablesOfs, hd.tablesSize) || !inside(hd.cEnd.ofs, hd.cEnd.size) || !inside(hd.cSel.ofs, hd.cSel.size) || !inside(hd.aEnd.ofs, hd.aEnd.size) || !inside(hd.aSel.ofs, hd.aSel.size)) return {};
    if(format == 1 || format > 14){ if(note) *note = QStringLiteral("a Crunch format XarQ doesn't read"); return {}; }
    if(hd.cEnd.num > 65535 || hd.cSel.num > 65535 || hd.aEnd.num > 65535 || hd.aSel.num > 65535) return {};
    // The tables, shared by both codings.
    Tables t; Codec c; c.start(p + hd.tablesOfs, hd.tablesSize);
    if(!readModel(c, t.ref)) return {};
    if(!hd.cEnd.num && !hd.aEnd.num) return {};
    if(hd.cEnd.num && (!readModel(c, t.endDm[0]) || !readModel(c, t.selDm[0]))) return {};
    if(hd.aEnd.num && (!readModel(c, t.endDm[1]) || !readModel(c, t.selDm[1]))) return {};
    hd.levelOfs = be(p + 70, 4); hd.levelEnd = hd.levels > 1 ? be(p + 74, 4) : hd.dataSize;
    if(hd.levelOfs >= hd.levelEnd || hd.levelEnd > hd.dataSize) return {};
    // DXT files don't say which Crunch wrote them: the right decoding ends where the level's data ends.
    QByteArray blocks; bool unity = format >= 10;
    if(!unity){
        Codec cc; QByteArray classic;
        const bool ok = classicLevel(p, hd, t, classic, cc);
        if(ok && !cc.past && cc.left() <= 2) blocks = classic;
        else {
            Codec cu; QByteArray u;
            if(unityLevel(p, hd, t, u, cu) && !cu.past && cu.left() <= 2){ blocks = u; unity = true; }
            else if(ok) blocks = classic;
        }
    } else { Codec cu; if(!unityLevel(p, hd, t, blocks, cu)) blocks.clear(); }
    if(blocks.isEmpty()) return {};
    const int bx = qMax(1, (w + 3) / 4), by = qMax(1, (h + 3) / 4);
    const uchar* out = reinterpret_cast<const uchar*>(blocks.constData());
    using F = gputex::Format;
    const bool dxt1 = format == 0, dxt5a = format == 9, dxn = format == 7 || format == 8;
    const F f = format == 10 || format == 13 ? F::ETC1 : format == 11 ? F::ETC2_RGB : format == 12 || format == 14 ? F::ETC2_RGBA8
              : dxt1 ? F::BC1A : dxt5a ? F::BC4 : dxn ? F::BC5 : F::BC3;
    QImage img = gputex::decode(f, out, blocks.size(), bx * 4, by * 4);
    if(img.isNull()) return img;
    img = img.copy(0, 0, w, h);
    // Swizzled and two-channel variants back to colours.
    if(format == 8 || format == 4 || format == 5 || format == 6 || dxt5a){
        for(int yy = 0; yy < img.height(); ++yy){
            QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(yy));
            for(int xx = 0; xx < img.width(); ++xx){
                const QRgb v = line[xx];
                if(format == 8) line[xx] = qRgb(qGreen(v), qRed(v), 0);                      // DXN Y then X
                else if(format == 4) line[xx] = qRgb(qAlpha(v), qGreen(v), 0);               // xGxR: red kept in alpha
                else if(format == 5) line[xx] = qRgb(qAlpha(v), qGreen(v), qBlue(v));        // xGBR
                else if(format == 6) line[xx] = qRgba(qAlpha(v), qGreen(v), qBlue(v), qRed(v));   // AGBR: red and alpha swapped
                else line[xx] = qRgb(qRed(v), qRed(v), qRed(v));                               // DXT5A: one channel, as grey
            }
        }
    }
    return img;
}

} // namespace textures
} // namespace xara
