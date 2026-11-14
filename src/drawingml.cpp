// DrawingML, the drawing language of Office Open XML: PowerPoint slides (.pptx and its show, template and
// macro forms) become pages; the drawings of Excel sheets and the shapes and pictures of Word documents come
// in too. A shape is a preset geometry (the 187 of ECMA-376, evaluated from their guide formulas) or a custom
// one, filled solid, with a gradient or a picture, outlined (width, dash, caps, arrowheads), and may carry text.
// Colours resolve through the theme and the master's colour map with their modifiers (tint, shade, luminance,
// alpha...); a shape without its own fill or line takes the theme's styles. Slides layer their master's and
// layout's shapes beneath their own and inherit placeholder positions and text styles from them. Pictures are
// cropped as the file says, tables become tables, SmartArt comes in through the drawing Office saves with it.
// Charts aren't drawn.
#include "prepress.h"
#include <QFontMetricsF>
#include <QHash>
#include <QPainter>
#include <QRegularExpression>
#include <QPainterPath>
#include <QXmlStreamReader>
#include <QtMath>
#include <functional>

namespace xara {
namespace {

const char* const kPresets[] = {
#include "dml_presets.inc"
};

constexpr double kEmuPerPx = 9525.0;                                          // 914400 EMU an inch, 96 px an inch

// ---- A small element tree -------------------------------------------------------------------------------------
struct X {
    QString n;                                         // local name
    QVector<QPair<QString, QString>> a;                // attributes; relationship ids as "r:id", "r:embed", "r:link"
    QVector<X> k;
    QString t;                                         // text content
    const X* c(QLatin1String name) const { for(const X& e : k) if(e.n == name) return &e; return nullptr; }
    const X* c(const char* name) const { return c(QLatin1String(name)); }
    template<typename... R> const X* c(const char* first, R... rest) const { const X* e = c(first); return e ? e->c(rest...) : nullptr; }
    QString at(const char* name, const QString& def = QString()) const { for(const auto& p : a) if(p.first == QLatin1String(name)) return p.second; return def; }
    bool has(const char* name) const { for(const auto& p : a) if(p.first == QLatin1String(name)) return true; return false; }
    double num(const char* name, double def = 0) const { bool ok = false; const double v = at(name).toDouble(&ok); return ok ? v : def; }
    bool flag(const char* name, bool def = false) const { const QString v = at(name); return v.isEmpty() ? def : (v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("on")); }
};

X parseXml(const QByteArray& xml){
    X root; if(xml.isEmpty()) return root;
    QXmlStreamReader r(xml);
    QVector<X*> stack{&root}; int nodes = 0;
    static const QString relNs = QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships");
    while(!r.atEnd()){
        const auto tok = r.readNext();
        if(tok == QXmlStreamReader::StartElement){
            if(++nodes > 4000000 || stack.size() > 256){ break; }
            X e; e.n = r.name().toString();
            for(const QXmlStreamAttribute& at : r.attributes())
                e.a << qMakePair(at.namespaceUri() == relNs ? QStringLiteral("r:") + at.name().toString() : at.name().toString(), at.value().toString());
            X* parent = stack.last(); parent->k << e; stack << &parent->k.last();
        } else if(tok == QXmlStreamReader::EndElement){ if(stack.size() > 1) stack.removeLast(); }
        else if(tok == QXmlStreamReader::Characters && stack.size() > 1) stack.last()->t += r.text();
    }
    return root;
}
// The first descendant (depth first) with this name.
const X* find(const X* e, QLatin1String name){
    if(!e) return nullptr;
    for(const X& k : e->k){ if(k.n == name) return &k; if(const X* f = find(&k, name)) return f; }
    return nullptr;
}

// ---- The package ----------------------------------------------------------------------------------------------
struct Pkg {
    const QMap<QString, QByteArray>& zip;
    QHash<QString, X> cache;
    explicit Pkg(const QMap<QString, QByteArray>& z) : zip(z) {}
    static QString dirOf(const QString& p){ const int k = p.lastIndexOf(QLatin1Char('/')); return k < 0 ? QString() : p.left(k); }
    static QString join(const QString& base, QString t){
        t.replace(QLatin1Char('\\'), QLatin1Char('/'));
        QString full = t.startsWith(QLatin1Char('/')) ? t.mid(1) : (base.isEmpty() ? t : base + QLatin1Char('/') + t);
        QStringList parts; for(const QString& s : full.split(QLatin1Char('/'))){ if(s == QLatin1String("..")){ if(!parts.isEmpty()) parts.removeLast(); } else if(!s.isEmpty() && s != QLatin1String(".")) parts << s; }
        return parts.join(QLatin1Char('/'));
    }
    QByteArray bytes(const QString& path) const {
        auto it = zip.constFind(path); if(it != zip.constEnd()) return it.value();
        for(auto i = zip.constBegin(); i != zip.constEnd(); ++i) if(i.key().compare(path, Qt::CaseInsensitive) == 0) return i.value();
        return {};
    }
    const X& xml(const QString& path){
        auto it = cache.find(path); if(it != cache.end()) return it.value();
        return cache.insert(path, parseXml(bytes(path))).value();
    }
    struct Rel { QString target, type; };
    QHash<QString, Rel> rels(const QString& part){                                // id → target part (external ones left out)
        QHash<QString, Rel> out;
        const QString name = part.mid(part.lastIndexOf(QLatin1Char('/')) + 1), dir = dirOf(part);
        const X& r = xml((dir.isEmpty() ? QString() : dir + QLatin1Char('/')) + QStringLiteral("_rels/") + name + QStringLiteral(".rels"));
        if(const X* root = r.c("Relationships")) for(const X& e : root->k)
            if(e.n == QLatin1String("Relationship") && e.at("TargetMode") != QLatin1String("External"))
                out.insert(e.at("Id"), Rel{join(dir, e.at("Target")), e.at("Type")});
        return out;
    }
    QString relOfType(const QString& part, const char* typeEnd){
        const auto r = rels(part);
        for(auto it = r.constBegin(); it != r.constEnd(); ++it) if(it.value().type.endsWith(QLatin1String(typeEnd))) return it.value().target;
        return {};
    }
};

// ---- Theme and colour -----------------------------------------------------------------------------------------
struct Theme {
    QHash<QString, QColor> scheme;
    QString major = QStringLiteral("Calibri Light"), minor = QStringLiteral("Calibri");
    QVector<const X*> fills, lines, bgFills;
    X root;
};

void readTheme(const X& x, Theme& th){
    th.root = x;
    const X* el = th.root.c("theme", "themeElements"); if(!el) return;
    if(const X* cs = el->c("clrScheme")) for(const X& e : cs->k){
        QColor c;
        if(const X* s = e.c("srgbClr")) c = QColor(QLatin1Char('#') + s->at("val"));
        else if(const X* s = e.c("sysClr")) c = QColor(QLatin1Char('#') + s->at("lastClr", s->at("val") == QLatin1String("window") ? QStringLiteral("FFFFFF") : QStringLiteral("000000")));
        if(c.isValid()) th.scheme.insert(e.n, c);
    }
    if(const X* fs = el->c("fontScheme")){
        if(const X* f = fs->c("majorFont", "latin")) if(!f->at("typeface").isEmpty()) th.major = f->at("typeface");
        if(const X* f = fs->c("minorFont", "latin")) if(!f->at("typeface").isEmpty()) th.minor = f->at("typeface");
    }
    if(const X* fm = el->c("fmtScheme")){
        if(const X* l = fm->c("fillStyleLst")) for(const X& e : l->k) th.fills << &e;
        if(const X* l = fm->c("lnStyleLst")) for(const X& e : l->k) th.lines << &e;
        if(const X* l = fm->c("bgFillStyleLst")) for(const X& e : l->k) th.bgFills << &e;
    }
}

struct Ctx {
    Pkg* pkg = nullptr;
    const Theme* theme = nullptr;
    QHash<QString, QString> clrMap;                   // bg1 → lt1 and so on
    QString part;                                      // the part the shapes come from, for its relationships
    QString tableStyles;                               // the presentation's table styles part
    QHash<QString, Pkg::Rel> rels;
    DmlImportStats* st = nullptr;
    int depth = 0;
};

// Tint (towards white) and shade (towards black) work on linear light, as Office does.
double toLinear(int c){ const double v = c / 255.0; return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
int toSrgb(double v){ v = qBound(0.0, v, 1.0); return qBound(0, int(std::round(255 * (v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055))), 255); }
QColor tinted(const QColor& c, double t){ auto f = [&](int v){ return toSrgb(toLinear(v) * t + (1 - t)); }; return QColor(f(c.red()), f(c.green()), f(c.blue()), c.alpha()); }
QColor shaded(const QColor& c, double s){ auto f = [&](int v){ return toSrgb(toLinear(v) * s); }; return QColor(f(c.red()), f(c.green()), f(c.blue()), c.alpha()); }

QColor fromHsl(double h, double s, double l, int alpha){ return QColor::fromHslF(qBound(0.0, h, 1.0 - 1e-9), qBound(0.0, s, 1.0), qBound(0.0, l, 1.0), alpha / 255.0); }

// The colour named by e's first colour child, with its modifiers; ph stands in for "phClr" in theme styles.
QColor colourIn(const X* e, const Ctx& cx, const QColor& ph = Qt::black){
    if(!e) return QColor();
    for(const X& k : e->k){
        QColor c;
        if(k.n == QLatin1String("srgbClr")) c = QColor(QLatin1Char('#') + k.at("val"));
        else if(k.n == QLatin1String("schemeClr")){
            QString v = k.at("val");
            if(v == QLatin1String("phClr")) c = ph;
            else {
                if(cx.clrMap.contains(v)) v = cx.clrMap.value(v);
                else if(v == QLatin1String("bg1")) v = QStringLiteral("lt1"); else if(v == QLatin1String("tx1")) v = QStringLiteral("dk1");
                else if(v == QLatin1String("bg2")) v = QStringLiteral("lt2"); else if(v == QLatin1String("tx2")) v = QStringLiteral("dk2");
                c = cx.theme ? cx.theme->scheme.value(v, QColor()) : QColor();
                if(!c.isValid()) c = v.startsWith(QLatin1String("lt")) ? QColor(Qt::white) : QColor(Qt::black);
            }
        }
        else if(k.n == QLatin1String("sysClr")) c = QColor(QLatin1Char('#') + k.at("lastClr", k.at("val") == QLatin1String("window") ? QStringLiteral("FFFFFF") : QStringLiteral("000000")));
        else if(k.n == QLatin1String("prstClr")){ QString v = k.at("val"); for(const char* p : {"dk", "lt", "med"}) if(v.startsWith(QLatin1String(p)) && v.size() > 3 && v[int(qstrlen(p))].isUpper()){ v = QLatin1String(p == QByteArray("dk") ? "dark" : p == QByteArray("lt") ? "light" : "medium") + v.mid(int(qstrlen(p))); break; }
                                                  c = QColor(v.toLower()); if(!c.isValid()) c = Qt::black; }
        else if(k.n == QLatin1String("scrgbClr")){ auto lin = [](double v){ v = qBound(0.0, v / 100000.0, 1.0); return int(std::round(255 * (v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055))); };
                                                   c = QColor(lin(k.num("r")), lin(k.num("g")), lin(k.num("b"))); }
        else if(k.n == QLatin1String("hslClr")) c = fromHsl(k.num("hue") / 21600000.0, k.num("sat") / 100000.0, k.num("lum") / 100000.0, 255);
        else continue;
        if(!c.isValid()) c = Qt::black;
        // The modifiers, in their order.
        for(const X& m : k.k){
            const double v = m.num("val") / 100000.0;
            if(m.n == QLatin1String("alpha")) c.setAlphaF(qBound(0.0, v, 1.0));
            else if(m.n == QLatin1String("alphaMod")) c.setAlphaF(qBound(0.0, c.alphaF() * v, 1.0));
            else if(m.n == QLatin1String("alphaOff")) c.setAlphaF(qBound(0.0, c.alphaF() + v, 1.0));
            else if(m.n == QLatin1String("lumMod") || m.n == QLatin1String("lumOff") || m.n == QLatin1String("satMod") || m.n == QLatin1String("satOff") || m.n == QLatin1String("hueMod") || m.n == QLatin1String("hueOff")){
                float h, s, l, al; c.getHslF(&h, &s, &l, &al); if(h < 0) h = 0;
                if(m.n == QLatin1String("lumMod")) l *= float(v); else if(m.n == QLatin1String("lumOff")) l += float(v);
                else if(m.n == QLatin1String("satMod")) s *= float(v); else if(m.n == QLatin1String("satOff")) s += float(v);
                else if(m.n == QLatin1String("hueMod")) h = float(std::fmod(h * v, 1.0)); else h = float(std::fmod(h + m.num("val") / 21600000.0 + 1.0, 1.0));
                c = fromHsl(h, s, l, c.alpha());
            }
            else if(m.n == QLatin1String("tint")) c = tinted(c, qBound(0.0, v, 1.0));
            else if(m.n == QLatin1String("shade")) c = shaded(c, qBound(0.0, v, 1.0));
            else if(m.n == QLatin1String("comp")){ float h, s, l, al; c.getHslF(&h, &s, &l, &al); c = fromHsl(std::fmod(qMax(0.0f, h) + 0.5, 1.0), s, l, c.alpha()); }
            else if(m.n == QLatin1String("inv")) c = QColor(255 - c.red(), 255 - c.green(), 255 - c.blue(), c.alpha());
            else if(m.n == QLatin1String("gray")){ const int g = qGray(c.rgb()); c = QColor(g, g, g, c.alpha()); }
        }
        return c;
    }
    return QColor();
}

// ---- Geometry -------------------------------------------------------------------------------------------------
struct GuideSet {
    QHash<QString, double> v;
    double get(const QString& t) const { bool ok = false; const double d = t.toDouble(&ok); return ok ? d : v.value(t, 0.0); }
    void builtins(double w, double h){
        const double ss = qMin(w, h), ls = qMax(w, h);
        const std::initializer_list<std::pair<const char*, double>> b = {
            {"w", w}, {"h", h}, {"l", 0}, {"t", 0}, {"r", w}, {"b", h}, {"hc", w / 2}, {"vc", h / 2}, {"ss", ss}, {"ls", ls},
            {"wd2", w / 2}, {"wd3", w / 3}, {"wd4", w / 4}, {"wd5", w / 5}, {"wd6", w / 6}, {"wd8", w / 8}, {"wd10", w / 10}, {"wd12", w / 12}, {"wd32", w / 32},
            {"hd2", h / 2}, {"hd3", h / 3}, {"hd4", h / 4}, {"hd5", h / 5}, {"hd6", h / 6}, {"hd8", h / 8}, {"hd10", h / 10},
            {"ssd2", ss / 2}, {"ssd4", ss / 4}, {"ssd6", ss / 6}, {"ssd8", ss / 8}, {"ssd16", ss / 16}, {"ssd32", ss / 32},
            {"cd2", 10800000}, {"cd4", 5400000}, {"cd8", 2700000}, {"3cd4", 16200000}, {"3cd8", 8100000}, {"5cd8", 13500000}, {"7cd8", 18900000}};
        for(const auto& p : b) v.insert(QLatin1String(p.first), p.second);
    }
    // A guide formula: an operator and up to three arguments; angles are in 60000ths of a degree.
    double eval(const QString& fmla) const {
        const QStringList t = fmla.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if(t.isEmpty()) return 0;
        auto a = [&](int i){ return i < t.size() ? get(t[i]) : 0.0; };
        const QString& op = t[0];
        const double deg = M_PI / 180.0 / 60000.0;
        if(op == QLatin1String("val")) return a(1);
        if(op == QLatin1String("*/")) return a(3) == 0 ? 0 : a(1) * a(2) / a(3);
        if(op == QLatin1String("+-")) return a(1) + a(2) - a(3);
        if(op == QLatin1String("+/")) return a(3) == 0 ? 0 : (a(1) + a(2)) / a(3);
        if(op == QLatin1String("?:")) return a(1) > 0 ? a(2) : a(3);
        if(op == QLatin1String("abs")) return std::fabs(a(1));
        if(op == QLatin1String("at2")) return std::atan2(a(2), a(1)) / deg;
        if(op == QLatin1String("cat2")) return a(1) * std::cos(std::atan2(a(3), a(2)));
        if(op == QLatin1String("sat2")) return a(1) * std::sin(std::atan2(a(3), a(2)));
        if(op == QLatin1String("cos")) return a(1) * std::cos(a(2) * deg);
        if(op == QLatin1String("sin")) return a(1) * std::sin(a(2) * deg);
        if(op == QLatin1String("tan")) return a(1) * std::tan(a(2) * deg);
        if(op == QLatin1String("max")) return qMax(a(1), a(2));
        if(op == QLatin1String("min")) return qMin(a(1), a(2));
        if(op == QLatin1String("mod")) return std::sqrt(a(1) * a(1) + a(2) * a(2) + a(3) * a(3));
        if(op == QLatin1String("pin")) return a(2) < a(1) ? a(1) : a(2) > a(3) ? a(3) : a(2);
        if(op == QLatin1String("sqrt")) return std::sqrt(qMax(0.0, a(1)));
        return 0;
    }
    void define(const QString& name, const QString& fmla){ const double d = eval(fmla); v.insert(name, std::isfinite(d) ? d : 0.0); }
};

struct GPath { QPainterPath path; char fill = 'n'; bool stroke = true; };

// One path's commands, already split into words: M x y, L x y, A wR hR st sw, Q.., C.., Z.
void runPath(const QVector<QStringList>& cmds, const GuideSet& g, double sx, double sy, QPainterPath& p){
    QPointF cur;
    auto pt = [&](const QStringList& c, int i){ return QPointF(g.get(c.value(i)) * sx, g.get(c.value(i + 1)) * sy); };
    for(const QStringList& c : cmds){
        if(c.isEmpty()) continue;
        const QString& op = c[0];
        if(op == QLatin1String("M")){ cur = pt(c, 1); p.moveTo(cur); }
        else if(op == QLatin1String("L")){ cur = pt(c, 1); p.lineTo(cur); }
        else if(op == QLatin1String("Q")){ const QPointF a = pt(c, 1); cur = pt(c, 3); p.quadTo(a, cur); }
        else if(op == QLatin1String("C")){ const QPointF a = pt(c, 1), b = pt(c, 3); cur = pt(c, 5); p.cubicTo(a, b, cur); }
        else if(op == QLatin1String("Z")){ p.closeSubpath(); cur = p.currentPosition(); }
        else if(op == QLatin1String("A")){
            // The angles are where on the ellipse the arc starts and how far it turns, measured as seen
            // (not as the ellipse's parameter): convert both ends, then keep the sweep's direction and size.
            const double wR = g.get(c.value(1)) * sx, hR = g.get(c.value(2)) * sy;
            const double st = g.get(c.value(3)) / 60000.0 * M_PI / 180.0, sw = g.get(c.value(4)) / 60000.0 * M_PI / 180.0;
            if(p.elementCount() == 0) p.moveTo(cur);
            if(wR <= 0 || hR <= 0){ continue; }
            auto param = [&](double a){ return std::atan2(wR * std::sin(a), hR * std::cos(a)); };
            const double t1 = param(st);
            double dt = param(st + sw) - t1;
            if(std::fabs(sw) >= 2 * M_PI - 1e-9) dt = sw;
            else { while(sw > 0 && dt < 0) dt += 2 * M_PI; while(sw < 0 && dt > 0) dt -= 2 * M_PI; if(std::fabs(sw) < 1e-12) dt = 0; }
            const QPointF centre = cur - QPointF(wR * std::cos(t1), hR * std::sin(t1));
            p.arcTo(QRectF(centre.x() - wR, centre.y() - hR, 2 * wR, 2 * hR), -t1 * 180 / M_PI, -dt * 180 / M_PI);
            cur = p.currentPosition();
        }
    }
}

struct Preset { QStringList av, gd; QVector<QPair<QStringList, QVector<QStringList>>> paths; QStringList rect; };
const QHash<QString, Preset>& presets(){
    static const QHash<QString, Preset> table = []{
        QHash<QString, Preset> t;
        for(const char* line : kPresets){
            const QStringList f = QString::fromLatin1(line).split(QLatin1Char('|'));
            if(f.size() != 5) continue;
            Preset p;
            p.av = f[1].split(QLatin1Char(';'), Qt::SkipEmptyParts); p.gd = f[2].split(QLatin1Char(';'), Qt::SkipEmptyParts);
            for(const QString& path : f[3].split(QLatin1Char('/'), Qt::SkipEmptyParts)){
                const int colon = path.indexOf(QLatin1Char(':'));
                QVector<QStringList> cmds; for(const QString& c : path.mid(colon + 1).split(QLatin1Char(';'), Qt::SkipEmptyParts)) cmds << c.split(QLatin1Char(' '), Qt::SkipEmptyParts);
                p.paths << qMakePair(path.left(colon).split(QLatin1Char(',')), cmds);
            }
            p.rect = f[4].split(QLatin1Char(' '));
            t.insert(f[0], p);
        }
        return t;
    }();
    return table;
}

QVector<GPath> presetGeometry(const QString& name, double w, double h, const QHash<QString, QString>& adj, QRectF* textRect){
    QVector<GPath> out;
    const auto& table = presets();
    auto it = table.constFind(name);
    if(it == table.constEnd()) return out;
    const Preset& pr = it.value();
    GuideSet g; g.builtins(w, h);
    for(const QString& d : pr.av){ const int e = d.indexOf(QLatin1Char('=')); const QString n = d.left(e); g.define(n, adj.value(n, d.mid(e + 1))); }
    for(const QString& d : pr.gd){ const int e = d.indexOf(QLatin1Char('=')); g.define(d.left(e), d.mid(e + 1)); }
    for(const auto& path : pr.paths){
        const double pw = path.first.value(0).toDouble(), ph = path.first.value(1).toDouble();
        GPath gp; gp.fill = path.first.value(2).isEmpty() ? 'n' : path.first.value(2)[0].toLatin1(); gp.stroke = path.first.value(3) != QLatin1String("0");
        runPath(path.second, g, pw > 0 ? w / pw : 1.0, ph > 0 ? h / ph : 1.0, gp.path);
        out << gp;
    }
    if(textRect && pr.rect.size() == 4) *textRect = QRectF(QPointF(g.get(pr.rect[0]), g.get(pr.rect[1])), QPointF(g.get(pr.rect[2]), g.get(pr.rect[3]))).normalized();
    return out;
}

QVector<GPath> customGeometry(const X* cg, double w, double h, QRectF* textRect){
    QVector<GPath> out;
    GuideSet g; g.builtins(w, h);
    for(const char* lst : {"avLst", "gdLst"}) if(const X* l = cg->c(lst)) for(const X& d : l->k) if(d.n == QLatin1String("gd")) g.define(d.at("name"), d.at("fmla"));
    if(const X* pl = cg->c("pathLst")) for(const X& p : pl->k){
        if(p.n != QLatin1String("path")) continue;
        GPath gp; const QString f = p.at("fill", QStringLiteral("norm"));
        gp.fill = f == QLatin1String("none") ? '0' : f == QLatin1String("lighten") ? 'L' : f == QLatin1String("lightenLess") ? 'l' : f == QLatin1String("darken") ? 'D' : f == QLatin1String("darkenLess") ? 'd' : 'n';
        gp.stroke = p.flag("stroke", true);
        QVector<QStringList> cmds;
        for(const X& c : p.k){
            QStringList cmd;
            auto pts = [&](){ for(const X& q : c.k) if(q.n == QLatin1String("pt")) cmd << q.at("x") << q.at("y"); };
            if(c.n == QLatin1String("moveTo")){ cmd << QStringLiteral("M"); pts(); }
            else if(c.n == QLatin1String("lnTo")){ cmd << QStringLiteral("L"); pts(); }
            else if(c.n == QLatin1String("quadBezTo")){ cmd << QStringLiteral("Q"); pts(); }
            else if(c.n == QLatin1String("cubicBezTo")){ cmd << QStringLiteral("C"); pts(); }
            else if(c.n == QLatin1String("arcTo")) cmd << QStringLiteral("A") << c.at("wR") << c.at("hR") << c.at("stAng") << c.at("swAng");
            else if(c.n == QLatin1String("close")) cmd << QStringLiteral("Z");
            if(!cmd.isEmpty()) cmds << cmd;
        }
        const double pw = p.num("w"), ph = p.num("h");
        runPath(cmds, g, pw > 0 ? w / pw : 1.0, ph > 0 ? h / ph : 1.0, gp.path);
        out << gp;
    }
    if(textRect) if(const X* r = cg->c("rect")) *textRect = QRectF(QPointF(g.get(r->at("l")), g.get(r->at("t"))), QPointF(g.get(r->at("r")), g.get(r->at("b")))).normalized();
    return out;
}

// ---- Fills and lines ------------------------------------------------------------------------------------------
struct Paint {
    int kind = -1;                                     // -1 unset, 0 none, 1 solid, 2 gradient, 3 picture, 4 group's
    QColor colour;
    QVector<GradStop> stops; bool linear = true; double angle = 0; QRectF focus{0.5, 0.5, 0, 0};
    QImage image; bool tile = false; double alpha = 1; double tileSx = 1, tileSy = 1, tileTx = 0, tileTy = 0;
};

QByteArray blipBytes(const X* blip, Ctx& cx){
    if(!blip) return {};
    QString id = blip->at("r:embed"); if(id.isEmpty()) id = blip->at("r:link");
    const QString target = cx.rels.value(id).target;
    return target.isEmpty() ? QByteArray() : cx.pkg->bytes(target);
}
// A Windows or Enhanced Metafile picture, as XarQ's own vector shapes.
Document metafile(const QByteArray& b){
    if(b.size() > 44 && b.mid(40, 4) == " EMF") return importEmf(b);
    if(b.size() > 22 && (b.startsWith(QByteArray("\xd7\xcd\xc6\x9a", 4)) || b.startsWith(QByteArray("\x01\x00\x09\x00", 4)) || b.startsWith(QByteArray("\x02\x00\x09\x00", 4)))) return importWmf(b);
    return {};
}
QImage blipImage(const X* blip, Ctx& cx, double* alpha, bool countMissing = true){
    const QByteArray raw = blipBytes(blip, cx);
    if(raw.isEmpty()) return {};
    QImage img = QImage::fromData(raw);
    if(img.isNull()){ if(cx.st && countMissing) ++cx.st->missingPictures; return {}; }
    if(alpha){ *alpha = 1; if(const X* a = blip->c("alphaModFix")) *alpha = qBound(0.0, a->num("amt", 100000) / 100000.0, 1.0); }
    if(const X* g = blip->c("grayscl")){ Q_UNUSED(g); img = img.convertToFormat(QImage::Format_Grayscale8); }
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

Paint readFill(const X* parent, Ctx& cx, const QColor& ph = Qt::black){
    Paint p; if(!parent) return p;
    for(const X& e : parent->k){
        if(e.n == QLatin1String("noFill")){ p.kind = 0; return p; }
        if(e.n == QLatin1String("solidFill")){ p.kind = 1; p.colour = colourIn(&e, cx, ph); if(!p.colour.isValid()) p.colour = ph; return p; }
        if(e.n == QLatin1String("gradFill")){
            p.kind = 2;
            if(const X* l = e.c("gsLst")) for(const X& s : l->k) if(s.n == QLatin1String("gs")) p.stops << GradStop{qBound(0.0, s.num("pos") / 100000.0, 1.0), colourIn(&s, cx, ph)};
            std::sort(p.stops.begin(), p.stops.end(), [](const GradStop& a, const GradStop& b){ return a.pos < b.pos; });
            if(const X* lin = e.c("lin")) p.angle = lin->num("ang") / 60000.0;
            if(const X* path = e.c("path")){ p.linear = false; if(const X* r = path->c("fillToRect")) p.focus = QRectF(QPointF(r->num("l") / 100000.0, r->num("t") / 100000.0), QPointF(1 - r->num("r") / 100000.0, 1 - r->num("b") / 100000.0)); }
            if(p.stops.isEmpty()){ p.kind = 1; p.colour = ph; }
            return p;
        }
        if(e.n == QLatin1String("blipFill")){
            p.kind = 3; p.image = blipImage(e.c("blip"), cx, &p.alpha);
            if(const X* t = e.c("tile")){ p.tile = true; p.tileSx = t->num("sx", 100000) / 100000.0; p.tileSy = t->num("sy", 100000) / 100000.0; p.tileTx = t->num("tx"); p.tileTy = t->num("ty"); }
            if(p.image.isNull()){ p.kind = 0; }
            return p;
        }
        if(e.n == QLatin1String("pattFill")){ p.kind = 1; p.colour = colourIn(e.c("fgClr"), cx, ph); if(!p.colour.isValid()) p.colour = Qt::black; return p; }
        if(e.n == QLatin1String("grpFill")){ p.kind = 4; return p; }
    }
    return p;
}

struct Line { bool on = false; QColor colour = Qt::black; double width = 9525; QVector<qreal> dash; Qt::PenCapStyle cap = Qt::FlatCap; Qt::PenJoinStyle join = Qt::RoundJoin; int head = 0, tail = 0; double headL = 3, headW = 3, tailL = 3, tailW = 3; bool set = false; };

// An arrowhead: its stock shape, and its length and width as XarQ's arrow scales (about three line widths at "med").
int arrowId(const X* end, double* length, double* width){
    if(!end) return 0;
    const QString t = end->at("type");
    auto sz = [](const QString& s){ return s == QLatin1String("sm") ? 2.0 : s == QLatin1String("lg") ? 5.0 : 3.0; };
    *length = sz(end->at("len")); *width = sz(end->at("w"));
    if(t == QLatin1String("triangle")) return -2;
    if(t == QLatin1String("arrow")) return -3;
    if(t == QLatin1String("stealth")) return -7;
    if(t == QLatin1String("diamond")) return -6;
    if(t == QLatin1String("oval")) return -5;
    return 0;
}

void applyLine(const X* ln, Ctx& cx, const QColor& ph, Line& L){
    if(!ln) return;
    L.set = true;
    if(ln->has("w")) L.width = ln->num("w");
    const Paint f = readFill(ln, cx, ph);
    if(f.kind == 0) L.on = false;
    else if(f.kind == 1){ L.on = true; L.colour = f.colour; }
    else if(f.kind == 2){ L.on = true; L.colour = f.stops.isEmpty() ? ph : f.stops.first().color; }
    const QString cap = ln->at("cap");
    if(cap == QLatin1String("rnd")) L.cap = Qt::RoundCap; else if(cap == QLatin1String("sq")) L.cap = Qt::SquareCap; else if(cap == QLatin1String("flat")) L.cap = Qt::FlatCap;
    if(ln->c("bevel")) L.join = Qt::BevelJoin; else if(ln->c("miter")) L.join = Qt::MiterJoin; else if(ln->c("round")) L.join = Qt::RoundJoin;
    if(const X* d = ln->c("prstDash")){
        const QString v = d->at("val");
        static const QHash<QString, QVector<qreal>> dashes = {
            {QStringLiteral("dot"), {1, 3}}, {QStringLiteral("dash"), {4, 3}}, {QStringLiteral("lgDash"), {8, 3}}, {QStringLiteral("dashDot"), {4, 3, 1, 3}},
            {QStringLiteral("lgDashDot"), {8, 3, 1, 3}}, {QStringLiteral("lgDashDotDot"), {8, 3, 1, 3, 1, 3}}, {QStringLiteral("sysDash"), {3, 1}},
            {QStringLiteral("sysDot"), {1, 1}}, {QStringLiteral("sysDashDot"), {3, 1, 1, 1}}, {QStringLiteral("sysDashDotDot"), {3, 1, 1, 1, 1, 1}}};
        L.dash = dashes.value(v);
    }
    if(const X* d = ln->c("custDash")){ L.dash.clear(); for(const X& s : d->k) if(s.n == QLatin1String("ds")) L.dash << qMax(0.01, s.num("d") / 100000.0) << qMax(0.01, s.num("sp") / 100000.0); }
    if(const X* h = ln->c("headEnd")) L.head = arrowId(h, &L.headL, &L.headW);
    if(const X* t = ln->c("tailEnd")) L.tail = arrowId(t, &L.tailL, &L.tailW);
}

// The theme's styles a shape refers to by index (p:style), standing in for what its own properties leave out.
struct StyleRefs { int fill = 0, line = 0; QColor fillColour, lineColour, fontColour; QString font; bool any = false; };
StyleRefs readStyle(const X* style, Ctx& cx){
    StyleRefs r; if(!style) return r;
    r.any = true;
    if(const X* f = style->c("fillRef")){ r.fill = int(f->num("idx")); r.fillColour = colourIn(f, cx); }
    if(const X* l = style->c("lnRef")){ r.line = int(l->num("idx")); r.lineColour = colourIn(l, cx); }
    if(const X* f = style->c("fontRef")){ r.fontColour = colourIn(f, cx); r.font = f->at("idx"); }
    return r;
}
Paint themeFill(int idx, const QColor& ph, Ctx& cx){
    Paint p; if(!cx.theme || idx <= 0) { p.kind = idx == 0 ? 0 : -1; return p; }
    const QVector<const X*>& l = idx >= 1000 ? cx.theme->bgFills : cx.theme->fills;
    const int i = (idx >= 1000 ? idx - 1000 : idx) - 1;
    if(i < 0 || i >= l.size()){ p.kind = 1; p.colour = ph; return p; }
    X wrap; wrap.k << *l[i];
    return readFill(&wrap, cx, ph);
}

// ---- Text -----------------------------------------------------------------------------------------------------
// Run and paragraph properties of one list level, merged from the cascade (later layers win).
struct Lvl {
    double size = 18; bool sizeSet = false; int bold = -1, italic = -1, underline = -1; QColor colour; QString face, cap; double baseline = 0;
    int align = -1; double marL = -1, indent = 0, spcBef = qQNaN(), spcAft = qQNaN(), lnSpc = qQNaN(); QString bullet; bool bulletSet = false;   // spacing: points, or minus a share of a line (NaN: unset)
};
struct LevelStyles { Lvl lvl[9]; };

void mergeRPr(const X* rp, Ctx& cx, Lvl& L){
    if(!rp) return;
    if(rp->has("sz")){ L.size = rp->num("sz") / 100.0; L.sizeSet = true; }
    if(rp->has("b")) L.bold = rp->flag("b") ? 1 : 0;
    if(rp->has("i")) L.italic = rp->flag("i") ? 1 : 0;
    if(rp->has("u")) L.underline = rp->at("u") != QLatin1String("none") ? 1 : 0;
    if(rp->has("cap")) L.cap = rp->at("cap");
    if(rp->has("baseline")) L.baseline = rp->num("baseline") / 100000.0;
    const Paint f = readFill(rp, cx); if(f.kind == 1) L.colour = f.colour; else if(f.kind == 2 && !f.stops.isEmpty()) L.colour = f.stops.first().color;
    if(const X* l = rp->c("latin")) if(!l->at("typeface").isEmpty()) L.face = l->at("typeface");
}
void mergePPr(const X* pp, Ctx& cx, Lvl& L){
    if(!pp) return;
    const QString al = pp->at("algn");
    if(!al.isEmpty()) L.align = al == QLatin1String("ctr") ? 1 : al == QLatin1String("r") ? 2 : (al == QLatin1String("just") || al == QLatin1String("dist")) ? 3 : 0;
    if(pp->has("marL")) L.marL = pp->num("marL");
    if(pp->has("indent")) L.indent = pp->num("indent");
    auto spacing = [&](const char* tag, double& out, bool isLine){
        if(const X* s = pp->c(tag)){ if(const X* p = s->c("spcPts")) out = isLine ? -(p->num("val") / 100.0) : p->num("val") / 100.0; else if(const X* p = s->c("spcPct")) out = isLine ? p->num("val") / 100000.0 : -(p->num("val") / 100000.0); } };
    spacing("spcBef", L.spcBef, false); spacing("spcAft", L.spcAft, false); spacing("lnSpc", L.lnSpc, true);
    if(pp->c("buNone")){ L.bullet.clear(); L.bulletSet = true; }
    if(const X* b = pp->c("buChar")){ L.bullet = b->at("char"); L.bulletSet = true; }
    if(pp->c("buAutoNum")){ L.bullet = QStringLiteral("#"); L.bulletSet = true; }
    mergeRPr(pp->c("defRPr"), cx, L);
}
void mergeLstStyle(const X* lst, Ctx& cx, LevelStyles& ts){
    if(!lst) return;
    if(const X* d = lst->c("defPPr")) for(Lvl& l : ts.lvl) mergePPr(d, cx, l);
    for(int i = 0; i < 9; ++i) mergePPr(lst->c(QStringLiteral("lvl%1pPr").arg(i + 1).toLatin1().constData()), cx, ts.lvl[i]);
}

struct BodyProps { double l = 91440, t = 45720, r = 91440, b = 45720; int anchor = 0; bool wrap = true; double fontScale = 1; int vert = 0; double rot = 0; bool set[5] = {}; };
void mergeBodyPr(const X* bp, BodyProps& b){
    if(!bp) return;
    if(bp->has("lIns")) b.l = bp->num("lIns"); if(bp->has("tIns")) b.t = bp->num("tIns");
    if(bp->has("rIns")) b.r = bp->num("rIns"); if(bp->has("bIns")) b.b = bp->num("bIns");
    const QString a = bp->at("anchor"); if(!a.isEmpty()) b.anchor = a == QLatin1String("ctr") ? 1 : a == QLatin1String("b") ? 2 : 0;
    if(bp->has("wrap")) b.wrap = bp->at("wrap") != QLatin1String("none");
    if(const X* n = bp->c("normAutofit")) b.fontScale = n->num("fontScale", 100000) / 100000.0;
    const QString v = bp->at("vert"); if(!v.isEmpty()) b.vert = (v == QLatin1String("vert") || v == QLatin1String("eaVert") || v == QLatin1String("wordArtVert")) ? 90 : v == QLatin1String("vert270") ? 270 : 0;
    if(bp->has("rot")) b.rot = bp->num("rot") / 60000.0;
}

QString resolveFace(const QString& f, const Ctx& cx){
    if(f.isEmpty() || f.startsWith(QLatin1String("+mn"))) return cx.theme ? cx.theme->minor : QStringLiteral("Calibri");
    if(f.startsWith(QLatin1String("+mj"))) return cx.theme ? cx.theme->major : QStringLiteral("Calibri Light");
    return f;
}

QString autoNumber(const QString& scheme, int n){
    auto roman = [](int v, bool upper){ static const int val[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
        static const char* sym[] = {"m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i"};
        QString r; for(int i = 0; i < 13 && v > 0; ++i) while(v >= val[i] && r.size() < 40){ r += QLatin1String(sym[i]); v -= val[i]; } return upper ? r.toUpper() : r; };
    auto alpha = [](int v, bool upper){ QString r; do { r.prepend(QChar((upper ? 'A' : 'a') + (v - 1) % 26)); v = (v - 1) / 26; } while(v > 0 && r.size() < 8); return r; };
    QString body = scheme.startsWith(QLatin1String("alphaLc")) ? alpha(n, false) : scheme.startsWith(QLatin1String("alphaUc")) ? alpha(n, true)
                 : scheme.startsWith(QLatin1String("romanLc")) ? roman(n, false) : scheme.startsWith(QLatin1String("romanUc")) ? roman(n, true) : QString::number(n);
    if(scheme.endsWith(QLatin1String("ParenBoth"))) return QLatin1Char('(') + body + QLatin1Char(')');
    if(scheme.endsWith(QLatin1String("ParenR"))) return body + QLatin1Char(')');
    if(scheme.endsWith(QLatin1String("Plain"))) return body;
    return body + QLatin1Char('.');
}

// A text body laid in `box` (EMU, the shape's local space) and placed by `place` (local EMU → page px): its
// paragraphs as text frames, one for each run of paragraphs that share a margin, alignment and spacing, stacked
// from the top, the middle or the bottom; bullets and numbers hang in front of their paragraphs.
// Sizes in points and lengths in EMU are absolute: `unit` is page px per nominal px for them, while the box follows `place`.
bool textBody(const X* body, const LevelStyles& base, const BodyProps& inheritedBody, const QRectF& box, const QTransform& place, double rotDeg,
              double unit, Ctx& cx, const QColor& defaultColour, const QString& name, QVector<Shape>& outShapes){
    if(!body) return false;
    BodyProps bp = inheritedBody; mergeBodyPr(body->c("bodyPr"), bp);
    LevelStyles ts = base; mergeLstStyle(body->c("lstStyle"), cx, ts);
    const double pxPerPt = 96.0 / 72.0;
    const double scale = unit;                                                // page px per nominal px, for point sizes
    const double emuPx = unit / kEmuPerPx;                                    // page px per absolute EMU (margins, indents)
    const double geom = qMax(1e-12, std::sqrt(qAbs(place.determinant())));   // page px per unit of the shape's own box
    struct Para { QString text; QVector<TextRun> runs; int align = 0; double marL = 0, indent = 0, before = 0, after = 0, lnSpc = 1; QFont tall; QString bullet; QColor bulletColour; QFont bulletFont; };
    QVector<Para> paras;
    int counters[9] = {};
    for(const X& p : body->k){
        if(p.n != QLatin1String("p")) continue;
        const X* pPr = p.c("pPr");
        const int lvl = qBound(0, pPr ? int(pPr->num("lvl")) : 0, 8);
        Lvl L = ts.lvl[lvl]; mergePPr(pPr, cx, L);
        Para P; P.align = qMax(0, L.align); P.marL = qMax(0.0, L.marL) * emuPx; P.indent = L.indent * emuPx;
        auto spacing = [&](double v, double size){ return v < 0 ? -v * size * 1.2 * pxPerPt * scale : v * pxPerPt * scale; };   // points, or a share of a line
        P.lnSpc = !std::isnan(L.lnSpc) && L.lnSpc > 0 ? L.lnSpc : 1.0;
        auto font = [&](const Lvl& R){ QFont f(resolveFace(R.face, cx)); f.setPixelSize(qMax(1, qRound(R.size * bp.fontScale * pxPerPt * scale)));
                                       f.setBold(R.bold == 1); f.setItalic(R.italic == 1); f.setUnderline(R.underline == 1); return f; };
        auto addRun = [&](QString s, const X* rPr){
            if(s.isEmpty()) return;
            Lvl R = L; mergeRPr(rPr, cx, R);
            if(R.cap == QLatin1String("all")) s = s.toUpper();
            TextRun run; run.start = int(P.text.size()); run.len = int(s.size()); run.font = font(R); run.color = R.colour.isValid() ? R.colour : defaultColour;
            if(R.cap == QLatin1String("small")) run.font.setCapitalization(QFont::SmallCaps);
            if(R.baseline != 0){                                                 // super- and subscripts rise or drop at two-thirds the size
                run.baseline = R.baseline * run.font.pixelSize(); run.font.setPixelSize(qMax(1, qRound(run.font.pixelSize() * 2.0 / 3))); }
            if(P.runs.isEmpty() || run.font.pixelSize() > P.tall.pixelSize()) P.tall = run.font;
            P.text += s; P.runs << run;
        };
        for(const X& r : p.k){
            if(r.n == QLatin1String("r") || r.n == QLatin1String("fld")){ if(const X* t = r.c("t")) addRun(t->t, r.c("rPr")); }
            else if(r.n == QLatin1String("br")) addRun(QStringLiteral("\n"), r.c("rPr"));
        }
        if(P.runs.isEmpty()){ Lvl R = L; mergeRPr(p.c("endParaRPr"), cx, R); P.tall = font(R); }
        const double sizeLines = P.tall.pixelSize() / (pxPerPt * scale);
        P.before = std::isnan(L.spcBef) ? 0.0 : qMax(0.0, spacing(L.spcBef, sizeLines));
        P.after = std::isnan(L.spcAft) ? 0.0 : qMax(0.0, spacing(L.spcAft, sizeLines));
        // The bullet: a character, or the next number at this level.
        const bool hasText = !P.text.trimmed().isEmpty();
        for(int i = lvl + 1; i < 9; ++i) counters[i] = 0;
        if(hasText && L.bulletSet && !L.bullet.isEmpty()){
            if(L.bullet == QLatin1String("#")){
                const X* an = pPr ? pPr->c("buAutoNum") : nullptr;
                if(an && an->has("startAt") && counters[lvl] == 0) counters[lvl] = int(an->num("startAt")) - 1;
                P.bullet = autoNumber(an ? an->at("type") : QStringLiteral("arabicPeriod"), ++counters[lvl]);
            } else P.bullet = L.bullet;
            P.bulletFont = P.runs.isEmpty() ? P.tall : P.runs.first().font;
            if(pPr) if(const X* bf = pPr->c("buFont")) if(!bf->at("typeface").isEmpty() && L.bullet != QLatin1String("#")) P.bulletFont.setFamily(resolveFace(bf->at("typeface"), cx));
            P.bulletFont.setBold(false); P.bulletFont.setItalic(false); P.bulletFont.setUnderline(false);
            if(pPr) if(const X* bs = pPr->c("buSzPct")) P.bulletFont.setPixelSize(qMax(1, qRound(P.bulletFont.pixelSize() * bs->num("val", 100000) / 100000.0)));
            P.bulletColour = P.runs.isEmpty() ? defaultColour : P.runs.first().color;
            if(pPr) if(const X* bc = pPr->c("buClr")){ const QColor c = colourIn(bc, cx); if(c.isValid()) P.bulletColour = c; }
        } else if(L.bulletSet && L.bullet == QLatin1String("#")) {} else if(!hasText) {}
        if(!L.bulletSet || L.bullet.isEmpty()) counters[lvl] = 0;
        paras << P;
    }
    bool any = false; for(const Para& P : paras) if(!P.text.trimmed().isEmpty()) any = true;
    if(!any) return false;
    // The text area: the geometry's text rectangle less the insets; vertical text turns it a quarter.
    const double inset = emuPx / geom;                                         // absolute EMU in the box's units
    QRectF area = box.adjusted(bp.l * inset, bp.t * inset, -bp.r * inset, -bp.b * inset);
    if(area.width() * geom < 1) area.setWidth(1 / geom);
    if(area.height() * geom < 1) area.setHeight(1 / geom);
    QSizeF areaPx(area.width() * geom, area.height() * geom);
    if(bp.vert == 90 || bp.vert == 270) areaPx.transpose();
    // Unwrapped text widens the area to its longest line.
    double width = areaPx.width();
    if(!bp.wrap) for(const Para& P : paras) for(const QString& ln : P.text.split(QLatin1Char('\n'))) width = qMax(width, P.marL + QFontMetricsF(P.tall).horizontalAdvance(ln) + 2);
    // Each paragraph's height as XarQ will set it: its tallest font's line height times the spacing.
    auto paraHeight = [&](const Para& P, double lineSpacing){
        const QFontMetricsF fm(P.tall);
        const double avail = qMax(1.0, width - P.marL - qMax(0.0, P.bullet.isEmpty() ? P.indent : 0.0));
        int lines = 0;
        for(const QString& ln : (P.text.isEmpty() ? QStringLiteral(" ") : P.text).split(QLatin1Char('\n'))){
            const QRectF b = fm.boundingRect(QRectF(0, 0, avail, 1e7), Qt::TextWordWrap, ln.isEmpty() ? QStringLiteral(" ") : ln);
            lines += qMax(1, int(std::round(b.height() / qMax(1.0, fm.lineSpacing()))));
        }
        return lines * fm.height() * lineSpacing;
    };
    // Group paragraphs into frames.
    struct Seg { int from, to; double top = 0, height = 0, lineSpacing = 1.18; };
    QVector<Seg> segs;
    for(int i = 0; i < paras.size(); ++i){
        const Para& P = paras[i];
        if(!segs.isEmpty()){
            const Para& Q = paras[segs.last().from];
            // XarQ's rich text has no paragraph spacing or indents of its own: a frame only holds paragraphs that need none between them.
            const Para& prev = paras[segs.last().to];
            if(qAbs(Q.marL - P.marL) < 0.5 && Q.align == P.align && qAbs(Q.lnSpc - P.lnSpc) < 1e-6 && prev.after < 0.5 && P.before < 0.5 && qAbs(Q.indent - P.indent) < 0.5){ segs.last().to = i; continue; }
        }
        segs << Seg{i, i};
    }
    double y = 0;
    for(Seg& sg : segs){
        sg.lineSpacing = paras[sg.from].lnSpc;                                  // single spacing is a font's own line height
        sg.top = y;
        double h = 0; for(int i = sg.from; i <= sg.to; ++i) h += paras[i].before + paraHeight(paras[i], sg.lineSpacing) + paras[i].after;
        sg.height = h; y += h;
    }
    const double total = y;
    const double dy = bp.anchor == 1 ? (areaPx.height() - total) / 2 : bp.anchor == 2 ? areaPx.height() - total : 0;
    const double dx = (!bp.wrap && width > areaPx.width()) ? (paras.first().align == 1 ? (areaPx.width() - width) / 2 : paras.first().align == 2 ? areaPx.width() - width : 0) : 0;
    // Place the frames: the area's centre lands where the shape puts it, turned with the shape.
    const QPointF centre = place.map(area.center());
    QTransform areaXf = QTransform::fromTranslate(-areaPx.width() / 2 + dx, -areaPx.height() / 2 + dy);
    areaXf *= QTransform().rotate(rotDeg + bp.rot + bp.vert);
    areaXf *= QTransform::fromTranslate(centre.x(), centre.y());
    const QString label = name.isEmpty() ? QString() : name;
    for(const Seg& sg : segs){
        Shape t; t.type = ShapeType::Text; t.textFrame = true;
        QString text; QVector<TextRun> runs;
        double paraTop = sg.top;
        for(int i = sg.from; i <= sg.to; ++i){
            const Para& P = paras[i];
            if(i > sg.from) text += QLatin1Char('\n');
            for(TextRun r : P.runs){ r.start += int(text.size()); runs << r; }
            // Its bullet, on the paragraph's first baseline.
            if(!P.bullet.isEmpty()){
                Shape b; b.type = ShapeType::Text; b.text = P.bullet; b.font = P.bulletFont; b.name = label.isEmpty() ? QStringLiteral("Bullet") : label + QStringLiteral(" bullet");
                b.fill.type = FillType::Solid; b.fill.color = P.bulletColour; b.stroke.enabled = false;
                const double bx = P.marL + qMin(0.0, P.indent);
                const double by = paraTop + P.before + QFontMetricsF(P.tall).ascent() - QFontMetricsF(P.bulletFont).ascent();
                b.transform = QTransform::fromTranslate(bx, by) * areaXf;
                outShapes << b;
            }
            paraTop += P.before + paraHeight(P, sg.lineSpacing) + P.after;
            text += P.text;
        }
        if(text.trimmed().isEmpty()) continue;
        const Para& F = paras[sg.from];
        t.text = text; t.textRuns = runs;
        t.name = label.isEmpty() ? text.simplified().left(32) : label;
        if(!runs.isEmpty()){ t.font = runs.first().font; t.fill.color = runs.first().color; } else { t.font = F.tall; t.fill.color = defaultColour; }
        t.fill.type = FillType::Solid; t.stroke.enabled = false;
        t.textAlign = F.align; t.firstLineIndent = F.bullet.isEmpty() ? qMax(-F.marL, F.indent) : 0;
        t.lineSpacing = sg.lineSpacing;
        const double inner = sg.height - F.before - paras[sg.to].after;
        // The margin and the space before move the frame; it stands a little taller than measured, as text past its foot isn't drawn.
        t.rect = QRectF(0, 0, qMax(1.0, width - F.marL), qMax(1.0, inner * 1.25 + QFontMetricsF(F.tall).height()));
        t.transform = QTransform::fromTranslate(F.marL, sg.top + F.before) * areaXf;
        outShapes << t;
        if(cx.st) ++cx.st->texts;
    }
    return !outShapes.isEmpty();
}

// ---- Shapes ---------------------------------------------------------------------------------------------------
// What a placeholder takes from its counterparts on the layout and the master.
struct Inherit {
    const X* xfrm = nullptr; const X* spPr = nullptr; const X* style = nullptr; const X* bodyPr = nullptr;
    QVector<const X*> lstStyles; QVector<const X*> bodyPrs;
};

QTransform boxTransform(const X* xfrm, QRectF* box){
    // The shape's local space is its box from (0,0); flips and the rotation turn about its centre.
    if(!xfrm){ *box = QRectF(); return QTransform(); }
    const X* off = xfrm->c("off"); const X* ext = xfrm->c("ext");
    const double x = off ? off->num("x") : 0, y = off ? off->num("y") : 0, w = ext ? ext->num("cx") : 0, h = ext ? ext->num("cy") : 0;
    *box = QRectF(0, 0, w, h);
    QTransform t = QTransform::fromTranslate(-w / 2, -h / 2);
    t *= QTransform::fromScale(xfrm->flag("flipH") ? -1 : 1, xfrm->flag("flipV") ? -1 : 1);
    t *= QTransform().rotate(xfrm->num("rot") / 60000.0);
    t *= QTransform::fromTranslate(x + w / 2, y + h / 2);
    return t;
}

struct Walker {
    Ctx cx;
    QVector<Shape> out;
    int budget = 200000;
    double unit = 1;                                   // page px per nominal px for absolute sizes (points, line widths)
    // A Word text box's paragraphs (w:txbxContent) laid in an area of the shape: supplied by the Word reader.
    std::function<QVector<Shape>(const X& content, const QSizeF& areaPx, const QTransform& areaToPage)> wordText;
    QString objectName;                                // a name for the next top-level object (Word's docPr)

    QColor lighten(const QColor& c, double k){ return QColor(int(c.red() + (255 - c.red()) * k), int(c.green() + (255 - c.green()) * k), int(c.blue() + (255 - c.blue()) * k), c.alpha()); }
    QColor darken(const QColor& c, double k){ return QColor(int(c.red() * k), int(c.green() * k), int(c.blue() * k), c.alpha()); }

    // A fill on a path already on the page: gradients run across the shape's box, carried there by `toPage`.
    void setFill(Shape& s, const Paint& p, char mode, const QRectF& box, const QTransform& toPage){
        auto shadeOf = [&](QColor c){ return mode == 'L' ? lighten(c, 0.4) : mode == 'l' ? lighten(c, 0.2) : mode == 'D' ? darken(c, 0.6) : mode == 'd' ? darken(c, 0.8) : c; };
        if(mode == '0' || p.kind <= 0 || p.kind == 4){ s.fill.type = FillType::None; return; }
        if(p.kind == 1){ s.fill.type = FillType::Solid; s.fill.color = shadeOf(p.colour); return; }
        if(p.kind == 2){
            s.fill.stops.clear(); for(GradStop g : p.stops){ g.color = shadeOf(g.color); s.fill.stops << g; }
            if(p.linear){
                const double a = qDegreesToRadians(p.angle); const QPointF d(std::cos(a), std::sin(a)), c = box.center();
                double lo = 1e300, hi = -1e300; for(const QPointF& q : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()}){ const double v = QPointF::dotProduct(q - c, d); lo = qMin(lo, v); hi = qMax(hi, v); }
                s.fill.type = FillType::Linear; s.fill.p0 = toPage.map(c + d * lo); s.fill.p1 = toPage.map(c + d * hi);
                s.fill.p2 = toPage.map(c + QPointF(-d.y(), d.x()) * (hi - lo) / 2);
            } else {
                // A path gradient: from its focus out to the box's far corner; the first stop is the outside.
                const QPointF f(box.left() + p.focus.center().x() * box.width(), box.top() + p.focus.center().y() * box.height());
                double r = 0; for(const QPointF& q : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()}) r = qMax(r, QLineF(f, q).length());
                s.fill.type = FillType::Radial; s.fill.p0 = toPage.map(f); s.fill.p1 = toPage.map(f + QPointF(r, 0)); s.fill.p2 = toPage.map(f + QPointF(0, r));
                for(GradStop& g : s.fill.stops) g.pos = 1 - g.pos;
                std::sort(s.fill.stops.begin(), s.fill.stops.end(), [](const GradStop& a, const GradStop& b){ return a.pos < b.pos; });
            }
            return;
        }
        if(p.kind == 3){
            s.fill.type = FillType::Bitmap; s.fill.image = p.image; s.fill.origImage = p.image; s.fill.tiled = p.tile;
            QRectF one = box;                                                   // stretched over the box, or one tile of the pattern
            if(p.tile) one = QRectF(box.left() + p.tileTx, box.top() + p.tileTy, qMax(1.0, p.image.width() * kEmuPerPx * qAbs(p.tileSx)), qMax(1.0, p.image.height() * kEmuPerPx * qAbs(p.tileSy)));
            s.fill.p0 = toPage.map(one.bottomLeft()); s.fill.p1 = toPage.map(one.bottomRight()); s.fill.p2 = toPage.map(one.topLeft());
            if(p.alpha < 1) s.opacity = p.alpha;
        }
    }

    void drawShape(const X& sp, const QTransform& parent, const Inherit& inh, const LevelStyles& textBase, const Paint& groupFill){
        if(--budget < 0) return;
        const X* nv = nullptr; for(const X& k : sp.k) if(k.n.startsWith(QLatin1String("nv")) || k.n == QLatin1String("cNvPr")) { nv = &k; break; }
        const X* cNvPr = nv ? (nv->n == QLatin1String("cNvPr") ? nv : nv->c("cNvPr")) : nullptr;
        if(cNvPr && cNvPr->flag("hidden")) return;
        QString name = cNvPr ? cNvPr->at("name") : QString();
        if(name.isEmpty()){ name = objectName; objectName.clear(); }
        const X* spPr = sp.c("spPr");
        const X* xfrm = spPr ? spPr->c("xfrm") : nullptr;
        if(!xfrm || !xfrm->c("ext")) xfrm = inh.xfrm;
        QRectF box; const QTransform local = boxTransform(xfrm, &box);
        if(box.isEmpty() && !(box.width() > 0 || box.height() > 0)) return;
        const QTransform toPage = local * parent;
        const double parentTurn = parent.determinant() > 0 ? qRadiansToDegrees(std::atan2(parent.m12(), parent.m11())) : 0;
        const double rot = (xfrm ? xfrm->num("rot") / 60000.0 : 0) + parentTurn;
        const StyleRefs sr = readStyle(sp.c("style") ? sp.c("style") : inh.style, cx);
        // Geometry.
        QRectF textRect = box; QVector<GPath> geo;
        const X* geomSrc = spPr && (spPr->c("prstGeom") || spPr->c("custGeom")) ? spPr : inh.spPr;
        if(geomSrc && geomSrc->c("custGeom")) geo = customGeometry(geomSrc->c("custGeom"), box.width(), box.height(), &textRect);
        else {
            const X* pg = geomSrc ? geomSrc->c("prstGeom") : nullptr;
            const QString prst = pg ? pg->at("prst", QStringLiteral("rect")) : QStringLiteral("rect");
            QHash<QString, QString> adj; if(pg) if(const X* av = pg->c("avLst")) for(const X& g : av->k) adj.insert(g.at("name"), g.at("fmla"));
            geo = presetGeometry(prst, box.width(), box.height(), adj, &textRect);
            if(geo.isEmpty()){ if(cx.st) ++cx.st->unknownPresets; geo = presetGeometry(QStringLiteral("rect"), box.width(), box.height(), {}, &textRect); }
        }
        // Fill: the shape's own, its placeholder's, its style's.
        Paint fill = readFill(spPr, cx);
        if(fill.kind < 0 && inh.spPr) fill = readFill(inh.spPr, cx);
        if(fill.kind < 0 && sr.any) fill = themeFill(sr.fill, sr.fillColour, cx);
        if(fill.kind == 4) fill = groupFill;
        // Line likewise.
        Line ln;
        if(sr.any && sr.line > 0 && cx.theme && sr.line - 1 < cx.theme->lines.size()){ ln.on = true; ln.colour = sr.lineColour; applyLine(cx.theme->lines[sr.line - 1], cx, sr.lineColour, ln); }
        if(inh.spPr) applyLine(inh.spPr->c("ln"), cx, sr.lineColour, ln);
        if(spPr) applyLine(spPr->c("ln"), cx, sr.lineColour.isValid() ? sr.lineColour : QColor(Qt::black), ln);
        const double lineScale = unit / kEmuPerPx;                            // line widths are absolute, whatever a group's scaling
        // Office paints every path's fill before any path's outline (a chart-plus's lines lie over its box).
        QVector<Shape> parts, outlines;
        const bool single = geo.size() == 1;
        for(const GPath& g : geo){
            if(g.path.isEmpty()) continue;
            Shape s; s.type = ShapeType::Path; s.name = name; s.path = toPage.map(g.path);
            setFill(s, fill, g.fill, box, toPage);
            Shape o = s; o.fill.type = FillType::None;
            const bool stroke = g.stroke && ln.on;
            if(stroke){
                o.stroke.enabled = true; o.stroke.color = ln.colour; o.stroke.width = qMax(0.25, ln.width * lineScale); o.stroke.cap = ln.cap; o.stroke.join = ln.join;
                o.stroke.dashes = ln.dash;
                if(ln.head){ o.stroke.startArrow = ln.head; o.stroke.startArrowW = ln.headL; o.stroke.startArrowH = ln.headW; }
                if(ln.tail){ o.stroke.endArrow = ln.tail; o.stroke.endArrowW = ln.tailL; o.stroke.endArrowH = ln.tailW; }
            }
            if(single){ if(stroke){ s.stroke = o.stroke; } else s.stroke.enabled = false;
                        if(s.fill.type != FillType::None || s.stroke.enabled) parts << s; continue; }
            s.stroke.enabled = false;
            if(s.fill.type != FillType::None) parts << s;
            if(stroke) outlines << o;
        }
        parts += outlines;
        // Text.
        QColor tc = sr.fontColour.isValid() ? sr.fontColour : colourIn(nullptr, cx);
        if(!tc.isValid()){ QHash<QString, QString> m = cx.clrMap; tc = cx.theme ? cx.theme->scheme.value(m.value(QStringLiteral("tx1"), QStringLiteral("dk1")), Qt::black) : QColor(Qt::black); }
        BodyProps bp; for(const X* b : inh.bodyPrs) mergeBodyPr(b, bp);
        LevelStyles ts = textBase;
        if(sr.fontColour.isValid()) for(Lvl& l : ts.lvl) l.colour = sr.fontColour;     // the style's font colour outranks the master's defaults
        for(const X* l : inh.lstStyles) mergeLstStyle(l, cx, ts);
        if(sr.any && !sr.font.isEmpty()) for(Lvl& l : ts.lvl) if(l.face.isEmpty()) l.face = sr.font == QLatin1String("major") ? QStringLiteral("+mj-lt") : QStringLiteral("+mn-lt");
        const X* body = sp.c("txBody"); if(!body) if(const X* tb = sp.c("txbx")) body = tb;
        // SmartArt drawings give the text its own box.
        QRectF tbox = textRect; QTransform tplace = toPage; double trot = rot;
        if(const X* tx = sp.c("txXfrm")){ QRectF b; tplace = boxTransform(tx, &b) * parent; tbox = b; trot = tx->num("rot") / 60000.0 + parentTurn; }
        QVector<Shape> text;
        bool hasText = false;
        if(const X* tb = sp.c("txbx")) if(const X* content = tb->c("txbxContent")) if(wordText){
            BodyProps wb = bp; mergeBodyPr(sp.c("bodyPr"), wb);
            const double geom = qMax(1e-12, std::sqrt(qAbs(tplace.determinant()))), inset = unit / kEmuPerPx / geom;
            const QRectF area = tbox.adjusted(wb.l * inset, wb.t * inset, -wb.r * inset, -wb.b * inset);
            const QSizeF areaPx(qMax(1.0, area.width() * geom), qMax(1.0, area.height() * geom));
            const QPointF c = tplace.map(area.center());
            QTransform xf = QTransform::fromTranslate(-areaPx.width() / 2, -areaPx.height() / 2); xf *= QTransform().rotate(trot + wb.rot + wb.vert); xf *= QTransform::fromTranslate(c.x(), c.y());
            text = wordText(*content, areaPx, xf); hasText = !text.isEmpty();
        }
        if(!hasText) hasText = body && body->n == QLatin1String("txBody") && textBody(body, ts, bp, tbox, tplace, trot, unit, cx, tc, name, text);
        if(parts.size() == 1 && !hasText){ out << parts[0]; if(cx.st) ++cx.st->shapes; return; }
        if(parts.isEmpty() && !hasText) return;
        if(parts.isEmpty() && text.size() == 1){ out << text.first(); return; }
        Shape g; g.type = ShapeType::Group; g.name = name; g.children = parts + text;
        out << g; if(cx.st) ++cx.st->shapes;
    }

    void drawPicture(const X& pic, const QTransform& parent, const Inherit& inh){
        if(--budget < 0) return;
        const X* nv = pic.c("nvPicPr"); const X* cNvPr = nv ? nv->c("cNvPr") : nullptr;
        if(cNvPr && cNvPr->flag("hidden")) return;
        const X* spPr = pic.c("spPr"); const X* xfrm = spPr ? spPr->c("xfrm") : nullptr;
        if(!xfrm || !xfrm->c("ext")) xfrm = inh.xfrm;
        QRectF box; const QTransform local = boxTransform(xfrm, &box);
        if(box.width() <= 0 || box.height() <= 0) return;
        const X* bf = pic.c("blipFill"); double alpha = 1;
        QImage img = blipImage(bf ? bf->c("blip") : nullptr, cx, &alpha, false);
        if(img.isNull()){
            const Document v = metafile(blipBytes(bf ? bf->c("blip") : nullptr, cx));
            const QRectF b = v.shapes.isEmpty() ? QRectF() : v.contentBounds();
            if(b.width() <= 0 || b.height() <= 0){ if(cx.st) ++cx.st->missingPictures; return; }
            Shape g; g.type = ShapeType::Group; g.name = cNvPr ? cNvPr->at("name") : QStringLiteral("Picture"); g.children = v.shapes;
            g.transform = QTransform::fromTranslate(-b.left(), -b.top()) * QTransform::fromScale(box.width() / b.width(), box.height() / b.height()) * local * parent;
            out << g; if(cx.st) ++cx.st->pictures;
            return;
        }
        if(const X* sr = bf->c("srcRect")){                                     // crop (negative values pad)
            const double l = sr->num("l") / 100000.0, t = sr->num("t") / 100000.0, r = sr->num("r") / 100000.0, b = sr->num("b") / 100000.0;
            const QRect keep = QRectF(l * img.width(), t * img.height(), (1 - l - r) * img.width(), (1 - t - b) * img.height()).toRect();
            if(keep.width() > 0 && keep.height() > 0 && keep != img.rect()){ QImage c(keep.size(), QImage::Format_ARGB32_Premultiplied); c.fill(Qt::transparent); QPainter pp(&c); pp.drawImage(-keep.topLeft(), img); pp.end(); img = c; }
        }
        Shape s = psdBitmapShape(img, 0, 0, cNvPr ? cNvPr->at("name") : QStringLiteral("Picture"));
        s.transform = QTransform::fromScale(box.width() / img.width(), box.height() / img.height()) * local * parent;
        if(alpha < 1) s.opacity = alpha;
        // A picture cut to a shape other than a rectangle.
        if(spPr) if(const X* pg = spPr->c("prstGeom")) if(pg->at("prst") != QLatin1String("rect")){
            QRectF tr; const QVector<GPath> geo = presetGeometry(pg->at("prst"), box.width(), box.height(), {}, &tr);
            if(!geo.isEmpty()){ QPainterPath clip = geo[0].path; s.regionPath = QTransform::fromScale(img.width() / box.width(), img.height() / box.height()).map(clip); }
        }
        out << s; if(cx.st) ++cx.st->pictures;
        Line ln; if(spPr) applyLine(spPr->c("ln"), cx, Qt::black, ln);
        if(ln.on){ Shape b; b.type = ShapeType::Path; b.name = s.name + QStringLiteral(" outline"); QPainterPath p; p.addRect(box); b.path = (local * parent).map(p);
                   b.fill.type = FillType::None; b.stroke.color = ln.colour; b.stroke.width = qMax(0.25, ln.width * unit / kEmuPerPx); out << b; }
    }

    // A table style's look by region (whole table, banded rows and columns, first and last row and column).
    struct Region { QColor fill, text, border; bool hasFill = false, hasText = false, bold = false, hasBorder = false; double borderW = 12700; };
    QHash<QString, Region> tableStyle(const QString& id){
        QHash<QString, Region> out;
        const X* lst = cx.tableStyles.isEmpty() ? nullptr : cx.pkg->xml(cx.tableStyles).c("tblStyleLst");
        const X* style = nullptr;
        if(lst) for(const X& st : lst->k) if(st.n == QLatin1String("tblStyle") && st.at("styleId") == id){ style = &st; break; }
        if(!style){
            // Office's default (Medium Style 2, Accent 1), which PowerPoint leaves out of the file when it's the only one used.
            if(id.compare(QLatin1String("{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}"), Qt::CaseInsensitive) != 0) return out;
            const QColor a = cx.theme ? cx.theme->scheme.value(QStringLiteral("accent1"), QColor(68, 114, 196)) : QColor(68, 114, 196);
            auto tint = [&](double t){ return tinted(a, t); };
            Region whole; whole.fill = tint(0.2); whole.hasFill = true; whole.border = Qt::white; whole.hasBorder = true; out.insert(QStringLiteral("wholeTbl"), whole);
            Region band; band.fill = tint(0.4); band.hasFill = true; out.insert(QStringLiteral("band1H"), band); out.insert(QStringLiteral("band1V"), band);
            Region first; first.fill = a; first.hasFill = true; first.text = Qt::white; first.hasText = true; first.bold = true;
            out.insert(QStringLiteral("firstRow"), first); out.insert(QStringLiteral("lastRow"), first);
            Region col; col.fill = a; col.hasFill = true; col.text = Qt::white; col.hasText = true; col.bold = true;
            out.insert(QStringLiteral("firstCol"), col); out.insert(QStringLiteral("lastCol"), col);
            return out;
        }
        for(const X& r : style->k){
            Region g;
            if(const X* tx = r.c("tcTxStyle")){
                g.bold = tx->at("b") == QLatin1String("on");
                QColor c = colourIn(tx, cx); if(!c.isValid()) c = colourIn(tx->c("fontRef"), cx);
                if(c.isValid()){ g.text = c; g.hasText = true; }
            }
            if(const X* tc = r.c("tcStyle")){
                if(const X* f = tc->c("fill")){ const Paint p = readFill(f, cx); if(p.kind == 1){ g.fill = p.colour; g.hasFill = true; } else if(p.kind == 0){ g.fill = Qt::transparent; g.hasFill = true; } }
                else if(const X* fr = tc->c("fillRef")){ const Paint p = themeFill(int(fr->num("idx")), colourIn(fr, cx), cx); if(p.kind == 1){ g.fill = p.colour; g.hasFill = true; } }
                if(const X* bd = tc->c("tcBdr")) for(const char* side : {"insideH", "insideV", "left", "top", "right", "bottom"}) if(const X* sd = bd->c(side)) if(const X* ln = sd->c("ln")){
                    const Paint p = readFill(ln, cx); if(p.kind == 1){ g.border = p.colour; g.hasBorder = true; g.borderW = ln->num("w", 12700); break; } }
            }
            out.insert(r.n, g);
        }
        return out;
    }

    void drawTable(const X& tbl, const QTransform& toPage, const QString& name){
        Shape s; s.type = ShapeType::Table; s.name = name.isEmpty() ? QStringLiteral("Table") : name;
        const double sc = std::sqrt(qAbs(toPage.determinant()));
        if(const X* g = tbl.c("tblGrid")) for(const X& c : g->k) if(c.n == QLatin1String("gridCol")) s.table.colW << c.num("w") * sc;
        const X* pr = tbl.c("tblPr");
        const X* sid = pr ? pr->c("tableStyleId") : nullptr;
        const QHash<QString, Region> style = tableStyle(sid ? sid->t.trimmed() : QString());
        auto flag = [&](const char* f){ return pr && pr->flag(f); };
        QVector<const X*> rowsX; for(const X& tr : tbl.k) if(tr.n == QLatin1String("tr")) rowsX << &tr;
        const int nRows = rowsX.size(), nCols = s.table.colW.size();
        double fontPx = 0; QColor border; double borderW = 0;
        for(int row = 0; row < nRows; ++row){
            const X& tr = *rowsX[row];
            s.table.rowH << tr.num("h") * sc;
            int col = 0;
            for(const X& tc : tr.k){
                if(tc.n != QLatin1String("tc")) continue;
                TableCell cell; cell.valign = 1; cell.textColor = Qt::black;
                if(tc.flag("hMerge") || tc.flag("vMerge")){ cell.rowSpan = 0; cell.colSpan = 0; }
                else { cell.colSpan = qMax(1, int(tc.num("gridSpan", 1))); cell.rowSpan = qMax(1, int(tc.num("rowSpan", 1))); }
                // The style, region by region: later ones win.
                QStringList regions{QStringLiteral("wholeTbl")};
                const int dataRow = row - (flag("firstRow") ? 1 : 0), dataCol = col - (flag("firstCol") ? 1 : 0);
                if(flag("bandRow") && dataRow >= 0) regions << (dataRow % 2 == 0 ? QStringLiteral("band1H") : QStringLiteral("band2H"));
                if(flag("bandCol") && dataCol >= 0) regions << (dataCol % 2 == 0 ? QStringLiteral("band1V") : QStringLiteral("band2V"));
                if(flag("firstCol") && col == 0) regions << QStringLiteral("firstCol");
                if(flag("lastCol") && col == nCols - 1) regions << QStringLiteral("lastCol");
                if(flag("lastRow") && row == nRows - 1) regions << QStringLiteral("lastRow");
                if(flag("firstRow") && row == 0) regions << QStringLiteral("firstRow");
                for(const QString& r : regions){ auto it = style.constFind(r); if(it == style.constEnd()) continue;
                    if(it->hasFill) cell.bg = it->fill; if(it->hasText) cell.textColor = it->text; if(it->bold) cell.bold = true;
                    if(it->hasBorder && !border.isValid()){ border = it->border; borderW = it->borderW; } }
                if(const X* body = tc.c("txBody")){
                    QStringList paras;
                    for(const X& p : body->k) if(p.n == QLatin1String("p")){
                        QString t; for(const X& r : p.k) if(const X* tt = r.c("t")) t += tt->t; paras << t;
                        if(const X* pp = p.c("pPr")){ const QString al = pp->at("algn"); if(al == QLatin1String("ctr")) cell.align = 1; else if(al == QLatin1String("r")) cell.align = 2; }
                        for(const X& r : p.k) if(const X* rp = r.c("rPr")){
                            if(rp->has("b")) cell.bold = rp->flag("b");
                            const Paint f = readFill(rp, cx); if(f.kind == 1) cell.textColor = f.colour;
                            if(rp->has("sz") && fontPx <= 0) fontPx = rp->num("sz") / 100.0 * 96 / 72;
                        }
                    }
                    cell.text = paras.join(QLatin1Char('\n'));
                }
                if(const X* tcPr = tc.c("tcPr")){
                    const Paint f = readFill(tcPr, cx); if(f.kind == 1) cell.bg = f.colour; else if(f.kind == 0) cell.bg = Qt::transparent;
                    const QString a = tcPr->at("anchor"); cell.valign = a == QLatin1String("ctr") ? 0 : a == QLatin1String("b") ? 2 : 1;
                    if(!border.isValid()) for(const char* side : {"lnL", "lnR", "lnT", "lnB"}) if(const X* ln = tcPr->c(side)){
                        const Paint p = readFill(ln, cx); if(p.kind == 1){ border = p.colour; borderW = ln->num("w", 12700); break; } }
                }
                s.table.cells << cell; ++col;
            }
            while(col < nCols){ s.table.cells << TableCell(); ++col; }
            while(col > s.table.colW.size()) s.table.colW << 50 * sc;
        }
        if(s.table.rowH.isEmpty() || s.table.colW.isEmpty()) return;
        if(s.table.cells.size() != s.table.rows() * s.table.cols()) s.table.cells.resize(s.table.rows() * s.table.cols());
        s.table.borderColor = border.isValid() ? border : QColor(Qt::transparent);
        s.table.borderW = border.isValid() ? qMax(0.5, borderW / kEmuPerPx * unit) : 0.0;
        s.table.pad = qMax(1.0, 91440 / kEmuPerPx * unit / 2);
        QFont f(cx.theme ? cx.theme->minor : QStringLiteral("Calibri")); f.setPixelSize(qMax(1, qRound((fontPx > 0 ? fontPx : 18 * 96 / 72.0) * unit)));
        s.font = f;
        s.transform = QTransform(toPage.m11() / sc, toPage.m12() / sc, toPage.m21() / sc, toPage.m22() / sc, toPage.dx(), toPage.dy());
        out << s; if(cx.st) ++cx.st->shapes;
    }

    // A graphic frame: a table, a chart (left out), SmartArt (from the drawing Office keeps with it), or an
    // embedded object's picture.
    void drawFrame(const X& gf, const QTransform& parent){
        if(--budget < 0) return;
        const X* xfrm = gf.c("xfrm"); if(!xfrm) if(const X* sp = gf.c("spPr")) xfrm = sp->c("xfrm");
        QRectF box; const QTransform local = boxTransform(xfrm, &box);
        const X* nv = nullptr; for(const X& k : gf.k) if(k.n.startsWith(QLatin1String("nv"))) nv = &k;
        const QString name = nv && nv->c("cNvPr") ? nv->c("cNvPr")->at("name") : QString();
        const X* data = gf.c("graphic", "graphicData"); if(!data) return;
        const QString uri = data->at("uri");
        if(const X* tbl = data->c("tbl")){ drawTable(*tbl, local * parent, name); return; }
        if(uri.endsWith(QLatin1String("/chart")) || uri.contains(QLatin1String("chartex"))){ if(cx.st) ++cx.st->charts; return; }
        if(uri.endsWith(QLatin1String("/diagram"))){
            const X* ids = data->c("relIds");
            const QString dm = ids ? cx.rels.value(ids->at("r:dm")).target : QString();
            QString drawingPart;
            if(!dm.isEmpty()) if(const X* ext = find(&cx.pkg->xml(dm), QLatin1String("dataModelExt"))) drawingPart = cx.rels.value(ext->at("relId")).target;
            if(drawingPart.isEmpty()) for(auto it = cx.rels.constBegin(); it != cx.rels.constEnd(); ++it) if(it.value().type.endsWith(QLatin1String("/diagramDrawing"))){ drawingPart = it.value().target; break; }
            if(drawingPart.isEmpty()) return;
            const X& dx = cx.pkg->xml(drawingPart);
            const X* tree = find(&dx, QLatin1String("spTree")); if(!tree) return;
            Ctx saved = cx; cx.part = drawingPart; cx.rels = cx.pkg->rels(drawingPart);
            const X* off = xfrm ? xfrm->c("off") : nullptr;                     // its shapes stand in the frame's space
            const QTransform at = QTransform::fromTranslate(off ? off->num("x") : 0, off ? off->num("y") : 0) * parent;
            const int before = out.size();
            walk(*tree, at, LevelStyles(), Paint(), nullptr);
            cx = saved;
            if(out.size() > before){ Shape g; g.type = ShapeType::Group; g.name = name.isEmpty() ? QStringLiteral("SmartArt") : name; g.children = out.mid(before); out.resize(before); out << g; if(cx.st) ++cx.st->smartArt; }
            return;
        }
        if(const X* ole = find(data, QLatin1String("oleObj"))) if(const X* pic = find(ole, QLatin1String("pic"))){ Inherit inh; inh.xfrm = xfrm; drawPicture(*pic, parent, inh); }
    }

    // Shape-level placeholder matching, supplied by the slide reader.
    std::function<Inherit(const X&)> placeholder;

    void walk(const X& tree, const QTransform& parent, const LevelStyles& textBase, const Paint& groupFill, const char* skipPlaceholders){
        if(++cx.depth > 64){ --cx.depth; return; }
        for(const X& e : tree.k){
            const X* el = &e;
            if(e.n == QLatin1String("AlternateContent")){                     // markup compatibility: a choice we read, else the fallback
                el = nullptr;
                for(const X& c : e.k) if(c.n == QLatin1String("Choice")){
                    const QStringList req = c.at("Requires").split(QLatin1Char(' '), Qt::SkipEmptyParts);
                    bool ok = !req.isEmpty(); for(const QString& r : req) if(!QStringList{QStringLiteral("a14"), QStringLiteral("p14"), QStringLiteral("wps"), QStringLiteral("wpg"), QStringLiteral("wp14"), QStringLiteral("a16"), QStringLiteral("p15")}.contains(r)) ok = false;
                    if(ok && !c.k.isEmpty()){ el = &c.k.first(); break; }
                }
                if(!el) if(const X* f = e.c("Fallback")) if(!f->k.isEmpty()) el = &f->k.first();
                if(!el) continue;
            }
            const QString& n = el->n;
            const bool isPh = skipPlaceholders && (n == QLatin1String("sp") || n == QLatin1String("pic")) && find(el, QLatin1String("ph"));
            if(isPh) continue;                                                  // a layout's or master's prompt, not drawing
            if(n == QLatin1String("sp") || n == QLatin1String("cxnSp") || n == QLatin1String("wsp")){
                Inherit inh; if(placeholder && !skipPlaceholders) inh = placeholder(*el);
                drawShape(*el, parent, inh, textBase, groupFill);
            }
            else if(n == QLatin1String("pic")){ Inherit inh; if(placeholder && !skipPlaceholders) inh = placeholder(*el); drawPicture(*el, parent, inh); }
            else if(n == QLatin1String("graphicFrame")) drawFrame(*el, parent);
            else if(n == QLatin1String("grpSp") || n == QLatin1String("wgp")){
                const X* gp = el->c("grpSpPr"); const X* xf = gp ? gp->c("xfrm") : nullptr;
                QTransform child;
                if(xf){
                    const X* off = xf->c("off"); const X* ext = xf->c("ext"); const X* coff = xf->c("chOff"); const X* cext = xf->c("chExt");
                    const double x = off ? off->num("x") : 0, y = off ? off->num("y") : 0, w = ext ? ext->num("cx") : 0, h = ext ? ext->num("cy") : 0;
                    const double cxo = coff ? coff->num("x") : x, cyo = coff ? coff->num("y") : y, cw = cext ? cext->num("cx") : w, ch = cext ? cext->num("cy") : h;
                    child = QTransform::fromTranslate(-cxo, -cyo) * QTransform::fromScale(cw > 0 ? w / cw : 1, ch > 0 ? h / ch : 1) * QTransform::fromTranslate(x, y);
                    QTransform turn = QTransform::fromTranslate(-(x + w / 2), -(y + h / 2));
                    turn *= QTransform::fromScale(xf->flag("flipH") ? -1 : 1, xf->flag("flipV") ? -1 : 1);
                    turn *= QTransform().rotate(xf->num("rot") / 60000.0);
                    turn *= QTransform::fromTranslate(x + w / 2, y + h / 2);
                    child *= turn;
                }
                Paint gfill = readFill(gp, cx); if(gfill.kind < 0 || gfill.kind == 4) gfill = groupFill;
                const int before = out.size();
                walk(*el, child * parent, textBase, gfill, skipPlaceholders);
                if(out.size() > before){
                    const X* cNvPr = find(el->c("nvGrpSpPr"), QLatin1String("cNvPr"));
                    Shape g; g.type = ShapeType::Group; g.name = cNvPr ? cNvPr->at("name") : QStringLiteral("Group");
                    g.children = out.mid(before); out.resize(before); out << g;
                }
            }
            else if(n == QLatin1String("contentPart")) continue;
        }
        --cx.depth;
    }
};

QHash<QString, QString> readClrMap(const X* m){
    QHash<QString, QString> out; if(!m) return out;
    for(const auto& a : m->a) out.insert(a.first, a.second);
    return out;
}

// The background of a slide (its own, else its layout's, else its master's) as a page-filling shape.
bool background(const X* cSld, Ctx& cx, const QSizeF& page, Shape& out){
    const X* bg = cSld ? cSld->c("bg") : nullptr; if(!bg) return false;
    Paint p;
    if(const X* pr = bg->c("bgPr")) p = readFill(pr, cx);
    else if(const X* ref = bg->c("bgRef")) p = themeFill(int(ref->num("idx")), colourIn(ref, cx), cx);
    if(p.kind <= 0) return p.kind == 0;
    Walker w; w.cx = cx;
    Shape s; s.type = ShapeType::Path; s.name = QStringLiteral("Background"); QPainterPath path; path.addRect(QRectF(QPointF(0, 0), page)); s.path = path; s.stroke.enabled = false;
    w.setFill(s, p, 'n', QRectF(QPointF(0, 0), page), QTransform());
    out = s;
    return true;
}

} // namespace

QPainterPath dmlPresetPath(const QString& name, double w, double h, const QHash<QString, QString>& adjust){
    QPainterPath all; QRectF tr;
    for(const GPath& g : presetGeometry(name, w, h, adjust, &tr)) all.addPath(g.path);
    return all;
}
QStringList dmlPresetNames(){ QStringList n = presets().keys(); n.sort(); return n; }

Document importPptxDocument(const QMap<QString, QByteArray>& zip, DmlImportStats* st){
    Document doc;
    Pkg pkg(zip);
    QString pres = pkg.relOfType(QString(), "/officeDocument");
    if(pres.isEmpty()) pres = QStringLiteral("ppt/presentation.xml");
    const X& px = pkg.xml(pres);
    const X* p = px.c("presentation"); if(!p) return doc;
    QSizeF page(9144000 / kEmuPerPx, 6858000 / kEmuPerPx);
    if(const X* sz = p->c("sldSz")) if(sz->num("cx") > 0 && sz->num("cy") > 0) page = QSizeF(sz->num("cx") / kEmuPerPx, sz->num("cy") / kEmuPerPx);
    const auto presRels = pkg.rels(pres);
    QStringList slides;
    if(const X* l = p->c("sldIdLst")) for(const X& s : l->k){ const QString t = presRels.value(s.at("r:id")).target; if(!t.isEmpty() && !slides.contains(t)) slides << t; }
    if(slides.isEmpty()) for(auto it = zip.constBegin(); it != zip.constEnd(); ++it) if(QRegularExpression(QStringLiteral("^ppt/slides/slide\\d+\\.xml$")).match(it.key()).hasMatch()) slides << it.key();
    LevelStyles presDefault; Ctx dummy; mergeLstStyle(p->c("defaultTextStyle"), dummy, presDefault);
    const QString tableStyles = pkg.relOfType(pres, "/tableStyles");
    QHash<QString, Theme> themes;
    QVector<QVector<Shape>> pages;
    for(const QString& slide : std::as_const(slides)){
        if(pages.size() >= 1000) break;
        const X& sx = pkg.xml(slide);
        const X* sld = sx.c("sld"); if(!sld) continue;
        const QString layout = pkg.relOfType(slide, "/slideLayout");
        const QString master = layout.isEmpty() ? QString() : pkg.relOfType(layout, "/slideMaster");
        const QString themePart = master.isEmpty() ? QString() : pkg.relOfType(master, "/theme");
        if(!themes.contains(themePart)){ Theme th; readTheme(pkg.xml(themePart), th); themes.insert(themePart, th); }
        const Theme& theme = themes[themePart];
        const X* lay = layout.isEmpty() ? nullptr : pkg.xml(layout).c("sldLayout");
        const X* mas = master.isEmpty() ? nullptr : pkg.xml(master).c("sldMaster");
        Walker w; w.cx.pkg = &pkg; w.cx.theme = &theme; w.cx.st = st; w.cx.tableStyles = tableStyles;
        w.cx.clrMap = readClrMap(mas ? mas->c("clrMap") : nullptr);
        if(lay) if(const X* o = lay->c("clrMapOvr", "overrideClrMapping")) w.cx.clrMap = readClrMap(o);
        if(const X* o = sld->c("clrMapOvr", "overrideClrMapping")) w.cx.clrMap = readClrMap(o);
        const QTransform toPx = QTransform::fromScale(1 / kEmuPerPx, 1 / kEmuPerPx);
        // Text styles of the master, by placeholder kind.
        auto masterStyle = [&](const char* which){ LevelStyles t = presDefault; if(mas) mergeLstStyle(mas->c("txStyles", which), w.cx, t); return t; };
        const LevelStyles titleStyle = masterStyle("titleStyle"), bodyStyle = masterStyle("bodyStyle"), otherStyle = masterStyle("otherStyle");
        auto findPh = [&](const X* root, const QString& type, const QString& idx) -> const X* {
            const X* tree = root ? root->c("cSld", "spTree") : nullptr; if(!tree) return nullptr;
            const X* byType = nullptr;
            std::function<const X*(const X&)> scan = [&](const X& t) -> const X* {
                for(const X& e : t.k){
                    if(e.n == QLatin1String("grpSp")){ if(const X* f = scan(e)) return f; continue; }
                    const X* ph = find(&e, QLatin1String("ph")); if(!ph) continue;
                    auto norm = [](QString t){ if(t.isEmpty() || t == QLatin1String("obj")) return QStringLiteral("body"); if(t == QLatin1String("ctrTitle")) return QStringLiteral("title"); return t; };
                    const QString ty = norm(ph->at("type")), want = norm(type);
                    if(!idx.isEmpty() && ph->at("idx") == idx) return &e;
                    if(!byType && (ty == want || (want == QLatin1String("subTitle") && ty == QLatin1String("body")))) byType = &e;
                }
                return nullptr; };
            if(const X* f = scan(*tree)) return f;
            return byType;
        };
        w.placeholder = [&](const X& e) -> Inherit {
            Inherit inh;
            const X* ph = find(&e, QLatin1String("ph"));
            if(!ph || e.n == QLatin1String("grpSp")) { inh.lstStyles.clear(); return inh; }
            const QString type = ph->at("type"), idx = ph->at("idx");
            const X* onLayout = findPh(lay, type, idx);
            QString mType = type; if(onLayout) if(const X* lph = find(onLayout, QLatin1String("ph"))) mType = lph->at("type");
            if(mType == QLatin1String("subTitle") || mType == QLatin1String("obj") || mType.isEmpty()) mType = QStringLiteral("body");
            const X* onMaster = findPh(mas, mType, QString());
            for(const X* src : {onMaster, onLayout}){
                if(!src) continue;
                if(const X* sp = src->c("spPr")){ if(sp->c("xfrm") && sp->c("xfrm")->c("ext")) inh.xfrm = sp->c("xfrm"); if(sp->c("prstGeom") || sp->c("custGeom") || readFill(sp, w.cx).kind >= 0 || sp->c("ln")) inh.spPr = sp; }
                if(src->c("style")) inh.style = src->c("style");
                if(const X* tb = src->c("txBody")){ inh.lstStyles << tb->c("lstStyle"); inh.bodyPrs << tb->c("bodyPr"); }
            }
            return inh;
        };
        QVector<Shape> shapes;
        Shape bg; const X* bgSrc = sld->c("cSld");
        Ctx bcx = w.cx; bcx.part = slide; bcx.rels = pkg.rels(slide);
        if(!(bgSrc && bgSrc->c("bg"))){ bgSrc = lay ? lay->c("cSld") : nullptr; bcx.part = layout; bcx.rels = pkg.rels(layout); }
        if(!(bgSrc && bgSrc->c("bg"))){ bgSrc = mas ? mas->c("cSld") : nullptr; bcx.part = master; bcx.rels = pkg.rels(master); }
        if(background(bgSrc, bcx, page, bg) && bg.fill.type != FillType::None) shapes << bg;
        else { Shape white; white.type = ShapeType::Path; white.name = QStringLiteral("Background"); QPainterPath pp; pp.addRect(QRectF(QPointF(0, 0), page)); white.path = pp; white.fill.type = FillType::Solid; white.fill.color = Qt::white; white.stroke.enabled = false; shapes << white; }
        // The master's and the layout's own shapes, then the slide's.
        const bool showMaster = sld->flag("showMasterSp", true) && (!lay || lay->flag("showMasterSp", true));
        auto layer = [&](const X* root, const QString& part, bool placeholdersAreDrawing){
            const X* tree = root ? root->c("cSld", "spTree") : nullptr; if(!tree) return;
            w.cx.part = part; w.cx.rels = pkg.rels(part);
            w.walk(*tree, toPx, otherStyle, Paint(), placeholdersAreDrawing ? nullptr : "skip");
        };
        if(showMaster && mas) layer(mas, master, false);
        if(lay) layer(lay, layout, false);
        // The slide: placeholders pick their master text style.
        {
            const X* tree = sld->c("cSld", "spTree");
            w.cx.part = slide; w.cx.rels = pkg.rels(slide);
            if(tree) for(const X& e : tree->k){
                const X* ph = find(&e, QLatin1String("ph"));
                const QString type = ph ? ph->at("type", QStringLiteral("body")) : QString();
                const LevelStyles& base = !ph ? otherStyle : (type == QLatin1String("title") || type == QLatin1String("ctrTitle")) ? titleStyle
                                       : (type == QLatin1String("body") || type == QLatin1String("subTitle") || type == QLatin1String("obj")) ? bodyStyle : otherStyle;
                X one; one.k << e;
                w.walk(one, toPx, base, Paint(), nullptr);
            }
        }
        shapes += w.out;
        pages << shapes;
    }
    if(pages.isEmpty()) return doc;
    doc.pageSize = page; doc.pageCount = pages.size();
    for(int i = 0; i < pages.size(); ++i){
        Shape g; g.type = ShapeType::Group; g.name = QStringLiteral("Slide %1").arg(i + 1); g.children = pages[i];
        g.transform = QTransform::fromTranslate(0, doc.pageRect(i).top());
        doc.shapes << g;
    }
    if(st) st->pages = pages.size();
    return doc;
}

Document importXlsxDrawings(const QMap<QString, QByteArray>& zip, DmlImportStats* st){
    Document doc;
    Pkg pkg(zip);
    QString wb = pkg.relOfType(QString(), "/officeDocument"); if(wb.isEmpty()) wb = QStringLiteral("xl/workbook.xml");
    const X* book = pkg.xml(wb).c("workbook"); if(!book) return doc;
    Theme theme; readTheme(pkg.xml(pkg.relOfType(wb, "/theme")), theme);
    LevelStyles sheetText; for(Lvl& l : sheetText.lvl) l.size = 11;             // Excel's shapes default to 11 points
    const auto wbRels = pkg.rels(wb);
    struct Sheet { QString name; QVector<Shape> shapes; QRectF bounds; };
    QVector<Sheet> sheets;
    if(const X* l = book->c("sheets")) for(const X& s : l->k){
        if(sheets.size() >= 256) break;
        const QString part = wbRels.value(s.at("r:id")).target; if(part.isEmpty()) continue;
        const X* ws = pkg.xml(part).c("worksheet"); if(!ws) continue;
        const QString drawing = pkg.relOfType(part, "/drawing"); if(drawing.isEmpty()) continue;
        // Column widths and row heights, in pixels, to find the anchor cells.
        double defCol = 64, defRow = 20;
        auto colPx = [](double w){ return std::floor(((256 * w + std::floor(128.0 / 7)) / 256) * 7); };
        if(const X* f = ws->c("sheetFormatPr")){
            if(f->has("defaultColWidth")) defCol = colPx(f->num("defaultColWidth"));
            else if(f->has("baseColWidth")) defCol = std::ceil((f->num("baseColWidth") * 7 + 5) / 8) * 8;
            if(f->has("defaultRowHeight")) defRow = f->num("defaultRowHeight") * 96 / 72;
        }
        QMap<int, double> cols, rows;
        if(const X* cl = ws->c("cols")) for(const X& c : cl->k) for(int i = int(c.num("min")); i <= int(c.num("max")) && i <= 16384 && i >= 1; ++i) cols.insert(i - 1, c.flag("hidden") ? 0 : colPx(c.num("width")));
        if(const X* sd = ws->c("sheetData")) for(const X& r : sd->k) if(r.n == QLatin1String("row") && r.has("ht")) rows.insert(int(r.num("r")) - 1, r.flag("hidden") ? 0 : r.num("ht") * 96 / 72);
        auto colX = [&](int c){ double x = 0; for(int i = 0; i < qMin(c, 16384); ++i) x += cols.value(i, defCol); return x; };
        auto rowY = [&](int r){ double y = 0; for(int i = 0; i < qMin(r, 1048576); ++i) y += rows.value(i, defRow); return y; };
        auto cellPt = [&](const X* m){ return m ? QPointF(colX(int(m->c("col") ? m->c("col")->t.toDouble() : 0)) + (m->c("colOff") ? m->c("colOff")->t.toDouble() : 0) / kEmuPerPx,
                                                        rowY(int(m->c("row") ? m->c("row")->t.toDouble() : 0)) + (m->c("rowOff") ? m->c("rowOff")->t.toDouble() : 0) / kEmuPerPx) : QPointF(); };
        Walker w; w.cx.pkg = &pkg; w.cx.theme = &theme; w.cx.st = st; w.cx.part = drawing; w.cx.rels = pkg.rels(drawing);
        const X* wsDr = pkg.xml(drawing).c("wsDr"); if(!wsDr) continue;
        for(const X& anchor : wsDr->k){
            QRectF at;
            if(anchor.n == QLatin1String("twoCellAnchor")) at = QRectF(cellPt(anchor.c("from")), cellPt(anchor.c("to"))).normalized();
            else if(anchor.n == QLatin1String("oneCellAnchor")){ const X* e = anchor.c("ext"); at = QRectF(cellPt(anchor.c("from")), QSizeF(e ? e->num("cx") / kEmuPerPx : 0, e ? e->num("cy") / kEmuPerPx : 0)); }
            else if(anchor.n == QLatin1String("absoluteAnchor")){ const X* p = anchor.c("pos"); const X* e = anchor.c("ext"); at = QRectF(p ? p->num("x") / kEmuPerPx : 0, p ? p->num("y") / kEmuPerPx : 0, e ? e->num("cx") / kEmuPerPx : 0, e ? e->num("cy") / kEmuPerPx : 0); }
            else continue;
            // The anchor places the object: map its own frame onto the anchor's cells.
            X one; for(const X& k : anchor.k) if(k.n != QLatin1String("from") && k.n != QLatin1String("to") && k.n != QLatin1String("ext") && k.n != QLatin1String("pos") && k.n != QLatin1String("clientData")) one.k << k;
            if(one.k.isEmpty()) continue;
            const X& obj = one.k.first();
            const X* xf = nullptr;
            if(const X* sp = obj.c("spPr")) xf = sp->c("xfrm"); else if(const X* gp = obj.c("grpSpPr")) xf = gp->c("xfrm"); else xf = obj.c("xfrm");
            QTransform place = QTransform::fromScale(1 / kEmuPerPx, 1 / kEmuPerPx);
            if(xf && xf->c("off") && xf->c("ext") && xf->c("ext")->num("cx") > 0 && xf->c("ext")->num("cy") > 0){
                const QRectF own(xf->c("off")->num("x"), xf->c("off")->num("y"), xf->c("ext")->num("cx"), xf->c("ext")->num("cy"));
                if(at.width() > 0 && at.height() > 0) place = QTransform::fromTranslate(-own.x(), -own.y()) * QTransform::fromScale(at.width() / own.width(), at.height() / own.height()) * QTransform::fromTranslate(at.x(), at.y());
            }
            w.unit = std::sqrt(qAbs(place.determinant())) * kEmuPerPx;            // an anchor that resizes its object resizes its lines and text
            w.walk(one, place, sheetText, Paint(), nullptr);
        }
        if(w.out.isEmpty()) continue;
        Sheet sh; sh.name = s.at("name"); sh.shapes = w.out;
        for(const Shape& x : sh.shapes) sh.bounds = sh.bounds.united(x.type == ShapeType::Path ? x.path.boundingRect() : x.type == ShapeType::Group ? QRectF() : x.transform.mapRect(x.type == ShapeType::Table ? QRectF(0, 0, 100, 100) : x.rect));
        sheets << sh;
    }
    if(sheets.isEmpty()) return doc;
    // Each sheet's drawing on a page that reaches from the sheet's corner to its farthest object.
    QSizeF size(200, 200);
    QVector<QRectF> extents;
    for(const Sheet& sh : std::as_const(sheets)){
        QRectF b; std::function<void(const QVector<Shape>&)> grow = [&](const QVector<Shape>& v){ for(const Shape& x : v){
            if(x.type == ShapeType::Group) grow(x.children);
            else if(x.type == ShapeType::Path) b = b.united(x.path.boundingRect());
            else if(x.type == ShapeType::Table){ double w = 0, h = 0; for(double c : x.table.colW) w += c; for(double r : x.table.rowH) h += r; b = b.united(x.transform.mapRect(QRectF(0, 0, w, h))); }
            else b = b.united(x.transform.mapRect(x.rect)); } };
        grow(sh.shapes); extents << b;
        size = size.expandedTo(QSizeF(b.right() + 40, b.bottom() + 40));
    }
    doc.pageSize = size; doc.pageCount = sheets.size();
    for(int i = 0; i < sheets.size(); ++i){
        Shape g; g.type = ShapeType::Group; g.name = sheets[i].name.isEmpty() ? QStringLiteral("Sheet %1").arg(i + 1) : sheets[i].name; g.children = sheets[i].shapes;
        g.transform = QTransform::fromTranslate(0, doc.pageRect(i).top());
        doc.shapes << g;
    }
    if(st) st->pages = sheets.size();
    return doc;
}

namespace {

// ---- Word -----------------------------------------------------------------------------------------------------
// Paragraph and run properties along Word's style chain (twips and half-points as Word keeps them).
struct WProps {
    int jc = -1; double before = qQNaN(), after = qQNaN(), line = qQNaN(); int lineRule = 0;   // 0 auto (240ths of a line), 1 exact, 2 at least
    double indL = qQNaN(), indR = qQNaN(), first = qQNaN();                                  // first: + first line, - hanging
    int b = -1, i = -1, u = -1, caps = -1, vert = 0; double sz = qQNaN(); QString font; QColor colour;
    QString numId; int ilvl = -1; bool pageBreakBefore = false;
};
bool wOn(const X* e){ if(!e) return false; const QString v = e->at("val"); return v.isEmpty() || v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("on"); }
void mergeWR(const X* r, WProps& w, const Theme& th){
    if(!r) return;
    if(const X* e = r->c("b")) w.b = wOn(e); if(const X* e = r->c("i")) w.i = wOn(e);
    if(const X* e = r->c("u")) w.u = e->at("val") != QLatin1String("none");
    if(const X* e = r->c("caps")) w.caps = wOn(e);
    if(const X* e = r->c("sz")) w.sz = e->num("val", 22) / 2;
    if(const X* e = r->c("color")){ const QString v = e->at("val"); if(v != QLatin1String("auto") && !v.isEmpty()) w.colour = QColor(QLatin1Char('#') + v); else if(v == QLatin1String("auto")) w.colour = QColor(); }
    if(const X* e = r->c("rFonts")){
        QString f = e->at("ascii"); if(f.isEmpty()) f = e->at("hAnsi");
        const QString theme = e->at("asciiTheme").isEmpty() ? e->at("hAnsiTheme") : e->at("asciiTheme");
        if(!theme.isEmpty()) f = theme.startsWith(QLatin1String("major")) ? th.major : th.minor;
        if(!f.isEmpty()) w.font = f;
    }
    if(const X* e = r->c("vertAlign")){ const QString v = e->at("val"); w.vert = v == QLatin1String("superscript") ? 1 : v == QLatin1String("subscript") ? -1 : 0; }
}
void mergeWP(const X* p, WProps& w){
    if(!p) return;
    if(const X* e = p->c("jc")){ const QString v = e->at("val"); w.jc = (v == QLatin1String("center")) ? 1 : (v == QLatin1String("right") || v == QLatin1String("end")) ? 2 : (v == QLatin1String("both") || v == QLatin1String("distribute")) ? 3 : 0; }
    if(const X* e = p->c("spacing")){
        if(e->has("before")) w.before = e->num("before"); if(e->has("after")) w.after = e->num("after");
        if(e->flag("beforeAutospacing")) w.before = 280; if(e->flag("afterAutospacing")) w.after = 280;   // "auto" spacing is 14 points
        if(e->has("line")){ w.line = e->num("line"); const QString r = e->at("lineRule"); w.lineRule = r == QLatin1String("exact") ? 1 : r == QLatin1String("atLeast") ? 2 : 0; }
    }
    if(const X* e = p->c("ind")){
        if(e->has("left")) w.indL = e->num("left"); else if(e->has("start")) w.indL = e->num("start");
        if(e->has("right")) w.indR = e->num("right"); else if(e->has("end")) w.indR = e->num("end");
        if(e->has("hanging")) w.first = -e->num("hanging"); else if(e->has("firstLine")) w.first = e->num("firstLine");
    }
    if(const X* n = p->c("numPr")){ if(const X* id = n->c("numId")) w.numId = id->at("val"); if(const X* l = n->c("ilvl")) w.ilvl = int(l->num("val")); }
    if(wOn(p->c("pageBreakBefore")) && p->c("pageBreakBefore")) w.pageBreakBefore = true;
}

struct Word {
    Pkg* pkg; const Theme* theme; DmlImportStats* st;
    X styles, numbering;
    WProps defaults;
    QHash<QString, const X*> styleById;
    QString defaultPara;
    QHash<QString, int> counters;                     // numId:level → the last number given
    QHash<QString, const X*> numToAbstract;

