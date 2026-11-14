// Autodesk DWF: a "(DWF V06.00)" package is a ZIP whose sections each hold a W2D ("WHIP!") stream; older files
// ("(DWF V00.xx)") are one bare stream after the header. W2D is a run of opcodes, single bytes for the
// binary forms and letters or parenthesised expressions for the ASCII ones: colour (RGBA, or an index into
// the AutoCAD colour map), line weight, fill on/off, visibility, and geometry in 32-bit (or 16-bit, relative
// to the last point) logical coordinates: lines, polylines and polygons, circles and ellipses (with arcs),
// polytriangles, Bézier curves and text. Extended binary objects carry their size, so unknown ones are
// skipped. Newer streams deflate the rest of the drawing with zlib against a preset dictionary that WHIP builds
// from strings and colours it expects to see (generated below, byte for byte); the older WHIP LZ scheme isn't
// read. Each stream becomes a page, its drawing fitted to it with y up. DWFx (XPS) goes to the XPS reader.
#include "prepress.h"
#include <QHash>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QXmlStreamReader>
#include <QtMath>
#include <cstring>
#include <functional>
#include <zlib.h>

namespace xara {
namespace {

// The default colour map of streams from revision 00.38 on: the Windows halftone palette (20 system colours
// around a 6x6x6 cube and a grey ramp).
QRgb windowsColour(int i){
    i &= 255;
    static const QRgb sys[20] = { 0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0, 0xc0dcc0, 0xa6caf0,
                                  0xfffbf0, 0xa0a0a4, 0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff };
    if(i < 10) return 0xff000000 | sys[i];
    if(i < 226){ const int k = i - 10; return qRgb(k / 36 * 51, k / 6 % 6 * 51, k % 6 * 51); }
    if(i < 246){ const int g = (i - 226) * 255 / 19; return qRgb(g, g, g); }
    return 0xff000000 | sys[i - 236];
}
// Older streams default to AutoCAD's colour index (colour 7 black, for white paper): 24 hues in 15 degree steps
// at five brightnesses, each full and half saturated, then a grey ramp.
QRgb acadColour(int i){
    i &= 255;
    static const QRgb base[10] = { 0x000000, 0xff0000, 0xffff00, 0x00ff00, 0x00ffff, 0x0000ff, 0xff00ff, 0x000000, 0x808080, 0xdcdcdc };
    if(i < 10) return 0xff000000 | base[i];
    if(i >= 250){ static const int g[6] = {84, 118, 152, 186, 220, 255}; return qRgb(g[i - 250], g[i - 250], g[i - 250]); }
    static const int vals[5] = {255, 221, 184, 149, 114};
    const int k = i - 10, off = k % 10;
    const double v = vals[off / 2], sat = (off & 1) ? 0.5 : 1.0, h = (k / 10) * 0.25, f = h - std::floor(h);
    const double lo = v * (1 - sat), dn = v * (1 - sat * f), up = v * (1 - sat * (1 - f));
    const double rgb[6][3] = { {v, up, lo}, {dn, v, lo}, {lo, v, up}, {lo, dn, v}, {up, lo, v}, {v, lo, dn} };
    const double* c = rgb[int(h) % 6];
    return qRgb(int(c[0] + 1e-9), int(c[1] + 1e-9), int(c[2] + 1e-9));
}

// The preset dictionary of WHIP's zlib streams: the expected extended-ASCII keywords, a colour-map opcode with
// the default palette, every four-digit number (as the de Bruijn sequence of order 4 over the ten digits), and
// the head of a second colour map.
const QByteArray& zlibDictionary(){
    static const QByteArray dict = []{
        QByteArray d;
        auto words = [&](std::initializer_list<const char*> w){ for(const char* k : w){ d += '('; d += k; d += ' '; } };
        const char cmap[] = "{\x04\x04\x00\x00\x01\x00\x00";
        words({"DashPattern", "LineStyle", "Copyright", "Keywords", "Viewport", "CodePage"});
        d.append(cmap, 8);
        for(int i = 0; i < 256; ++i){ const QRgb c = windowsColour(i); d += char(qBlue(c)); d += char(qGreen(c)); d += char(qRed(c)); d += char(0xff); }
        words({"NamedView", "Author", "Background", "Bezier", "Bounds", "Clip", "Color", "ColorMap", "Comment", "Created", "Creator", "Description", "DrawingInfo"});
        d += ")(Embed 'image/vnd.dwg;' 'AutoCAD-r13' 'unknown.dwg' '')";
        words({"View", "Gouraud", "Image", "Layer", "LineCap", "LineJoin", "LinePattern", "LineWeight", "Modified", "Projection", "Scale",
               "SourceCreated", "SourceFilename", "SourceModified", "Text"});
        d += "(URL 'http://www.com')'ftp://ftp.";
        int a[9] = {}; QByteArray seq;                                          // Fredricksen-Kessler-Maiorana
        std::function<void(int, int)> gen = [&](int t, int pr){
            if(t > 4){ if(4 % pr == 0) for(int j = 1; j <= pr; ++j) seq += char('0' + a[j]); return; }
            a[t] = a[t - pr]; gen(t + 1, pr);
            for(int j = a[t - pr] + 1; j < 10; ++j){ a[t] = j; gen(t + 1, t); }
        };
        gen(1, 1);
        d += seq; d += seq.left(3);
        d.append(cmap, 8); d += char(0xff);
        return d;
    }();
    return dict;
}

// AutoCAD's multiline-text codes as plain text: \P breaks the line, {} group, \~ is a hard space, a stacked
// fraction \Sa^b; reads a/b, and the formatting codes (\H height, \f font, \C colour, \L underline...) drop out.
// %%d, %%p and %%c are the degree, plus-minus and diameter signs.
QString mtextPlain(const QString& in){
    if(!in.contains(QLatin1Char('\\')) && !in.contains(QLatin1Char('{')) && !in.contains(QLatin1String("%%"))) return in;
    QString out; out.reserve(in.size());
    for(int i = 0; i < in.size(); ++i){
        const QChar c = in[i];
        if(c == QLatin1Char('{') || c == QLatin1Char('}')) continue;
        if(c == QLatin1Char('%') && i + 2 < in.size() && in[i + 1] == QLatin1Char('%')){
            const QChar k = in[i + 2].toLower();
            if(k == QLatin1Char('d')){ out += QChar(0x00b0); i += 2; continue; }
            if(k == QLatin1Char('p')){ out += QChar(0x00b1); i += 2; continue; }
            if(k == QLatin1Char('c')){ out += QChar(0x2300); i += 2; continue; }
            if(k == QLatin1Char('u') || k == QLatin1Char('o') || k == QLatin1Char('k')){ i += 2; continue; }
            if(k == QLatin1Char('%')){ out += QLatin1Char('%'); i += 2; continue; }
        }
        if(c != QLatin1Char('\\') || i + 1 >= in.size()){ out += c; continue; }
        const QChar k = in[++i];
        switch(k.unicode()){
        case 'P': case 'X': out += QLatin1Char('\n'); break;
        case '~': out += QLatin1Char(' '); break;
        case '\\': case '{': case '}': out += k; break;
        case 'L': case 'l': case 'O': case 'o': case 'K': case 'k': break;
        case 'S': { const int e = in.indexOf(QLatin1Char(';'), i); const QString f = in.mid(i + 1, (e < 0 ? in.size() : e) - i - 1);
                    out += QString(f).replace(QLatin1Char('^'), QLatin1Char('/')).replace(QLatin1Char('#'), QLatin1Char('/')); i = e < 0 ? in.size() : e; break; }
        case 'U': if(i + 5 < in.size() && in[i + 1] == QLatin1Char('+')){ bool ok = false; const uint u = in.mid(i + 2, 4).toUInt(&ok, 16); if(ok){ out += QChar(char16_t(u)); i += 5; break; } }
                  out += k; break;
        default:
            if(QStringLiteral("HWQTACcFfp").contains(k)){ const int e = in.indexOf(QLatin1Char(';'), i); i = e < 0 ? in.size() : e; }
            else out += k;
        }
    }
    return out;
}

struct W2d {
    const uchar* p; qint64 n, o = 0;
    QPointF cur;
    QRgb colour = 0xff000000; bool fill = false, visible = true; double weight = 0, fontHeight = 0; int fontRotation = 0; QString fontName;
    QVector<QRgb> map;
    QVector<Shape> shapes; DwfImportStats* st; int depth = 0, revision;
    W2d(const uchar* d, qint64 len, DwfImportStats* s, int rev) : p(d), n(len), st(s), revision(rev) { for(int i = 0; i < 256; ++i) map << (rev >= 38 ? windowsColour(i) : acadColour(i)); }

