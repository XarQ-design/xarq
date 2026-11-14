// Adobe Flash SWF: its stage and timeline, frame by frame. Shapes are edges between a left and a right fill
// (and a line), stitched here into closed outlines per fill: solid, linear, radial or focal gradients, and
// bitmap fills through their matrices. Bitmaps come from JPEG (with their shared tables and alpha), and
// lossless 8-bit palette, 15- and 24-bit and ARGB data. Static text is drawn with its font's glyph outlines,
// text fields with a matching system font. Buttons show their up state; morph shapes their start. The display
// list plays through the frames: placing, moving, recolouring (colour transforms) and removing characters,
// clip layers masking the depths above them, sprites running on their own clocks. Each frame becomes an
// animation frame; a one-frame movie is a plain drawing. ActionScript, sound, video and filters are left out.
#include "prepress.h"
#include <QHash>
#include <QPainterPath>
#include <QtMath>
#include <functional>
#include <zlib.h>
#ifdef HAVE_LZMA
#include <lzma.h>
#endif

namespace xara {
namespace {

struct Bits {
    const uchar* p; qint64 n, o = 0; int bit = 0;
    Bits(const uchar* d, qint64 len) : p(d), n(len) {}
    void align(){ if(bit){ bit = 0; ++o; } }
    bool atEnd() const { return o >= n; }
    quint32 ub(int nb){ quint32 v = 0; for(int i = 0; i < nb; ++i){ if(o >= n) return v << (nb - i - 1); v = (v << 1) | ((p[o] >> (7 - bit)) & 1); if(++bit == 8){ bit = 0; ++o; } } return v; }
    qint32 sb(int nb){ if(nb <= 0) return 0; if(nb > 32) nb = 32; quint32 v = ub(nb); if(nb < 32 && (v & (1u << (nb - 1)))) v |= ~0u << nb; return qint32(v); }
    double fb(int nb){ return sb(nb) / 65536.0; }
    int u8(){ align(); return o < n ? p[o++] : (o = n, 0); }
    int u16(){ const int a = u8(); return a | (u8() << 8); }
    int s16(){ return qint16(u16()); }
    quint32 u32(){ const quint32 a = quint32(u16()); return a | (quint32(u16()) << 16); }
    double fixed8(){ return s16() / 256.0; }
    QByteArray string(){ align(); QByteArray s; while(o < n && p[o]) s += char(p[o++]); ++o; return s; }
    QColor rgb(){ const int r = u8(); const int g = u8(); const int b = u8(); return QColor(r, g, b); }
    QColor rgba(){ const int r = u8(); const int g = u8(); const int b = u8(); const int a = u8(); return QColor(r, g, b, a); }
    QRectF rect(){ align(); const int nb = int(ub(5)); const double x0 = sb(nb), x1 = sb(nb), y0 = sb(nb), y1 = sb(nb); align(); return QRectF(QPointF(x0, y0), QPointF(x1, y1)); }
    QTransform matrix(){
        align();
        double sx = 1, sy = 1, r0 = 0, r1 = 0;
        if(ub(1)){ const int nb = int(ub(5)); sx = fb(nb); sy = fb(nb); }
        if(ub(1)){ const int nb = int(ub(5)); r0 = fb(nb); r1 = fb(nb); }
        const int nt = int(ub(5)); const double tx = sb(nt), ty = sb(nt);
        align();
        return QTransform(sx, r0, r1, sy, tx, ty);
    }
};

// A colour transform: each channel times its multiplier plus its offset.
struct CXform {
    double m[4] = {1, 1, 1, 1}, a[4] = {0, 0, 0, 0};
    bool identity() const { for(int i = 0; i < 4; ++i) if(m[i] != 1 || a[i] != 0) return false; return true; }
    QColor apply(const QColor& c) const {
        const int v[4] = {c.red(), c.green(), c.blue(), c.alpha()}; int o[4];
        for(int i = 0; i < 4; ++i) o[i] = qBound(0, int(std::round(v[i] * m[i] + a[i])), 255);
        return QColor(o[0], o[1], o[2], o[3]);
    }
    CXform then(const CXform& outer) const { CXform r; for(int i = 0; i < 4; ++i){ r.m[i] = m[i] * outer.m[i]; r.a[i] = a[i] * outer.m[i] + outer.a[i]; } return r; }
    static CXform read(Bits& b, bool alpha){
        CXform c; b.align();
        const bool add = b.ub(1), mult = b.ub(1); const int nb = int(b.ub(4));
        if(mult){ for(int i = 0; i < (alpha ? 4 : 3); ++i) c.m[i] = b.sb(nb) / 256.0; }
        if(add){ for(int i = 0; i < (alpha ? 4 : 3); ++i) c.a[i] = b.sb(nb); }
        b.align(); return c;
    }
};

struct FillStyle { int type = 0; QColor colour; QTransform m; QVector<GradStop> stops; double focal = 0; int spread = 0, bitmap = -1; bool smooth = true, repeat = true; };
struct LineStyle { double width = 20; QColor colour; bool hasFill = false; FillStyle fill; Qt::PenCapStyle cap = Qt::RoundCap; Qt::PenJoinStyle join = Qt::RoundJoin; };

struct Edge { qint32 ax, ay, cx, cy, bx, by; bool curve; };
struct Character {
    enum Kind { None, Shape_, Bitmap, Text, Sprite, Button, Font } kind = None;
    QVector<Shape> shapes;                             // in twips
    QImage image;
    struct Glyph { QPainterPath path; };
    QVector<QPainterPath> glyphs; double emSquare = 1024; QString fontName; bool bold = false, italic = false;
    // A sprite's own timeline.
    int timeline = -1;
    struct Part { int character = 0, depth = 0; QTransform m; CXform cx; };
    QVector<Part> buttonParts;                         // a button's up state
};

struct Place { int depth = 0, character = -1, ratio = -1, clipDepth = 0, blend = 0; double blur = -1; bool move = false, hasMatrix = false, hasCx = false; QTransform m; CXform cx; bool remove = false; };
struct Timeline { QVector<QVector<Place>> frames; QVector<QString> labels; };

struct Swf {
    QHash<int, Character> chars;
    QVector<Timeline> timelines;                       // 0 is the movie's
    QByteArray jpegTables;
    QColor background = Qt::white;
    SwfImportStats* st = nullptr;
    int version = 0;
    int budget = 3000000;                              // shapes a whole import may make

