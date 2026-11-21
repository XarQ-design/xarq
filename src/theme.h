#pragma once
// The interface's two looks, after Subtitld's interface/theme.py: dark (the one everything is written in) and light.
//
// There is one palette, the dark one: the stylesheet, the chrome SVGs and the painted widgets are all written in its
// colours. The light look is that palette turned over by what each colour is drawn as (text, icon, surface, line),
// so the painters only pass their colours through color(), ink() or surface(). Text keeps WCAG AA (4.5:1) or better
// on the light grounds. What the turning over doesn't get right is set by hand in assets/theme-light.qss (added
// after the turned-over stylesheet) and assets/chrome/light/ (sprites drawn for the light look).
//
// The pick ("ui/theme" in the settings) is "system" (the default), "dark" or "light".
#include <QColor>
#include <QIcon>
#include <QObject>
#include <QString>
#include <QStringList>

class QApplication;

namespace theme {

enum Role { Text, Icon, SolidIcon, Surface, Line, Auto };

QStringList choices();                 // "system", "dark", "light"
QString setting();                     // the saved pick
void setSetting(const QString& choice);// save the pick and draw in it at once
QString systemTheme();                 // the system's look, "dark" or "light"
bool isLight();                        // the look drawn in now

// Install on the application (once, before any widget): the palette, the stylesheet and the filter that turns over
// the stylesheets widgets set on themselves. `darkQss` is the application's stylesheet in the dark look.
void install(QApplication& app, const QString& darkQss);

// A dark-look colour in the current look, drawn as `role`. Content colours (the document's) are never passed.
QColor color(const QColor& dark, Role role=Auto);
inline QColor surface(const QColor& dark){ return color(dark, Surface); }   // a fill: pale fills (keys) stay pale
inline QColor ink(const QColor& dark){ return color(dark, Text); }          // text and strokes

// The light versions of dark-look sources (exposed for the self-test and the --themedump diagnostic).
struct Rgba { int r, g, b, a; };
Rgba mapRgba(int r, int g, int b, int a, Role role);
QString mapStylesheet(const QString& qss);
QString mapSvg(const QString& svg, Role role=Icon);
// --themedump <dir>: the turned-over stylesheet, every chrome SVG in each role and a colour table, for comparing
// with Subtitld's theme.py.
int dump(const QString& dir);

// A widget whose own stylesheet carries document colours (a swatch) sets this property so it is not turned over.
inline constexpr const char* KeepProperty = "xqThemeKeep";

// Emits changed() after the look changed (stylesheet and palette already applied): painters that cache colours or
// icons rebuild them here. Widgets that paint through color() at paint time need nothing.
class Notifier : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void changed(bool light);
};
Notifier* notifier();

} // namespace theme