    bool have(qint64 k) const { return k >= 0 && o + k <= n; }
    int u8(){ return have(1) ? p[o++] : (o = n, 0); }
    int i16(){ if(!have(2)){ o = n; return 0; } const int v = qint16(p[o] | p[o + 1] << 8); o += 2; return v; }
    int u16(){ if(!have(2)){ o = n; return 0; } const int v = p[o] | p[o + 1] << 8; o += 2; return v; }
    qint64 i32(){ if(!have(4)){ o = n; return 0; } const qint32 v = qint32(quint32(p[o]) | quint32(p[o + 1]) << 8 | quint32(p[o + 2]) << 16 | quint32(p[o + 3]) << 24); o += 4; return v; }
    qint64 u32(){ return quint32(i32()); }
    int count(){ const int c = u8(); return c ? c : 256 + u16(); }
    void ws(){ while(o < n && (p[o] == ' ' || p[o] == '\t' || p[o] == '\n' || p[o] == '\r')) ++o; }
    qint64 asciiInt(){ ws(); qint64 v = 0; bool neg = false; if(o < n && (p[o] == '-' || p[o] == '+')) neg = p[o++] == '-'; while(o < n && p[o] >= '0' && p[o] <= '9' && v < (qint64(1) << 40)) v = v * 10 + (p[o++] - '0'); return neg ? -v : v; }
    QPointF asciiPoint(){ const double x = double(asciiInt()); ws(); if(o < n && p[o] == ',') ++o; const double y = double(asciiInt()); return QPointF(x, y); }
    QPointF rel32(){ const double x = double(i32()); const double y = double(i32()); cur += QPointF(x, y); return cur; }   // x first: arguments have no order
    QPointF rel16(){ const int x = i16(); const int y = i16(); cur += QPointF(x, y); return cur; }
    QVector<QPointF> set32(){ QVector<QPointF> v; const int c = count(); for(int i = 0; i < c && o < n; ++i) v << rel32(); return v; }
    QVector<QPointF> set16(){ QVector<QPointF> v; const int c = count(); for(int i = 0; i < c && o < n; ++i) v << rel16(); return v; }
    QRgb colour4(){                                                            // RGBA since revision 00.33, BGRA before
        if(!have(4)){ o = n; return 0xff000000; }
        const uchar* c = p + o; o += 4;
        return revision >= 33 ? qRgba(c[0], c[1], c[2], c[3]) : qRgba(c[2], c[1], c[0], c[3]);
    }
    QVector<QPointF> gouraudSet(bool b16, QRgb* average){                      // points, each with its colour
        QVector<QPointF> v; const int c = count(); qint64 r = 0, g = 0, b = 0;
        for(int i = 0; i < c && o < n; ++i){ v << (b16 ? rel16() : rel32()); const QRgb k = colour4(); r += qRed(k); g += qGreen(k); b += qBlue(k); }
        if(!v.isEmpty()) *average = qRgb(int(r / v.size()), int(g / v.size()), int(b / v.size()));
        return v;
    }
    QVector<QPointF> setAscii(){ QVector<QPointF> v; const qint64 c = asciiInt(); for(qint64 i = 0; i < c && i < 100000 && o < n; ++i) v << asciiPoint(); return v; }
    QString whipString(){
        ws();
        if(o < n && p[o] == '{'){ ++o; const qint64 len = i32(); if(len < 0 || !have(len * 2)){ o = n; return QString(); }
            const QString s = QString::fromUtf16(reinterpret_cast<const char16_t*>(p + o), qsizetype(len)); o += len * 2; if(o < n && p[o] == '}') ++o; return s; }
        if(o < n && (p[o] == '"' || p[o] == '\'')){ const uchar q = p[o++]; QByteArray b; while(o < n && p[o] != q){ if(p[o] == '\\' && o + 1 < n) ++o; b.append(char(p[o++])); } ++o; return QString::fromLatin1(b); }
        const qint64 s = o; while(o < n && p[o] != ' ' && p[o] != '\t' && p[o] != '\n' && p[o] != '\r' && p[o] != ')') ++o;
        return QString::fromLatin1(reinterpret_cast<const char*>(p + s), int(o - s));
    }

