// Crunch (.crn): DXT textures shrunk by vector quantisation and Huffman coding, after crn_decomp.h by Rich
// Geldreich (public domain). Palettes of colour endpoints (two RGB565 colours), colour selectors (2 bits a
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
    void start(const uchar* d, qint64 n){ p = d; end = d + qMax<qint64>(0, n); buf = 0; count = 0; }
    quint32 get(int n){
        if(n <= 0) return 0;
        while(count < n){ const quint64 c = p < end ? *p++ : 0; count += 8; buf |= c << (64 - count); }
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

} // namespace

QImage decodeCrn(const uchar* p, qint64 n, QString* note){
    if(n < 74 || be(p, 2) != (('H' << 8) | 'x')) return {};
    const int headerSize = int(be(p + 2, 2)); const quint32 dataSize = be(p + 6, 4);
    const int w = int(be(p + 12, 2)), h = int(be(p + 14, 2)), levels = p[16], format = p[18];
    if(w < 1 || h < 1 || levels < 1 || headerSize < 62 + 4 * levels || headerSize > n || qint64(w) * h > (qint64(1) << 28)) return {};
    if(dataSize > quint64(n)) return {};
    struct Palette { quint32 ofs, size; int num; };
    auto palette = [&](int at){ return Palette{be(p + at, 3), be(p + at + 3, 3), int(be(p + at + 6, 2))}; };
    const Palette cEnd = palette(33), cSel = palette(41), aEnd = palette(49), aSel = palette(57);
    const quint32 tablesSize = be(p + 65, 2), tablesOfs = be(p + 67, 3);
    auto inside = [&](quint32 o, quint32 s){ return quint64(o) + s <= quint64(dataSize); };
    if(!inside(tablesOfs, tablesSize) || !inside(cEnd.ofs, cEnd.size) || !inside(cSel.ofs, cSel.size) || !inside(aEnd.ofs, aEnd.size) || !inside(aSel.ofs, aSel.size)) return {};
    if(format == 10){ if(note) *note = QStringLiteral("ETC1 Crunch data"); return {}; }
    if(format == 1 || format > 10){ if(note) *note = QStringLiteral("a Crunch format XarQ doesn't read"); return {}; }
    // The tables, then the palettes.
    Codec c; c.start(p + tablesOfs, tablesSize);
    Model chunkDm, endDm[2], selDm[2];
    if(!readModel(c, chunkDm)) return {};
    if(!cEnd.num && !aEnd.num) return {};
    if(cEnd.num && (!readModel(c, endDm[0]) || !readModel(c, selDm[0]))) return {};
    if(aEnd.num && (!readModel(c, endDm[1]) || !readModel(c, selDm[1]))) return {};
    QVector<quint32> colourEnds, colourSels; QVector<quint16> alphaEnds, alphaSels;
    if(cEnd.num){
        c.start(p + cEnd.ofs, cEnd.size); Model dm[2];
        if(!readModel(c, dm[0]) || !readModel(c, dm[1])) return {};
        quint32 a = 0, b = 0, cc = 0, d = 0, e = 0, f = 0;
        for(int i = 0; i < cEnd.num; ++i){
            const int da = dm[0].decode(c), db = dm[1].decode(c), dc = dm[0].decode(c), dd = dm[0].decode(c), de = dm[1].decode(c), df = dm[0].decode(c);
            if((da | db | dc | dd | de | df) < 0) return {};
            a = (a + da) & 31; b = (b + db) & 63; cc = (cc + dc) & 31; d = (d + dd) & 31; e = (e + de) & 63; f = (f + df) & 31;
            colourEnds << (cc | b << 5 | a << 11 | f << 16 | e << 21 | d << 27);
        }
        c.start(p + cSel.ofs, cSel.size); Model sm;
        if(!readModel(c, sm)) return {};
        static const int fromLinear[4] = {0, 2, 3, 1};
        int cur[16] = {};
        for(int i = 0; i < cSel.num; ++i){
            for(int j = 0; j < 8; ++j){ const int s = sm.decode(c); if(s < 0 || s >= 49) return {}; cur[j * 2] = (cur[j * 2] + s % 7 - 3) & 3; cur[j * 2 + 1] = (cur[j * 2 + 1] + s / 7 - 3) & 3; }
            quint32 v = 0; for(int k = 0; k < 16; ++k) v |= quint32(fromLinear[cur[k]]) << (2 * k);
            colourSels << v;
        }
    }
    if(aEnd.num){
        c.start(p + aEnd.ofs, aEnd.size); Model dm;
        if(!readModel(c, dm)) return {};
        quint32 a = 0, b = 0;
        for(int i = 0; i < aEnd.num; ++i){ const int sa = dm.decode(c), sb = dm.decode(c); if(sa < 0 || sb < 0) return {}; a = (a + sa) & 255; b = (b + sb) & 255; alphaEnds << quint16(a | b << 8); }
        c.start(p + aSel.ofs, aSel.size); Model sm;
        if(!readModel(c, sm)) return {};
        static const int fromLinear[8] = {0, 2, 3, 4, 5, 6, 7, 1};
        int cur[16] = {};
        for(int i = 0; i < aSel.num; ++i){
            for(int j = 0; j < 8; ++j){ const int s = sm.decode(c); if(s < 0 || s >= 225) return {}; cur[j * 2] = (cur[j * 2] + s % 15 - 7) & 7; cur[j * 2 + 1] = (cur[j * 2 + 1] + s / 15 - 7) & 7; }
            quint64 v = 0; for(int k = 0; k < 16; ++k) v |= quint64(fromLinear[cur[k]]) << (3 * k);
            alphaSels << quint16(v) << quint16(v >> 16) << quint16(v >> 32);
        }
    }
    // The biggest level, chunk by chunk, into plain DXT blocks.
    const quint32 levelOfs = be(p + 70, 4), levelEnd = levels > 1 ? be(p + 74, 4) : dataSize;
    if(levelOfs >= levelEnd || levelEnd > dataSize) return {};
    c.start(p + levelOfs, levelEnd - levelOfs);
    const int bx = qMax(1, (w + 3) / 4), by = qMax(1, (h + 3) / 4), cx = (bx + 1) / 2, cy = (by + 1) / 2;
    const bool dxt1 = format == 0, dxt5a = format == 9, dxn = format == 7 || format == 8;
    if((dxt1 || !dxt5a) && !dxn && (colourEnds.isEmpty() || colourSels.isEmpty()) ) return {};
    if(!dxt1 && (alphaEnds.isEmpty() || alphaSels.isEmpty())) return {};
    const int blockBytes = (dxt1 || dxt5a) ? 8 : 16;
    QByteArray blocks(int(qint64(bx) * by * blockBytes), '\0');
    uchar* out = reinterpret_cast<uchar*>(blocks.data());
    static const int tiles[8][4] = { {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 1, 2}, {1, 2, 0, 0}, {0, 1, 0, 2}, {1, 0, 2, 0}, {0, 1, 2, 3} };
    static const int tileCount[8] = {1, 2, 2, 3, 3, 3, 3, 4};
    auto put32 = [](uchar* d, quint32 v){ d[0] = uchar(v); d[1] = uchar(v >> 8); d[2] = uchar(v >> 16); d[3] = uchar(v >> 24); };
    auto putAlpha = [&](uchar* d, quint16 ends, int sel){ d[0] = uchar(ends); d[1] = uchar(ends >> 8); for(int k = 0; k < 3; ++k){ const quint16 s = alphaSels[sel * 3 + k]; d[2 + 2 * k] = uchar(s); d[3 + 2 * k] = uchar(s >> 8); } };
    auto step = [](quint32& index, int delta, int size){ index += quint32(delta); if(int(index) >= size) index -= quint32(size); };
    quint32 chunkBits = 1, prevCE = 0, prevCS = 0, prevAE = 0, prevAS = 0, prevAE1 = 0, prevAS1 = 0;
    const int nCE = colourEnds.size(), nCS = colourSels.size(), nAE = alphaEnds.size(), nAS = aSel.num;
    for(int y = 0; y < cy; ++y){
        const bool skipBottom = y == cy - 1 && (by & 1);
        for(int k = 0; k < cx; ++k){
            const int x = (y & 1) ? cx - 1 - k : k;
            if(chunkBits == 1){ const int s = chunkDm.decode(c); if(s < 0) return {}; chunkBits = quint32(s) | 512; }
            const int enc = int(chunkBits & 7); chunkBits >>= 3;
            const bool skipRight = (bx & 1) && x == cx - 1;
            quint32 ce[4] = {}, ae[4] = {}, ae1[4] = {};
            if(!dxt1){
                for(int i = 0; i < tileCount[enc]; ++i){ const int d = endDm[1].decode(c); if(d < 0) return {}; step(prevAE, d, nAE); ae[i] = alphaEnds[int(prevAE)]; }
                if(dxn) for(int i = 0; i < tileCount[enc]; ++i){ const int d = endDm[1].decode(c); if(d < 0) return {}; step(prevAE1, d, nAE); ae1[i] = alphaEnds[int(prevAE1)]; }
            }
            if(!dxn && !dxt5a) for(int i = 0; i < tileCount[enc]; ++i){ const int d = endDm[0].decode(c); if(d < 0) return {}; step(prevCE, d, nCE); ce[i] = colourEnds[int(prevCE)]; }
            for(int yy = 0; yy < 2; ++yy) for(int xx = 0; xx < 2; ++xx){
                int as = 0, as1 = 0, cs = 0;
                if(!dxt1){ const int d = selDm[1].decode(c); if(d < 0) return {}; step(prevAS, d, nAS); as = int(prevAS); }
                if(dxn){ const int d = selDm[1].decode(c); if(d < 0) return {}; step(prevAS1, d, nAS); as1 = int(prevAS1); }
                if(!dxn && !dxt5a){ const int d = selDm[0].decode(c); if(d < 0) return {}; step(prevCS, d, nCS); cs = int(prevCS); }
                if((xx && skipRight) || (yy && skipBottom)) continue;
                const int t = tiles[enc][xx + yy * 2];
                uchar* blk = out + (qint64(y * 2 + yy) * bx + (x * 2 + xx)) * blockBytes;
                if(dxt1){ put32(blk, ce[t]); put32(blk + 4, colourSels[cs]); }
                else if(dxt5a) putAlpha(blk, quint16(ae[t]), as);
                else if(dxn){ putAlpha(blk, quint16(ae[t]), as); putAlpha(blk + 8, quint16(ae1[t]), as1); }
                else { putAlpha(blk, quint16(ae[t]), as); put32(blk + 8, ce[t]); put32(blk + 12, colourSels[cs]); }
            }
        }
    }
    using F = gputex::Format;
    const F f = dxt1 ? F::BC1A : dxt5a ? F::BC4 : dxn ? F::BC5 : F::BC3;
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
