// theme.cpp — the light look, a port of Subtitld's interface/theme.py (same rules, same numbers), so the two apps
// turn their shared dark palette over the same way. See theme.h.
#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <set>

namespace theme {

namespace {

bool g_light = false;
QString g_darkQss;
QString g_system;          // the system's look, read when the pick was last applied

// Python's round(): halves to even, as the original rounds.
int pyRound(double v){ return int(std::nearbyint(v)); }
double pyMod1(double v){ double m=std::fmod(v,1.0); return m<0 ? m+1.0 : m; }

// colorsys.rgb_to_hls / hls_to_rgb, 0..1.
void rgbToHls(double r, double g, double b, double& h, double& l, double& s){
    const double maxc=std::max({r,g,b}), minc=std::min({r,g,b}), sumc=maxc+minc, rangec=maxc-minc;
    l=sumc/2.0;
    if(minc==maxc){ h=0; s=0; return; }
    s = l<=0.5 ? rangec/sumc : rangec/(2.0-maxc-minc);
    const double rc=(maxc-r)/rangec, gc=(maxc-g)/rangec, bc=(maxc-b)/rangec;
    if(r==maxc) h=bc-gc; else if(g==maxc) h=2.0+rc-bc; else h=4.0+gc-rc;
    h=pyMod1(h/6.0);
}
double hlsV(double m1, double m2, double hue){
    hue=pyMod1(hue);
    if(hue<1.0/6.0) return m1+(m2-m1)*hue*6.0;
    if(hue<0.5) return m2;
    if(hue<2.0/3.0) return m1+(m2-m1)*(2.0/3.0-hue)*6.0;
    return m1;
}
void hlsToRgb(double h, double l, double s, double& r, double& g, double& b){
    if(s==0.0){ r=g=b=l; return; }
    const double m2 = l<=0.5 ? l*(1.0+s) : l+s-(l*s), m1=2.0*l-m2;
    r=hlsV(m1,m2,h+1.0/3.0); g=hlsV(m1,m2,h); b=hlsV(m1,m2,h-1.0/3.0);
}
void hlsTo255(double h, double l, double s, int& r, int& g, int& b){
    double fr,fg,fb; hlsToRgb(h,l,s,fr,fg,fb); r=pyRound(fr*255); g=pyRound(fg*255); b=pyRound(fb*255);
}
double lightnessOf(int r, int g, int b){ double h,l,s; rgbToHls(r/255.0,g/255.0,b/255.0,h,l,s); return l; }

// --- colours ---
//
// A colour's light counterpart depends on what it is drawn as:
// - text: every text turns dark, the pale ones (the most read) darkest, keeping the dark look's order of emphasis;
//   dark text on a pale key stays dark;
// - an icon's pale strokes and fills: one blue, the palette's #566f84;
// - a surface: the dark grounds turn light, keeping their order (a raised button is lighter than the panel under it,
//   a well darker), so depth reads the same; pale surfaces (keys, tonal buttons) stay pale; translucent lightenings
//   (hover) become translucent darkenings;
// - a line: dark separators become light-grey ones, pale outlines darker ones.
// Saturated colours keep their hue: as text and lines they get dark enough to read on white; as fills they stay as
// they are, the dark ones becoming pale tints. Black stays black (shadows, contact lines), lighter.

// Surfaces darker than this are grounds (turn light); lighter ones (keys, handles) stay pale or mid.
const double kGround=0.5;
// Dark-look lightness of a ground → light-look lightness: rising, so the dark look's raised pieces (lighter) are the
// light look's lighter ones too.
const double kGrounds[][2]={{0.0,0.865},{0.10,0.88},{0.15,0.90},{0.22,0.93},{0.30,0.955},{0.40,0.985},{0.50,1.0}};
// The light look's icons: the palette's #566f84, and #4a6376 for the white (emphasised) ones.
const Rgba kIconBlue{0x56,0x6f,0x84,255}, kIconEmphasis{0x4a,0x63,0x76,255};
const double kInkHue=0.575;         // the hue of the palette's blue-greys (#b8cee0)
const double kAccentChroma=0.22;    // above this chroma (max - min channel) a colour is an accent, not a blue-grey

double interpolateGrounds(double x){
    const int n=int(sizeof kGrounds/sizeof kGrounds[0]);
    for(int i=0;i+1<n;++i){
        const double x0=kGrounds[i][0], y0=kGrounds[i][1], x1=kGrounds[i+1][0], y1=kGrounds[i+1][1];
        if(x<=x1) return y0+(y1-y0)*(x-x0)/(x1-x0);
    }
    return kGrounds[n-1][1];
}

// Black, or a translucent near-black: a shadow or a contact line, which stays dark in both looks. An opaque
// near-black is a background, and turns light.
bool isShadow(int r, int g, int b, int a=255){ return (r==0&&g==0&&b==0) || (a<255 && std::max({r,g,b})<=12); }

double linear(double c){ c/=255.0; return c<=0.04045 ? c/12.92 : std::pow((c+0.055)/1.055, 2.4); }
double luminance(int r, int g, int b){ return 0.2126*linear(r)+0.7152*linear(g)+0.0722*linear(b); }
double contrastWithWhite(int r, int g, int b){ return (1.0+0.05)/(luminance(r,g,b)+0.05); }

// The lightness, at most `l`, at which hue `h` reaches `ratio` against white.
double readable(double h, double l, double s, double ratio){
    while(l>0){
        int r,g,b; hlsTo255(h,l,s,r,g,b);
        if(contrastWithWhite(r,g,b)>=ratio) return l;
        l-=0.01;
    }
    return 0.0;
}

// Text's lightness: the palette's own dark blues, white's text as #304251 (0.25), #b8cee0's as #3e5363 (0.32).
double textLightness(double l){ return l>=0.5 ? 0.25+(1-l)*0.35 : std::min(l,0.32); }

// --- stylesheets and SVGs ---

const QRegularExpression& colourRe(){
    static const QRegularExpression re(R"(#[0-9a-fA-F]{8}\b|#[0-9a-fA-F]{6}\b|#[0-9a-fA-F]{3}\b|rgba?\([^)]*\))"
                                       R"(|(?<![\w\-#])(?:black|white|grey|gray|red)\b(?![\w\-]))");
    return re;
}

bool parseColour(const QString& text, bool argb, Rgba& out){
    if(text.startsWith('#')){
        QString d=text.mid(1);
        if(d.size()==3){ QString x; for(QChar c : d){ x+=c; x+=c; } d=x; }
        int a=255; QString rgb=d;
        if(d.size()==8){
            if(argb){ a=d.left(2).toInt(nullptr,16); rgb=d.mid(2); }
            else    { rgb=d.left(6); a=d.mid(6).toInt(nullptr,16); }
        }
        bool ok1,ok2,ok3;
        out={rgb.mid(0,2).toInt(&ok1,16), rgb.mid(2,2).toInt(&ok2,16), rgb.mid(4,2).toInt(&ok3,16), a};
        return ok1&&ok2&&ok3;
    }
    if(text.startsWith("rgb")){
        const int open=text.indexOf('(');
        const QStringList parts=text.mid(open+1, text.size()-open-2).split(',');
        if(parts.size()<3) return false;
        bool ok=true; int c[3];
        for(int i=0;i<3;++i){ bool k; c[i]=int(parts[i].trimmed().toDouble(&k)); ok=ok&&k; }
        int a=255;
        if(parts.size()>3){
            const QString p=parts[3].trimmed(); bool k;
            QString num=p; while(num.endsWith('%')) num.chop(1);
            const double v=num.toDouble(&k); ok=ok&&k;
            a = p.endsWith('%') ? pyRound(v*2.55) : (v<=1 && p.contains('.')) ? pyRound(v*255) : pyRound(v);
        }
        out={c[0],c[1],c[2],a};
        return ok;
    }
    const QString n=text.toLower();
    if(n=="black") out={0,0,0,255};
    else if(n=="white") out={255,255,255,255};
    else if(n=="grey"||n=="gray") out={128,128,128,255};
    else if(n=="red") out={255,0,0,255};
    else return false;
    return true;
}

QString hex2(int v){ return QString("%1").arg(v,2,16,QChar('0')); }

QString writeColour(const Rgba& c, const QString& original, bool argb){
    if(original.startsWith('#') && (original.size()==4 || original.size()==7)) return "#"+hex2(c.r)+hex2(c.g)+hex2(c.b);
    if(original.startsWith('#') && !argb) return "#"+hex2(c.r)+hex2(c.g)+hex2(c.b)+hex2(c.a);
    return QString("rgba(%1, %2, %3, %4)").arg(c.r).arg(c.g).arg(c.b).arg(c.a);
}

QString mapColourText(const QString& text, bool argb, Role role){
    Rgba c; if(!parseColour(text,argb,c)) return text;
    return writeColour(mapRgba(c.r,c.g,c.b,c.a,role), text, argb);
}

template<class F> QString replaceAll(const QString& text, const QRegularExpression& re, F fn){
    QString out; qsizetype last=0;
    for(auto it=re.globalMatch(text); it.hasNext();){
        const auto m=it.next();
        out+=text.mid(last, m.capturedStart()-last);
        out+=fn(m);
        last=m.capturedEnd();
    }
    out+=text.mid(last);
    return out;
}

Role roleOf(const QString& prop){
    if(prop=="color" || prop=="selection-color") return Text;
    if(prop.startsWith("border") || prop.startsWith("outline") || prop.startsWith("gridline")) return Line;
    return Surface;
}

// A gradient's stops, keeping the depth they have on the dark look: around the light counterpart of their mean, they
// differ in lightness as they did (the grounds' curve would flatten them), the lightest at most white.
QString mapGradient(const QString& value, Role role){
    struct Stop { qsizetype start, end; QString text; Rgba c; bool ok; };
    QVector<Stop> stops;
    for(auto it=colourRe().globalMatch(value); it.hasNext();){
        const auto m=it.next(); Stop s{m.capturedStart(), m.capturedEnd(), m.captured(0), {}, false};
        s.ok=parseColour(s.text,true,s.c); stops<<s;
    }
    auto opaqueStop=[](const Rgba& c){ return c.a==255 && !isShadow(c.r,c.g,c.b); };
    QVector<Rgba> opaque;
    for(const auto& s : stops) if(s.ok && opaqueStop(s.c)) opaque<<s.c;
    if(role!=Surface || opaque.size()<2)
        return replaceAll(value, colourRe(), [&](const QRegularExpressionMatch& m){ return mapColourText(m.captured(0),true,role); });
    double sum=0, top=0; double mr=0,mg=0,mb=0;
    for(const auto& c : opaque){ const double l=lightnessOf(c.r,c.g,c.b); sum+=l; top=std::max(top,l); mr+=c.r; mg+=c.g; mb+=c.b; }
    const double mean=sum/opaque.size();
    const int n=int(opaque.size());
    const Rgba centreC=mapRgba(pyRound(mr/n), pyRound(mg/n), pyRound(mb/n), 255, Surface);
    const double centre=std::min(lightnessOf(centreC.r,centreC.g,centreC.b), 1.0-(top-mean));
    QString out; qsizetype last=0;
    for(const auto& s : stops){
        out+=value.mid(last, s.start-last);
        if(s.ok && opaqueStop(s.c)){
            double h,l,sat; rgbToHls(s.c.r/255.0,s.c.g/255.0,s.c.b/255.0,h,l,sat);
            int r,g,b; hlsTo255(h, std::min(1.0, centre+(l-mean)), sat, r,g,b);
            out+=writeColour({r,g,b,255}, s.text, true);
        } else out+=mapColourText(s.text,true,role);
        last=s.end;
    }
    out+=value.mid(last);
    return out;
}

// A hovered or selected item drawn as a lighter ground on the dark look is a tint of the palette's #566f84 on the
// light one (a lighter ground than white there is none).
const char* kHoverTint="rgba(86, 111, 132, 26)";
const char* kSelectedTint="rgba(86, 111, 132, 64)";

// --- SVGs ---

const QRegularExpression& svgShapeRe(){
    static const QRegularExpression re(R"(<(?:path|rect|circle|ellipse|polygon|polyline|line|stop|g)\b[^>]*>)");
    return re;
}
const QRegularExpression& svgOpacityRe(){
    static const QRegularExpression re(R"(((?:fill-|stroke-|stop-)?opacity\s*[=:]\s*["']?)([0-9.]+))");
    return re;
}

// A shape or a gradient stop in translucent black (an SVG's default colour) is a darkening: on a light ground it
// keeps a fifth of it.
QString lighterBlack(const QString& tag){
    static const QRegularExpression paintRe(R"((?:fill|stroke|stop-color)\s*[=:]\s*["']?\s*(#[0-9a-fA-F]{3,6}|black|none|url))");
    QStringList paints;
    for(auto it=paintRe.globalMatch(tag); it.hasNext();){ const QString p=it.next().captured(1).toLower(); if(p!="none") paints<<p; }
    if(paints.isEmpty() && !tag.contains("stroke")) paints<<"#000";
    if(paints.isEmpty()) return tag;
    for(const QString& p : paints) if(p!="#000" && p!="#000000" && p!="black") return tag;
    if(!svgOpacityRe().match(tag).hasMatch()){
        if(tag.startsWith("<stop"))   // a black stop, opaque: a shadow's dark end
            return tag.endsWith("/>") ? tag.left(tag.size()-2)+" stop-opacity=\"0.2\"/>" : tag.left(tag.size()-1)+" stop-opacity=\"0.2\">";
        return tag;
    }
    return replaceAll(tag, svgOpacityRe(), [](const QRegularExpressionMatch& m){
        return m.captured(1)+QString::number(m.captured(2).toDouble()*0.2,'g',3); });
}

// A pale piece's colour on the light look: the palette's pale blues as they are, a neutral grey given their hue (grey
// reads as disabled on a light ground), its dark ink as it is.
Rgba pieceColour(const Rgba& c){
    double h,l,s; rgbToHls(c.r/255.0,c.g/255.0,c.b/255.0,h,l,s);
    if(l<0.6 || (std::max({c.r,c.g,c.b})-std::min({c.r,c.g,c.b}))/255.0>kAccentChroma) return c;
    if(s>=0.05) return c;
    int r,g,b; hlsTo255(kInkHue,l,0.39,r,g,b);
    return {r,g,b,c.a};
}

QString mapSvgColour(const QString& text, Role role){
    const QString stripped=text.trimmed();
    Rgba c; if(!parseColour(stripped,false,c)) return text;
    c = role==Surface ? pieceColour(c) : mapRgba(c.r,c.g,c.b,c.a,role);
    const qsizetype lead=text.size()-QString(text).remove(QRegularExpression("^\\s+")).size();
    return text.left(lead)+writeColour(c,stripped,false);
}

// --- files ---

const QString kChrome=":/xarq/assets/chrome";

QString lightDir(){
    const QString base=QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    return (base.isEmpty() ? QDir::tempPath()+"/xarq" : base)+"/theme-light";
}

QByteArray readAll(const QString& path){ QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

// The SVGs the stylesheet lays under widgets (border and background images): grounds, whatever their shape.
std::set<QString> stylesheetGrounds(){
    static const QRegularExpression re(R"((?:border|background)-image\s*:\s*url\(['"]?:/xarq/assets/chrome/([^)'"]+\.svg))");
    std::set<QString> out;
    for(auto it=re.globalMatch(g_darkQss); it.hasNext();) out.insert(it.next().captured(1));
    return out;
}

// What an SVG is, for its light version:
// - "piece": a key, a tonal button, a tab, a handle: mostly a pale surface (named so, or with dark ink on it); it
//   stays pale;
// - "ink": an icon drawn in dark ink (it sits on a pale key); kept;
// - "solid": a pale icon filled, its figure cut out of it; mid-tone;
// - "icon": other icons (pale strokes, dark) and dark grounds (light).
// The save button's green (unsaved changes) is kept as it is.
QString kindOf(const QString& name, const QByteArray& svg, const std::set<QString>& grounds){
    if(name.startsWith("save-unsaved")) return "ink";
    static const QRegularExpression pieceRe(R"(^(?:tab-|scrollbar-handle|save-|export-|check-on))");
    QImage image(48,48,QImage::Format_ARGB32); image.fill(Qt::transparent);
    { QPainter p(&image); QSvgRenderer(svg).render(&p); }
    int covered=0, pale=0, ink=0;
    for(int y=0;y<48;++y) for(int x=0;x<48;++x){
        const QColor px=image.pixelColor(x,y);
        if(px.alpha()>200){ ++covered; pale+=px.lightnessF()>0.6; ink+=px.lightnessF()<0.4; }
    }
    if(!covered) return "icon";
    const bool ground=grounds.count(name)>0;
    if(pale>covered*0.6){
        if(covered>48*48*0.5 && (ink>covered*0.02 || pieceRe.match(name).hasMatch())) return "piece";
        if(covered>48*48*0.4 && !ground) return "solid";
        return "icon";
    }
    if(ink>covered*0.6 && covered<48*48*0.85 && !ground) return "ink";
    return "icon";
}

// Write the light SVGs (turned over, or drawn for the light look in assets/chrome/light/) into the cache.
QString ensureLightGraphics(){
    const QString target=lightDir();
    QDir().mkpath(target);
    const auto grounds=stylesheetGrounds();
    for(const QString& name : QDir(kChrome).entryList({"*.svg"}, QDir::Files)){
        QByteArray out=readAll(kChrome+"/light/"+name);
        if(out.isEmpty()){
            const QByteArray src=readAll(kChrome+"/"+name);
            const QString kind=kindOf(name,src,grounds);
            out = kind=="ink" ? src : mapSvg(QString::fromUtf8(src), kind=="piece" ? Surface : kind=="solid" ? SolidIcon : Icon).toUtf8();
        }
        const QString path=target+"/"+name;
        if(readAll(path)!=out){ QFile f(path); if(f.open(QIODevice::WriteOnly|QIODevice::Truncate)) f.write(out); }
    }
    return target;
}

// The application's stylesheet in the current look; its sprites point at the current look's files.
QString stylesheet(){
    if(!g_light) return g_darkQss;
    const QString folder=ensureLightGraphics();
    QString text=g_darkQss;
    text.remove(QRegularExpression(R"(/\*.*?\*/)", QRegularExpression::DotMatchesEverythingOption));
    text=mapStylesheet(text)+"\n"+QString::fromUtf8(readAll(":/xarq/assets/theme-light.qss"));
    static const QRegularExpression urlRe(R"(url\((["']?):/xarq/assets/chrome/([^)'"]+?\.svg)\1\))");
    return replaceAll(text, urlRe, [&](const QRegularExpressionMatch& m){ return "url("+folder+"/"+m.captured(2)+")"; });
}

QPalette palette(){
    // XarQ palette — Subtitld's slate ink scale + sky accent (#B8CEE0).
    const QColor chrome(0x0D,0x12,0x16), panel(0x1A,0x23,0x2B), raised(0x21,0x2D,0x37),
                 well(0x30,0x42,0x51), textStrong(0xF0,0xF6,0xFB), textBody(0xB8,0xCE,0xE0),
                 textSec(0x8E,0xA6,0xB9), textDis(0x56,0x6F,0x84), accent(0xB8,0xCE,0xE0);
    QPalette p;
    p.setColor(QPalette::Window, surface(chrome));
    p.setColor(QPalette::WindowText, ink(textBody));
    p.setColor(QPalette::Base, surface(well));
    p.setColor(QPalette::AlternateBase, surface(panel));
    p.setColor(QPalette::Text, ink(textBody));
    p.setColor(QPalette::BrightText, ink(textStrong));
    p.setColor(QPalette::Button, surface(raised));
    p.setColor(QPalette::ButtonText, ink(textBody));
    p.setColor(QPalette::ToolTipBase, surface(QColor(0x1A,0x23,0x2B)));
    p.setColor(QPalette::ToolTipText, ink(Qt::white));
    p.setColor(QPalette::Highlight, surface(accent));
    p.setColor(QPalette::HighlightedText, ink(QColor(0x30,0x42,0x51)));
    p.setColor(QPalette::Link, ink(QColor(0xC8,0xDB,0xE9)));
    p.setColor(QPalette::PlaceholderText, ink(textSec));
    p.setColor(QPalette::Disabled, QPalette::Text, ink(textDis));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, ink(textDis));
    p.setColor(QPalette::Disabled, QPalette::WindowText, ink(textDis));
    return p;
}

// A widget's own stylesheet, written in the dark look's colours, turned over with the rest. The dark original is kept
// on the widget so a change of look can redo it.
const char* kDarkSheet="xqThemeDark";
const char* kSetSheet="xqThemeSet";

void restyle(QWidget* w){
    const QString dark=w->property(kDarkSheet).toString();
    // A sheet of bare declarations (no selector) is the widget's own: turned over as one block.
    const bool bare=!dark.contains('{');
    QString want=dark;
    if(g_light){ want=mapStylesheet(bare ? "{"+dark+"}" : dark); if(bare) want=want.mid(1, want.size()-2); }
    w->setProperty(kSetSheet, want);
    if(w->styleSheet()!=want) w->setStyleSheet(want);
}

class SheetFilter : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if((e->type()==QEvent::StyleChange || e->type()==QEvent::Polish) && o->isWidgetType()){
            auto* w=static_cast<QWidget*>(o);
            const QString sheet=w->styleSheet();
            if(!sheet.isEmpty() && !w->property(KeepProperty).toBool() && sheet!=w->property(kSetSheet).toString()){
                w->setProperty(kDarkSheet, sheet);   // set by the code: a dark-look sheet
                restyle(w);
            }
        }
        return QObject::eventFilter(o,e);
    }
};

void applyLook(bool light){
    auto* app=qobject_cast<QApplication*>(QCoreApplication::instance());
    const bool changed = light!=g_light;
    g_light=light;
    if(!app) return;
    app->setPalette(palette());
    app->setStyleSheet(stylesheet());
    if(changed){
        for(QWidget* w : QApplication::allWidgets())
            if(w->property(kDarkSheet).isValid()) restyle(w);
        for(QWidget* w : QApplication::allWidgets()) w->update();
        emit notifier()->changed(light);
    }
}

QString resolve(const QString& choice){ return choice=="system" ? systemTheme() : choice; }

// Draw in the look picked (or the system's). Qt's own pieces the stylesheet doesn't reach (native dialogs) are told
// the picked look too, and left to the system's when that is the pick.
void apply(){
    const QString choice=setting();
#if QT_VERSION >= QT_VERSION_CHECK(6,8,0)
    if(auto* app=qobject_cast<QGuiApplication*>(QCoreApplication::instance())){
        g_system.clear();
        app->styleHints()->unsetColorScheme();
        g_system=systemTheme();
        if(choice=="dark") app->styleHints()->setColorScheme(Qt::ColorScheme::Dark);
        else if(choice=="light") app->styleHints()->setColorScheme(Qt::ColorScheme::Light);
    }
#endif
    applyLook(resolve(choice)=="light");
}

} // namespace

QStringList choices(){ return {"system","dark","light"}; }

QString setting(){
    // The self-test and the headless renders always draw in the dark look, whatever the user picked.
    const QString env=qEnvironmentVariable("XARQ_THEME");
    if(choices().contains(env)) return env;
    const QString v=QSettings("XarQ","XarQ").value("ui/theme","system").toString();
    return choices().contains(v) ? v : "system";
}

void setSetting(const QString& choice){
    QSettings("XarQ","XarQ").setValue("ui/theme", choices().contains(choice) ? choice : "system");
    apply();
}

QString systemTheme(){
    if(!g_system.isEmpty()) return g_system;
#if QT_VERSION >= QT_VERSION_CHECK(6,5,0)
    if(auto* app=qobject_cast<QGuiApplication*>(QCoreApplication::instance()))
        if(app->styleHints()->colorScheme()==Qt::ColorScheme::Light) return "light";
#endif
    return "dark";
}

bool isLight(){ return g_light; }

Notifier* notifier(){ static QPointer<Notifier> n; if(!n) n=new Notifier(QCoreApplication::instance()); return n; }

void install(QApplication& app, const QString& darkQss){
    g_darkQss=darkQss;
    app.installEventFilter(new SheetFilter(&app));
    g_light=false;
    app.setPalette(palette());
    apply();
    // A change of the system's look is followed while it is the one picked (with a look picked, the changes are Qt
    // reporting that pick).
#if QT_VERSION >= QT_VERSION_CHECK(6,5,0)
    QObject::connect(app.styleHints(), &QStyleHints::colorSchemeChanged, &app, [](Qt::ColorScheme scheme){
        if(setting()!="system") return;
        g_system = scheme==Qt::ColorScheme::Light ? "light" : "dark";
        applyLook(g_system=="light");
    });
#endif
}

Rgba mapRgba(int r, int g, int b, int a, Role role){
    if(isShadow(r,g,b,a))   // a darkening: a line (an edge) keeps some of it, a wash over an area little
        return {r,g,b, a==255 ? a : pyRound(a*(role==Surface ? 0.15 : 0.4))};
    // A field's slanted label (shapes::NEST), a step off the field: chosen by hand, turning over loses the step.
    if((role==Surface || role==Auto) && r==0x33 && g==0x47 && b==0x5a) return {0xd0,0xda,0xe3,a};
    double h,l,s; rgbToHls(r/255.0,g/255.0,b/255.0,h,l,s);
    if((role==Auto || role==Icon || role==SolidIcon) && l<0.5) role=Surface;
    else if(role==Auto) role=Text;
    if(a<255 && std::min({r,g,b})>=245){
        // translucent white: a lightening over a ground, a darkening on light; a 1px highlight line stays one
        if(role==Surface) return {0,0,0,pyRound(a*0.6)};
        if(role==Line) return {r,g,b,a};
    }
    const bool accent=(std::max({r,g,b})-std::min({r,g,b}))/255.0>kAccentChroma;
    if((role==Text || role==Icon || role==SolidIcon) && !accent){
        // the palette's darks (#304251, #3e5363); white and the greys, with no hue of their own, take theirs
        if(s>0.05) s=std::min(s,0.26); else { h=kInkHue; s=0.26; }
    }
    double lightness;
    if(role==Text){
        lightness = accent ? readable(h,l,s,6.0) : textLightness(l);
        // secondary and hint text: a dark ground lets a translucent text read better than a light one does
        if(a>=64 && a<255) a=pyRound(a+(255-a)*0.65);
    } else if(role==Icon || role==SolidIcon){
        if(accent) lightness = role==Icon ? readable(h,l,s,3.4) : l;
        else {   // one blue for every icon: filled or stroked, an icon's blue is the same as its neighbours'
            Rgba c = l>0.95 ? kIconEmphasis : kIconBlue; c.a=a; return c;
        }
    } else if(role==Line){
        lightness = accent ? std::min(l,0.45) : (l<kGround ? 0.80 : 0.2+(1-l)*0.9);
    } else {
        if(accent){
            if(l>=0.35) return {r,g,b,a};
            lightness=0.92; s=std::min(s,0.5);
        } else if(a<255 && l>=0.45){
            return {86,111,132,pyRound(a*0.7)};   // a pale wash over a ground (hover, selection), in #566f84
        } else if(l<kGround) lightness=interpolateGrounds(l);
        else lightness=l+(1-l)*0.3;
    }
    lightness=std::clamp(lightness,0.0,1.0);
    int nr,ng,nb; hlsTo255(h,lightness,s,nr,ng,nb);
    return {nr,ng,nb,a};
}

QColor color(const QColor& dark, Role role){
    if(!g_light || !dark.isValid() || dark.alpha()==0) return dark;
    const QColor c=dark.toRgb();
    const Rgba m=mapRgba(c.red(),c.green(),c.blue(),c.alpha(),role);
    return QColor(m.r,m.g,m.b,m.a);
}

QString mapStylesheet(const QString& text){
    static const QRegularExpression blockRe(R"(([^{}]*)\{([^{}]*)\})");
    static const QRegularExpression declRe(R"(([a-z\-]+)(\s*:\s*)([^;{}]+))");
    static const QRegularExpression stateRe(R"(:hover|:selected|:checked|\[selected="?true"?\]|\[selected_\w+="?true"?\])");
    auto declaration=[](const QRegularExpressionMatch& m, const QString& state){
        const QString prop=m.captured(1), sep=m.captured(2); QString value=m.captured(3);
        const Role role=roleOf(prop);
        if(!state.isEmpty() && role==Surface && (prop=="background" || prop=="background-color")){
            static const QRegularExpression whole("^(?:"+colourRe().pattern()+")$");
            const auto c=whole.match(value.trimmed());
            Rgba rgba;
            if(c.hasMatch() && parseColour(c.captured(0),true,rgba)){
                const auto& [r,g,b,a]=rgba;
                if(a==255 && !isShadow(r,g,b) && lightnessOf(r,g,b)<kGround && (std::max({r,g,b})-std::min({r,g,b}))/255.0<=kAccentChroma)
                    return prop+sep+QString(value).replace(c.captured(0), state=="hover" ? kHoverTint : kSelectedTint);
            }
        }
        if(value.contains("gradient")) return prop+sep+mapGradient(value,role);
        return prop+sep+replaceAll(value, colourRe(), [&](const QRegularExpressionMatch& x){ return mapColourText(x.captured(0),true,role); });
    };
    return replaceAll(text, blockRe, [&](const QRegularExpressionMatch& block){
        const QString selectors=block.captured(1);
        std::set<QString> states;
        for(auto it=stateRe.globalMatch(selectors); it.hasNext();) states.insert(it.next().captured(0));
        const QString state = states.empty() ? QString() : (states.size()==1 && *states.begin()==":hover") ? "hover" : "selected";
        return selectors+"{"+replaceAll(block.captured(2), declRe, [&](const QRegularExpressionMatch& d){ return declaration(d,state); })+"}";
    });
}

QString mapSvg(const QString& text, Role role){
    // masks and clip paths keep their colours (they are coverage, not colour)
    static const QRegularExpression skipRe(R"(<(mask|clipPath)\b.*?</\1>)", QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression colRe(R"(#[0-9a-fA-F]{6}\b|#[0-9a-fA-F]{3}\b|rgba?\([^)]*\)|(?<=["':])\s*(?:black|white)\b)");
    auto mapped=[&](const QString& part){
        const QString blacks=replaceAll(part, svgShapeRe(), [](const QRegularExpressionMatch& m){ return lighterBlack(m.captured(0)); });
        return replaceAll(blacks, colRe, [&](const QRegularExpressionMatch& m){ return mapSvgColour(m.captured(0),role); });
    };
    QString out; qsizetype last=0;
    for(auto it=skipRe.globalMatch(text); it.hasNext();){
        const auto m=it.next();
        out+=mapped(text.mid(last, m.capturedStart()-last));
        out+=m.captured(0);
        last=m.capturedEnd();
    }
    out+=mapped(text.mid(last));
    return out;
}

int dump(const QString& dir){
    QDir().mkpath(dir);
    auto write=[&](const QString& name, const QString& text){ QFile f(dir+"/"+name); if(f.open(QIODevice::WriteOnly|QIODevice::Truncate)) f.write(text.toUtf8()); };
    QString qss=g_darkQss; qss.remove(QRegularExpression(R"(/\*.*?\*/)", QRegularExpression::DotMatchesEverythingOption));
    write("dark.qss", qss);
    write("light.qss", mapStylesheet(qss));
    const auto grounds=stylesheetGrounds();
    QString kinds;
    for(const QString& name : QDir(kChrome).entryList({"*.svg"}, QDir::Files)){
        const QByteArray src=readAll(kChrome+"/"+name);
        write("src-"+name, QString::fromUtf8(src));
        kinds+=name+" "+kindOf(name,src,grounds)+"\n";
        const std::pair<const char*,Role> roles[]={{"icon",Icon},{"solid",SolidIcon},{"surface",Surface}};
        for(const auto& [tag,role] : roles) write(QString(tag)+"-"+name, mapSvg(QString::fromUtf8(src), role));
    }
    write("kinds.txt", kinds);
    QString table;
    const char* names[]={"text","icon","solid icon","surface","line","auto"};
    for(int r=0;r<256;r+=17) for(int g=0;g<256;g+=17) for(int b=0;b<256;b+=51) for(int a : {255,200,90,30}) for(int role=0;role<6;++role){
        const Rgba m=mapRgba(r,g,b,a,Role(role));
        table+=QString("%1 %2 %3 %4 %5 -> %6 %7 %8 %9\n").arg(r).arg(g).arg(b).arg(a).arg(names[role]).arg(m.r).arg(m.g).arg(m.b).arg(m.a);
    }
    write("colours.txt", table);
    return 0;
}

} // namespace theme