    void emitPath(const QPainterPath& path, bool closed){
        if(!visible || path.isEmpty() || shapes.size() > 2000000) return;
        Shape s; s.type = ShapeType::Path; s.path = path; s.fillRule = path.fillRule(); s.name = closed && fill ? QStringLiteral("Fill") : QStringLiteral("Line");
        if(closed && fill){ s.fill.type = FillType::Solid; s.fill.color = QColor::fromRgba(colour); s.stroke.enabled = false; }
        else { s.fill.type = FillType::None; s.stroke.enabled = true; s.stroke.color = QColor::fromRgba(colour); s.stroke.width = weight; s.stroke.cap = Qt::RoundCap; }
        shapes << s; if(st) ++st->objects;
    }
    void poly(const QVector<QPointF>& v, bool closeIfFill){
        if(v.size() < 2) return;
        QPainterPath path(v[0]); for(int i = 1; i < v.size(); ++i) path.lineTo(v[i]);
        const bool closed = closeIfFill && fill && v.size() >= 3;
        if(closed) path.closeSubpath();
        emitPath(path, closed);
    }
    void ellipse(QPointF c, double a, double b, int start, int end, int tilt){
        if(start == 0 && end == 0) end = 65536; else if(end <= start) end += 65536;
        const int segs = qBound(16, int(std::ceil((end - start) / 65536.0 * 96)), 144);
        const double t = tilt * 2 * M_PI / 65536.0;
        QPainterPath path;
        for(int i = 0; i <= segs; ++i){
            const double ang = (start + (end - start) * double(i) / segs) * 2 * M_PI / 65536.0;
            const double x = std::cos(ang) * a, y = std::sin(ang) * b;
            const QPointF q(c.x() + x * std::cos(t) - y * std::sin(t), c.y() + x * std::sin(t) + y * std::cos(t));
            if(i == 0) path.moveTo(q); else path.lineTo(q);
        }
        const bool whole = end - start >= 65536;
        if(whole) path.closeSubpath();
        emitPath(path, whole);
    }
    void triangles(const QVector<QPointF>& v){                                 // a strip: each point makes a triangle with the two before
        if(v.size() < 3) return;
        QPainterPath path; path.setFillRule(Qt::WindingFill);
        for(int i = 2; i < v.size(); ++i){ path.moveTo(v[i - 2]); path.lineTo(v[i - 1]); path.lineTo(v[i]); path.closeSubpath(); }
        const bool was = fill; fill = true; emitPath(path, true); fill = was;
    }
    void text(QPointF at, const QString& s){
        if(!visible || s.trimmed().isEmpty()) return;
        const QString plain = mtextPlain(s);
        if(plain.trimmed().isEmpty()) return;
        Shape t; t.type = ShapeType::Text; t.text = plain; t.name = plain.simplified().left(32);
        t.font = QFont(fontName.isEmpty() ? QStringLiteral("DejaVu Sans") : fontName); t.font.setPixelSize(qMax(1, int(fontHeight > 0 ? fontHeight : 100)));
        t.fill.type = FillType::Solid; t.fill.color = QColor::fromRgba(colour); t.stroke.enabled = false;
        // the position is the first baseline; a text shape hangs from its top
        t.transform = QTransform::fromTranslate(0, -QFontMetricsF(t.font).ascent()) * QTransform::fromScale(1, -1)
                    * QTransform().rotate(fontRotation * 360.0 / 65536.0) * QTransform::fromTranslate(at.x(), at.y());
        shapes << t; if(st) ++st->texts;
    }

