#pragma once
// Shared drawing helpers for XarQ's painted chrome, after Subtitld's interface/shapes.py.
//
// The look: no borders, depth from a vertical gradient (raised: lighter at the top, darker at the bottom; sunken: the
// other way round, softer at the bottom), 3px corners on rectangles, 75° slants with 6px curves, and a 1px 45% black
// contact line under raised buttons.
#include <QColor>
#include <QIcon>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <algorithm>
#include <cmath>

namespace shapes {

inline const double SLANT = 75.0*M_PI/180.0;
inline const double RUN15 = std::tan(15.0*M_PI/180.0);    // horizontal lean of a 75° side per pixel of height
inline const double CURVE = 6;                            // curve on a slanted corner
inline const double T6 = CURVE*std::tan(SLANT/2);         // how far a 6px curve reaches along each side of a 75° turn
inline const double RADIUS = 3;                           // corner radius on rectangles
inline const double DEPTH = 6;                            // gradient: ±6% HSL lightness

inline const QColor CONTACT(0,0,0,115);                   // 1px line under a raised button
inline const QColor TOP_SHADOW(0,0,0,46);                 // 1px line at the top of a sunk (pressed/checked) zone
inline const QColor HOVER(255,255,255,18);                // faint tint over a hovered zone

inline const QColor RAISED(0x4a,0x5f,0x70);               // default button
inline const QColor SUNK(0x2e,0x3e,0x4c);                 // pressed/checked button
inline const QColor NEST(0x33,0x47,0x5a);                 // inner key, a step darker than its parent
inline const QColor FIELD(0x2a,0x39,0x46);                // input well
inline const QColor LIGHT(0xb8,0xce,0xe0);                // icon/text light

enum Kind { Raised, Sunken };

// Lighten (pct > 0) or darken (pct < 0) a colour in HSL lightness by pct percent of the range.
inline QColor shade(const QColor& c, double pct){
    float h,s,l,a; c.getHslF(&h,&s,&l,&a);
    QColor q; q.setHslF(std::max(h,0.f), s, float(std::clamp(l+pct/100.0, 0.0, 1.0)), a);
    return q;
}

// Vertical gradient over y0..y1: Raised (+depth → −depth) or Sunken (−depth → +depth/2).
inline QLinearGradient gradient(double y0, double y1, const QColor& base, Kind kind=Raised, double depth=DEPTH){
    QLinearGradient g(0,y0,0,y1);
    if(kind==Raised){ g.setColorAt(0, shade(base,depth));  g.setColorAt(1, shade(base,-depth)); }
    else            { g.setColorAt(0, shade(base,-depth)); g.setColorAt(1, shade(base,depth/2)); }
    return g;
}

// The colour a gradient ends on, for blending one zone into the next without an edge.
inline QColor bottomColour(const QColor& base, Kind kind=Raised, double depth=DEPTH){
    return shade(base, kind==Raised ? -depth : depth/2);
}

inline QPainterPath rectPath(const QRectF& r){ QPainterPath p; p.addRect(r); return p; }
inline QPainterPath roundedPath(const QRectF& r, double radius=RADIUS){ QPainterPath p; p.addRoundedRect(r,radius,radius); return p; }

inline QPointF unit(double x, double y){ const double n=std::hypot(x,y); return {x/n, y/n}; }

// Line to `t` before `corner` along direction d1, then a curve leaving `t` along d2 (cubic circle arc).
inline void fillet(QPainterPath& path, QPointF corner, QPointF d1, QPointF d2, double t){
    const double th=std::acos(std::clamp(d1.x()*d2.x()+d1.y()*d2.y(), -1.0, 1.0));
    if(th<1e-6){ path.lineTo(corner); return; }
    const double r=t/std::tan(th/2), h=4.0/3.0*std::tan(th/4)*r;
    const QPointF s=corner-d1*t, e=corner+d2*t;
    path.lineTo(s);
    path.cubicTo(s+d1*h, e-d2*h, e);
}

// Hangs from a line above: square top corners, rounded bottom corners.
inline QPainterPath hangingRect(double x, double y, double w, double h, double radius=RADIUS){
    QPainterPath p(QPointF(x,y));
    p.lineTo(x+w,y);
    p.lineTo(x+w,y+h-radius);
    p.quadTo(x+w,y+h, x+w-radius,y+h);
    p.lineTo(x+radius,y+h);
    p.quadTo(x,y+h, x,y+h-radius);
    p.closeSubpath();
    return p;
}

// A notch hanging from the edge at y: top spans x..x+w, the sides lean in at 75° to the bottom, 6px curves at the
// bottom corners, and the top corners flare out into the edge (so it reads as cut into it).
inline QPainterPath hangingTrap(double x, double y, double w, double h){
    const double run=h*RUN15;
    const QPointF dl=unit(run,h), dr=unit(run,-h), right(1,0);
    QPainterPath p(QPointF(x-T6,y));
    fillet(p, {x,y}, right, dl, T6);
    fillet(p, {x+run,y+h}, dl, right, T6);
    fillet(p, {x+w-run,y+h}, right, dr, T6);
    fillet(p, {x+w,y}, dr, right, T6);
    p.closeSubpath();
    return p;
}

// hangingTrap flipped vertically: stands on the line at `bottom`, narrowing upwards to bottom - h.
inline QPainterPath standingTrap(double x, double w, double h, double bottom){
    return QTransform(1,0,0,-1,0,2*bottom-h).map(hangingTrap(x,bottom-h,w,h));
}

// A raised piece: gradient fill and, optionally, the 1px contact line along its bottom.
inline void fillRaised(QPainter& p, const QPainterPath& path, const QRectF& r, const QColor& base=RAISED, bool contact=true){
    p.fillPath(path, gradient(r.top(), r.bottom(), base));
    if(contact) p.fillPath(path.subtracted(path.translated(0,-1)), CONTACT);
}

// A sunk zone (pressed/checked): sunken gradient and a 1px shadow along its top.
inline void fillSunk(QPainter& p, const QPainterPath& path, const QRectF& r, const QColor& base=SUNK, const QColor& shadow=TOP_SHADOW){
    p.fillPath(path, gradient(r.top(), r.bottom(), base, Sunken));
    p.fillPath(path.intersected(rectPath(QRectF(r.x()-1, r.top(), r.width()+2, 1))), shadow);
}

// An ambient icon at 50% opacity that comes to 100% under the pointer. Qt draws a QToolButton's Active pixmap while
// it is hovered only when the button is auto-raised, so set autoRaise on the button too.
inline QIcon fadedIcon(const QIcon& icon, const QSize& size, double rest=0.5){
    QIcon out;
    for(const qreal dpr : {1.0, 2.0}){
        const QPixmap full=icon.pixmap(size, dpr);
        QPixmap faded(full.size()); faded.setDevicePixelRatio(full.devicePixelRatio()); faded.fill(Qt::transparent);
        { QPainter p(&faded); p.setOpacity(rest); p.drawPixmap(0,0,full); }
        out.addPixmap(faded, QIcon::Normal); out.addPixmap(full, QIcon::Active);
    }
    return out;
}

} // namespace shapes
