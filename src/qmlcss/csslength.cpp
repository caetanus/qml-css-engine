#include "qmlcss/csslength.h"

#include "csstheme_p.h" // g_layoutStringParses

#include <QRegularExpression>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>

namespace QmlCss {

double cssParseFloat(const QString &s)
{
    int i = 0;
    const int n = s.size();
    while (i < n && s[i].isSpace())
        ++i;
    const int start = i;
    if (i < n && (s[i] == QLatin1Char('+') || s[i] == QLatin1Char('-')))
        ++i;
    bool digits = false;
    while (i < n && s[i].isDigit()) { ++i; digits = true; }
    if (i < n && s[i] == QLatin1Char('.')) {
        ++i;
        while (i < n && s[i].isDigit()) { ++i; digits = true; }
    }
    if (digits && i < n && (s[i] == QLatin1Char('e') || s[i] == QLatin1Char('E'))) {
        int j = i + 1;
        if (j < n && (s[j] == QLatin1Char('+') || s[j] == QLatin1Char('-')))
            ++j;
        if (j < n && s[j].isDigit()) {
            i = j;
            while (i < n && s[i].isDigit())
                ++i;
        }
    }
    if (!digits)
        return std::nan("");
    bool ok = false;
    const double v = s.mid(start, i - start).toDouble(&ok);
    return ok ? v : std::nan("");
}

double cssAspectRatio(const QString &v)
{
    if (v.isEmpty())
        return std::nan("");
    if (v.contains(QLatin1Char('/'))) {
        const QStringList p = v.split(QLatin1Char('/'));
        const double a = cssParseFloat(p.value(0)), b = cssParseFloat(p.value(1));
        return (a > 0 && b > 0) ? a / b : std::nan("");
    }
    const double r = cssParseFloat(v);
    return r > 0 ? r : std::nan("");
}

namespace {

// Trimmed string value of a style key, or empty.
QString styleStr(const QVariantMap &style, const QString &key)
{
    const auto it = style.constFind(key);
    return it == style.constEnd() ? QString() : it->toString().trimmed();
}

// The literal translation of the engine's evalExpr dispatch + calcUnit suffix tests, run once:
// every branch that yields NaN for ANY avail/viewport (empty, a non-numeric token) is Absent/Invalid;
// otherwise the kind records which formula calcUnit would apply to the parsed number.
CssLength parseLength(const QString &raw, QStringList &exprs)
{
    CssLength l;
    const QString s = raw.trimmed();
    if (s.isEmpty())
        return l; // Absent
    ++Detail::g_layoutStringParses;
    const QString low = s.toLower();
    if (low.contains(QLatin1String("calc(")) || low.contains(QLatin1String("min("))
        || low.contains(QLatin1String("max(")) || low.contains(QLatin1String("clamp("))) {
        l.kind = CssLength::Expr;
        l.value = exprs.size();
        exprs.push_back(s);
        return l;
    }
    const double p = cssParseFloat(s);
    if (std::isnan(p)) {
        l.kind = CssLength::Invalid; // auto / fit-content / … (NaN times anything stays NaN)
        return l;
    }
    l.value = p;
    if (s.endsWith(QLatin1Char('%'))) l.kind = CssLength::Percent;
    else if (low.endsWith(QLatin1String("vmin"))) l.kind = CssLength::Vmin;
    else if (low.endsWith(QLatin1String("vmax"))) l.kind = CssLength::Vmax;
    else if (low.endsWith(QLatin1String("vw"))) l.kind = CssLength::Vw;
    else if (low.endsWith(QLatin1String("vh"))) l.kind = CssLength::Vh;
    else l.kind = CssLength::Number; // px / bare number
    return l;
}

// The engine's box() shorthand split (1/2/3/4 tokens -> top/right/bottom/left), per token parsed.
void parseBox(const QString &value, CssLength out[4], QStringList &exprs)
{
    for (int i = 0; i < 4; ++i)
        out[i] = CssLength();
    if (value.isEmpty())
        return;
    ++Detail::g_layoutStringParses;
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList parts = value.trimmed().split(ws, Qt::SkipEmptyParts);
    QVarLengthArray<CssLength, 4> n;
    for (const QString &p : parts)
        n.push_back(parseLength(p, exprs));
    if (n.size() == 1) { out[0] = out[1] = out[2] = out[3] = n[0]; }
    else if (n.size() == 2) { out[0] = n[0]; out[1] = n[1]; out[2] = n[0]; out[3] = n[1]; }
    else if (n.size() == 3) { out[0] = n[0]; out[1] = n[1]; out[2] = n[2]; out[3] = n[1]; }
    else if (n.size() >= 4) { out[0] = n[0]; out[1] = n[1]; out[2] = n[2]; out[3] = n[3]; }
}

CssLayoutStyle::Align parseAlign(const QString &al)
{
    using A = CssLayoutStyle::Align;
    if (al == QLatin1String("stretch")) return A::Stretch;
    if (al == QLatin1String("center")) return A::Center;
    if (al == QLatin1String("flex-end") || al == QLatin1String("end")) return A::End;
    return A::Other;
}

} // namespace

CssLayoutStyle CssLayoutStyle::parse(const QVariantMap &style)
{
    CssLayoutStyle st;
    QStringList &ex = st.exprs;

    const QString display = styleStr(style, QStringLiteral("display"));
    st.displayNone = display == QLatin1String("none");
    const QString disp = display.isEmpty() ? QStringLiteral("block") : display;
    st.displayGrid = disp == QLatin1String("grid");
    st.displayFlex = disp.contains(QLatin1String("flex"));
    st.positionAbsolute = styleStr(style, QStringLiteral("position")) == QLatin1String("absolute");

    QString flexDir = styleStr(style, QStringLiteral("flex-direction"));
    if (flexDir.isEmpty()) flexDir = QStringLiteral("row");
    st.directionRow = flexDir.startsWith(QLatin1String("row"));
    st.wrap = styleStr(style, QStringLiteral("flex-wrap")).startsWith(QLatin1String("wrap"));

    QString justify = styleStr(style, QStringLiteral("justify-content"));
    if (justify.isEmpty()) justify = QStringLiteral("flex-start");
    if (justify == QLatin1String("center")) st.justify = Justify::Center;
    else if (justify == QLatin1String("flex-end") || justify == QLatin1String("end")) st.justify = Justify::End;
    else if (justify == QLatin1String("space-between")) st.justify = Justify::SpaceBetween;
    else if (justify == QLatin1String("space-around")) st.justify = Justify::SpaceAround;
    else if (justify == QLatin1String("space-evenly")) st.justify = Justify::SpaceEvenly;
    else st.justify = Justify::Start;

    QString alignItems = styleStr(style, QStringLiteral("align-items"));
    if (alignItems.isEmpty()) alignItems = QStringLiteral("stretch");
    st.alignItems = parseAlign(alignItems);
    const QString alignSelf = styleStr(style, QStringLiteral("align-self"));
    st.alignSelf = alignSelf.isEmpty() ? Align::Inherit : parseAlign(alignSelf);

    QString alignContent = styleStr(style, QStringLiteral("align-content"));
    if (alignContent.isEmpty()) alignContent = QStringLiteral("stretch");
    if (alignContent == QLatin1String("center")) st.alignContent = AlignContent::Center;
    else if (alignContent == QLatin1String("flex-end") || alignContent == QLatin1String("end")) st.alignContent = AlignContent::End;
    else if (alignContent == QLatin1String("space-between")) st.alignContent = AlignContent::SpaceBetween;
    else if (alignContent == QLatin1String("space-around")) st.alignContent = AlignContent::SpaceAround;
    else if (alignContent == QLatin1String("stretch")) st.alignContent = AlignContent::Stretch;
    else st.alignContent = AlignContent::Start;

    QString gapStr = styleStr(style, QStringLiteral("gap"));
    if (gapStr.isEmpty()) gapStr = styleStr(style, QStringLiteral("row-gap"));
    if (gapStr.isEmpty()) gapStr = styleStr(style, QStringLiteral("column-gap"));
    st.gap = parseLength(gapStr, ex);

    st.borderBox = styleStr(style, QStringLiteral("box-sizing")) == QLatin1String("border-box");
    {
        bool ok = false;
        const int o = styleStr(style, QStringLiteral("order")).toInt(&ok);
        st.order = ok ? o : 0;
    }

    double g = 0;
    const QString fg = styleStr(style, QStringLiteral("flex-grow"));
    const QString fx = styleStr(style, QStringLiteral("flex"));
    if (!fg.isEmpty()) g = cssParseFloat(fg);
    else if (!fx.isEmpty()) g = cssParseFloat(fx);
    if (std::isnan(g)) g = 0;
    st.grow = g;
    // The shrink default depends only on this style's own grow, so it is decided here too.
    double sh = g > 0 ? 1 : 0;
    const QString fsh = styleStr(style, QStringLiteral("flex-shrink"));
    if (!fsh.isEmpty()) {
        sh = cssParseFloat(fsh);
    } else if (!fx.isEmpty()) {
        ++Detail::g_layoutStringParses;
        static const QRegularExpression fws(QStringLiteral("\\s+"));
        const QStringList fp = fx.trimmed().split(fws, Qt::SkipEmptyParts);
        if (fp.size() >= 2) sh = cssParseFloat(fp[1]);
    }
    if (std::isnan(sh)) sh = 0;
    st.shrink = sh;

    st.aspect = cssAspectRatio(styleStr(style, QStringLiteral("aspect-ratio")));

    st.width = parseLength(styleStr(style, QStringLiteral("width")), ex);
    st.height = parseLength(styleStr(style, QStringLiteral("height")), ex);
    st.minWidth = parseLength(styleStr(style, QStringLiteral("min-width")), ex);
    st.maxWidth = parseLength(styleStr(style, QStringLiteral("max-width")), ex);
    st.minHeight = parseLength(styleStr(style, QStringLiteral("min-height")), ex);
    st.maxHeight = parseLength(styleStr(style, QStringLiteral("max-height")), ex);
    st.flexBasis = parseLength(styleStr(style, QStringLiteral("flex-basis")), ex);

    static const QString sides[4] = { QStringLiteral("top"), QStringLiteral("right"),
                                      QStringLiteral("bottom"), QStringLiteral("left") };
    parseBox(styleStr(style, QStringLiteral("margin")), st.marginBox, ex);
    parseBox(styleStr(style, QStringLiteral("padding")), st.paddingBox, ex);
    parseBox(styleStr(style, QStringLiteral("border-width")), st.borderWidthBox, ex);
    st.borderAll = parseLength(styleStr(style, QStringLiteral("border")).section(QLatin1Char(' '), 0, 0), ex);
    for (int i = 0; i < 4; ++i) {
        st.marginSide[i] = parseLength(styleStr(style, QStringLiteral("margin-") + sides[i]), ex);
        st.paddingSide[i] = parseLength(styleStr(style, QStringLiteral("padding-") + sides[i]), ex);
        const QString sw = styleStr(style, QStringLiteral("border-") + sides[i] + QStringLiteral("-width"));
        st.borderSide[i] = !sw.isEmpty()
            ? parseLength(sw, ex)
            : parseLength(styleStr(style, QStringLiteral("border-") + sides[i]).section(QLatin1Char(' '), 0, 0), ex);
    }

    const QString inset = styleStr(style, QStringLiteral("inset"));
    st.insetBox = !inset.isEmpty();
    if (st.insetBox) {
        parseBox(inset, st.offset, ex);
    } else {
        for (int i = 0; i < 4; ++i)
            st.offset[i] = parseLength(styleStr(style, sides[i]), ex);
    }
    return st;
}

QExplicitlySharedDataPointer<CssLayoutStyle> cssSharedLayoutStyle(const QVariantMap &style)
{
    // Bounded LRU cache (GUI thread only), step 5b: the one-entry memo this replaces only shared two
    // CONSECUTIVE boxes of the same resolved style — an ALTERNATING sequence (`.row > .id, .label`:
    // row, id, label, row, id, label, …) evicted the match before it came back around, so every box
    // got its own freshly-parsed CssLayoutStyle even though the underlying map is the SAME shared
    // identity for every box of a given class (the context node's own-resolve memo, 4b-2). 16 entries
    // is enough slack for a few distinct classes in rotation; each hit is a linear scan of IDENTITY
    // compares only (isSharedWith, O(1) each — at most 16 pointer compares). Holding every entry's map
    // keeps its data alive, so identity cannot be reused by an unrelated map while compared.
    struct Entry {
        QVariantMap from;
        QExplicitlySharedDataPointer<CssLayoutStyle> style;
    };
    static constexpr int kCapacity = 16;
    static Entry cache[kCapacity];
    static int count = 0;

    for (int i = 0; i < count; ++i) {
        if (style.isSharedWith(cache[i].from)) {
            // LRU: bubble the hit to the front so a class still in rotation never ages out behind
            // ones used less recently.
            Entry hit = cache[i];
            for (int j = i; j > 0; --j)
                cache[j] = cache[j - 1];
            cache[0] = hit;
            return cache[0].style;
        }
    }
    Entry fresh{ style, QExplicitlySharedDataPointer<CssLayoutStyle>(new CssLayoutStyle(CssLayoutStyle::parse(style))) };
    const int n = std::min(count + 1, kCapacity);
    for (int j = n - 1; j > 0; --j)
        cache[j] = cache[j - 1];
    cache[0] = fresh;
    count = n;
    return cache[0].style;
}

} // namespace QmlCss
