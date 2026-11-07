// Aseprite (.aseprite / .ase), after Aseprite's published file specification (docs/ase-file-specs.md). A
// 128-byte header (frame count, size, colour depth, flags), then per frame a 16-byte header (duration, chunk
// count) and its chunks: layers (in the first frame, bottom first, each with its depth in the tree), cels (a
// layer's picture in this frame: zlib-compressed pixels, a link to another frame's cel, or tilemap indices),
// palettes, tilesets and tags. Pixels are RGBA (straight alpha), grey + alpha, or palette indices.
//
// Each Aseprite frame becomes a frame of the document (a layer); inside it, Aseprite's layers become objects,
// groups become groups, with their opacity, blend mode and visibility. Bitmaps are pixelated.
#include "prepress.h"
#include <QHash>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <functional>
#include <zlib.h>

namespace xara {
namespace {

const qint64 kMaxPixels = qint64(1) << 26, kMaxTotalPixels = qint64(1) << 28;

struct Rd {
    const uchar* p; qint64 n;
    quint32 u8(qint64 o) const { return o >= 0 && o < n ? p[o] : 0; }
    quint32 u16(qint64 o) const { return o >= 0 && o + 2 <= n ? quint32(p[o]) | quint32(p[o + 1]) << 8 : 0; }
    qint32 s16(qint64 o) const { return qint16(u16(o)); }
    quint32 u32(qint64 o) const { return o >= 0 && o + 4 <= n ? quint32(p[o]) | quint32(p[o + 1]) << 8 | quint32(p[o + 2]) << 16 | quint32(p[o + 3]) << 24 : 0; }
    QString str(qint64 o, qint64* len = nullptr) const {
        const qint64 l = u16(o);
        if(len) *len = 2 + l;
        if(o + 2 + l > n) return QString();
        return QString::fromUtf8(reinterpret_cast<const char*>(p + o + 2), int(l));
    }
};

struct ALayer { QString name; int flags = 0, type = 0, level = 0, blend = 0, opacity = 255, tileset = -1, parent = -1; QVector<int> kids; };
struct ACel { int layer = -1, x = 0, y = 0, opacity = 255, z = 0; QImage img; int linkTo = -1; bool tilemap = false;
              int tw = 0, th = 0; QVector<quint32> tiles; quint32 idMask = 0, xFlip = 0, yFlip = 0, dFlip = 0; };
struct ATileset { int tw = 0, th = 0, count = 0; bool zeroEmpty = true; QImage img; };

QByteArray inflateExact(const uchar* s, qint64 n, qint64 size){
    if(size <= 0 || size > (qint64(1) << 30) || n <= 0 || n > 0x7fffffff) return QByteArray();
    QByteArray out(int(size), Qt::Uninitialized);
    uLongf len = uLongf(size);
    const int rc = uncompress(reinterpret_cast<Bytef*>(out.data()), &len, s, uLong(n));
    if(rc != Z_OK && rc != Z_BUF_ERROR) return QByteArray();
    if(qint64(len) < size) std::memset(out.data() + len, 0, size_t(size - qint64(len)));
    return out;
}

QByteArray blendKey(int m, bool* known){
    static const char* const keys[] = { "norm", "mul ", "scrn", "over", "dark", "lite", "div ", "idiv", "hLit", "sLit", "diff", "smud",
                                        "hue ", "sat ", "colr", "lum ", "lddg", "fsub", "fdiv" };
    *known = m >= 0 && m < 19;
    return *known ? QByteArray(keys[m]) : QByteArrayLiteral("norm");
}

} // namespace

Document importAseDocument(const QByteArray& data, AseImportStats* st){
    Document doc;
    AseImportStats local; if(!st) st = &local;
    const Rd r{ reinterpret_cast<const uchar*>(data.constData()), data.size() };
    if(r.n < 128 || r.u16(4) != 0xA5E0) return doc;
    const int frames = int(r.u16(6)), W = int(r.u16(8)), H = int(r.u16(10)), depth = int(r.u16(12));
    const quint32 hflags = r.u32(14); const int speed = int(r.u16(18)), clearIndex = int(r.u8(28));
    const int pw = int(r.u8(34)), ph = int(r.u8(35));
    if(frames < 1 || W < 1 || H < 1 || qint64(W) * H > kMaxPixels || (depth != 32 && depth != 16 && depth != 8)) return doc;
    st->colorMode = depth == 32 ? QStringLiteral("RGBA") : depth == 16 ? QStringLiteral("greyscale") : QStringLiteral("indexed");
    const int bpp = depth / 8;

    QVector<QRgb> palette(256, qRgba(0, 0, 0, 255)); bool newPalette = false;
    QVector<ALayer> layers; QHash<int, ATileset> tilesets;
    QVector<QHash<int, ACel>> cels(frames);          // per frame: layer index → cel
    QVector<int> durations(frames, speed > 0 ? speed : 100);
    QVector<FrameTag> tags; int tagUserData = -1;    // the tags the next user data chunks colour
    qint64 budget = kMaxTotalPixels;

    // A cel's pixels as ARGB; the palette's clear index is see-through except on the background layer.
    auto pixels = [&](const uchar* s, int w, int h, bool background) -> QImage {
        QImage img(w, h, QImage::Format_ARGB32);
        if(img.isNull()) return img;
        for(int y = 0; y < h; ++y){
            QRgb* d = reinterpret_cast<QRgb*>(img.scanLine(y));
            const uchar* q = s + qint64(y) * w * bpp;
            for(int x = 0; x < w; ++x, q += bpp){
                if(bpp == 4) d[x] = qRgba(q[0], q[1], q[2], q[3]);
                else if(bpp == 2) d[x] = qRgba(q[0], q[0], q[0], q[1]);
                else d[x] = (q[0] == clearIndex && !background) ? 0u : palette[q[0]];
            }
        }
        return img;
    };

    qint64 o = 128;
    for(int f = 0; f < frames && o + 16 <= r.n; ++f){
        const qint64 fsize = r.u32(o);
        if(r.u16(o + 4) != 0xF1FA || fsize < 16 || fsize > r.n - o){ st->badCels++; break; }
        const int dur = int(r.u16(o + 8));
        if(dur > 0) durations[f] = dur;
        quint32 count = r.u32(o + 12); if(!count) count = r.u16(o + 6);
        qint64 c = o + 16; const qint64 fend = o + fsize;
        for(quint32 k = 0; k < count && c + 6 <= fend; ++k){
            const qint64 csize = r.u32(c); const int type = int(r.u16(c + 4));
            if(csize < 6 || csize > fend - c) break;
            const qint64 b = c + 6, cend = c + csize;
            switch(type){
            case 0x0004: case 0x0011: if(!newPalette){                                      // old palettes
                int idx = 0; const int packets = int(r.u16(b)); qint64 q = b + 2;
                for(int pk = 0; pk < packets && q + 2 <= cend; ++pk){
                    idx += int(r.u8(q)); int n = int(r.u8(q + 1)); if(!n) n = 256; q += 2;
                    for(int i = 0; i < n && q + 3 <= cend; ++i, q += 3, ++idx){
                        if(idx >= 256) break;
                        int rr = int(r.u8(q)), gg = int(r.u8(q + 1)), bb = int(r.u8(q + 2));
                        if(type == 0x0011){ rr = rr * 255 / 63; gg = gg * 255 / 63; bb = bb * 255 / 63; }
                        palette[idx] = qRgba(rr, gg, bb, 255);
                    }
                }
            } break;
            case 0x2019: {                                                                  // palette
                newPalette = true;
                const quint32 size = r.u32(b), from = r.u32(b + 4), to = r.u32(b + 8);
                if(size > 0 && size <= 65536 && int(size) > palette.size()) palette.resize(int(size));
                qint64 q = b + 20;
                for(quint32 i = from; i <= to && i < quint32(palette.size()) && q + 6 <= cend; ++i){
                    const quint32 fl = r.u16(q);
                    palette[int(i)] = qRgba(int(r.u8(q + 2)), int(r.u8(q + 3)), int(r.u8(q + 4)), int(r.u8(q + 5)));
                    q += 6;
                    if(fl & 1){ qint64 l = 0; r.str(q, &l); q += l; }
                }
            } break;
            case 0x2004: {                                                                  // layer
                if(layers.size() >= 4096) break;
                ALayer L; L.flags = int(r.u16(b)); L.type = int(r.u16(b + 2)); L.level = int(r.u16(b + 4));
                L.blend = int(r.u16(b + 10)); L.opacity = (hflags & 1) ? int(r.u8(b + 12)) : 255;
                qint64 l = 0; L.name = r.str(b + 16, &l);
                if(L.type == 2) L.tileset = int(r.u32(b + 16 + l));
                layers << L;
            } break;
            case 0x2005: {                                                                  // cel
                ACel cel; cel.layer = int(r.u16(b)); cel.x = r.s16(b + 2); cel.y = r.s16(b + 4);
                cel.opacity = int(r.u8(b + 6)); const int ct = int(r.u16(b + 7)); cel.z = r.s16(b + 9);
                const qint64 dat = b + 16;
                if(cel.layer < 0 || cel.layer >= 4096) break;
                const bool background = cel.layer < layers.size() && (layers[cel.layer].flags & 8);
                if(ct == 1){ cel.linkTo = int(r.u16(dat)); st->linkedCels++; }
                else if(ct == 0 || ct == 2){
                    const int w = int(r.u16(dat)), h = int(r.u16(dat + 2));
                    if(w < 1 || h < 1 || qint64(w) * h > kMaxPixels || qint64(w) * h > budget){ st->badCels++; break; }
                    const qint64 size = qint64(w) * h * bpp;
                    QByteArray raw;
                    if(ct == 2) raw = inflateExact(r.p + dat + 4, cend - dat - 4, size);
                    else if(dat + 4 + size <= cend) raw = QByteArray(reinterpret_cast<const char*>(r.p + dat + 4), int(size));
                    if(raw.size() != size){ st->badCels++; break; }
                    budget -= qint64(w) * h;
                    cel.img = pixels(reinterpret_cast<const uchar*>(raw.constData()), w, h, background);
                }
                else if(ct == 3){
                    const int tw = int(r.u16(dat)), th = int(r.u16(dat + 2)), bits = int(r.u16(dat + 4));
                    if(tw < 1 || th < 1 || qint64(tw) * th > (1 << 22) || (bits != 8 && bits != 16 && bits != 32)){ st->badCels++; break; }
                    cel.tilemap = true; cel.tw = tw; cel.th = th;
                    cel.idMask = r.u32(dat + 6); cel.xFlip = r.u32(dat + 10); cel.yFlip = r.u32(dat + 14); cel.dFlip = r.u32(dat + 18);
                    const QByteArray raw = inflateExact(r.p + dat + 32, cend - dat - 32, qint64(tw) * th * (bits / 8));
                    if(raw.isEmpty()){ st->badCels++; break; }
                    const Rd t{ reinterpret_cast<const uchar*>(raw.constData()), raw.size() };
                    for(qint64 i = 0; i < qint64(tw) * th; ++i) cel.tiles << (bits == 32 ? t.u32(i * 4) : bits == 16 ? t.u16(i * 2) : t.u8(i));
                    st->tilemaps++;
                }
                else break;
                cels[f].insert(cel.layer, cel);
            } break;
            case 0x2018: {                                                                  // tags
                const int n = int(r.u16(b)); qint64 q = b + 10; tagUserData = tags.size();
                for(int i = 0; i < n && q + 17 <= cend && tags.size() < 1024; ++i){
                    FrameTag t; t.from = int(r.u16(q)); t.to = int(r.u16(q + 2)); const int dir = int(r.u8(q + 4));
                    t.direction = dir == 1 ? 1 : dir >= 2 ? 2 : 0;
                    t.color = QColor(int(r.u8(q + 13)), int(r.u8(q + 14)), int(r.u8(q + 15)));
                    qint64 l = 0; t.name = r.str(q + 17, &l); q += 17 + l;
                    t.from = qBound(0, t.from, frames - 1); t.to = qBound(t.from, t.to, frames - 1);
                    tags << t;
                }
            } break;
            case 0x2020: {                                                                  // user data: a tag's colour
                if(tagUserData >= 0 && tagUserData < tags.size()){
                    const quint32 fl = r.u32(b); qint64 q = b + 4;
                    if(fl & 1){ qint64 l = 0; r.str(q, &l); q += l; }
                    if(fl & 2) tags[tagUserData].color = QColor(int(r.u8(q)), int(r.u8(q + 1)), int(r.u8(q + 2)), qMax(1, int(r.u8(q + 3))));
                    ++tagUserData;
                }
            } break;
            case 0x2023: {                                                                  // tileset
                ATileset ts; const int id = int(r.u32(b)); const quint32 fl = r.u32(b + 4);
                ts.count = int(r.u32(b + 8)); ts.tw = int(r.u16(b + 12)); ts.th = int(r.u16(b + 14)); ts.zeroEmpty = fl & 4;
                qint64 l = 0; r.str(b + 32, &l); qint64 q = b + 32 + l;
                if(fl & 1) q += 8;
                if((fl & 2) && ts.tw > 0 && ts.th > 0 && ts.count > 0 && qint64(ts.tw) * ts.th * ts.count <= kMaxPixels){
                    const qint64 len = r.u32(q);
                    const QByteArray raw = inflateExact(r.p + q + 4, qMin(len, cend - q - 4), qint64(ts.tw) * ts.th * ts.count * bpp);
                    if(!raw.isEmpty()) ts.img = pixels(reinterpret_cast<const uchar*>(raw.constData()), ts.tw, ts.th * ts.count, false);
                }
                tilesets.insert(id, ts);
            } break;
            default: break;
            }
            c = cend;
        }
        o = fend;
    }
    if(layers.isEmpty()) return doc;

    // The layer tree: each layer's parent is the last layer one level up.
    { QVector<int> stack;
      for(int i = 0; i < layers.size(); ++i){
          const int lvl = qMin(layers[i].level, int(stack.size()));
          stack.resize(lvl);
          layers[i].parent = lvl > 0 ? stack[lvl - 1] : -1;
          if(layers[i].parent >= 0) layers[layers[i].parent].kids << i;
          stack << i;
      } }
    QVector<int> top; for(int i = 0; i < layers.size(); ++i) if(layers[i].parent < 0) top << i;

    // A cel's picture: its own, a linked frame's, or its tilemap drawn from the tileset.
    std::function<const ACel*(int, int, int)> celAt = [&](int f, int layer, int hops) -> const ACel* {
        auto it = cels[f].constFind(layer);
        if(it == cels[f].constEnd()) return nullptr;
        if(it->linkTo >= 0){ if(hops > 8 || it->linkTo >= frames || it->linkTo == f) return nullptr; const ACel* t = celAt(it->linkTo, layer, hops + 1); return t; }
        return &it.value();
    };
    QHash<qint64, QImage> tilemapImages;
    auto tilemapImage = [&](const ACel& cel, int layer, int f) -> QImage {
        const qint64 key = qint64(f) << 16 | layer;
        if(tilemapImages.contains(key)) return tilemapImages.value(key);
        const ATileset ts = tilesets.value(layers[layer].tileset);
        QImage img;
        if(ts.img.isNull() || ts.tw < 1 || qint64(cel.tw) * ts.tw * cel.th * ts.th > kMaxPixels){ tilemapImages.insert(key, img); return img; }
        img = QImage(cel.tw * ts.tw, cel.th * ts.th, QImage::Format_ARGB32); img.fill(Qt::transparent);
        QPainter g(&img); g.setCompositionMode(QPainter::CompositionMode_Source);
        for(int ty = 0; ty < cel.th; ++ty) for(int tx = 0; tx < cel.tw; ++tx){
            const quint32 v = cel.tiles.value(ty * cel.tw + tx);
            const quint32 id = cel.idMask ? (v & cel.idMask) : v;
            if((ts.zeroEmpty && id == 0) || (!ts.zeroEmpty && v == 0xffffffffu) || int(id) >= ts.count) continue;
            QImage tile = ts.img.copy(0, int(id) * ts.th, ts.tw, ts.th);
            if(cel.dFlip && (v & cel.dFlip)) tile = tile.transformed(QTransform(0, 1, 1, 0, 0, 0));
            const bool fx = cel.xFlip && (v & cel.xFlip), fy = cel.yFlip && (v & cel.yFlip);
            if(fx || fy) tile = tile.transformed(QTransform::fromScale(fx ? -1 : 1, fy ? -1 : 1));
            g.drawImage(tx * ts.tw, ty * ts.th, tile);
        }
        g.end();
        tilemapImages.insert(key, img);
        return img;
    };

    std::function<QVector<Shape>(const QVector<int>&, int, int)> build = [&](const QVector<int>& list, int f, int depthLeft) -> QVector<Shape> {
        // Z-index (spec note 5): a cel may be drawn some layers earlier or later than its layer.
        QVector<QPair<int, int>> order;
        for(int i : list){ const ACel* c = layers[i].type == 1 ? nullptr : celAt(f, i, 0); order << qMakePair(i, c ? cels[f].value(i).z : 0); }
        std::stable_sort(order.begin(), order.end(), [](const QPair<int,int>& a, const QPair<int,int>& b){
            const int oa = a.first + a.second, ob = b.first + b.second; return oa < ob || (oa == ob && a.second < b.second); });
        QVector<Shape> out;
        for(const auto& pr : order){
            const int i = pr.first; const ALayer& L = layers[i];
            if(L.flags & 64) continue;                                                    // a reference layer isn't part of the picture
            bool known = true; const QByteArray key = blendKey(L.blend, &known);
            Shape s;
            if(L.type == 1){
                if(depthLeft <= 0) continue;
                s.type = ShapeType::Group; s.name = L.name;
                s.children = build(L.kids, f, depthLeft - 1);
                if(s.children.isEmpty()) continue;
                const bool valid = hflags & 2;                                            // group blend/opacity only when the header says so
                s.opacity = valid ? L.opacity / 255.0 : 1.0;
                s.isolate = valid && (L.opacity < 255 || L.blend != 0);
                if(valid) psdApplyBlend(s, key);
            } else {
                const ACel* c = celAt(f, i, 0);
                if(!c) continue;
                QImage img = c->tilemap ? tilemapImage(*c, i, f) : c->img;
                if(img.isNull()) continue;
                s = psdBitmapShape(img, c->x, c->y, L.name);
                s.fill.pixelated = true;
                s.opacity = (L.opacity / 255.0) * (c->opacity / 255.0);     // a linked cel shares its source's opacity too
                psdApplyBlend(s, key);
            }
            if(!known) st->unknownModes++;
            s.visible = L.flags & 1;
            out << s;
        }
        return out;
    };

    doc.pageSize = QSizeF(W, H);
    const double aspect = (pw > 0 && ph > 0 && pw != ph) ? double(pw) / ph : 1.0;
    if(aspect != 1.0) doc.pageSize = QSizeF(W * aspect, H);
    doc.layers.clear();
    for(int f = 0; f < frames; ++f){
        Layer L; L.name = QStringLiteral("Frame %1").arg(f + 1); L.visible = (f == 0); L.frameDelayMs = durations[f];
        doc.layers << L;
        for(Shape s : build(top, f, 32)){
            s.layer = f;
            if(aspect != 1.0) s.transform = s.transform * QTransform::fromScale(aspect, 1.0);
            doc.shapes << s;
        }
    }
    doc.frameTags = tags;
    for(const ALayer& L : layers){ if(L.type == 1) st->groups++; else if(!(L.flags & 64)) st->layers++; else st->referenceLayers++; }
    st->frames = frames;
    return doc;
}

} // namespace xara