    // ---- styles ----
    QVector<GradStop> gradient(Bits& b, bool alpha, int* spread){
        b.align(); *spread = int(b.ub(2)); b.ub(2); const int count = int(b.ub(4));
        QVector<GradStop> stops;
        for(int i = 0; i < count && !b.atEnd(); ++i){ const int ratio = b.u8(); const QColor c = alpha ? b.rgba() : b.rgb(); stops << GradStop{ratio / 255.0, c}; }
        std::stable_sort(stops.begin(), stops.end(), [](const GradStop& x, const GradStop& y){ return x.pos < y.pos; });
        for(int i = 1; i < stops.size(); ++i) if(stops[i].pos <= stops[i - 1].pos) stops[i].pos = qMin(1.0, stops[i - 1].pos + 1e-4);   // equal ratios: a hard step
        return stops;
    }
    FillStyle fillStyle(Bits& b, int shapeVer){
        FillStyle f; f.type = b.u8();
        if(f.type == 0x00) f.colour = shapeVer >= 3 ? b.rgba() : b.rgb();
        else if(f.type == 0x10 || f.type == 0x12 || f.type == 0x13){ f.m = b.matrix(); f.stops = gradient(b, shapeVer >= 3, &f.spread); if(f.type == 0x13) f.focal = b.fixed8(); }
        else if(f.type >= 0x40 && f.type <= 0x43){ f.bitmap = b.u16(); f.m = b.matrix(); f.repeat = !(f.type & 1); f.smooth = !(f.type & 2); }
        else { if(st) ++st->skipped; }
        return f;
    }
    QVector<FillStyle> fillStyles(Bits& b, int shapeVer){
        int n = b.u8(); if(n == 0xFF && shapeVer >= 2) n = b.u16();
        QVector<FillStyle> v; for(int i = 0; i < n && !b.atEnd(); ++i) v << fillStyle(b, shapeVer); return v;
    }
    QVector<LineStyle> lineStyles(Bits& b, int shapeVer){
        int n = b.u8(); if(n == 0xFF) n = b.u16();
        QVector<LineStyle> v;
        for(int i = 0; i < n && !b.atEnd(); ++i){
            LineStyle l; l.width = b.u16();
            if(shapeVer >= 4){
                const int startCap = int(b.ub(2)), join = int(b.ub(2)); l.hasFill = b.ub(1); b.ub(1); b.ub(1); b.ub(1); b.ub(5); b.ub(1); b.ub(2);
                l.cap = startCap == 1 ? Qt::FlatCap : startCap == 2 ? Qt::SquareCap : Qt::RoundCap;
                l.join = join == 1 ? Qt::BevelJoin : join == 2 ? Qt::MiterJoin : Qt::RoundJoin;
                if(join == 2) b.u16();
                if(l.hasFill){ l.fill = fillStyle(b, 4); l.colour = l.fill.type == 0 ? l.fill.colour : (l.fill.stops.isEmpty() ? QColor(Qt::black) : l.fill.stops.first().color); }
                else l.colour = b.rgba();
            } else l.colour = shapeVer >= 3 ? b.rgba() : b.rgb();
            v << l;
        }
        return v;
    }

    // ---- shapes ----
    // A fill style on an outline in twips: gradients and bitmaps through their matrices.
    void applyFill(Shape& s, const FillStyle& f){
        if(f.type == 0x00){ s.fill.type = FillType::Solid; s.fill.color = f.colour; return; }
        if(f.type == 0x10 || f.type == 0x12 || f.type == 0x13){
            s.fill.stops = f.stops;
            s.fill.spread = f.spread == 1 ? QGradient::ReflectSpread : f.spread == 2 ? QGradient::RepeatSpread : QGradient::PadSpread;
            if(f.type == 0x10){ s.fill.type = FillType::Linear; s.fill.p0 = f.m.map(QPointF(-16384, 0)); s.fill.p1 = f.m.map(QPointF(16384, 0)); s.fill.p2 = f.m.map(QPointF(-16384, 16384)); }
            else { s.fill.type = FillType::Radial; s.fill.p0 = f.m.map(QPointF(16384 * f.focal, 0)); s.fill.p1 = f.m.map(QPointF(16384, 0)); s.fill.p2 = f.m.map(QPointF(0, 16384));
                   if(f.focal != 0) s.fill.p0 = f.m.map(QPointF(0, 0)); }
            if(s.fill.stops.isEmpty()){ s.fill.type = FillType::Solid; s.fill.color = Qt::black; }
            return;
        }
        if(f.type >= 0x40 && f.type <= 0x43){
            const Character& c = chars.value(f.bitmap);
            if(c.kind != Character::Bitmap || c.image.isNull()){ s.fill.type = FillType::Solid; s.fill.color = QColor(255, 0, 0, 0); if(st) ++st->missingBitmaps; return; }
            s.fill.type = FillType::Bitmap; s.fill.image = c.image; s.fill.origImage = c.image; s.fill.tiled = f.repeat; s.fill.pixelated = !f.smooth;
            const double w = c.image.width(), h = c.image.height();
            s.fill.p0 = f.m.map(QPointF(0, h)); s.fill.p1 = f.m.map(QPointF(w, h)); s.fill.p2 = f.m.map(QPointF(0, 0));
            return;
        }
        s.fill.type = FillType::None;
    }

