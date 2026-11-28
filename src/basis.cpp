// Basis Universal: ETC1S data supercompressed with BasisLZ, in .basis files and in KTX 2.0 (scheme 1), after
// the BasisLZ/ETC1S bitstream specification of KTX 2.0 (Khronos): global codebooks of endpoints (a 5-bit colour
// and an intensity table) and selectors (2 bits a texel), Huffman tables whose code lengths are themselves
// run-length and Huffman coded as Deflate's are, and per-slice block indices predicted from the left, the
// block above or the one up-left, with a move-to-front history of selectors and runs. Video frames may reuse
// the previous frame's blocks. Alpha is a second slice. .basis files may also hold UASTC blocks, which gputex
// decodes.
#include "textures.h"
#include "gputex.h"
#include <QVector>
#include <cstring>

namespace xara {
namespace textures {
namespace {

struct Bits {
    const uchar* p; qint64 n; qint64 bit = 0;
    Bits(const uchar* d, qint64 len) : p(d), n(len) {}
    bool over() const { return bit > n * 8; }
    quint32 get(int count){
        quint32 v = 0;
        for(int i = 0; i < count; ++i, ++bit){ const qint64 b = bit >> 3; if(b < n) v |= quint32((p[b] >> (bit & 7)) & 1) << i; }
        return v;
    }
    quint32 vlc(int chunk){                                                     // groups of chunk bits, the top one saying "more"
        quint32 v = 0; int ofs = 0;
        for(;;){ const quint32 s = get(chunk); v |= (s & ((1u << (chunk - 1)) - 1)) << ofs; ofs += chunk - 1; if(!(s >> (chunk - 1)) || ofs >= 32) break; }
        return v;
    }
};

// A canonical Huffman code, Deflate's way: codes counted up by length, read a bit at a time.
struct Huffman {
    int counts[17] = {}; QVector<int> symbols; bool valid = false;
    void build(const QVector<int>& lengths){
        std::fill(counts, counts + 17, 0); symbols.clear();
        int used = 0;
        for(int l : lengths){ if(l < 0 || l > 16) return; if(l){ ++counts[l]; ++used; } }
        if(!used) return;
        int offs[17] = {}; for(int l = 1; l < 16; ++l) offs[l + 1] = offs[l] + counts[l];
        symbols.resize(used);
        for(int s = 0; s < lengths.size(); ++s) if(lengths[s]) symbols[offs[lengths[s]]++] = s;
        valid = true;
    }
    int decode(Bits& b) const {
        if(!valid) return -1;
        int code = 0, first = 0, index = 0;
        for(int l = 1; l <= 16; ++l){
            code |= int(b.get(1));
            const int c = counts[l];
            if(code - first < c) return symbols[index + code - first];
            index += c; first += c; first <<= 1; code <<= 1;
        }
        return -1;
    }
};

bool readHuffman(Bits& b, Huffman& h){
    static const int order[21] = {17, 18, 19, 20, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15, 16};
    const int total = int(b.get(14));
    if(!total) return false;
    const int codes = int(b.get(5));
    if(codes < 1 || codes > 21) return false;
    QVector<int> small(21, 0);
    for(int i = 0; i < codes; ++i) small[order[i]] = int(b.get(3));
    Huffman lengthsCode; lengthsCode.build(small);
    if(!lengthsCode.valid) return false;
    QVector<int> lengths(total, 0);
    int cur = 0;
    while(cur < total){
        const int s = lengthsCode.decode(b);
        if(s < 0 || b.over()) return false;
        if(s <= 16) lengths[cur++] = s;
        else if(s == 17 || s == 18){ const int run = s == 17 ? int(b.get(3)) + 3 : int(b.get(7)) + 11; if(cur + run > total) return false; cur += run; }
        else { if(!cur || !lengths[cur - 1]) return false; const int run = s == 19 ? int(b.get(2)) + 3 : int(b.get(7)) + 7; if(cur + run > total) return false;
               const int prev = lengths[cur - 1]; for(int i = 0; i < run; ++i) lengths[cur++] = prev; }
    }
    // One used symbol still takes a bit.
    int used = 0, last = -1; for(int i = 0; i < total; ++i) if(lengths[i]){ ++used; last = i; }
    if(used == 1) lengths[last] = 1;
    h.build(lengths);
    return h.valid;
}

struct Endpoint { int r, g, b, inten; };
struct Codebooks {
    QVector<Endpoint> endpoints; QVector<quint32> selectors;    // selectors: 4 bytes, a row each, 2 bits a texel from the LSB
    Huffman pred, delta, sel, rle; int historySize = 0; bool ok = false;
};

bool readEndpoints(const uchar* p, qint64 n, int count, Codebooks& cb){
    Bits b(p, n); Huffman m0, m1, m2, inten;
    if(!readHuffman(b, m0) || !readHuffman(b, m1) || !readHuffman(b, m2) || !readHuffman(b, inten)) return false;
    const bool gray = b.get(1);
    int prevInten = 0, prev[3] = {16, 16, 16};
    cb.endpoints.resize(count);
    for(int i = 0; i < count; ++i){
        const int d = inten.decode(b); if(d < 0) return false;
        Endpoint& e = cb.endpoints[i]; e.inten = (prevInten + d) & 7; prevInten = e.inten;
        int v[3];
        for(int c = 0; c < (gray ? 1 : 3); ++c){
            const Huffman& m = prev[c] <= 9 ? m0 : prev[c] <= 21 ? m1 : m2;
            const int dd = m.decode(b); if(dd < 0) return false;
            v[c] = (prev[c] + dd) & 31; prev[c] = v[c];
        }
        if(gray){ v[1] = v[2] = v[0]; prev[1] = prev[2] = prev[0]; }
        e.r = v[0]; e.g = v[1]; e.b = v[2];
        if(b.over()) return false;
    }
    return true;
}

bool readSelectors(const uchar* p, qint64 n, int count, Codebooks& cb){
    Bits b(p, n);
    if(b.get(2)) return false;
    const bool raw = b.get(1);
    cb.selectors.resize(count);
    if(raw){ for(int i = 0; i < count; ++i){ quint32 s = 0; for(int j = 0; j < 4; ++j) s |= b.get(8) << (8 * j); cb.selectors[i] = s; } return !b.over(); }
    Huffman delta; if(!readHuffman(b, delta)) return false;
    quint32 s = 0; for(int j = 0; j < 4; ++j) s |= b.get(8) << (8 * j);
    if(count) cb.selectors[0] = s;
    for(int i = 1; i < count; ++i){
        quint32 v = 0;
        for(int j = 0; j < 4; ++j){ const int d = delta.decode(b); if(d < 0) return false; v |= quint32((d ^ (cb.selectors[i - 1] >> (8 * j))) & 0xff) << (8 * j); }
        cb.selectors[i] = v;
    }
    return !b.over();
}

bool readTables(const uchar* p, qint64 n, Codebooks& cb){
    Bits b(p, n);
    if(!readHuffman(b, cb.pred) || !readHuffman(b, cb.delta) || !readHuffman(b, cb.sel) || !readHuffman(b, cb.rle)) return false;
    cb.historySize = int(b.get(13));
    return !b.over();
}

// A slice's blocks as (endpoint, selector) index pairs; prevFrame carries a video's last frame for P-frames.
bool decodeSlice(const uchar* p, qint64 n, int bx, int by, const Codebooks& cb, bool pFrame, QVector<quint32>* prevFrame, QVector<quint32>& out){
    const int nEnd = cb.endpoints.size(), nSel = cb.selectors.size();
    if(bx < 1 || by < 1 || qint64(bx) * by > (qint64(1) << 24) || !nEnd || !nSel) return false;
    Bits b(p, n);
    out.resize(bx * by);
    QVector<int> preds[2] = {QVector<int>(bx, 0), QVector<int>(bx, 0)};
    QVector<quint8> predBits((bx + 1) / 2, 0);
    QVector<int> history(cb.historySize, 0); int rover = cb.historySize / 2;
    const int rleSymbol = nSel + cb.historySize;
    int selRun = 0, curPred = 0, prevPredSym = 0, predRepeat = 0, prevEndpoint = 0;
    const bool video = prevFrame != nullptr;
    if(video && prevFrame->size() != bx * by) prevFrame->fill(0, bx * by);
    for(int y = 0; y < by; ++y){
        const int row = y & 1;
        for(int x = 0; x < bx; ++x){
            if(!(x & 1)){
                if(!(y & 1)){
                    if(predRepeat){ --predRepeat; curPred = prevPredSym; }
                    else {
                        curPred = cb.pred.decode(b); if(curPred < 0) return false;
                        if(curPred == 256){ predRepeat = int(b.vlc(5)) + 3 - 1; curPred = prevPredSym; }
                        else prevPredSym = curPred;
                    }
                    predBits[x >> 1] = quint8(curPred >> 4);
                } else curPred = predBits[x >> 1];
            }
            const int pred = curPred & 3; curPred >>= 2;
            int e = 0, s = 0;
            if(pred == 0) e = prevEndpoint;
            else if(pred == 1) e = preds[row ^ 1][x];
            else if(pred == 2){
                if(pFrame && video){ e = int((*prevFrame)[y * bx + x] & 0xffff); s = int((*prevFrame)[y * bx + x] >> 16); }
                else e = x > 0 ? preds[row ^ 1][x - 1] : 0;
            } else {
                const int d = cb.delta.decode(b); if(d < 0) return false;
                e = d + prevEndpoint; if(e >= nEnd) e -= nEnd;
            }
            if(e < 0 || e >= nEnd)return false;
            preds[row][x] = e; prevEndpoint = e;
            if(!pFrame || pred != 2){
                int sym;
                if(selRun > 0){ --selRun; sym = nSel; }
                else {
                    sym = cb.sel.decode(b); if(sym < 0) return false;
                    if(sym == rleSymbol){
                        const int run = cb.rle.decode(b); if(run < 0) return false;
                        selRun = (run == 63 ? int(b.vlc(8)) : run) + 3;
                        sym = nSel; --selRun;
                    }
                }
                if(sym >= nSel){
                    const int h = sym - nSel; if(h >= history.size()) return false;
                    s = history[h];
                    if(h){ std::swap(history[h / 2], history[h]); }
                } else {
                    s = sym;
                    if(!history.isEmpty()){ history[rover++] = s; if(rover == history.size()) rover = history.size() / 2; }
                }
            }
            if(s < 0 || s >= nSel)return false;
            out[y * bx + x] = quint32(e) | quint32(s) << 16;
            if(video) (*prevFrame)[y * bx + x] = out[y * bx + x];
            if(b.over())return false;
        }
    }
    return true;
}

// ETC1S blocks to pixels: the endpoint's colour (5 bits widened to 8) moved by its intensity table, as each
// texel's selector picks: -b, -a, +a or +b.
QImage pixels(const QVector<quint32>& blocks, int bx, int by, int w, int h, const Codebooks& cb){
    static const int table[8][4] = { {-8, -2, 2, 8}, {-17, -5, 5, 17}, {-29, -9, 9, 29}, {-42, -13, 13, 42},
                                     {-60, -18, 18, 60}, {-80, -24, 24, 80}, {-106, -33, 33, 106}, {-183, -47, 47, 183} };
    QImage img(w, h, QImage::Format_ARGB32);
    if(img.isNull()) return img;
    for(int j = 0; j < by; ++j) for(int i = 0; i < bx; ++i){
        const quint32 v = blocks[j * bx + i];
        const Endpoint& e = cb.endpoints[int(v & 0xffff)]; const quint32 s = cb.selectors[int(v >> 16)];
        const int r = e.r << 3 | e.r >> 2, g = e.g << 3 | e.g >> 2, b = e.b << 3 | e.b >> 2;
        for(int y = 0; y < 4 && j * 4 + y < h; ++y){
            QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(j * 4 + y)) + i * 4;
            for(int x = 0; x < 4 && i * 4 + x < w; ++x){
                const int m = table[e.inten][(s >> (8 * y + 2 * x)) & 3];
                line[x] = qRgb(qBound(0, r + m, 255), qBound(0, g + m, 255), qBound(0, b + m, 255));
            }
        }
    }
    return img;
}
void putAlpha(QImage& img, const QImage& alpha){
    for(int y = 0; y < img.height() && y < alpha.height(); ++y){
        QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y)); const QRgb* a = reinterpret_cast<const QRgb*>(alpha.constScanLine(y));
        for(int x = 0; x < img.width() && x < alpha.width(); ++x) d[x] = (d[x] & 0xffffff) | uint(qGreen(a[x])) << 24;
    }
}