    // A parenthesised (extended ASCII) object.
    void extended(){
        const qint64 start = o; int nest = 0; uchar quote = 0;
        while(o < n){
            const uchar c = p[o++];
            if(quote){ if(c == '\\') ++o; else if(c == quote) quote = 0; }
            else if(c == '"' || c == '\'') quote = c;
            else if(c == '(') ++nest;
            else if(c == ')' && --nest <= 0) break;
        }
        const QByteArray e(reinterpret_cast<const char*>(p + start), int(qMin<qint64>(o - start, 4096)));
        const int sp = e.indexOf(' ');
        const QByteArray tok = e.mid(1, sp > 0 ? sp - 1 : e.size() - 2);
        auto nums = [&](){ QVector<double> v; for(const QByteArray& w : e.mid(tok.size() + 1).replace(',', ' ').replace(')', ' ').split(' ')){ bool ok = false; const double d = w.toDouble(&ok); if(ok) v << d; } return v; };
        if(tok == "Color"){ const QVector<double> v = nums(); if(v.size() >= 3) colour = qRgba(int(v[0]) & 255, int(v[1]) & 255, int(v[2]) & 255, v.size() >= 4 ? int(v[3]) & 255 : 255); }
        else if(tok == "LineWeight"){ const QVector<double> v = nums(); if(!v.isEmpty()) weight = qAbs(v[0]); }
        else if(tok == "Font"){
            const int h = e.indexOf("(Height "); if(h >= 0) fontHeight = qAbs(e.mid(h + 8, 16).split(')')[0].toDouble());
            const int r = e.indexOf("(Rotation "); if(r >= 0) fontRotation = e.mid(r + 10, 16).split(')')[0].toInt();
            const int nm = e.indexOf("(Name "); if(nm >= 0){ QByteArray v = e.mid(nm + 6, 128); v = v.left(v.indexOf(')')); fontName = QString::fromLatin1(v).remove(QLatin1Char('"')).remove(QLatin1Char('\'')).trimmed(); }
        }
        else if(tok == "ColorMap"){ const QVector<double> v = nums(); if(!v.isEmpty()){ const int c = qBound(0, int(v[0]), 256); map.clear(); for(int i = 0; i < c; ++i) map << (1 + i * 4 + 3 < v.size() ? qRgba(int(v[1 + i * 4]) & 255, int(v[2 + i * 4]) & 255, int(v[3 + i * 4]) & 255, int(v[4 + i * 4]) & 255) : 0xff000000); } }
        else if(tok == "EndOfDWF") o = n;
        else if(st) ++st->skipped;
    }
    // A sized (extended binary) object: '{', a 4-byte size counted from the opcode to the closing '}', the opcode.
    // A zlib block may leave the size 0: it runs to the end of its deflate stream, and the rest of the drawing is in it.
    void extendedBinary(){
        const qint64 sizeAt = o; const qint64 size = u32(); const int op = u16();
        const qint64 end = sizeAt + 4 + size;
        if(op == 0x0011 && depth < 4){
            QByteArray out; z_stream z; std::memset(&z, 0, sizeof z);
            if(inflateInit(&z) != Z_OK){ o = n; return; }
            z.next_in = const_cast<Bytef*>(p + o); z.avail_in = uInt(qMin<qint64>((size >= 2 && end <= n) ? end - o : n - o, qint64(UINT_MAX)));
            char buf[65536]; int rc;
            do {
                z.next_out = reinterpret_cast<Bytef*>(buf); z.avail_out = sizeof buf; rc = inflate(&z, Z_NO_FLUSH);
                if(rc == Z_NEED_DICT){ const QByteArray& d = zlibDictionary(); rc = inflateSetDictionary(&z, reinterpret_cast<const Bytef*>(d.constData()), uInt(d.size())); continue; }
                out.append(buf, int(sizeof buf - z.avail_out));
            } while(rc == Z_OK && out.size() < (256 << 20));
            o = qint64(z.next_in - p);                                         // not total_in: it leaves out the header read before Z_NEED_DICT
            inflateEnd(&z);
            if(!out.isEmpty()){
                W2d inner(reinterpret_cast<const uchar*>(out.constData()), out.size(), st, 600);
                inner.cur = cur; inner.colour = colour; inner.fill = fill; inner.visible = visible; inner.weight = weight; inner.map = map; inner.depth = depth + 1;
                inner.fontName = fontName; inner.fontHeight = fontHeight; inner.fontRotation = fontRotation;
                inner.run();
                shapes += inner.shapes; cur = inner.cur; colour = inner.colour; fill = inner.fill; visible = inner.visible; weight = inner.weight; map = inner.map;
                fontName = inner.fontName; fontHeight = inner.fontHeight; fontRotation = inner.fontRotation;
            }
            if(size >= 2 && end <= n) o = end; else { ws(); if(o < n && p[o] == '}') ++o; }
            return;
        }
        if(size < 2 || end > n){ o = n; return; }
        if(op == 0x0001){                                                       // a colour map: a count (0 means 256), then BGRA
            const int c = u8() ? p[o - 1] : 256; map.clear();
            for(int i = 0; i < c && have(4); ++i) map << colour4();
            while(map.size() < 256) map << 0xff000000;
        } else if(op == 0x0002 || (op >= 0x0004 && op <= 0x0008) || op == 0x000C) image(op, end);
        else if(op == 0x0010 || op == 0x0123){ if(st) ++st->lzCompressed; }  // WHIP's own LZ scheme
        else if(st) ++st->skipped;
        o = end;
    }