    // A SHAPEWITHSTYLE (or a glyph's SHAPE, `styles` false): XarQ shapes in twips, fills stitched from edges.
    bool winding = false;                              // DefineShape4's non-zero fill rule
    QVector<Shape> shape(Bits& b, int shapeVer, bool styles, QPainterPath* glyph = nullptr){
        QVector<FillStyle> fills; QVector<LineStyle> lines;
        if(styles){ fills = fillStyles(b, shapeVer); lines = lineStyles(b, shapeVer); }
        else if(usePreset && !glyph){ fills = presetFills; lines = presetLines; }      // a morph shape's start styles
        b.align(); int fillBits = int(b.ub(4)), lineBits = int(b.ub(4));
        QVector<Shape> out;
        // Edges per (style group, style), drawn in order: fills first, then lines.
        struct Group { QVector<FillStyle> fills; QVector<LineStyle> lines; QVector<QVector<Edge>> fillEdges, lineEdges; };
        QVector<Group> groups(1); groups[0].fills = fills; groups[0].lines = lines;
        groups[0].fillEdges.resize(fills.size()); groups[0].lineEdges.resize(lines.size());
        int f0 = 0, f1 = 0, ln = 0; qint32 x = 0, y = 0;
        QPainterPath gpath; gpath.setFillRule(Qt::OddEvenFill);
        auto addEdge = [&](const Edge& e){
            Group& g = groups.last();
            if(glyph){ if(e.curve) gpath.quadTo(e.cx, e.cy, e.bx, e.by); else gpath.lineTo(e.bx, e.by); return; }
            if(f1 > 0 && f1 <= g.fillEdges.size()) g.fillEdges[f1 - 1] << e;
            if(f0 > 0 && f0 <= g.fillEdges.size()) g.fillEdges[f0 - 1] << Edge{e.bx, e.by, e.cx, e.cy, e.ax, e.ay, e.curve};
            if(ln > 0 && ln <= g.lineEdges.size()) g.lineEdges[ln - 1] << e;
        };
        int guard = 0;
        while(!b.atEnd() && ++guard < 4000000){
            if(b.ub(1)){                                                       // an edge
                const bool straight = b.ub(1); const int nb = int(b.ub(4)) + 2;
                Edge e; e.ax = x; e.ay = y;
                if(straight){
                    qint32 dx = 0, dy = 0;
                    if(b.ub(1)){ dx = b.sb(nb); dy = b.sb(nb); }
                    else if(b.ub(1)) dy = b.sb(nb); else dx = b.sb(nb);
                    e.curve = false; e.bx = x + dx; e.by = y + dy; e.cx = (e.ax + e.bx) / 2; e.cy = (e.ay + e.by) / 2;
                } else {
                    const qint32 cdx = b.sb(nb), cdy = b.sb(nb), adx = b.sb(nb), ady = b.sb(nb);
                    e.curve = true; e.cx = x + cdx; e.cy = y + cdy; e.bx = e.cx + adx; e.by = e.cy + ady;
                }
                x = e.bx; y = e.by;
                addEdge(e);
            } else {
                const int flags = int(b.ub(5));
                if(!flags) break;                                              // end of the shape
                if(flags & 1){ const int nb = int(b.ub(5)); x = b.sb(nb); y = b.sb(nb); if(glyph) gpath.moveTo(x, y); }
                if(flags & 2) f0 = int(b.ub(fillBits));
                if(flags & 4) f1 = int(b.ub(fillBits));
                if(flags & 8) ln = int(b.ub(lineBits));
                if((flags & 16) && styles && shapeVer >= 2){
                    Group g; g.fills = fillStyles(b, shapeVer); g.lines = lineStyles(b, shapeVer);
                    g.fillEdges.resize(g.fills.size()); g.lineEdges.resize(g.lines.size());
                    groups << g; b.align(); fillBits = int(b.ub(4)); lineBits = int(b.ub(4));
                }
            }
        }
        b.align();
        if(glyph){ *glyph = gpath; return out; }
        for(const Group& g : std::as_const(groups)){
            // Fills: follow each edge to the next that starts where it ends, closing the loop.
            for(int i = 0; i < g.fillEdges.size(); ++i){
                const QVector<Edge>& es = g.fillEdges[i]; if(es.isEmpty()) continue;
                QMultiHash<quint64, int> starts; for(int k = 0; k < es.size(); ++k) starts.insert((quint64(quint32(es[k].ax)) << 32) | quint32(es[k].ay), k);
                QVector<bool> used(es.size(), false);
                QPainterPath path; path.setFillRule(winding ? Qt::WindingFill : Qt::OddEvenFill);
                for(int k = 0; k < es.size(); ++k){
                    if(used[k]) continue;
                    int cur = k; path.moveTo(es[k].ax, es[k].ay);
                    while(cur >= 0 && !used[cur]){
                        used[cur] = true; const Edge& e = es[cur];
                        if(e.curve) path.quadTo(e.cx, e.cy, e.bx, e.by); else path.lineTo(e.bx, e.by);
                        int next = -1; const quint64 key = (quint64(quint32(e.bx)) << 32) | quint32(e.by);
                        for(auto it = starts.constFind(key); it != starts.constEnd() && it.key() == key; ++it) if(!used[it.value()]){ next = it.value(); break; }
                        cur = next;
                    }
                    path.closeSubpath();
                }
                Shape s; s.type = ShapeType::Path; s.name = QStringLiteral("Fill"); s.path = path; s.fillRule = path.fillRule(); s.stroke.enabled = false;
                applyFill(s, g.fills[i]);
                if(s.fill.type != FillType::None) out << s;
            }
            for(int i = 0; i < g.lineEdges.size(); ++i){
                const QVector<Edge>& es = g.lineEdges[i]; if(es.isEmpty()) continue;
                QPainterPath path; qint32 px = INT_MIN, py = INT_MIN;
                for(const Edge& e : es){
                    if(e.ax != px || e.ay != py) path.moveTo(e.ax, e.ay);
                    if(e.curve) path.quadTo(e.cx, e.cy, e.bx, e.by); else path.lineTo(e.bx, e.by);
                    px = e.bx; py = e.by;
                }
                const LineStyle& l = g.lines[i];
                Shape s; s.type = ShapeType::Path; s.name = QStringLiteral("Line"); s.path = path; s.fill.type = FillType::None;
                s.stroke.enabled = true; s.stroke.color = l.colour; s.stroke.width = qMax(1.0, l.width); s.stroke.cap = l.cap; s.stroke.join = l.join;
                out << s;
            }
        }
        if(st) st->shapes += out.size();
        return out;
    }