quint32 le(const uchar* p, int bytes){ quint32 v = 0; for(int i = 0; i < bytes; ++i) v |= quint32(p[i]) << (8 * i); return v; }

} // namespace

QImage decodeBasisLz(const uchar* gd, qint64 gdSize, int image, const uchar* level, qint64 levelSize, int w, int h){
    if(gdSize < 20 || image < 0) return {};
    const int nEnd = int(le(gd, 2)), nSel = int(le(gd + 2, 2));
    const quint32 eLen = le(gd + 4, 4), sLen = le(gd + 8, 4), tLen = le(gd + 12, 4), xLen = le(gd + 16, 4);
    const qint64 descs = 20, need = descs + qint64(image + 1) * 20;
    if(need > gdSize) return {};
    const qint64 imageCount = (gdSize - 20 - qint64(eLen) - sLen - tLen - xLen) / 20;
    if(imageCount <= image) return {};
    const qint64 eOff = 20 + imageCount * 20, sOff = eOff + eLen, tOff = sOff + sLen;
    if(tOff + tLen > gdSize) return {};
    Codebooks cb;
    if(!readEndpoints(gd + eOff, eLen, nEnd, cb) || !readSelectors(gd + sOff, sLen, nSel, cb) || !readTables(gd + tOff, tLen, cb)) return {};
    const uchar* d = gd + descs + qint64(image) * 20;
    const quint32 flags = le(d, 4), rgbOff = le(d + 4, 4), rgbLen = le(d + 8, 4), aOff = le(d + 12, 4), aLen = le(d + 16, 4);
    const int bx = (w + 3) / 4, by = (h + 3) / 4;
    if(quint64(rgbOff) + rgbLen > quint64(levelSize)) return {};
    QVector<quint32> blocks;
    if(!decodeSlice(level + rgbOff, rgbLen, bx, by, cb, flags & 2, nullptr, blocks)) return {};
    QImage img = pixels(blocks, bx, by, w, h, cb);
    if(aLen && quint64(aOff) + aLen <= quint64(levelSize) && decodeSlice(level + aOff, aLen, bx, by, cb, flags & 2, nullptr, blocks)) putAlpha(img, pixels(blocks, bx, by, w, h, cb));
    return img;
}

