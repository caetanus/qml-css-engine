// CssTheme value-parsing API: colors, lengths, fonts, gradients, shadows, timing, transforms.
#include "csstheme.h"
#include "csstheme_p.h"

#include "csslayout.h"
#include "valueparser.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QJSValue>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQuickItem>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVarLengthArray>
#include <QUrl>
#include <algorithm>

namespace QmlCss {

using namespace Detail;

QVariantMap CssTheme::parseAnimation(const QString &cssValue) const
{
    return CssValueParser::parseAnimation(cssValue);
}

QVariantList CssTheme::keyframes(const QString &name) const
{
    return m_keyframes.value(name);
}

// Coerce a CssQmlItem identity property (cssAlternateId / cssClass) to a string list:
// it may be authored as a single string ("waybar", "focused"), a space-separated string,
// or a QML list (["network", "nm-applet"]).
QStringList cssVariantToStringList(const QVariant &v)
{
    if (!v.isValid())
        return {};
    // A QML `var` array (e.g. `cssClass: active ? ["active"] : []`) arrives here wrapped as a
    // QJSValue, NOT a QVariantList — unwrap it to its variant form before coercing, otherwise
    // it falls through to toString() (empty) and every state class is silently dropped.
    if (v.metaType().id() == qMetaTypeId<QJSValue>())
        return cssVariantToStringList(v.value<QJSValue>().toVariant());
    if (v.metaType().id() == QMetaType::QStringList)
        return v.toStringList();
    if (v.metaType().id() == QMetaType::QVariantList) {
        QStringList out;
        const QVariantList list = v.toList();
        for (const QVariant &e : list) {
            const QString s = e.toString().trimmed();
            if (!s.isEmpty())
                out << s;
        }
        return out;
    }
    return v.toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}


QColor CssTheme::parseColor(const QString &cssColor) const
{
    return CssValueParser::parseColor(cssColor);
}

qreal CssTheme::parseLength(const QString &value, qreal fallback) const
{
    double out = 0.0;
    if (CssValueParser::parseLengthPx(value, &out))
        return out;
    return fallback;
}

qreal CssTheme::parseFontSize(const QString &value, qreal fallbackPt) const
{
    // Resolve a CSS font-size to POINTS (QML Text.pointSize), centrally, so no component
    // hand-converts units. CSS px are device-independent pixels → points ×72/96; pt is
    // points; em/rem scale `fallbackPt` (the element's base size); a bare number is points.
    const QString s = value.trimmed().toLower();
    // Forgiving number+unit parse so "13px" / "10pt" / "1.5rem" / "11" all work.
    static const QRegularExpression numRe(QStringLiteral("^\\s*(-?\\d*\\.?\\d+)\\s*([a-z%]*)\\s*$"));
    const QRegularExpressionMatch m = numRe.match(s);
    if (!m.hasMatch())
        return fallbackPt;
    const double num = m.captured(1).toDouble();
    const QString unit = m.captured(2);
    if (unit == QLatin1String("px"))
        return num * 72.0 / 96.0;
    if (unit == QLatin1String("em") || unit == QLatin1String("rem"))
        return num * fallbackPt;
    return num; // pt or unitless → points
}

namespace {
QVector<qreal> cssBox(const QString &value)
{
    if (value.trimmed().isEmpty())
        return { 0, 0, 0, 0 };
    const QStringList parts = CssValueParser::splitTopLevelWhitespace(value.trimmed());
    QVector<qreal> n;
    for (int i = 0; i < parts.size() && i < 4; ++i) {
        double v = 0;
        n.push_back(CssValueParser::parseLengthPx(parts.at(i), &v) ? v : 0);
    }
    if (n.size() == 1) return { n[0], n[0], n[0], n[0] };
    if (n.size() == 2) return { n[0], n[1], n[0], n[1] };
    if (n.size() == 3) return { n[0], n[1], n[2], n[1] };
    if (n.size() >= 4) return { n[0], n[1], n[2], n[3] };
    return { 0, 0, 0, 0 };
}

QString sideName(int side)
{
    static const QString names[4] = {
        QStringLiteral("top"), QStringLiteral("right"),
        QStringLiteral("bottom"), QStringLiteral("left"),
    };
    return names[std::clamp(side, 0, 3)];
}
} // namespace

int CssTheme::fontWeight(const QString &value) const
{
    const QString s = value.trimmed().toLower();
    if (s.isEmpty() || s == QLatin1String("normal") || s == QLatin1String("400"))
        return QFont::Normal;
    if (s == QLatin1String("bold") || s == QLatin1String("700"))
        return QFont::Bold;
    if (s == QLatin1String("bolder"))
        return QFont::ExtraBold;
    if (s == QLatin1String("lighter"))
        return QFont::Light;

    bool ok = false;
    const int n = s.toInt(&ok);
    if (!ok) return QFont::Normal;
    if (n <= 100) return QFont::Thin;
    if (n <= 300) return QFont::Light;
    if (n <= 400) return QFont::Normal;
    if (n <= 500) return QFont::Medium;
    if (n <= 600) return QFont::DemiBold;
    if (n <= 700) return QFont::Bold;
    if (n <= 800) return QFont::ExtraBold;
    return QFont::Black;
}

bool CssTheme::isProportionalLineHeight(const QString &value) const
{
    const QString s = value.trimmed().toLower();
    if (s.isEmpty() || s == QLatin1String("normal"))
        return true;
    static const QRegularExpression unitRe(QStringLiteral("[a-z%]"));
    return !unitRe.match(s).hasMatch();
}

qreal CssTheme::parseLineHeight(const QString &value, qreal fallbackPx) const
{
    const QString s = value.trimmed().toLower();
    if (s.isEmpty() || s == QLatin1String("normal"))
        return 1.0;
    if (isProportionalLineHeight(s)) {
        bool ok = false;
        const qreal n = s.toDouble(&ok);
        return ok ? n : 1.0;
    }
    return parseLength(s, fallbackPx);
}

qreal CssTheme::boxSideLength(const QVariantMap &style, const QString &prefix, int side) const
{
    const QVector<qreal> box = cssBox(style.value(prefix).toString());
    const QString key = prefix + QLatin1Char('-') + sideName(side);
    const auto it = style.constFind(key);
    if (it == style.constEnd())
        return box.value(std::clamp(side, 0, 3), 0);
    return parseLength(it->toString(), box.value(std::clamp(side, 0, 3), 0));
}

bool CssTheme::hasSideBorder(const QVariantMap &style) const
{
    static const QStringList sides = { QStringLiteral("top"), QStringLiteral("right"),
                                      QStringLiteral("bottom"), QStringLiteral("left") };
    for (const QString &side : sides) {
        if (style.contains(QStringLiteral("border-") + side)
            || style.contains(QStringLiteral("border-") + side + QStringLiteral("-width"))
            || style.contains(QStringLiteral("border-") + side + QStringLiteral("-color"))
            || style.contains(QStringLiteral("border-") + side + QStringLiteral("-style")))
            return true;
    }
    return false;
}

QVariantMap CssTheme::borderSide(const QVariantMap &style, const QString &side,
                                 qreal defaultWidth, const QColor &defaultColor) const
{
    const QString prefix = QStringLiteral("border-") + side;
    const QVariantMap shorthand = style.contains(prefix) ? parseBorder(style.value(prefix).toString()) : QVariantMap();

    const qreal width = style.contains(prefix + QStringLiteral("-width"))
        ? parseLength(style.value(prefix + QStringLiteral("-width")).toString(), 0)
        : shorthand.value(QStringLiteral("width"), defaultWidth).toReal();
    const QColor color = style.contains(prefix + QStringLiteral("-color"))
        ? parseColor(style.value(prefix + QStringLiteral("-color")).toString())
        : shorthand.value(QStringLiteral("color"), defaultColor).value<QColor>();
    const QString borderStyle = style.contains(prefix + QStringLiteral("-style"))
        ? style.value(prefix + QStringLiteral("-style")).toString()
        : shorthand.value(QStringLiteral("style"), QStringLiteral("solid")).toString();

    QVariantMap out;
    out.insert(QStringLiteral("width"), width);
    out.insert(QStringLiteral("color"), color);
    out.insert(QStringLiteral("style"), borderStyle);
    out.insert(QStringLiteral("visible"), width > 0 && color.alphaF() > 0
               && borderStyle != QLatin1String("none") && borderStyle != QLatin1String("hidden"));
    return out;
}

QString CssTheme::resolveFontFamily(const QString &value, const QString &fallback) const
{
    // Memoized: applyToText re-resolves the family on every (re)apply — 70+ labels per page
    // times inheritance re-applies hammered QFontDatabase. Cleared when a downloaded
    // @font-face registers (see registerFontData).
    const QString cacheKey = value + QLatin1Char('\x1f') + fallback;
    const auto hit = m_fontFamilyCache.constFind(cacheKey);
    if (hit != m_fontFamilyCache.constEnd())
        return *hit;
    return *m_fontFamilyCache.insert(cacheKey, resolveFontFamilyUncached(value, fallback));
}

QString CssTheme::resolveFontFamilyUncached(const QString &value, const QString &fallback) const
{
    const QStringList families = QFontDatabase::families();
    const auto hasFamily = [&](const QString &candidate) {
        return std::any_of(families.cbegin(), families.cend(), [&](const QString &family) {
            return family.compare(candidate, Qt::CaseInsensitive) == 0;
        });
    };
    const auto clean = [](QString candidate) {
        candidate = candidate.trimmed();
        if ((candidate.startsWith(QLatin1Char('"')) && candidate.endsWith(QLatin1Char('"')))
            || (candidate.startsWith(QLatin1Char('\'')) && candidate.endsWith(QLatin1Char('\''))))
            candidate = candidate.mid(1, candidate.size() - 2).trimmed();
        return candidate;
    };
    const auto alias = [&](const QString &candidate) -> QString {
        const QString low = candidate.toLower();
        if (low == QLatin1String("sans-serif"))
            return hasFamily(QStringLiteral("Noto Sans")) ? QStringLiteral("Noto Sans") : QStringLiteral("Sans Serif");
        if (low == QLatin1String("serif"))
            return hasFamily(QStringLiteral("Noto Serif")) ? QStringLiteral("Noto Serif") : QStringLiteral("Serif");
        if (low == QLatin1String("monospace"))
            return hasFamily(QStringLiteral("Noto Sans Mono")) ? QStringLiteral("Noto Sans Mono") : QStringLiteral("Monospace");
        if (low == QLatin1String("helvetica") || low == QLatin1String("helvetica neue"))
            return hasFamily(QStringLiteral("Nimbus Sans")) ? QStringLiteral("Nimbus Sans") : QString();
        if (low == QLatin1String("arial"))
            return hasFamily(QStringLiteral("Liberation Sans")) ? QStringLiteral("Liberation Sans") : QString();
        return QString();
    };

    for (const QString &part : CssValueParser::splitTopLevel(value, QLatin1Char(','))) {
        const QString candidate = clean(part);
        if (candidate.isEmpty())
            continue;
        if (hasFamily(candidate))
            return candidate;
        const QString mapped = alias(candidate);
        if (!mapped.isEmpty())
            return mapped;
    }
    if (!fallback.isEmpty())
        return resolveFontFamily(fallback, QString());
    return hasFamily(QStringLiteral("Noto Sans")) ? QStringLiteral("Noto Sans") : QStringLiteral("Sans Serif");
}

// Parse the colour stops of a gradient (the parts after any head token), distributing
// positions evenly where omitted. Returns an empty list if there are fewer than 2 stops.
static QVariantList parseGradientStops(const QStringList &parts, int firstStop)
{
    QList<QColor> colors;
    QList<double> positions;
    for (int i = firstStop; i < parts.size(); ++i) {
        const QStringList toks = CssValueParser::splitTopLevelWhitespace(parts.at(i));
        if (toks.isEmpty())
            continue;
        colors.append(CssValueParser::parseColor(toks.first()));
        double pos = -1.0;
        if (toks.size() >= 2 && toks.at(1).endsWith(QLatin1Char('%')))
            pos = toks.at(1).left(toks.at(1).length() - 1).toDouble() / 100.0;
        positions.append(pos);
    }
    if (colors.size() < 2)
        return {};

    // Web parity: CSS interpolates gradient colors in PREMULTIPLIED alpha, so a
    // `transparent` stop takes the hue of its neighbour and only the alpha ramps.
    // QGradient interpolates straight (non-premultiplied), and `transparent` parses as
    // BLACK alpha-0 — the ramp then passes through dark translucent greys and a pastel
    // page veil renders muddy brown. Give fully-transparent stops the nearest opaque
    // neighbour's RGB (alpha stays 0) — exact for the common endpoint-transparent case.
    for (int i = 0; i < colors.size(); ++i) {
        if (colors.at(i).alpha() != 0)
            continue;
        for (int d = 1; d < colors.size(); ++d) {
            const int lo = i - d, hi = i + d;
            const QColor *src = nullptr;
            if (lo >= 0 && colors.at(lo).alpha() != 0)
                src = &colors.at(lo);
            else if (hi < colors.size() && colors.at(hi).alpha() != 0)
                src = &colors.at(hi);
            if (src) {
                colors[i] = QColor(src->red(), src->green(), src->blue(), 0);
                break;
            }
        }
    }

    QVariantList stops;
    for (int i = 0; i < colors.size(); ++i) {
        double pos = positions.at(i);
        if (pos < 0.0)
            pos = static_cast<double>(i) / static_cast<double>(colors.size() - 1);
        QVariantMap stop;
        stop.insert(QStringLiteral("position"), pos);
        stop.insert(QStringLiteral("color"), colors.at(i));
        stops.append(stop);
    }
    return stops;
}

// A CSS position token to a 0..1 fraction (percentage or keyword); `fallback` otherwise.
static double cssPositionFraction(const QString &token, double fallback)
{
    const QString t = token.trimmed();
    if (t.endsWith(QLatin1Char('%')))
        return t.left(t.length() - 1).toDouble() / 100.0;
    if (t.compare(QLatin1String("left"), Qt::CaseInsensitive) == 0
        || t.compare(QLatin1String("top"), Qt::CaseInsensitive) == 0)
        return 0.0;
    if (t.compare(QLatin1String("center"), Qt::CaseInsensitive) == 0)
        return 0.5;
    if (t.compare(QLatin1String("right"), Qt::CaseInsensitive) == 0
        || t.compare(QLatin1String("bottom"), Qt::CaseInsensitive) == 0)
        return 1.0;
    return fallback;
}

QVariantMap CssTheme::parseGradient(const QString &cssValue) const
{
    const QString s = cssValue.trimmed();
    if (!s.endsWith(QLatin1Char(')')))
        return {};

    const bool radial = s.startsWith(QLatin1String("radial-gradient("), Qt::CaseInsensitive);
    const bool linear = s.startsWith(QLatin1String("linear-gradient("), Qt::CaseInsensitive);
    if (!radial && !linear)
        return {};

    const QString prefix = radial ? QStringLiteral("radial-gradient(") : QStringLiteral("linear-gradient(");
    const QString inner = s.mid(prefix.length(), s.length() - prefix.length() - 1);
    QStringList parts = CssValueParser::splitTopLevel(inner, QLatin1Char(','));
    for (QString &p : parts)
        p = p.trimmed();
    if (parts.isEmpty())
        return {};

    if (radial) {
        // radial-gradient([<shape> | <size>] [at <pos>], <stops>). Only the centre `at`
        // position is honoured (cx/cy as 0..1 fractions); shape/size default to a centred
        // ellipse covering the box.
        double cx = 0.5, cy = 0.5;
        int firstStop = 0;
        const QString head = parts.first();
        const bool headIsConfig = head.contains(QLatin1String(" at "), Qt::CaseInsensitive)
            || head.startsWith(QLatin1String("circle"), Qt::CaseInsensitive)
            || head.startsWith(QLatin1String("ellipse"), Qt::CaseInsensitive);
        if (headIsConfig) {
            firstStop = 1;
            const int atIdx = head.indexOf(QLatin1String("at "), 0, Qt::CaseInsensitive);
            if (atIdx >= 0) {
                const QStringList pos = CssValueParser::splitTopLevelWhitespace(head.mid(atIdx + 3));
                if (pos.size() >= 1) cx = cssPositionFraction(pos.at(0), 0.5);
                cy = pos.size() >= 2 ? cssPositionFraction(pos.at(1), 0.5) : cx;
            }
        }
        const QVariantList stops = parseGradientStops(parts, firstStop);
        if (stops.isEmpty())
            return {};
        QVariantMap result;
        result.insert(QStringLiteral("type"), QStringLiteral("radial"));
        result.insert(QStringLiteral("cx"), cx);
        result.insert(QStringLiteral("cy"), cy);
        result.insert(QStringLiteral("stops"), stops);
        return result;
    }

    double angle = 180.0; // CSS default direction: "to bottom"
    int firstStop = 0;
    const QString head = parts.first();
    if (head.endsWith(QLatin1String("deg"), Qt::CaseInsensitive)) {
        angle = head.left(head.length() - 3).trimmed().toDouble();
        firstStop = 1;
    } else if (head.startsWith(QLatin1String("to "), Qt::CaseInsensitive)) {
        angle = cssSideToAngle(head.mid(3));
        firstStop = 1;
    }

    const QVariantList stops = parseGradientStops(parts, firstStop);
    if (stops.isEmpty())
        return {};

    QVariantMap result;
    result.insert(QStringLiteral("type"), QStringLiteral("linear"));
    result.insert(QStringLiteral("angle"), angle);
    result.insert(QStringLiteral("stops"), stops);
    return result;
}

QVariantMap CssTheme::parseBorder(const QString &cssValue) const
{
    // `border` shorthand: <width> <style> <color> (any order, any subset). Returns
    // { width (px), style, color (QColor) } for the parts present.
    QVariantMap out;
    const QStringList toks = CssValueParser::splitTopLevelWhitespace(cssValue.trimmed());
    static const QStringList styleKeywords = {
        QStringLiteral("none"), QStringLiteral("hidden"), QStringLiteral("solid"),
        QStringLiteral("dashed"), QStringLiteral("dotted"), QStringLiteral("double"),
        QStringLiteral("groove"), QStringLiteral("ridge"), QStringLiteral("inset"),
        QStringLiteral("outset"),
    };
    for (const QString &tok : toks) {
        double w = 0.0;
        if (CssValueParser::parseLengthPx(tok, &w)) {
            out.insert(QStringLiteral("width"), w);
            continue;
        }
        if (styleKeywords.contains(tok, Qt::CaseInsensitive)) {
            out.insert(QStringLiteral("style"), tok.toLower());
            continue;
        }
        const QColor c = CssValueParser::parseColor(tok);
        if (c.isValid())
            out.insert(QStringLiteral("color"), c);
    }
    return out;
}

QVariantList CssTheme::parseGradientLayers(const QString &cssValue) const
{
    // CSS `background` can stack several comma-separated layers (the FIRST listed paints on
    // TOP). Each layer is a gradient or a solid colour. Top-level commas separate layers
    // (commas inside gradient()/rgba() are protected by splitTopLevel).
    QVariantList layers;
    const QStringList segments = CssValueParser::splitTopLevel(cssValue, QLatin1Char(','));
    for (const QString &segment : segments) {
        const QString t = segment.trimmed();
        if (t.isEmpty())
            continue;
        if (t.startsWith(QLatin1String("linear-gradient("), Qt::CaseInsensitive)
            || t.startsWith(QLatin1String("radial-gradient("), Qt::CaseInsensitive)) {
            const QVariantMap g = parseGradient(t);
            if (!g.isEmpty())
                layers.append(g);
        } else {
            const QColor c = CssValueParser::parseColor(t);
            if (c.isValid()) {
                QVariantMap m;
                m.insert(QStringLiteral("type"), QStringLiteral("color"));
                m.insert(QStringLiteral("color"), c);
                layers.append(m);
            }
        }
    }
    return layers;
}

static QVariantMap parseOneBoxShadow(const QString &segment)
{
    const QStringList toks = CssValueParser::splitTopLevelWhitespace(segment.trimmed());

    bool inset = false;
    QList<double> lengths;
    QColor color(0, 0, 0, 128);
    for (const QString &tok : toks) {
        if (tok.compare(QLatin1String("inset"), Qt::CaseInsensitive) == 0) {
            inset = true;
            continue;
        }
        double value = 0.0;
        if (CssValueParser::parseLengthPx(tok, &value)) {
            lengths.append(value);
            continue;
        }
        color = CssValueParser::parseColor(tok);
    }
    if (lengths.size() < 2)
        return {};

    QVariantMap result;
    result.insert(QStringLiteral("x"), lengths.value(0, 0.0));
    result.insert(QStringLiteral("y"), lengths.value(1, 0.0));
    result.insert(QStringLiteral("blur"), lengths.value(2, 0.0));
    result.insert(QStringLiteral("spread"), lengths.value(3, 0.0));
    result.insert(QStringLiteral("color"), color);
    result.insert(QStringLiteral("inset"), inset);
    return result;
}

QVariantMap CssTheme::parseBoxShadow(const QString &cssValue) const
{
    const QString s = cssValue.trimmed();
    if (s.isEmpty() || s.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0)
        return {};
    // First shadow only — take the first comma-separated segment.
    return parseOneBoxShadow(CssValueParser::splitTopLevel(s, QLatin1Char(',')).first());
}

QVariantList CssTheme::parseBoxShadowList(const QString &cssValue) const
{
    QVariantList shadows;
    const QString s = cssValue.trimmed();
    if (s.isEmpty() || s.compare(QLatin1String("none"), Qt::CaseInsensitive) == 0)
        return shadows;
    const QStringList segments = CssValueParser::splitTopLevel(s, QLatin1Char(','));
    for (const QString &segment : segments) {
        const QVariantMap shadow = parseOneBoxShadow(segment);
        if (!shadow.isEmpty())
            shadows.append(shadow);
    }
    return shadows;
}

int CssTheme::parseDuration(const QString &cssValue, int fallbackMs) const
{
    return CssValueParser::parseDuration(cssValue, fallbackMs);
}

int CssTheme::parseEasing(const QString &cssValue, int fallbackType) const
{
    return static_cast<int>(CssValueParser::parseEasing(cssValue, static_cast<QEasingCurve::Type>(fallbackType)));
}

QVariantMap CssTheme::parseTransition(const QString &cssValue) const
{
    return CssValueParser::parseTransition(cssValue);
}

QVariantMap CssTheme::parseTransform(const QString &cssValue) const
{
    QVariantMap result{
        {QStringLiteral("rotate"), 0.0},
        {QStringLiteral("scale"), 1.0},
        {QStringLiteral("scaleX"), 1.0},
        {QStringLiteral("scaleY"), 1.0},
        {QStringLiteral("translateX"), 0.0},
        {QStringLiteral("translateY"), 0.0},
    };
    if (cssValue.trimmed().isEmpty()) {
        return result;
    }

    const auto firstNumber = [](const QString &raw, double fallback) -> double {
        static const QRegularExpression re(QStringLiteral("(-?\\d*\\.?\\d+)"));
        const auto m = re.match(raw);
        return m.hasMatch() ? m.captured(1).toDouble() : fallback;
    };
    const auto angleDeg = [&firstNumber](const QString &raw) -> double {
        const double v = firstNumber(raw, 0.0);
        const QString s = raw.toLower();
        if (s.contains(QLatin1String("turn"))) {
            return v * 360.0;
        }
        if (s.contains(QLatin1String("rad"))) { // grad is rare; treat any "rad" as radians
            return v * 180.0 / 3.141592653589793;
        }
        return v; // "deg" or bare number
    };

    static const QRegularExpression fnRe(QStringLiteral("([a-zA-Z]+)\\s*\\(([^)]*)\\)"));
    auto it = fnRe.globalMatch(cssValue);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString fn = m.captured(1).toLower();
        const QStringList args = m.captured(2).split(QLatin1Char(','));
        if (args.isEmpty()) {
            continue;
        }
        if (fn == QLatin1String("rotate")) {
            result.insert(QStringLiteral("rotate"), angleDeg(args.at(0)));
        } else if (fn == QLatin1String("scale")) {
            const double sx = firstNumber(args.at(0), 1.0);
            const double sy = args.size() >= 2 ? firstNumber(args.at(1), sx) : sx;
            result.insert(QStringLiteral("scaleX"), sx);
            result.insert(QStringLiteral("scaleY"), sy);
            result.insert(QStringLiteral("scale"), sx);
        } else if (fn == QLatin1String("scalex")) {
            result.insert(QStringLiteral("scaleX"), firstNumber(args.at(0), 1.0));
        } else if (fn == QLatin1String("scaley")) {
            result.insert(QStringLiteral("scaleY"), firstNumber(args.at(0), 1.0));
        } else if (fn == QLatin1String("translate")) {
            result.insert(QStringLiteral("translateX"), firstNumber(args.at(0), 0.0));
            if (args.size() >= 2) {
                result.insert(QStringLiteral("translateY"), firstNumber(args.at(1), 0.0));
            }
        } else if (fn == QLatin1String("translatex")) {
            result.insert(QStringLiteral("translateX"), firstNumber(args.at(0), 0.0));
        } else if (fn == QLatin1String("translatey")) {
            result.insert(QStringLiteral("translateY"), firstNumber(args.at(0), 0.0));
        }
    }
    return result;
}

} // namespace QmlCss