    // ---- bitmaps ----
    QImage jpeg(const QByteArray& data){
        QByteArray d = data;
        if(d.startsWith("\xff\xd9\xff\xd8")) d = d.mid(4);                  // an old encoder's stray end-and-start
        int k; while((k = d.indexOf("\xff\xd9\xff\xd8")) > 0) d.remove(k, 4);   // tables and image in one stream
        QImage img = QImage::fromData(d);
        if(img.isNull() && !jpegTables.isEmpty()){ QByteArray t = jpegTables; if(t.endsWith("\xff\xd9")) t.chop(2); QByteArray body = d; if(body.startsWith("\xff\xd8")) body = body.mid(2); img = QImage::fromData(t + body); }
        return img;
    }
    QByteArray inflate(const uchar* p, qint64 n, qint64 expected){
        QByteArray out; if(expected <= 0 || expected > (qint64(1) << 30)) return out;
        out.resize(int(expected));
        uLongf len = uLongf(expected);
        if(uncompress(reinterpret_cast<Bytef*>(out.data()), &len, p, uLong(n)) != Z_OK && len == 0) return QByteArray();
        out.resize(int(len)); return out;
    }
    QImage lossless(Bits& b, qint64 end, bool alpha){
        const int format = b.u8(), w = b.u16(), h = b.u16();
        if(w <= 0 || h <= 0 || qint64(w) * h > (qint64(1) << 26)) return {};
        int colours = 0; if(format == 3) colours = b.u8() + 1;
        const qint64 stride8 = (w + 3) & ~3, stride16 = ((w * 2) + 3) & ~3;
        const qint64 expected = format == 3 ? colours * (alpha ? 4 : 3) + stride8 * h : format == 4 ? stride16 * h : qint64(w) * h * 4;
        const QByteArray d = inflate(b.p + b.o, end - b.o, expected);
        if(d.size() < expected) return {};
        const uchar* p = reinterpret_cast<const uchar*>(d.constData());
        QImage img(w, h, QImage::Format_ARGB32);
        auto px = [&](int r, int g, int bl, int a){ return alpha && a < 255 ? (a == 0 ? qRgba(0, 0, 0, 0) : qRgba(qMin(255, r * 255 / a), qMin(255, g * 255 / a), qMin(255, bl * 255 / a), a)) : qRgba(r, g, bl, alpha ? a : 255); };
        if(format == 3){
            QVector<QRgb> pal; const uchar* c = p; for(int i = 0; i < colours; ++i){ pal << (alpha ? px(c[0], c[1], c[2], c[3]) : qRgb(c[0], c[1], c[2])); c += alpha ? 4 : 3; }
            for(int y = 0; y < h; ++y){ QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y)); const uchar* row = c + y * stride8; for(int x = 0; x < w; ++x) line[x] = pal.value(row[x], 0); }
        } else if(format == 4){
            for(int y = 0; y < h; ++y){ QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y)); const uchar* row = p + y * stride16;
                for(int x = 0; x < w; ++x){ const int v = row[x * 2] << 8 | row[x * 2 + 1]; line[x] = qRgb(((v >> 10) & 31) * 255 / 31, ((v >> 5) & 31) * 255 / 31, (v & 31) * 255 / 31); } }
        } else if(format == 5){
            for(int y = 0; y < h; ++y){ QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y)); const uchar* row = p + qint64(y) * w * 4;
                for(int x = 0; x < w; ++x) line[x] = px(row[x * 4 + 1], row[x * 4 + 2], row[x * 4 + 3], row[x * 4]); }
        } else return {};
        return img;
    }

    // ---- fonts and text ----
    void defineFont(Bits& b, int tag, qint64 end){
        Character f; f.kind = Character::Font;
        const int id = b.u16();
        if(tag == 10){
            const qint64 base = b.o; const int first = b.u16(); const int count = first / 2;
            QVector<int> offs{first}; for(int i = 1; i < count; ++i) offs << b.u16();
            for(int i = 0; i < count; ++i){ Bits g(b.p, end); g.o = base + offs[i]; QPainterPath path; shape(g, 1, false, &path); f.glyphs << path; }
        } else {
            const int flags = b.u8(); b.u8(); const int nameLen = b.u8();
            f.fontName = QString::fromUtf8(QByteArray(reinterpret_cast<const char*>(b.p + b.o), int(qBound(qint64(0), qint64(nameLen), end - b.o))));
            f.fontName.remove(QChar(0)); b.o += nameLen;
            f.bold = flags & 1; f.italic = flags & 2;
            const bool wideOffsets = flags & 8;
            const int count = b.u16(); const qint64 base = b.o;
            QVector<quint32> offs; for(int i = 0; i < count && !b.atEnd(); ++i) offs << (wideOffsets ? b.u32() : quint32(b.u16()));
            for(int i = 0; i < offs.size(); ++i){ Bits g(b.p, end); g.o = base + offs[i]; QPainterPath path; shape(g, 1, false, &path); f.glyphs << path; }
            if(tag == 75) f.emSquare = 20480;
        }
        chars.insert(id, f);
    }
    void defineText(Bits& b, int tag){
        Character t; t.kind = Character::Text;
        const int id = b.u16(); b.rect(); const QTransform m = b.matrix();
        const int glyphBits = b.u8(), advanceBits = b.u8();
        int font = -1; double height = 240, x = 0, y = 0; QColor colour = Qt::black;
        int guard = 0;
        while(!b.atEnd() && ++guard < 100000){
            const int flags = b.u8(); if(!flags) break;
            if(flags & 8) font = b.u16();
            if(flags & 4) colour = tag == 33 ? b.rgba() : b.rgb();
            if(flags & 1) x = b.s16();
            if(flags & 2) y = b.s16();
            if(flags & 8) height = b.u16();
            const int count = b.u8();
            const Character& f = chars.value(font);
            QPainterPath path; path.setFillRule(Qt::OddEvenFill);
            for(int i = 0; i < count && !b.atEnd(); ++i){
                const int g = int(b.ub(glyphBits)), adv = b.sb(advanceBits);
                if(f.kind == Character::Font && g >= 0 && g < f.glyphs.size()){
                    const double sc = height / f.emSquare;
                    path.addPath((QTransform::fromScale(sc, sc) * QTransform::fromTranslate(x, y)).map(f.glyphs[g]));
                }
                x += adv;
            }
            b.align();
            if(!path.isEmpty()){ Shape s; s.type = ShapeType::Path; s.name = QStringLiteral("Text"); s.path = m.map(path); s.fill.type = FillType::Solid; s.fill.color = colour; s.stroke.enabled = false; t.shapes << s; if(st) ++st->texts; }
        }
        chars.insert(id, t);
    }
    void defineEditText(Bits& b){
        Character t; t.kind = Character::Text;
        const int id = b.u16(); const QRectF bounds = b.rect();
        const int f1 = b.u8(), f2 = b.u8();
        const bool hasText = f1 & 0x80, hasColour = f1 & 0x04, hasMax = f1 & 0x02, hasFont = f1 & 0x01;
        const bool hasFontClass = f2 & 0x80, hasLayout = f2 & 0x20, border = f2 & 0x08, html = f2 & 0x02;
        int font = -1; double height = 240; QColor colour = Qt::black; int align = 0; double left = 0;
        if(hasFont) font = b.u16();
        if(hasFontClass) b.string();
        if(hasFont || hasFontClass) height = b.u16();
        if(hasColour) colour = b.rgba();
        if(hasMax) b.u16();
        if(hasLayout){ align = b.u8(); left = b.u16(); b.u16(); b.s16(); b.s16(); }
        b.string();
        QString text = hasText ? QString::fromUtf8(b.string()) : QString();
        if(html){ text.replace(QRegularExpression(QStringLiteral("<(br|/p)\\b[^>]*>"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("\n"));
                  text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
                  text.replace(QStringLiteral("&lt;"), QStringLiteral("<")).replace(QStringLiteral("&gt;"), QStringLiteral(">")).replace(QStringLiteral("&quot;"), QStringLiteral("\"")).replace(QStringLiteral("&amp;"), QStringLiteral("&")); }
        if(border){ Shape r; r.type = ShapeType::Path; r.name = QStringLiteral("Text field"); QPainterPath p; p.addRect(bounds); r.path = p; r.fill.type = FillType::Solid; r.fill.color = Qt::white; r.stroke.color = Qt::black; r.stroke.width = 20; t.shapes << r; }
        if(!text.trimmed().isEmpty()){
            Shape s; s.type = ShapeType::Text; s.text = text.trimmed(); s.name = s.text.simplified().left(32);
            const Character& fc = chars.value(font);
            QFont qf(fc.fontName.isEmpty() || fc.fontName.startsWith(QLatin1Char('_')) ? QStringLiteral("DejaVu Sans") : fc.fontName); qf.setBold(fc.bold); qf.setItalic(fc.italic);
            qf.setPixelSize(qMax(1, qRound(height / 20)));
            s.font = qf; s.fill.type = FillType::Solid; s.fill.color = colour; s.stroke.enabled = false;
            s.textFrame = true; s.textAlign = align == 1 ? 2 : align == 2 ? 1 : align == 3 ? 3 : 0;
            s.rect = QRectF(0, 0, qMax(20.0, (bounds.width() - left) / 20), qMax(20.0, bounds.height() / 20));
            s.transform = QTransform::fromScale(20, 20) * QTransform::fromTranslate(bounds.left() + left + 40, bounds.top() + 40);   // kept in twips like the rest
            t.shapes << s; if(st) ++st->texts;
        }
        chars.insert(id, t);
    }
    void defineButton(Bits& b, int tag, qint64 end){
        Character c; c.kind = Character::Button;
        const int id = b.u16();
        if(tag == 34){ b.u8(); b.u16(); }
        int guard = 0;
        while(b.o < end && ++guard < 10000){
            const int flags = b.u8(); if(!flags) break;
            Character::Part part; part.character = b.u16(); part.depth = b.u16(); part.m = b.matrix();
            if(tag == 34) part.cx = CXform::read(b, true);
            if(flags & 0x10){ if(st) ++st->skipped; break; }                    // a filter list: its length isn't recorded, so stop
            if(flags & 0x20) b.u8();
            if(flags & 0x01) c.buttonParts << part;
        }
        std::sort(c.buttonParts.begin(), c.buttonParts.end(), [](const Character::Part& x, const Character::Part& y){ return x.depth < y.depth; });
        chars.insert(id, c);
    }
    void defineMorph(Bits& b, int tag){
        // The start shape: the start halves of the styles, then the start edges.
        Character c; c.kind = Character::Shape_;
        const int id = b.u16(); b.rect(); b.rect();
        if(tag == 84){ b.rect(); b.rect(); b.u8(); }
        b.u32();
        auto morphGradient = [&](FillStyle& f){ const int count = b.u8(); for(int i = 0; i < count; ++i){ const int r0 = b.u8(); const QColor c0 = b.rgba(); b.u8(); b.rgba(); f.stops << GradStop{r0 / 255.0, c0}; } };
        int nf = b.u8(); if(nf == 0xFF) nf = b.u16();
        QVector<FillStyle> fills;
        for(int i = 0; i < nf && !b.atEnd(); ++i){
            FillStyle f; f.type = b.u8();
            if(f.type == 0) { f.colour = b.rgba(); b.rgba(); }
            else if(f.type == 0x10 || f.type == 0x12 || f.type == 0x13){ f.m = b.matrix(); b.matrix(); morphGradient(f); if(f.type == 0x13){ f.focal = b.fixed8(); b.fixed8(); } }
            else if(f.type >= 0x40 && f.type <= 0x43){ f.bitmap = b.u16(); f.m = b.matrix(); b.matrix(); }
            fills << f;
        }
        int nl = b.u8(); if(nl == 0xFF) nl = b.u16();
        QVector<LineStyle> lines;
        for(int i = 0; i < nl && !b.atEnd(); ++i){
            LineStyle l; l.width = b.u16(); b.u16();
            if(tag == 84){ b.ub(2); const int join = int(b.ub(2)); const bool hasFill = b.ub(1); b.ub(8); b.ub(3); if(join == 2) b.u16();
                           if(hasFill){ FillStyle f; f.type = b.u8(); if(f.type == 0){ l.colour = b.rgba(); b.rgba(); } else if(f.type == 0x10 || f.type == 0x12 || f.type == 0x13){ b.matrix(); b.matrix(); morphGradient(f); l.colour = f.stops.isEmpty() ? QColor(Qt::black) : f.stops.first().color; if(f.type == 0x13){ b.fixed8(); b.fixed8(); } } else { b.u16(); b.matrix(); b.matrix(); } }
                           else { l.colour = b.rgba(); b.rgba(); } }
            else { l.colour = b.rgba(); b.rgba(); }
            lines << l;
        }
        // Parse the start edges against these styles by building a SHAPEWITHSTYLE-like stream view.
        QVector<Shape> shapes = shapeWithStyles(b, fills, lines);
        c.shapes = shapes;
        chars.insert(id, c);
    }
    QVector<Shape> shapeWithStyles(Bits& b, const QVector<FillStyle>& fills, const QVector<LineStyle>& lines){
        // shape() reads its own styles; morph shapes supply theirs, so swap them in for the edges' parse.
        QVector<Shape> out;
        presetFills = fills; presetLines = lines; usePreset = true;
        out = shape(b, 3, false, nullptr);
        usePreset = false;
        return out;
    }
    QVector<FillStyle> presetFills; QVector<LineStyle> presetLines; bool usePreset = false;

    // ---- the timeline ----
    int parseTimeline(Bits& b, qint64 end, int depth){
        const int index = timelines.size(); timelines << Timeline(); timelines[index].frames << QVector<Place>(); timelines[index].labels << QString();
        int guard = 0;
        while(b.o + 2 <= end && ++guard < 2000000){
            const int head = b.u16(); const int tag = head >> 6; qint64 len = head & 0x3f;
            if(len == 0x3f) len = b.u32();
            const qint64 start = b.o, tagEnd = qMin(end, start + len);
            if(len < 0 || start + len > end){ break; }
            Bits t(b.p, tagEnd); t.o = start;
            auto& frames = timelines[index].frames;
            switch(tag){
            case 0: b.o = end; break;
            case 1: if(frames.size() < 5000){ frames << QVector<Place>(); timelines[index].labels << QString(); } break;
            case 9: if(depth == 0) background = t.rgb(); break;
            case 2: case 22: case 32: case 83: {
                const int id = t.u16(); t.rect();
                winding = false;
                if(tag == 83){ t.rect(); winding = t.u8() & 4; }
                Character c; c.kind = Character::Shape_;
                const int ver = tag == 2 ? 1 : tag == 22 ? 2 : tag == 32 ? 3 : 4;
                if(budget > 0){ c.shapes = shape(t, ver, true); budget -= c.shapes.size(); }
                winding = false;
                chars.insert(id, c); break; }
            case 8: jpegTables = QByteArray(reinterpret_cast<const char*>(t.p + start), int(len)); break;
            case 6: case 21: case 35: case 90: {
                const int id = t.u16();
                qint64 dataLen = tagEnd - t.o, alphaAt = -1;
                if(tag == 35 || tag == 90){ dataLen = qMin<qint64>(t.u32(), tagEnd - t.o - (tag == 90 ? 2 : 0)); if(tag == 90) t.u16(); alphaAt = t.o + dataLen; }
                Character c; c.kind = Character::Bitmap;
                const QByteArray data(reinterpret_cast<const char*>(t.p + t.o), int(qMax<qint64>(0, dataLen)));
                c.image = jpeg(data).convertToFormat(QImage::Format_ARGB32);
                if(!c.image.isNull() && alphaAt >= 0 && alphaAt < tagEnd && !data.startsWith("\x89PNG") && !data.startsWith("GIF")){
                    const QByteArray a = inflate(t.p + alphaAt, tagEnd - alphaAt, qint64(c.image.width()) * c.image.height());
                    if(a.size() == c.image.width() * c.image.height())
                        for(int y = 0; y < c.image.height(); ++y){ QRgb* line = reinterpret_cast<QRgb*>(c.image.scanLine(y)); for(int x = 0; x < c.image.width(); ++x){ const int al = uchar(a[y * c.image.width() + x]);
                            line[x] = al == 0 ? 0 : qRgba(qMin(255, qRed(line[x]) * 255 / al), qMin(255, qGreen(line[x]) * 255 / al), qMin(255, qBlue(line[x]) * 255 / al), al); } }
                }
                if(c.image.isNull() && st) ++st->missingBitmaps;
                chars.insert(id, c); break; }
            case 20: case 36: { const int id = t.u16(); Character c; c.kind = Character::Bitmap; c.image = lossless(t, tagEnd, tag == 36); if(c.image.isNull() && st) ++st->missingBitmaps; chars.insert(id, c); break; }
            case 10: case 48: case 75: defineFont(t, tag, tagEnd); break;
            case 11: case 33: defineText(t, tag); break;
            case 37: defineEditText(t); break;
            case 7: case 34: defineButton(t, tag, tagEnd); break;
            case 46: case 84: defineMorph(t, tag); break;
            case 39: if(depth < 8){ const int id = t.u16(); t.u16(); Character c; c.kind = Character::Sprite; c.timeline = parseTimeline(t, tagEnd, depth + 1); chars.insert(id, c); } break;
            case 43: if(!timelines[index].labels.isEmpty()) timelines[index].labels.last() = QString::fromUtf8(t.string()); break;
            case 4: { Place p; p.character = t.u16(); p.depth = t.u16(); p.m = t.matrix(); p.hasMatrix = true; if(t.o < tagEnd){ p.cx = CXform::read(t, false); p.hasCx = true; } frames.last() << p; break; }
            case 26: case 70: {
                const int f = t.u8(); const int f2 = tag == 70 ? t.u8() : 0;
                Place p; p.move = f & 1; p.depth = t.u16();
                if(tag == 70 && ((f2 & 8) || ((f2 & 16) && (f & 2)))) t.string();      // class name
                if(f & 2) p.character = t.u16();
                if(f & 4){ p.m = t.matrix(); p.hasMatrix = true; }
                if(f & 8){ p.cx = CXform::read(t, true); p.hasCx = true; }
                if(f & 16) p.ratio = t.u16();
                if(f & 32) t.string();
                if(f & 64) p.clipDepth = t.u16();
                if(f2 & 1){                                                     // filters: a blur comes along, the rest are skipped by their sizes
                    p.blur = 0; const int count = t.u8();
                    for(int i = 0; i < count && t.o < tagEnd; ++i){
                        const int id = t.u8();
                        if(id == 1){ const double bx = t.u32() / 65536.0, by = t.u32() / 65536.0; t.u8(); p.blur = qMax(p.blur, (bx + by) / 2); }
                        else if(id == 0) t.o += 23; else if(id == 2) t.o += 15; else if(id == 3) t.o += 27;
                        else if(id == 4 || id == 7){ const int n = t.u8(); t.o += 5 * n + 19; }
                        else if(id == 5){ const int mx = t.u8(), my = t.u8(); t.o += 8 + 4 * mx * my + 5; }
                        else if(id == 6) t.o += 80;
                        else break;
                    }
                }
                if((f2 & 2) && t.o < tagEnd) p.blend = t.u8();
                frames.last() << p; break; }
            case 5: { t.u16(); Place p; p.remove = true; p.depth = t.u16(); frames.last() << p; break; }
            case 28: { Place p; p.remove = true; p.depth = t.u16(); frames.last() << p; break; }
            default: break;
            }
            b.o = tagEnd;
        }
        // A trailing empty frame (no ShowFrame after it) isn't a frame.
        if(timelines[index].frames.size() > 1 && timelines[index].frames.last().isEmpty()){ timelines[index].frames.removeLast(); timelines[index].labels.removeLast(); }
        return index;
    }

    // ---- playing it ----
    struct Instance { int character = -1, placedAt = 0, clipDepth = 0, blend = 0; double blur = 0; QTransform m; CXform cx; };
    // The display list of `timeline` after `frame` frames.
    QMap<int, Instance> displayList(int timeline, int frame){
        QMap<int, Instance> list;
        const Timeline& tl = timelines[timeline];
        for(int f = 0; f <= frame && f < tl.frames.size(); ++f) for(const Place& p : tl.frames[f]){
            if(p.remove){ list.remove(p.depth); continue; }
            auto it = list.find(p.depth);
            if(p.move && it != list.end()){
                if(p.character >= 0 && p.character != it->character){ it->character = p.character; it->placedAt = f; }
                if(p.hasMatrix) it->m = p.m; if(p.hasCx) it->cx = p.cx; if(p.clipDepth) it->clipDepth = p.clipDepth;
                if(p.blend) it->blend = p.blend; if(p.blur >= 0) it->blur = p.blur;
            } else if(p.character >= 0){
                Instance in; in.character = p.character; in.placedAt = f; in.m = p.m; in.cx = p.cx; in.clipDepth = p.clipDepth; in.blend = p.blend; in.blur = qMax(0.0, p.blur); list.insert(p.depth, in);
            }
        }
        return list;
    }
    static void recolour(Shape& s, const CXform& cx){
        if(cx.identity()) return;
        if(s.type == ShapeType::Group){ for(Shape& k : s.children) recolour(k, cx); return; }
        if(s.fill.type == FillType::Solid) s.fill.color = cx.apply(s.fill.color);
        for(GradStop& g : s.fill.stops) g.color = cx.apply(g.color);
        if(s.fill.type == FillType::Bitmap && (cx.m[3] != 1 || cx.a[3] != 0)) s.opacity *= qBound(0.0, cx.m[3] + cx.a[3] / 255.0, 1.0);
        if(s.stroke.enabled) s.stroke.color = cx.apply(s.stroke.color);
        for(TextRun& r : s.textRuns) r.color = cx.apply(r.color);
    }
    static void place(Shape& s, const QTransform& m){
        if(s.type == ShapeType::Path){
            s.path = m.map(s.path);
            const double sc = std::sqrt(qAbs(m.determinant()));
            if(s.stroke.enabled) s.stroke.width *= sc;
            if(s.fill.type == FillType::Linear || s.fill.type == FillType::Radial || s.fill.type == FillType::Bitmap){ s.fill.p0 = m.map(s.fill.p0); s.fill.p1 = m.map(s.fill.p1); s.fill.p2 = m.map(s.fill.p2); }
        } else s.transform = s.transform * m;
    }
    // What a character draws at `frame` frames into its life (a sprite's own clock), in the parent's twips.
    QVector<Shape> draw(int id, int age, const QTransform& m, const CXform& cx, int depth){
        QVector<Shape> out; if(depth > 16 || budget <= 0) return out;
        const Character& c = chars.value(id);
        if(c.kind == Character::Shape_ || c.kind == Character::Text){
            for(Shape s : c.shapes){ place(s, m); recolour(s, cx); out << s; }
        } else if(c.kind == Character::Bitmap){
            if(!c.image.isNull()){ Shape s = psdBitmapShape(c.image.convertToFormat(QImage::Format_ARGB32_Premultiplied), 0, 0, QStringLiteral("Bitmap"));
                                   s.transform = QTransform::fromScale(20, 20) * m; recolour(s, cx); out << s; }
        } else if(c.kind == Character::Button){
            for(const Character::Part& p : c.buttonParts) out += draw(p.character, age, p.m * m, p.cx.then(cx), depth + 1);
        } else if(c.kind == Character::Sprite && c.timeline >= 0){
            const int n = qMax(1, int(timelines[c.timeline].frames.size()));
            out += frameShapes(c.timeline, age % n, m, cx, depth + 1);
        }
        budget -= out.size();
        return out;
    }
    QVector<Shape> frameShapes(int timeline, int frame, const QTransform& m, const CXform& cx, int depth){
        QVector<Shape> out;
        const QMap<int, Instance> list = displayList(timeline, frame);
        // Clip layers: a character with a clip depth masks the depths above it up to that depth.
        struct Clip { int until; QVector<Shape> mask; QVector<Shape> content; };
        QVector<Clip> clips;
        for(auto it = list.constBegin(); it != list.constEnd(); ++it){
            while(!clips.isEmpty() && it.key() > clips.last().until){
                Clip c = clips.takeLast();
                Shape g; g.type = ShapeType::Group; g.name = QStringLiteral("Mask"); g.isClipView = true; g.children = c.mask + c.content; g.clipSplit = c.mask.size();
                if(c.mask.isEmpty() || c.content.isEmpty()) g.isClipView = false;
                QVector<Shape>& target = clips.isEmpty() ? out : clips.last().content;
                if(!c.content.isEmpty()) target << g;
            }
            const Instance& in = it.value();
            QVector<Shape> v = draw(in.character, frame - in.placedAt, in.m * m, in.cx.then(cx), depth);
            if(in.clipDepth > 0){                                               // a mask: only its fills count, and it isn't drawn
                QVector<Shape> keyholes; std::function<void(const Shape&, const QTransform&)> flat = [&](const Shape& x, const QTransform& t){
                    if(x.type == ShapeType::Group){ for(const Shape& k : x.children) flat(k, x.transform * t); return; }
                    if(x.fill.type == FillType::None) return;
                    Shape k; k.type = ShapeType::Path; k.name = QStringLiteral("Mask"); k.path = (x.transform * t).map(x.localPath()); k.fill.type = FillType::None; k.stroke.enabled = false;
                    keyholes << k; };
                for(const Shape& x : v) flat(x, QTransform());
                clips << Clip{in.clipDepth, keyholes, {}}; continue;
            }
            // A blend mode or a blur applies to the instance as a whole.
            static const QHash<int, QPainter::CompositionMode> modes = {{3, QPainter::CompositionMode_Multiply}, {4, QPainter::CompositionMode_Screen},
                {5, QPainter::CompositionMode_Lighten}, {6, QPainter::CompositionMode_Darken}, {7, QPainter::CompositionMode_Difference}, {8, QPainter::CompositionMode_Plus},
                {9, QPainter::CompositionMode_Difference}, {13, QPainter::CompositionMode_Overlay}, {14, QPainter::CompositionMode_HardLight}};
            if(!v.isEmpty() && (modes.contains(in.blend) || in.blur > 0)){
                Shape g; g.type = ShapeType::Group; g.name = QStringLiteral("Instance"); g.children = v; g.isolate = true;
                g.blend = modes.value(in.blend, QPainter::CompositionMode_SourceOver);
                if(in.blur > 0) g.blur = in.blur * 20 * std::sqrt(qAbs((in.m * m).determinant())) / 20;   // in pixels, as the stage scales
                v = {g};
            }
            if(!clips.isEmpty()) clips.last().content += v; else out += v;
        }
        while(!clips.isEmpty()){
            Clip c = clips.takeLast();
            Shape g; g.type = ShapeType::Group; g.name = QStringLiteral("Mask"); g.isClipView = !c.mask.isEmpty(); g.children = c.mask + c.content; g.clipSplit = c.mask.size();
            QVector<Shape>& target = clips.isEmpty() ? out : clips.last().content;
            if(!c.content.isEmpty()) target << g;
        }
        return out;
    }
};

} // namespace