    // A raster between two corners: JPEG and PNG as they are, raw RGB(A), indexed (the current colour map) or mapped
    // (its own map, 1 or 8 bits a pixel). Row 0 is the top. Group 3/4 fax images aren't read.
    void image(int op, qint64 end){
        const int cols = u16(), rows = u16();
        const double x0 = double(i32()), y0 = double(i32()), x1 = double(i32()), y1 = double(i32());
        i32();                                                                  // identifier
        QVector<QRgb> pal;
        if(op == 0x0002 || op == 0x0005){ const int c = u8() ? p[o - 1] : 256; for(int i = 0; i < c && have(4); ++i) pal << colour4(); }
        const qint64 size = u32();
        if(size <= 0 || !have(size) || o + size > end || cols <= 0 || rows <= 0 || qint64(cols) * rows > (qint64(1) << 26)){ if(st) ++st->skipped; return; }
        const uchar* d = p + o;
        QImage img;
        if(op == 0x0008 || op == 0x000C) img = QImage::fromData(d, int(size));
        else {
            const int bpp = op == 0x0006 ? 24 : op == 0x0007 ? 32 : op == 0x0002 ? 1 : (op == 0x0004 || op == 0x0005) ? 8 : 0;
            const qint64 stride = (qint64(cols) * bpp + 7) / 8;
            if(bpp && stride * rows <= size){
                img = QImage(cols, rows, QImage::Format_ARGB32);
                const QVector<QRgb>& m = op == 0x0004 ? map : pal;
                for(int y = 0; y < rows; ++y){
                    const uchar* r = d + y * stride; QRgb* out = reinterpret_cast<QRgb*>(img.scanLine(y));
                    for(int x = 0; x < cols; ++x){
                        if(bpp == 24) out[x] = qRgb(r[x * 3], r[x * 3 + 1], r[x * 3 + 2]);
                        else if(bpp == 32) out[x] = qRgba(r[x * 4], r[x * 4 + 1], r[x * 4 + 2], r[x * 4 + 3]);
                        else { const int i = bpp == 8 ? r[x] : (r[x >> 3] >> (7 - (x & 7))) & 1; out[x] = m.value(i, 0xff000000); }
                    }
                }
            }
        }
        if(img.isNull()){ if(st) ++st->skipped; return; }
        if(!visible) return;
        Shape s = psdBitmapShape(img.convertToFormat(QImage::Format_ARGB32_Premultiplied), 0, 0, QStringLiteral("Image"));
        const double l = qMin(x0, x1), r = qMax(x0, x1), b = qMin(y0, y1), t = qMax(y0, y1);
        s.transform = QTransform((r - l) / cols, 0, 0, -(t - b) / rows, l, t);     // top row at the top, y going up
        shapes << s; if(st) ++st->images;
    }
    // Filled outlines, holes cut by the even-odd rule.
    void contours(bool b16){
        const int nc = count(); QVector<int> counts;
        for(int i = 0; i < nc && o < n; ++i) counts << count();
        QPainterPath path; path.setFillRule(Qt::OddEvenFill);
        for(int c : std::as_const(counts)){
            QPolygonF poly; for(int i = 0; i < c && o < n; ++i) poly << (b16 ? rel16() : rel32());
            if(poly.size() >= 3){ path.addPolygon(poly); path.closeSubpath(); }
        }
        const bool was = fill; fill = true; emitPath(path, true); fill = was;
    }
    void bezier(const QVector<QPointF>& v){
        if(v.size() < 4){ poly(v, false); return; }
        QPainterPath path(v[0]);
        int i = 1; for(; i + 2 < v.size(); i += 3) path.cubicTo(v[i], v[i + 1], v[i + 2]);
        for(; i < v.size(); ++i) path.lineTo(v[i]);
        emitPath(path, false);
    }