    void load(const QString& docPart){
        styles = pkg->xml(pkg->relOfType(docPart, "/styles"));
        numbering = pkg->xml(pkg->relOfType(docPart, "/numbering"));
        if(const X* root = styles.c("styles")){
            if(const X* dd = root->c("docDefaults")){ mergeWR(dd->c("rPrDefault", "rPr"), defaults, *theme); mergeWP(dd->c("pPrDefault", "pPr"), defaults); }
            for(const X& e : root->k) if(e.n == QLatin1String("style")){ styleById.insert(e.at("styleId"), &e); if(e.at("type") == QLatin1String("paragraph") && e.flag("default")) defaultPara = e.at("styleId"); }
        }
        if(std::isnan(defaults.sz)) defaults.sz = 10;
        if(defaults.font.isEmpty()) defaults.font = theme->minor;
        if(const X* root = numbering.c("numbering")){
            QHash<QString, const X*> abstracts;
            for(const X& e : root->k) if(e.n == QLatin1String("abstractNum")) abstracts.insert(e.at("abstractNumId"), &e);
            for(const X& e : root->k) if(e.n == QLatin1String("num")) if(const X* a = e.c("abstractNumId")) numToAbstract.insert(e.at("numId"), abstracts.value(a->at("val")));
        }
    }
    void applyStyle(const QString& id, WProps& w, int depth = 0){
        const X* st = styleById.value(id); if(!st || depth > 20) return;
        if(const X* b = st->c("basedOn")) applyStyle(b->at("val"), w, depth + 1);
        mergeWP(st->c("pPr"), w); mergeWR(st->c("rPr"), w, *theme);
    }
    WProps paragraphProps(const X* pPr){
        WProps w = defaults;
        const X* ps = pPr ? pPr->c("pStyle") : nullptr;
        applyStyle(ps ? ps->at("val") : defaultPara, w);
        if(pPr){ mergeWP(pPr, w); }
        return w;
    }
    const X* listLevel(const WProps& w){
        if(w.numId.isEmpty() || w.numId == QLatin1String("0")) return nullptr;
        const X* a = numToAbstract.value(w.numId); if(!a) return nullptr;
        for(const X& l : a->k) if(l.n == QLatin1String("lvl") && int(l.num("ilvl")) == qMax(0, w.ilvl)) return &l;
        return nullptr;
    }
    QString listLabel(const WProps& w, const X* lvl){
        const QString fmt = lvl->c("numFmt") ? lvl->c("numFmt")->at("val") : QStringLiteral("decimal");
        QString text = lvl->c("lvlText") ? lvl->c("lvlText")->at("val") : QStringLiteral("%1.");
        const int level = qMax(0, w.ilvl);
        const QString key = w.numId + QLatin1Char(':') + QString::number(level);
        for(auto it = counters.begin(); it != counters.end(); ){ const QStringList k = it.key().split(QLatin1Char(':')); if(k[0] == w.numId && k.value(1).toInt() > level) it = counters.erase(it); else ++it; }
        const int start = lvl->c("start") ? int(lvl->c("start")->num("val", 1)) : 1;
        const int n = counters.value(key, start - 1) + 1; counters.insert(key, n);
        if(fmt == QLatin1String("bullet")) return text.isEmpty() ? QStringLiteral("•") : (text[0].unicode() >= 0xF000 ? QStringLiteral("•") : text);
        if(fmt == QLatin1String("none")) return QString();
        auto num = [&](int lv) -> QString {
            const int v = lv == level ? n : counters.value(w.numId + QLatin1Char(':') + QString::number(lv), 1);
            QString f = fmt;
            if(lv != level) if(const X* a = numToAbstract.value(w.numId)) for(const X& l : a->k) if(l.n == QLatin1String("lvl") && int(l.num("ilvl")) == lv && l.c("numFmt")) f = l.c("numFmt")->at("val");
            const QString scheme = f == QLatin1String("lowerLetter") ? QStringLiteral("alphaLcPlain") : f == QLatin1String("upperLetter") ? QStringLiteral("alphaUcPlain")
                                 : f == QLatin1String("lowerRoman") ? QStringLiteral("romanLcPlain") : f == QLatin1String("upperRoman") ? QStringLiteral("romanUcPlain") : QStringLiteral("arabicPlain");
            return autoNumber(scheme, v); };
        for(int lv = 0; lv < 9; ++lv) text.replace(QStringLiteral("%%1").arg(lv + 1), num(lv));
        return text;
    }
    QFont font(const WProps& w, double unit){
        QFont f(w.font.isEmpty() ? theme->minor : w.font);
        double pt = std::isnan(w.sz) ? 11 : w.sz; if(w.vert != 0) pt *= 2.0 / 3;
        f.setPixelSize(qMax(1, qRound(pt * 96 / 72 * unit)));
        f.setBold(w.b == 1); f.setItalic(w.i == 1); f.setUnderline(w.u == 1);
        return f;
    }
    // A paragraph's text as runs, and the drawings it holds, in order.
    struct Para { QString text; QVector<TextRun> runs; QFont tall; QVector<const X*> drawings; bool pageBreakAfter = false; WProps props; QString label; QFont labelFont; QColor labelColour; };
    Para read(const X& p, double unit){
        Para P; P.props = paragraphProps(p.c("pPr"));
        P.tall = font(P.props, unit);
        std::function<void(const X&)> walkRuns = [&](const X& e){
            for(const X& r : e.k){
                if(r.n == QLatin1String("r")){
                    WProps w = P.props;
                    if(const X* rp = r.c("rPr")){ if(const X* rs = rp->c("rStyle")) applyStyle(rs->at("val"), w); mergeWR(rp, w, *theme); }
                    for(const X& c : r.k){
                        QString t;
                        if(c.n == QLatin1String("t")) t = c.t;
                        else if(c.n == QLatin1String("tab")) t = QStringLiteral("    ");
                        else if(c.n == QLatin1String("br")){ if(c.at("type") == QLatin1String("page")) P.pageBreakAfter = true; else t = QStringLiteral("\n"); }
                        else if(c.n == QLatin1String("cr")) t = QStringLiteral("\n");
                        else if(c.n == QLatin1String("drawing")) P.drawings << &c;
                        else if(c.n == QLatin1String("AlternateContent")){ for(const X& ch : c.k) if(ch.n == QLatin1String("Choice")) if(const X* d = find(&ch, QLatin1String("drawing"))){ P.drawings << d; break; } }
                        if(t.isEmpty()) continue;
                        if(w.caps == 1) t = t.toUpper();
                        TextRun run; run.start = int(P.text.size()); run.len = int(t.size()); run.font = font(w, unit); run.color = w.colour.isValid() ? w.colour : QColor(Qt::black);
                        if(w.vert != 0) run.baseline = w.vert * run.font.pixelSize() * 0.5;
                        if(P.runs.isEmpty() || run.font.pixelSize() > P.tall.pixelSize()) P.tall = run.font;
                        P.text += t; P.runs << run;
                    }
                } else if(r.n == QLatin1String("hyperlink") || r.n == QLatin1String("smartTag") || r.n == QLatin1String("ins") || r.n == QLatin1String("sdt") || r.n == QLatin1String("sdtContent") || r.n == QLatin1String("fldSimple")) walkRuns(r);
            }
        };
        walkRuns(p);
        if(const X* lvl = listLevel(P.props)){
            if(const X* lp = lvl->c("pPr")) mergeWP(lp, P.props);
            if(const X* pp = p.c("pPr")) mergeWP(pp, P.props);                 // the paragraph's own indents win over the list's
            P.label = listLabel(P.props, lvl);
            WProps lw = P.props; mergeWR(lvl->c("rPr"), lw, *theme);
            P.labelFont = font(lw, unit); if(lvl->c("rPr") && lvl->c("rPr")->c("rFonts")) { const QString f = lvl->c("rPr")->c("rFonts")->at("ascii"); if(f.startsWith(QLatin1String("Symbol")) || f.startsWith(QLatin1String("Wingdings"))) P.labelFont.setFamily(theme->minor); }
            P.labelColour = P.runs.isEmpty() ? QColor(Qt::black) : P.runs.first().color;
        }
        return P;
    }
    double lineHeight(const WProps& w, const QFont& f){
        const double h = QFontMetricsF(f).height();
        if(std::isnan(w.line)) return h;
        if(w.lineRule == 0) return h * w.line / 240.0;
        const double px = w.line / 15.0; return w.lineRule == 1 ? px : qMax(h, px);
    }
    // Lay paragraphs from `top` down an area `width` wide: text frames and list labels (in area px), the height used.
    QVector<Shape> paragraphs(const QVector<Para>& paras, double width, double unit, double* heightOut){
        QVector<Shape> out; double y = 0;
        for(const Para& P : paras){
            const WProps& w = P.props;
            const double before = std::isnan(w.before) ? 0 : w.before / 15.0 * unit, after = std::isnan(w.after) ? 0 : w.after / 15.0 * unit;
            const double indL = (std::isnan(w.indL) ? 0 : w.indL) / 15.0 * unit, indR = (std::isnan(w.indR) ? 0 : w.indR) / 15.0 * unit;
            const double first = (std::isnan(w.first) ? 0 : w.first) / 15.0 * unit;
            const double lh = lineHeight(w, P.tall), spacing = lh / qMax(1.0, QFontMetricsF(P.tall).height());
            const double avail = qMax(10.0, width - indL - indR);
            y += before;
            int lines = 0;
            const QFontMetricsF fm(P.tall);
            for(const QString& ln : (P.text.isEmpty() ? QStringLiteral(" ") : P.text).split(QLatin1Char('\n'))){
                const QRectF b = fm.boundingRect(QRectF(0, 0, avail, 1e7), Qt::TextWordWrap, ln.isEmpty() ? QStringLiteral(" ") : ln);
                lines += qMax(1, int(std::round(b.height() / qMax(1.0, fm.lineSpacing()))));
            }
            const double h = lines * lh;
            if(!P.label.isEmpty()){
                Shape b; b.type = ShapeType::Text; b.text = P.label; b.font = P.labelFont; b.name = QStringLiteral("List label");
                b.fill.type = FillType::Solid; b.fill.color = P.labelColour; b.stroke.enabled = false;
                b.transform = QTransform::fromTranslate(indL + qMin(0.0, first), y + fm.ascent() - QFontMetricsF(P.labelFont).ascent());
                out << b;
            }
            if(!P.text.trimmed().isEmpty()){
                Shape t; t.type = ShapeType::Text; t.textFrame = true; t.text = P.text; t.textRuns = P.runs; t.font = P.runs.first().font;
                t.fill.type = FillType::Solid; t.fill.color = P.runs.first().color; t.stroke.enabled = false;
                t.name = P.text.simplified().left(32); t.textAlign = qMax(0, w.jc); t.lineSpacing = spacing;
                const double x = indL + (P.label.isEmpty() ? qMax(-indL, first) : 0);    // XarQ's rich frames have no first-line indent: a first line's offset moves the whole paragraph
                t.rect = QRectF(0, 0, qMax(10.0, avail - (x - indL)), h * 1.25 + lh);
                t.transform = QTransform::fromTranslate(x, y);
                out << t; if(st) ++st->texts;
            }
            y += h + after;
        }
        if(heightOut) *heightOut = y;
        return out;
    }
};

} // namespace

Document importDocxDocument(const QMap<QString, QByteArray>& zip, DmlImportStats* st){
    Document doc;
    Pkg pkg(zip);
    QString main = pkg.relOfType(QString(), "/officeDocument"); if(main.isEmpty()) main = QStringLiteral("word/document.xml");
    const X* body = pkg.xml(main).c("document", "body"); if(!body) return doc;
    Theme theme; readTheme(pkg.xml(pkg.relOfType(main, "/theme")), theme);
    Word wd; wd.pkg = &pkg; wd.theme = &theme; wd.st = st; wd.load(main);
    // The page: the last section's size and margins (twips).
    double pw = 816, ph = 1056, mt = 96, mb = 96, ml = 96, mr = 96;
    if(const X* sp = body->c("sectPr")){
        if(const X* sz = sp->c("pgSz")){ pw = sz->num("w", 12240) / 15; ph = sz->num("h", 15840) / 15; }
        if(const X* m = sp->c("pgMar")){ mt = qAbs(m->num("top", 1440)) / 15; mb = qAbs(m->num("bottom", 1440)) / 15; ml = m->num("left", 1440) / 15; mr = m->num("right", 1440) / 15; }
    }
    pw = qBound(100.0, pw, 20000.0); ph = qBound(100.0, ph, 20000.0);
    const double textW = qMax(50.0, pw - ml - mr);
    QVector<QVector<Shape>> pages(1);
    double y = mt;
    auto newPage = [&](){ if(pages.size() < 2000) pages.append(QVector<Shape>()); y = mt; };
    auto room = [&](double h){ if(y + h > ph - mb && y > mt + 1) newPage(); };
    Walker w; w.cx.pkg = &pkg; w.cx.theme = &theme; w.cx.st = st; w.cx.part = main; w.cx.rels = pkg.rels(main);
    w.wordText = [&](const X& content, const QSizeF& area, const QTransform& toPage){
        QVector<Word::Para> paras; for(const X& p : content.k) if(p.n == QLatin1String("p")) paras << wd.read(p, 1.0);
        double h = 0; QVector<Shape> v = wd.paragraphs(paras, area.width(), 1.0, &h);
        for(Shape& s : v) s.transform = s.transform * toPage;
        return v;
    };
    // A drawing: its object scaled into `rect` (page px) on the current page.
    auto placeDrawing = [&](const X& drawing, const QRectF& rect, bool behind){
        const X* g = find(&drawing, QLatin1String("graphicData")); if(!g) return;
        const X* docPr = find(&drawing, QLatin1String("docPr"));
        w.out.clear(); w.objectName = docPr ? docPr->at("name") : QString();
        for(const X& obj : g->k){
            const X* xf = nullptr;
            if(const X* sp = obj.c("spPr")) xf = sp->c("xfrm"); else if(const X* gp = obj.c("grpSpPr")) xf = gp->c("xfrm");
            QTransform place = QTransform::fromScale(1 / kEmuPerPx, 1 / kEmuPerPx) * QTransform::fromTranslate(rect.x(), rect.y());
            if(xf && xf->c("ext") && xf->c("ext")->num("cx") > 0 && xf->c("ext")->num("cy") > 0){
                const QRectF own(xf->c("off") ? xf->c("off")->num("x") : 0, xf->c("off") ? xf->c("off")->num("y") : 0, xf->c("ext")->num("cx"), xf->c("ext")->num("cy"));
                place = QTransform::fromTranslate(-own.x(), -own.y()) * QTransform::fromScale(rect.width() / own.width(), rect.height() / own.height()) * QTransform::fromTranslate(rect.x(), rect.y());
            } else if(obj.n == QLatin1String("wpc")){                          // a drawing canvas: its shapes in the frame's space
                place = QTransform::fromScale(1 / kEmuPerPx, 1 / kEmuPerPx) * QTransform::fromTranslate(rect.x(), rect.y());
            }
            if(obj.n == QLatin1String("chart")){ if(st) ++st->charts; continue; }
            X one; one.k << (obj.n == QLatin1String("wpc") ? obj : obj);
            if(obj.n == QLatin1String("wpc")){ X inner; for(const X& k : obj.k) inner.k << k; w.walk(inner, place, LevelStyles(), Paint(), nullptr); }
            else w.walk(one, place, LevelStyles(), Paint(), nullptr);
        }
        if(w.out.isEmpty()) return;
        QVector<Shape>& pg = pages.last();
        if(behind) pg = w.out + pg; else pg += w.out;
        w.out.clear();
    };
    auto emu = [](const X* e){ return e ? e->t.toDouble() / kEmuPerPx : 0.0; };
    // Body content, top to bottom.
    std::function<void(const X&)> block = [&](const X& container){
        for(const X& e : container.k){
            if(e.n == QLatin1String("p")){
                Word::Para P = wd.read(e, 1.0);
                if(P.props.pageBreakBefore && y > mt + 1) newPage();
                const double paraTop = y;
                // Inline drawings first: side by side as characters would be, wrapping at the margin, each line aligned as the paragraph.
                QVector<const X*> anchored;
                QVector<QPair<const X*, QSizeF>> line; double lineW = 0;
                const double indent = std::isnan(P.props.indL) ? 0 : P.props.indL / 15;
                auto flushLine = [&](){
                    if(line.isEmpty()) return;
                    double lh = 0; for(const auto& d : line) lh = qMax(lh, d.second.height());
                    room(lh);
                    double dx = P.props.jc == 1 ? ml + (textW - lineW) / 2 : P.props.jc == 2 ? pw - mr - lineW : ml + indent;
                    for(const auto& d : line){ placeDrawing(*d.first, QRectF(dx, y + lh - d.second.height(), d.second.width(), d.second.height()), false); dx += d.second.width(); }
                    y += lh; line.clear(); lineW = 0;
                };
                for(const X* d : P.drawings){
                    if(const X* in = d->c("inline")){
                        const X* ext = in->c("extent"); const double dw = ext ? ext->num("cx") / kEmuPerPx : 0, dh = ext ? ext->num("cy") / kEmuPerPx : 0;
                        if(dw <= 0 || dh <= 0) continue;
                        if(!line.isEmpty() && lineW + dw > textW - indent) flushLine();
                        line << qMakePair(in, QSizeF(dw, dh)); lineW += dw;
                    } else if(d->c("anchor")) anchored << d->c("anchor");
                }
                flushLine();
                // The text.
                double h = 0; QVector<Shape> frames = wd.paragraphs({P}, textW, 1.0, &h);
                if(!P.drawings.isEmpty() && P.text.trimmed().isEmpty()) h = 0;      // a drawing's paragraph: its line is the drawing
                room(h);
                for(Shape& f : frames){ f.transform = f.transform * QTransform::fromTranslate(ml, y); pages.last() << f; }
                // Floating drawings, placed against the page, its margins or this paragraph.
                for(const X* a : anchored){
                    const X* ext = a->c("extent"); const double dw = ext ? ext->num("cx") / kEmuPerPx : 0, dh = ext ? ext->num("cy") / kEmuPerPx : 0;
                    if(dw <= 0 || dh <= 0) continue;
                    double ax = ml, ay = paraTop;
                    if(const X* ph = a->c("positionH")){
                        const QString rel = ph->at("relativeFrom");
                        const double base = rel == QLatin1String("page") ? 0 : rel == QLatin1String("rightMargin") ? pw - mr : ml;
                        const double span = rel == QLatin1String("page") ? pw : rel == QLatin1String("leftMargin") ? ml : rel == QLatin1String("rightMargin") ? mr : textW;
                        if(const X* al = ph->c("align")){ const QString v = al->t.trimmed(); ax = base + (v == QLatin1String("center") ? (span - dw) / 2 : (v == QLatin1String("right") || v == QLatin1String("outside")) ? span - dw : 0); }
                        else ax = base + emu(ph->c("posOffset"));
                    }
                    if(const X* pv = a->c("positionV")){
                        const QString rel = pv->at("relativeFrom");
                        const double base = rel == QLatin1String("page") ? 0 : (rel == QLatin1String("margin") || rel == QLatin1String("topMargin")) ? (rel == QLatin1String("topMargin") ? 0 : mt) : rel == QLatin1String("bottomMargin") ? ph - mb : paraTop;
                        const double span = rel == QLatin1String("page") ? ph : rel == QLatin1String("margin") ? ph - mt - mb : rel == QLatin1String("topMargin") ? mt : rel == QLatin1String("bottomMargin") ? mb : dh;
                        if(const X* al = pv->c("align")){ const QString v = al->t.trimmed(); ay = base + (v == QLatin1String("center") ? (span - dh) / 2 : (v == QLatin1String("bottom") || v == QLatin1String("outside")) ? span - dh : 0); }
                        else ay = base + emu(pv->c("posOffset"));
                    }
                    if(a->flag("simplePos") && a->c("simplePos")) { ax = a->c("simplePos")->num("x") / kEmuPerPx; ay = a->c("simplePos")->num("y") / kEmuPerPx; }
                    placeDrawing(*a, QRectF(ax, ay, dw, dh), a->flag("behindDoc"));
                }
                y += h;
                if(P.pageBreakAfter) newPage();
                if(const X* pp = e.c("pPr")) if(pp->c("sectPr")) newPage();
            }
            else if(e.n == QLatin1String("tbl")){
                // A table: rows sized to their tallest cell, split across pages. A one-column table is layout, not
                // data: its cells' content (nested tables and all) flows like the body's.
                Shape t; t.type = ShapeType::Table; t.name = QStringLiteral("Table");
                if(const X* g = e.c("tblGrid")) for(const X& c : g->k) if(c.n == QLatin1String("gridCol")) t.table.colW << qMax(4.0, c.num("w") / 15);
                if(t.table.colW.size() <= 1){ for(const X& tr : e.k) if(tr.n == QLatin1String("tr")) for(const X& tc : tr.k) if(tc.n == QLatin1String("tc")) block(tc); continue; }
                const X* bd = e.c("tblPr") ? e.c("tblPr")->c("tblBorders") : nullptr;
                t.table.borderColor = Qt::black; t.table.borderW = 0.75;
                if(bd){ const X* side = bd->c("insideH") ? bd->c("insideH") : bd->c("top");
                    if(side){ if(side->at("val") == QLatin1String("nil") || side->at("val") == QLatin1String("none")) t.table.borderColor = Qt::transparent;
                              else { const QString c = side->at("color"); if(!c.isEmpty() && c != QLatin1String("auto")) t.table.borderColor = QColor(QLatin1Char('#') + c); t.table.borderW = qMax(0.5, side->num("sz", 4) / 8 * 96 / 72); } } }
                else t.table.borderColor = Qt::transparent;
                QVector<double> rowH; QVector<TableCell> cells; QFont tf = wd.font(wd.defaults, 1.0); bool fontSet = false;
                for(const X& tr : e.k){
                    if(tr.n != QLatin1String("tr")) continue;
                    double rh = 0; if(const X* trp = tr.c("trPr")) if(const X* hh = trp->c("trHeight")) rh = hh->num("val") / 15;
                    int col = 0;
                    for(const X& tc : tr.k){
                        if(tc.n != QLatin1String("tc")) continue;
                        TableCell cell; cell.valign = 1; cell.textColor = Qt::black;
                        const X* pr = tc.c("tcPr");
                        const int span = pr && pr->c("gridSpan") ? qMax(1, int(pr->c("gridSpan")->num("val", 1))) : 1;
                        cell.colSpan = span;
                        if(pr && pr->c("vMerge") && pr->c("vMerge")->at("val") != QLatin1String("restart")){ cell.colSpan = 0; cell.rowSpan = 0; }
                        if(pr) if(const X* sh = pr->c("shd")){ const QString f = sh->at("fill"); if(!f.isEmpty() && f != QLatin1String("auto")) cell.bg = QColor(QLatin1Char('#') + f); }
                        QStringList texts; double cw = 0; for(int k = col; k < col + span && k < t.table.colW.size(); ++k) cw += t.table.colW[k];
                        double ch = 0;
                        QVector<const X*> cellParas;                                    // its paragraphs, nested tables' included
                        std::function<void(const X&)> gather = [&](const X& c){ for(const X& k : c.k){ if(k.n == QLatin1String("p")) cellParas << &k; else if(k.n == QLatin1String("tbl") || k.n == QLatin1String("tr") || k.n == QLatin1String("tc") || k.n == QLatin1String("sdt") || k.n == QLatin1String("sdtContent")) gather(k); } };
                        gather(tc);
                        for(const X* pp : cellParas){ const X& p = *pp;
                            Word::Para P = wd.read(p, 1.0); texts << P.text;
                            if(!fontSet && !P.runs.isEmpty()){ tf = P.runs.first().font; fontSet = true; }
                            if(!P.runs.isEmpty()){ cell.textColor = P.runs.first().color; cell.bold = P.runs.first().font.bold(); }
                            if(P.props.jc == 1) cell.align = 1; else if(P.props.jc == 2) cell.align = 2;
                            double h = 0; wd.paragraphs({P}, qMax(10.0, cw - 10), 1.0, &h); ch += h;
                        }
                        cell.text = texts.join(QLatin1Char('\n'));
                        rh = qMax(rh, ch + 10);
                        cells << cell;
                        for(int k = 1; k < span; ++k){ TableCell cov; cov.colSpan = 0; cov.rowSpan = 0; cells << cov; }
                        col += span;
                    }
                    while(col < t.table.colW.size()){ cells << TableCell(); ++col; }
                    while(cells.size() % t.table.colW.size()) cells << TableCell();
                    rowH << qMax(rh, 12.0);
                }
                // Vertical merges: the starting cell spans the rows that continue it.
                const int nc = t.table.colW.size(), nr = rowH.size();
                for(int r = 0; r < nr; ++r) for(int c = 0; c < nc; ++c){ TableCell& cl = cells[r * nc + c]; if(cl.colSpan == 0 || cl.rowSpan == 0) continue;
                    int span = 1; while(r + span < nr && cells[(r + span) * nc + c].rowSpan == 0 && cells[(r + span) * nc + c].colSpan == 0) ++span; cl.rowSpan = span; }
                t.font = tf; t.table.pad = 5;
                int r0 = 0;
                while(r0 < nr){
                    double h = 0; int r1 = r0;
                    while(r1 < nr && (r1 == r0 || y + h + rowH[r1] <= ph - mb)) h += rowH[r1++];
                    if(y + rowH[r0] > ph - mb && y > mt + 1){ newPage(); continue; }
                    Shape part = t; part.table.rowH = rowH.mid(r0, r1 - r0); part.table.cells = cells.mid(r0 * nc, (r1 - r0) * nc);
                    for(TableCell& c : part.table.cells) if(c.rowSpan > 1) c.rowSpan = qMin(c.rowSpan, r1 - r0);
                    part.transform = QTransform::fromTranslate(ml, y);
                    pages.last() << part; if(st) ++st->shapes;
                    y += h; r0 = r1;
                    if(r0 < nr) newPage();
                }
            }
            else if(e.n == QLatin1String("sdt")){ if(const X* c = e.c("sdtContent")) block(*c); }
        }
    };
    block(*body);
    while(pages.size() > 1 && pages.last().isEmpty()) pages.removeLast();
    bool any = false; for(const auto& p : pages) if(!p.isEmpty()) any = true;
    if(!any) return doc;
    doc.pageSize = QSizeF(pw, ph); doc.pageCount = pages.size();
    for(int i = 0; i < pages.size(); ++i){
        Shape g; g.type = ShapeType::Group; g.name = QStringLiteral("Page %1").arg(i + 1); g.children = pages[i];
        g.transform = QTransform::fromTranslate(0, doc.pageRect(i).top());
        doc.shapes << g;
    }
    if(st) st->pages = pages.size();
    return doc;
}

} // namespace xara