QVector<QImage> decodeBasis(const uchar* p, qint64 n, QString* note, int* frameUs){
    QVector<QImage> out;
    if(n < 77 || le(p, 2) != (('B' << 8) | 's')) return out;
    const int headerSize = int(le(p + 4, 2));
    const int slices = int(le(p + 14, 3)), format = p[20], flags = int(le(p + 21, 2)), type = p[23];
    if(frameUs) *frameUs = int(le(p + 24, 3));
    const int nEnd = int(le(p + 39, 2)); const quint32 eOff = le(p + 41, 4), eLen = le(p + 45, 3);
    const int nSel = int(le(p + 48, 2)); const quint32 sOff = le(p + 50, 4), sLen = le(p + 54, 3);
    const quint32 tOff = le(p + 57, 4), tLen = le(p + 61, 4), descOff = le(p + 65, 4);
    if(headerSize < 77 || slices < 1 || slices > (1 << 20) || quint64(descOff) + quint64(slices) * 23 > quint64(n)) return out;
    // UASTC HDR 4x4 and ASTC HDR 6x6 slices are plain ASTC blocks, UASTC HDR 6x6 intermediate its own stream; XUASTC isn't read.
    if(format > 4){ if(note) *note = QStringLiteral("XUASTC Basis Universal data"); return out; }
    if(flags & 8){ if(note) *note = QStringLiteral("a Basis file that uses another file's codebook"); return out; }
    Codebooks cb;
    if(format == 0){
        if(quint64(eOff) + eLen > quint64(n) || quint64(sOff) + sLen > quint64(n) || quint64(tOff) + tLen > quint64(n)) return out;
        if(!readEndpoints(p + eOff, eLen, nEnd, cb) || !readSelectors(p + sOff, sLen, nSel, cb) || !readTables(p + tOff, tLen, cb)) return out;
    }
    const bool alphaSlices = flags & 4, video = type == 3;
    QVector<quint32> prevRgb, prevAlpha, blocks;
    for(int i = 0; i < slices && out.size() < 1000; ++i){
        const uchar* d = p + descOff + qint64(i) * 23;
        const int level = d[3], sflags = d[4], w = int(le(d + 5, 2)), h = int(le(d + 7, 2)), bx = int(le(d + 9, 2)), by = int(le(d + 11, 2));
        const quint32 off = le(d + 13, 4), len = le(d + 17, 4);
        const int bs = format == 3 || format == 4 ? 6 : 4;                     // block size
        if(quint64(off) + len > quint64(n) || w < 1 || h < 1 || bx < (w + bs - 1) / bs || by < (h + bs - 1) / bs) return out;
        const bool isAlpha = format == 0 && alphaSlices && (i & 1);
        if(level != 0){ continue; }                                              // the biggest level of each image
        if(format == 4){
            const QImage img = gputex::decodeUastcHdr6x6i(p + off, len, w, h);
            if(!img.isNull()) out << img;
            continue;
        }
        if(format == 2 || format == 3){
            QImage img = gputex::decode(gputex::Format::ASTC, p + off, len, bx * bs, by * bs, bs, bs);
            if(!img.isNull()) out << img.copy(0, 0, w, h);
            continue;
        }
        if(format == 1){
            QImage img = gputex::decode(gputex::Format::UASTC, p + off, len, bx * 4, by * 4);
            if(!img.isNull()) out << img.copy(0, 0, w, h);
            continue;
        }
        const bool pFrame = video && !(sflags & 2);
        if(!decodeSlice(p + off, len, bx, by, cb, pFrame, video ? (isAlpha ? &prevAlpha : &prevRgb) : nullptr, blocks))return out;
        const QImage img = pixels(blocks, bx, by, w, h, cb);
        if(isAlpha){ if(!out.isEmpty()) putAlpha(out.last(), img); }
        else out << img;
    }
    return out;
}

} // namespace textures
} // namespace xara