    void run(){
        while(o < n && shapes.size() < 2000000){
            ws(); if(o >= n) break;
            const int op = p[o++];
            switch(op){
            case 0x00: break;
            case 0x03: colour = colour4(); break;
            case 0x06: { const int f = u16(); if(f & 1) fontName = whipString(); for(int b : {2, 4, 8, 16}) if(f & b) u8();
                         if(f & 0x20) fontHeight = qAbs(double(i32())); if(f & 0x40) fontRotation = u16(); for(int b : {0x80, 0x100, 0x200}) if(f & b) u16(); if(f & 0x400) i32(); break; }
            case 0x07: { QRgb avg = colour; const QVector<QPointF> v = gouraudSet(true, &avg); const QRgb was = colour; colour = avg; triangles(v); colour = was; break; }
            case 0x0b: contours(true); break;
            case 0x0c: { const QPointF a = rel16(), b = rel16(); poly({a, b}, false); break; }
            case 0x0e: break;                                                   // the next object node
            case 0x10: poly(set16(), true); break;
            case 0x11: { QRgb avg = colour; const QVector<QPointF> v = gouraudSet(true, &avg); const QRgb was = colour; colour = avg; poly(v, false); colour = was; break; }
            case 0x12: { const QPointF c = rel16(); const int r = u16(); ellipse(c, r, r, 0, 0, 0); break; }
            case 0x14: triangles(set16()); break;
            case 0x17: weight = qAbs(double(i32())); break;
            case 0x18: { const QPointF at = rel32(); const QString s = whipString(); text(at, s);
                         for(int k = 0; k < 2; ++k){ const int c = count() - 1; for(int i = 0; i < c && o < n; ++i) count(); }   // over- and underscore positions
                         o += 32;                                               // the bounds, four points
                         if(revision >= 600){ const int c = count() - 1; for(int i = 0; i < c && o < n; ++i) count(); }
                         break; }
            case 0x8d: set16(); break;                                          // a macro (symbol) drawn at points: its definition isn't kept
            case 0x92: { const QPointF c = rel32(); const double r = double(u32()); const int s = u16(), e = u16(); ellipse(c, r, r, s, e, 0); break; }
            case 0xac: case 0xcc: count(); break;                               // layer, line pattern
            case '{': extendedBinary(); break;
            case '(': --o; extended(); break;
            case 'B': bezier(setAscii()); break;
            case 'C': colour = map.value(int(asciiInt()) & 255, 0xff000000); break;
            case 'E': { const QPointF c = asciiPoint(), ax = asciiPoint(); ellipse(c, ax.x(), ax.y(), 0, 0, 0); break; }
            case 'F': fill = true; break;
            case 'G': case 'S': asciiInt(); break;                              // macro index, macro scale
            case 'L': { const QPointF a = asciiPoint(), b = asciiPoint(); poly({a, b}, false); break; }
            case 'M': setAscii(); break;
            case 'N': i32(); break;
            case 'O': { const double x = double(i32()); const double y = double(i32()); cur = QPointF(x, y); break; }
            case 'P': poly(setAscii(), true); break;
            case 'R': { const QPointF c = asciiPoint(); const double r = double(asciiInt()); ellipse(c, r, r, 0, 0, 0); break; }
            case 'T': triangles(setAscii()); break;
            case 'V': visible = true; break;
            case 'b': bezier(set32()); break;
            case 'c': colour = map.value(u8(), 0xff000000); break;
            case 'e': { const QPointF c = rel32(); const double a = double(u32()), b = double(u32()); const int s = u16(), e = u16(), t = u16(); ellipse(c, a, b, s, e, t); break; }
            case 'f': fill = false; break;
            case 'g': { QRgb avg = colour; const QVector<QPointF> v = gouraudSet(false, &avg); const QRgb was = colour; colour = avg; triangles(v); colour = was; break; }
            case 'k': contours(false); break;
            case 'l': { const QPointF a = rel32(), b = rel32(); poly({a, b}, false); break; }
            case 'm': set32(); break;
            case 'n': i16(); break;
            case 'p': poly(set32(), true); break;
            case 'q': { QRgb avg = colour; const QVector<QPointF> v = gouraudSet(false, &avg); const QRgb was = colour; colour = avg; poly(v, false); colour = was; break; }
            case 'r': { const QPointF c = rel32(); const double r = double(u32()); ellipse(c, r, r, 0, 0, 0); break; }
            case 's': i32(); break;
            case 't': triangles(set32()); break;
            case 'v': visible = false; break;
            case 'x': { const QPointF at = rel32(); text(at, whipString()); break; }
            default: if(st) ++st->unknownOpcodes; o = n; break;        // an unknown single-byte opcode: its length can't be known
            }
        }
    }
};

QRectF logicalBounds(const QVector<Shape>& shapes){
    QRectF b;
    for(const Shape& s : shapes) b = b.united(s.type == ShapeType::Text ? QRectF(s.transform.map(QPointF(0, 0)), QSizeF(1, 1)) : s.type == ShapeType::Rect ? s.transform.mapRect(s.rect) : s.path.boundingRect());
    return b;
}

// A W2D stream's drawing in its own logical coordinates (y up).
QVector<Shape> w2dShapes(const QByteArray& data, DwfImportStats* st){
    qint64 start = 0; int revision = 600;
    if(data.startsWith("(W2D V") || data.startsWith("(DWF V")){
        const int e = data.indexOf(')'); if(e < 0) return {};
        start = e + 1;
        const QList<QByteArray> v = data.mid(6, e - 6).split('.');                // "06.00" is revision 600
        if(v.size() == 2) revision = v[0].toInt() * 100 + v[1].toInt();
    }
    W2d w(reinterpret_cast<const uchar*>(data.constData()) + start, data.size() - start, st, revision);
    w.run();
    return w.shapes;
}

// Shapes moved onto the page by m; line widths and font sizes follow its scale.
QVector<Shape> placed(const QVector<Shape>& in, const QTransform& m){
    const double sc = std::sqrt(qAbs(m.determinant()));
    QVector<Shape> out; out.reserve(in.size());
    for(Shape s : in){
        if(s.type == ShapeType::Text){                                         // the scale goes into the font size, not the transform
            const int px = qMax(1, s.font.pixelSize()), npx = qMax(1, qRound(px * sc));
            s.font.setPixelSize(npx);
            s.transform = QTransform::fromScale(double(px) / npx, double(px) / npx) * s.transform * m;
        } else if(s.type == ShapeType::Rect){                                  // an image
            s.transform = s.transform * m;
        } else {
            s.path = m.map(s.path);
            s.stroke.width = qMax(0.5, s.stroke.width * sc);
        }
        out << s;
    }
    return out;
}

struct DwfStream { QString href, role; QTransform toPaper; };
struct DwfSheet { QString name; QSizeF paper; double pxPerUnit = 96; QColor paperColour = Qt::white; QVector<DwfStream> streams; bool model3d = false; };

// An ePlot section descriptor: the paper and, for each W2D resource, the matrix from its units to the paper's.
DwfSheet readDescriptor(const QByteArray& xml){
    DwfSheet sh; QXmlStreamReader x(xml);
    auto nums = [](const QString& v){ QVector<double> o; for(const QString& w : v.split(QLatin1Char(' '), Qt::SkipEmptyParts)) o << w.toDouble(); return o; };
    while(!x.atEnd()){
        if(x.readNext() != QXmlStreamReader::StartElement) continue;
        const QXmlStreamAttributes a = x.attributes(); const QStringView el = x.name();
        if(el == QLatin1String("Page")) sh.name = a.value(QLatin1String("name")).toString();
        else if(el == QLatin1String("Paper")){
            sh.paper = QSizeF(a.value(QLatin1String("width")).toDouble(), a.value(QLatin1String("height")).toDouble());
            sh.pxPerUnit = a.value(QLatin1String("units")).toString().startsWith(QLatin1String("mm")) ? 96 / 25.4 : 96;
            const QVector<double> c = nums(a.value(QLatin1String("color")).toString());
            if(c.size() >= 3) sh.paperColour = QColor(int(c[0]) & 255, int(c[1]) & 255, int(c[2]) & 255);
        } else if(el.endsWith(QLatin1String("Resource"))){
            const QString mime = a.value(QLatin1String("mime")).toString();
            if(mime.contains(QLatin1String("w3d"))) sh.model3d = true;
            if(mime != QLatin1String("application/x-w2d")) continue;
            DwfStream ds; ds.href = a.value(QLatin1String("href")).toString().replace(QLatin1Char('\\'), QLatin1Char('/')); ds.role = a.value(QLatin1String("role")).toString();
            const QVector<double> m = nums(a.value(QLatin1String("transform")).toString());   // 4x4, column by column
            if(m.size() == 16) ds.toPaper = QTransform(m[0], m[1], m[4], m[5], m[12], m[13]);
            sh.streams << ds;
        }
    }
    return sh;
}

} // namespace

QByteArray dwfZlibDictionary(){ return zlibDictionary(); }

Document importDwfDocument(const QByteArray& data, const QMap<QString, QByteArray>& zipIn, DwfImportStats* st){
    Document doc;
    QMap<QString, QByteArray> zip;                                              // packages from Windows name their parts with backslashes
    for(auto it = zipIn.constBegin(); it != zipIn.constEnd(); ++it) zip.insert(QString(it.key()).replace(QLatin1Char('\\'), QLatin1Char('/')), it.value());
    struct Page { QString name; QSizeF size; QColor colour; QVector<Shape> shapes; };
    QVector<Page> pages;
    QStringList used;
    if(!zip.isEmpty()){
        // Sheets in the manifest's order, each with its paper and its streams (drawing, then markup) placed on it.
        QStringList sections;
        for(auto it = zip.constBegin(); it != zip.constEnd(); ++it) if(it.key().endsWith(QLatin1String("manifest.xml"), Qt::CaseInsensitive)){
            QXmlStreamReader x(it.value());
            while(!x.atEnd()) if(x.readNext() == QXmlStreamReader::StartElement && x.name() == QLatin1String("Section")) sections << x.attributes().value(QLatin1String("name")).toString();
        }
        for(auto it = zip.constBegin(); it != zip.constEnd(); ++it) if(it.key().endsWith(QLatin1String("/descriptor.xml"), Qt::CaseInsensitive)){
            const QString sec = it.key().left(it.key().size() - 15);
            if(!sections.contains(sec)) sections << sec;
        }
        for(const QString& sec : std::as_const(sections)){
            if(pages.size() >= 200) break;
            const QByteArray desc = zip.value(sec + QLatin1String("/descriptor.xml"));
            if(desc.isEmpty()) continue;
            const DwfSheet sh = readDescriptor(desc);
            if(sh.model3d && st) ++st->models3d;
            if(sh.streams.isEmpty() || sh.paper.width() <= 0 || sh.paper.height() <= 0) continue;
            Page pg; pg.name = sh.name; pg.colour = sh.paperColour;
            pg.size = QSizeF(qBound(1.0, sh.paper.width() * sh.pxPerUnit, 1e6), qBound(1.0, sh.paper.height() * sh.pxPerUnit, 1e6));
            const QTransform toPage = QTransform::fromScale(sh.pxPerUnit, -sh.pxPerUnit) * QTransform::fromTranslate(0, pg.size.height());
            for(const DwfStream& ds : sh.streams){
                if(!zip.contains(ds.href)) continue;
                used << ds.href;
                const QVector<Shape> v = placed(w2dShapes(zip.value(ds.href), st), ds.toPaper * toPage);
                if(v.isEmpty()) continue;
                if(ds.role.contains(QLatin1String("markup"))){ Shape g; g.type = ShapeType::Group; g.name = QStringLiteral("Markup"); g.children = v; pg.shapes << g; }
                else pg.shapes += v;
            }
            if(!pg.shapes.isEmpty()) pages << pg;
        }
    }
    // Streams with no sheet to put them on (or a bare stream): each on a page fitted to its drawing.
    QVector<QByteArray> loose;
    if(zip.isEmpty()){ if(data.startsWith("(DWF V") || data.startsWith("(W2D V")) loose << data; }
    else for(auto it = zip.constBegin(); it != zip.constEnd(); ++it) if(it.key().endsWith(QLatin1String(".w2d"), Qt::CaseInsensitive) && !used.contains(it.key())) loose << it.value();
    for(const QByteArray& s : std::as_const(loose)){
        if(pages.size() >= 200) break;
        const QVector<Shape> v = w2dShapes(s, st);
        const QRectF b = logicalBounds(v);
        if(v.isEmpty() || (b.width() <= 0 && b.height() <= 0)) continue;
        const double sc = 1600.0 / qMax(1e-9, qMax(b.width(), b.height()));
        Page pg; pg.colour = Qt::white;
        pg.size = QSizeF(qMax(1.0, b.width() * sc), qMax(1.0, b.height() * sc));
        pg.shapes = placed(v, QTransform::fromTranslate(-b.left(), -b.bottom()) * QTransform::fromScale(sc, -sc));
        pages << pg;
    }
    if(pages.isEmpty()) return doc;
    QSizeF biggest;
    for(const Page& pg : std::as_const(pages)) biggest = biggest.expandedTo(pg.size);
    doc.pageSize = biggest; doc.pageCount = pages.size();
    for(int i = 0; i < pages.size(); ++i){
        Shape g; g.type = ShapeType::Group; g.name = pages[i].name.isEmpty() ? QStringLiteral("Sheet %1").arg(i + 1) : pages[i].name; g.children = pages[i].shapes;
        g.transform = QTransform::fromTranslate(0, doc.pageRect(i).top());
        doc.shapes << g;
    }
    if(st) st->pages = pages.size();
    return doc;
}

} // namespace xara