Document importSwfDocument(const QByteArray& file, SwfImportStats* st){
    Document doc;
    if(file.size() < 21) return doc;
    const char sig = file[0];
    if(file.mid(1, 2) != "WS" || (sig != 'F' && sig != 'C' && sig != 'Z')) return doc;
    const int version = uchar(file[3]);
    const quint32 length = quint32(uchar(file[4])) | quint32(uchar(file[5])) << 8 | quint32(uchar(file[6])) << 16 | quint32(uchar(file[7])) << 24;
    QByteArray body;
    const qint64 want = qBound<qint64>(0, qint64(length) - 8, qint64(256) << 20);
    if(sig == 'F') body = file.mid(8);
    else if(sig == 'C'){
        z_stream z{}; if(inflateInit(&z) != Z_OK) return doc;
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(file.constData()) + 8); z.avail_in = uInt(file.size() - 8);
        body.resize(int(qMax<qint64>(want, 64))); int rc;
        do { if(qint64(z.total_out) >= body.size()){ if(body.size() >= (256 << 20)) break; body.resize(qMin(body.size() * 2, 256 << 20)); }
             z.next_out = reinterpret_cast<Bytef*>(body.data()) + z.total_out; z.avail_out = uInt(body.size() - qint64(z.total_out)); rc = inflate(&z, Z_NO_FLUSH); }
        while(rc == Z_OK);
        body.resize(int(z.total_out)); inflateEnd(&z);
    } else {
#ifdef HAVE_LZMA
        // LZMA: the compressed size, the 5-byte properties, then the stream; liblzma reads it as an .lzma file.
        if(file.size() < 17) return doc;
        QByteArray alone = file.mid(12, 5);
        for(int i = 0; i < 8; ++i) alone += char(i < 4 ? (quint64(want) >> (8 * i)) & 0xff : 0);
        alone += file.mid(17);
        lzma_stream z = LZMA_STREAM_INIT;
        if(lzma_alone_decoder(&z, UINT64_MAX) != LZMA_OK) return doc;
        body.resize(int(qMax<qint64>(want, 64)));
        z.next_in = reinterpret_cast<const uint8_t*>(alone.constData()); z.avail_in = size_t(alone.size());
        z.next_out = reinterpret_cast<uint8_t*>(body.data()); z.avail_out = size_t(body.size());
        lzma_ret rc; do { rc = lzma_code(&z, LZMA_FINISH); } while(rc == LZMA_OK && z.avail_out > 0 && z.avail_in > 0);
        body.resize(int(z.total_out)); lzma_end(&z);
#else
        if(st) st->lzma = true;
        return doc;
#endif
    }
    if(body.size() < 10) return doc;
    Swf swf; swf.st = st; swf.version = version;
    Bits b(reinterpret_cast<const uchar*>(body.constData()), body.size());
    const QRectF stage = b.rect();
    const double rate = b.u16() / 256.0; b.u16();
    const int main = swf.parseTimeline(b, body.size(), 0);
    if(swf.timelines.isEmpty()) return doc;
    const QSizeF page(qBound(1.0, stage.width() / 20, 20000.0), qBound(1.0, stage.height() / 20, 20000.0));
    const QTransform toPx = QTransform::fromTranslate(-stage.left(), -stage.top()) * QTransform::fromScale(0.05, 0.05);
    const int frames = swf.timelines[main].frames.size();
    doc.pageSize = page;
    const int delay = rate > 0 ? qBound(10, int(std::round(1000 / rate)), 60000) : 83;
    // The stage colour: a layer behind every frame.
    Layer bgLayer; bgLayer.name = QStringLiteral("Background"); bgLayer.frameBackground = true;
    Shape bg; bg.type = ShapeType::Path; bg.name = QStringLiteral("Stage"); QPainterPath bp; bp.addRect(QRectF(QPointF(0, 0), page)); bg.path = bp;
    bg.fill.type = FillType::Solid; bg.fill.color = swf.background; bg.stroke.enabled = false;
    if(frames <= 1){
        doc.shapes << bg;
        for(Shape s : swf.frameShapes(main, 0, QTransform(), CXform(), 0)){ Swf::place(s, toPx); doc.shapes << s; }
        if(st) st->frames = 1;
    } else {
        doc.layers.clear(); doc.layers << bgLayer; bg.layer = 0; doc.shapes << bg;
        QString tag; int tagFrom = 0;
        for(int f = 0; f < frames && swf.budget > 0; ++f){
            Layer L; L.name = QStringLiteral("Frame %1").arg(f + 1); L.visible = f == 0; L.frameDelayMs = delay;
            doc.layers << L;
            for(Shape s : swf.frameShapes(main, f, QTransform(), CXform(), 0)){ Swf::place(s, toPx); s.layer = doc.layers.size() - 1; doc.shapes << s; }
            const QString label = swf.timelines[main].labels.value(f);
            if(!label.isEmpty()){ if(!tag.isEmpty()) doc.frameTags << FrameTag{tag, tagFrom, f - 1}; tag = label; tagFrom = f; }
            if(st) st->frames = f + 1;
        }
        if(!tag.isEmpty()) doc.frameTags << FrameTag{tag, tagFrom, (st ? st->frames : frames) - 1};
    }
    if(st){ st->version = version; st->frameRate = rate; }
    return doc;
}

} // namespace xara
