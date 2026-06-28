#include "csslayout.h"

#include "csstheme.h"

#include <QQuickItem>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>

namespace {

// Trimmed string value of a style key, or empty.
QString styleStr(const QVariantMap &style, const QString &key)
{
    const auto it = style.constFind(key);
    return it == style.constEnd() ? QString() : it->toString().trimmed();
}

// JS parseFloat: leading numeric prefix (sign, digits, ., exponent); NaN if none.
double jsParseFloat(const QString &s)
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

bool isLayoutChild(QQuickItem *k)
{
    return k && (k->property("style").isValid() || k->property("cssPrimitive").isValid());
}

} // namespace

CssLayoutEngine::CssLayoutEngine(CssTheme *theme, QObject *parent)
    : QObject(parent)
    , m_theme(theme)
    , m_flushTimer(new QTimer(this))
{
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(0);
    connect(m_flushTimer, &QTimer::timeout, this, &CssLayoutEngine::flush);
}

// --- scheduling -------------------------------------------------------------------------

void CssLayoutEngine::requestLayout(QQuickItem *root, QQuickItem *content)
{
    if (!root || !content)
        return;
    m_pending.insert(root, content);
    if (!m_flushTimer->isActive())
        m_flushTimer->start();
}

void CssLayoutEngine::flush()
{
    // Snapshot: layouts may queue more work (a child's implicit size change re-queues its
    // parent) — those land in a fresh batch and reschedule.
    const auto batch = m_pending;
    m_pending.clear();
    for (auto it = batch.constBegin(); it != batch.constEnd(); ++it) {
        if (it.key() && it.value())
            layout(it.key(), it.value());
    }
}

// --- length / calc ----------------------------------------------------------------------

qreal CssLayoutEngine::length(const QString &value, qreal avail) const
{
    return evalExpr(value, avail);
}

double CssLayoutEngine::calcUnit(const QString &tokIn, double avail) const
{
    const QString tok = tokIn.trimmed();
    if (tok.isEmpty())
        return std::nan("");
    if (tok.endsWith(QLatin1Char('%'))) {
        const double p = jsParseFloat(tok);
        return std::isnan(p) ? std::nan("") : avail * p / 100.0;
    }
    const QString low = tok.toLower();
    const double vw = m_theme ? m_theme->viewportWidth() : 0;
    const double vh = m_theme ? m_theme->viewportHeight() : 0;
    if (low.endsWith(QLatin1String("vmin")))
        return jsParseFloat(tok) * std::min(vw, vh) / 100.0;
    if (low.endsWith(QLatin1String("vmax")))
        return jsParseFloat(tok) * std::max(vw, vh) / 100.0;
    if (low.endsWith(QLatin1String("vw")))
        return jsParseFloat(tok) * vw / 100.0;
    if (low.endsWith(QLatin1String("vh")))
        return jsParseFloat(tok) * vh / 100.0;
    return jsParseFloat(tok); // px / bare number; NaN for "auto"/"fit-content"/…
}

namespace {
struct Tok {
    enum Type { Op, Id, Num } type;
    QString text;
};

QVector<Tok> calcTokenize(const QString &s)
{
    QVector<Tok> t;
    int i = 0;
    const int n = s.size();
    while (i < n) {
        const QChar c = s[i];
        if (c.isSpace()) { ++i; continue; }
        if (QStringLiteral("()+*/,-").contains(c)) { t.push_back({ Tok::Op, QString(c) }); ++i; continue; }
        if (c.isLetter()) {
            int j = i;
            while (j < n && s[j].isLetter())
                ++j;
            t.push_back({ Tok::Id, s.mid(i, j - i).toLower() });
            i = j;
            continue;
        }
        int num = i;
        while (num < n && (s[num].isDigit() || s[num] == QLatin1Char('.')))
            ++num;
        int u = num;
        while (u < n && (s[u].isLetter() || s[u] == QLatin1Char('%')))
            ++u;
        t.push_back({ Tok::Num, s.mid(i, u - i) });
        i = u;
    }
    return t;
}
} // namespace

double CssLayoutEngine::evalExpr(const QString &value, double avail) const
{
    const QString s = value.trimmed();
    if (s.isEmpty())
        return std::nan("");
    const QString low = s.toLower();
    if (!low.contains(QLatin1String("calc(")) && !low.contains(QLatin1String("min("))
        && !low.contains(QLatin1String("max(")) && !low.contains(QLatin1String("clamp(")))
        return calcUnit(s, avail);

    const QVector<Tok> t = calcTokenize(s);
    int i = 0;
    // Recursive descent: add/sub over mul/div over primary (with parens, fns, unary sign).
    std::function<double()> primary, mul, add;
    primary = [&]() -> double {
        if (i >= t.size())
            return std::nan("");
        const Tok &tk = t[i];
        if (tk.type == Tok::Op && tk.text == QLatin1String("-")) { ++i; return -primary(); }
        if (tk.type == Tok::Op && tk.text == QLatin1String("+")) { ++i; return primary(); }
        if (tk.type == Tok::Op && tk.text == QLatin1String("(")) {
            ++i;
            const double v = add();
            if (i < t.size() && t[i].text == QLatin1String(")")) ++i;
            return v;
        }
        if (tk.type == Tok::Id) {
            const QString name = tk.text;
            ++i;
            if (i < t.size() && t[i].text == QLatin1String("(")) {
                ++i;
                QVector<double> args;
                args.push_back(add());
                while (i < t.size() && t[i].text == QLatin1String(",")) { ++i; args.push_back(add()); }
                if (i < t.size() && t[i].text == QLatin1String(")")) ++i;
                if (name == QLatin1String("min")) return *std::min_element(args.begin(), args.end());
                if (name == QLatin1String("max")) return *std::max_element(args.begin(), args.end());
                if (name == QLatin1String("clamp") && args.size() >= 3)
                    return std::min(std::max(args[1], args[0]), args[2]);
                return args.isEmpty() ? std::nan("") : args[0]; // calc()/unknown
            }
            return std::nan(""); // bare keyword
        }
        if (tk.type == Tok::Num) { ++i; return calcUnit(tk.text, avail); }
        ++i;
        return std::nan("");
    };
    mul = [&]() -> double {
        double v = primary();
        while (i < t.size() && t[i].type == Tok::Op
               && (t[i].text == QLatin1String("*") || t[i].text == QLatin1String("/"))) {
            const QString op = t[i].text;
            ++i;
            const double r = primary();
            v = (op == QLatin1String("*")) ? v * r : v / r;
        }
        return v;
    };
    add = [&]() -> double {
        double v = mul();
        while (i < t.size() && t[i].type == Tok::Op
               && (t[i].text == QLatin1String("+") || t[i].text == QLatin1String("-"))) {
            const QString op = t[i].text;
            ++i;
            const double r = mul();
            v = (op == QLatin1String("+")) ? v + r : v - r;
        }
        return v;
    };
    return add();
}

// --- box-model helpers ------------------------------------------------------------------

double CssLayoutEngine::sizeOf(const QVariantMap &style, const QString &key, double avail) const
{
    const QString v = styleStr(style, key);
    return v.isEmpty() ? std::nan("") : evalExpr(v, avail);
}

QVector<double> CssLayoutEngine::box(const QString &value) const
{
    if (value.isEmpty())
        return { 0, 0, 0, 0 };
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    const QStringList parts = value.trimmed().split(ws, Qt::SkipEmptyParts);
    QVector<double> n;
    for (const QString &p : parts) {
        const double x = evalExpr(p, 0);
        n.push_back(std::isnan(x) ? 0 : x);
    }
    if (n.size() == 1) return { n[0], n[0], n[0], n[0] };
    if (n.size() == 2) return { n[0], n[1], n[0], n[1] };
    if (n.size() == 3) return { n[0], n[1], n[2], n[1] };
    if (n.size() >= 4) return { n[0], n[1], n[2], n[3] };
    return { 0, 0, 0, 0 };
}

QVector<double> CssLayoutEngine::paddingOf(const QVariantMap &style) const
{
    QVector<double> b = box(styleStr(style, "padding"));
    const QString sides[4] = { QStringLiteral("padding-top"), QStringLiteral("padding-right"),
                               QStringLiteral("padding-bottom"), QStringLiteral("padding-left") };
    for (int i = 0; i < 4; ++i) {
        const QString s = styleStr(style, sides[i]);
        if (!s.isEmpty()) {
            const double r = evalExpr(s, 0);
            if (!std::isnan(r)) b[i] = r;
        }
    }
    return b;
}

QVector<double> CssLayoutEngine::marginOf(const QVariantMap &style) const
{
    QVector<double> b = box(styleStr(style, "margin"));
    const QString sides[4] = { QStringLiteral("margin-top"), QStringLiteral("margin-right"),
                               QStringLiteral("margin-bottom"), QStringLiteral("margin-left") };
    for (int i = 0; i < 4; ++i) {
        const QString s = styleStr(style, sides[i]);
        if (!s.isEmpty()) {
            const double r = evalExpr(s, 0);
            if (!std::isnan(r)) b[i] = r;
        }
    }
    return b;
}

double CssLayoutEngine::aspectOf(const QVariantMap &style) const
{
    const QString v = styleStr(style, "aspect-ratio");
    if (v.isEmpty())
        return std::nan("");
    if (v.contains(QLatin1Char('/'))) {
        const QStringList p = v.split(QLatin1Char('/'));
        const double a = jsParseFloat(p.value(0)), b = jsParseFloat(p.value(1));
        return (a > 0 && b > 0) ? a / b : std::nan("");
    }
    const double r = jsParseFloat(v);
    return r > 0 ? r : std::nan("");
}

// --- geometry assignment ----------------------------------------------------------------

void CssLayoutEngine::place(QQuickItem *k, double x, double y, double w, double h) const
{
    if (!std::isnan(w) && w >= 0 && std::abs(k->width() - w) > 0.5) k->setWidth(w);
    if (!std::isnan(h) && h >= 0 && std::abs(k->height() - h) > 0.5) k->setHeight(h);
    if (!std::isnan(x) && std::abs(k->x() - x) > 0.5) k->setX(x);
    if (!std::isnan(y) && std::abs(k->y() - y) > 0.5) k->setY(y);
}

void CssLayoutEngine::placeAbsolute(QQuickItem *k, double ox, double oy, double cw, double ch) const
{
    const QVariantMap ks = k->property("style").toMap();
    double t = std::nan(""), r = std::nan(""), b = std::nan(""), l = std::nan("");
    const QString inset = styleStr(ks, "inset");
    if (!inset.isEmpty()) {
        const QVector<double> ib = box(inset);
        t = ib[0]; r = ib[1]; b = ib[2]; l = ib[3];
    } else {
        const QString ts = styleStr(ks, "top"); if (!ts.isEmpty()) t = evalExpr(ts, ch);
        const QString rs = styleStr(ks, "right"); if (!rs.isEmpty()) r = evalExpr(rs, cw);
        const QString bs = styleStr(ks, "bottom"); if (!bs.isEmpty()) b = evalExpr(bs, ch);
        const QString ls = styleStr(ks, "left"); if (!ls.isEmpty()) l = evalExpr(ls, cw);
    }
    double w = sizeOf(ks, "width", cw);
    double h = sizeOf(ks, "height", ch);
    const double ar = aspectOf(ks);
    if (!std::isnan(ar) && ar > 0) {
        if (!std::isnan(w) && std::isnan(h)) h = w / ar;
        else if (!std::isnan(h) && std::isnan(w)) w = h * ar;
    }
    double x;
    if (!std::isnan(l) && !std::isnan(r)) { x = ox + l; if (std::isnan(w)) w = cw - l - r; }
    else if (!std::isnan(r)) { if (std::isnan(w)) w = k->implicitWidth(); x = ox + cw - r - w; }
    else { if (std::isnan(w)) w = k->implicitWidth(); x = ox + (std::isnan(l) ? 0 : l); }
    double y;
    if (!std::isnan(t) && !std::isnan(b)) { y = oy + t; if (std::isnan(h)) h = ch - t - b; }
    else if (!std::isnan(b)) { if (std::isnan(h)) h = k->implicitHeight(); y = oy + ch - b - h; }
    else { if (std::isnan(h)) h = k->implicitHeight(); y = oy + (std::isnan(t) ? 0 : t); }
    place(k, x, y, w, h);
}

// --- main entry -------------------------------------------------------------------------

void CssLayoutEngine::layout(QQuickItem *root, QQuickItem *content)
{
    if (!root || !content)
        return;
    const QVariantMap rootStyle = root->property("style").toMap();
    const QVector<double> pad = paddingOf(rootStyle);
    const double originX = pad[3];
    const double originY = pad[0];
    const double cw = std::max(0.0, root->width() - pad[1] - pad[3]);
    const double ch = std::max(0.0, root->height() - pad[0] - pad[2]);

    QString disp = styleStr(rootStyle, "display");
    if (disp.isEmpty())
        disp = QStringLiteral("block");
    const bool isGrid = (disp == QLatin1String("grid"));
    const bool isFlex = disp.contains(QLatin1String("flex"));

    QList<QQuickItem *> flow, abs;
    const auto kids = content->childItems();
    for (QQuickItem *k : kids) {
        if (!isLayoutChild(k))
            continue;
        const QVariantMap ks = k->property("style").toMap();
        if (styleStr(ks, "display") == QLatin1String("none"))
            continue;
        if (styleStr(ks, "position") == QLatin1String("absolute"))
            abs.push_back(k);
        else
            flow.push_back(k);
    }

    const QPair<double, double> contentSize = isGrid
        ? layoutGrid(flow, rootStyle, cw, ch, originX, originY)
        : layoutFlex(flow, rootStyle, cw, ch, originX, originY, isFlex);

    for (QQuickItem *k : abs)
        placeAbsolute(k, originX, originY, cw, ch);

    const double iw = contentSize.first + pad[1] + pad[3];
    const double ih = contentSize.second + pad[0] + pad[2];
    if (std::abs(root->implicitWidth() - iw) > 0.5) root->setImplicitWidth(iw);
    if (std::abs(root->implicitHeight() - ih) > 0.5) root->setImplicitHeight(ih);
}

// --- flexbox ----------------------------------------------------------------------------

QPair<double, double> CssLayoutEngine::layoutFlex(const QList<QQuickItem *> &flow,
                                                  const QVariantMap &rootStyle, double cw, double ch,
                                                  double originX, double originY, bool isFlex) const
{
    QString flexDir = styleStr(rootStyle, "flex-direction");
    if (flexDir.isEmpty()) flexDir = QStringLiteral("row");
    const bool horizontal = isFlex ? flexDir.startsWith(QLatin1String("row")) : false;
    const double mainAvail = horizontal ? cw : ch;
    const double crossAvail = horizontal ? ch : cw;

    QString gapStr = styleStr(rootStyle, "gap");
    if (gapStr.isEmpty()) gapStr = styleStr(rootStyle, "row-gap");
    if (gapStr.isEmpty()) gapStr = styleStr(rootStyle, "column-gap");
    const double gap = gapStr.isEmpty() ? 0 : evalExpr(gapStr, 0);

    QString justify = styleStr(rootStyle, "justify-content");
    if (justify.isEmpty()) justify = QStringLiteral("flex-start");
    QString alignItems = styleStr(rootStyle, "align-items");
    if (alignItems.isEmpty()) alignItems = QStringLiteral("stretch");

    const int n = flow.size();
    QVector<double> mains(n), crosses(n), grows(n);
    QVector<bool> cfixed(n);
    QVector<QPair<double, double>> mMar(n), cMar(n);
    double usedMain = 0, totalGrow = 0;

    for (int i = 0; i < n; ++i) {
        const QVariantMap ks = flow[i]->property("style").toMap();
        const QVector<double> mar = marginOf(ks);
        mMar[i] = horizontal ? qMakePair(mar[3], mar[1]) : qMakePair(mar[0], mar[2]);
        cMar[i] = horizontal ? qMakePair(mar[0], mar[2]) : qMakePair(mar[3], mar[1]);

        const bool wExplicit = !std::isnan(sizeOf(ks, "width", cw));
        const bool hExplicit = !std::isnan(sizeOf(ks, "height", ch));
        double w = sizeOf(ks, "width", cw);
        double h = sizeOf(ks, "height", ch);
        const double ar = aspectOf(ks);
        if (!std::isnan(ar) && ar > 0) {
            if (!std::isnan(w) && std::isnan(h)) h = w / ar;
            else if (!std::isnan(h) && std::isnan(w)) w = h * ar;
        }
        if (std::isnan(w)) w = flow[i]->implicitWidth();
        if (std::isnan(h)) h = flow[i]->implicitHeight();

        mains[i] = horizontal ? w : h;
        crosses[i] = horizontal ? h : w;
        cfixed[i] = horizontal ? (hExplicit || (!std::isnan(ar) && wExplicit))
                               : (wExplicit || (!std::isnan(ar) && hExplicit));

        double g = 0;
        const QString fg = styleStr(ks, "flex-grow");
        if (!fg.isEmpty()) g = jsParseFloat(fg);
        else { const QString fx = styleStr(ks, "flex"); if (!fx.isEmpty()) g = jsParseFloat(fx); }
        if (std::isnan(g)) g = 0;
        grows[i] = g;

        usedMain += mains[i] + mMar[i].first + mMar[i].second;
        totalGrow += g;
    }

    if (n > 1) usedMain += gap * (n - 1);
    double freeSpace = mainAvail - usedMain;

    if (freeSpace > 0 && totalGrow > 0) {
        for (int i = 0; i < n; ++i) mains[i] += freeSpace * grows[i] / totalGrow;
        freeSpace = 0;
    }

    double pos = 0;
    double between = gap;
    if (totalGrow == 0 && freeSpace > 0) {
        if (justify == QLatin1String("center")) pos = freeSpace / 2;
        else if (justify == QLatin1String("flex-end") || justify == QLatin1String("end")) pos = freeSpace;
        else if (justify == QLatin1String("space-between") && n > 1) between = gap + freeSpace / (n - 1);
        else if (justify == QLatin1String("space-around") && n > 0) { between = gap + freeSpace / n; pos = freeSpace / (2 * n); }
        else if (justify == QLatin1String("space-evenly") && n > 0) { between = gap + freeSpace / (n + 1); pos = freeSpace / (n + 1); }
    }

    double maxCross = 0;
    for (int i = 0; i < n; ++i) {
        const QVariantMap ks = flow[i]->property("style").toMap();
        const double msz = mains[i];
        double csz = crosses[i];
        QString al = styleStr(ks, "align-self");
        if (al.isEmpty()) al = alignItems;
        const double crossInner = crossAvail - cMar[i].first - cMar[i].second;
        if ((al == QLatin1String("stretch") || al.isEmpty()) && !cfixed[i]) csz = crossInner;
        double crossPos = cMar[i].first;
        if (al == QLatin1String("center")) crossPos = cMar[i].first + (crossInner - csz) / 2;
        else if (al == QLatin1String("flex-end") || al == QLatin1String("end")) crossPos = crossAvail - cMar[i].second - csz;

        const double mainPos = pos + mMar[i].first;
        if (horizontal) place(flow[i], originX + mainPos, originY + crossPos, msz, csz);
        else place(flow[i], originX + crossPos, originY + mainPos, csz, msz);

        pos = mainPos + msz + mMar[i].second + between;
        const double totalCross = csz + cMar[i].first + cMar[i].second;
        if (totalCross > maxCross) maxCross = totalCross;
    }
    const double usedMainFinal = n > 0 ? (pos - between) : 0;
    return horizontal ? qMakePair(usedMainFinal, maxCross) : qMakePair(maxCross, usedMainFinal);
}

// --- grid -------------------------------------------------------------------------------

QStringList CssLayoutEngine::gridSplit(const QString &s) const
{
    QStringList out;
    int depth = 0;
    QString cur;
    for (const QChar c : s) {
        if (c == QLatin1Char('(')) ++depth;
        else if (c == QLatin1Char(')')) --depth;
        if ((c == QLatin1Char(' ') || c == QLatin1Char('\t')) && depth == 0) {
            if (!cur.isEmpty()) { out << cur; cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

QString CssLayoutEngine::expandRepeat(const QString &specIn) const
{
    QString out = specIn;
    int guard = 0;
    while (guard++ < 64) {
        const int idx = out.toLower().indexOf(QLatin1String("repeat("));
        if (idx < 0)
            break;
        const int open = idx + 6; // the "("
        int depth = 0, end = -1;
        for (int i = open; i < out.size(); ++i) {
            if (out[i] == QLatin1Char('(')) ++depth;
            else if (out[i] == QLatin1Char(')')) { if (--depth == 0) { end = i; break; } }
        }
        if (end < 0)
            break;
        const QString inner = out.mid(open + 1, end - open - 1);
        const int comma = inner.indexOf(QLatin1Char(','));
        if (comma < 0)
            break;
        const int nrep = inner.left(comma).trimmed().toInt();
        const QString pattern = inner.mid(comma + 1).trimmed();
        QStringList rep;
        for (int j = 0; j < std::max(1, nrep); ++j)
            rep << pattern;
        out = out.left(idx) + rep.join(QLatin1Char(' ')) + out.mid(end + 1);
    }
    return out;
}

CssLayoutEngine::Track CssLayoutEngine::parseTrack(const QString &tokenIn, double avail) const
{
    const QString t = tokenIn.trimmed().toLower();
    if (t == QLatin1String("auto") || t == QLatin1String("min-content") || t == QLatin1String("max-content"))
        return { Track::Auto, 0 };
    if (t.startsWith(QLatin1String("minmax("))) {
        const QString inner = t.mid(7, t.lastIndexOf(QLatin1Char(')')) - 7);
        const QStringList parts = inner.split(QLatin1Char(','));
        return parseTrack(parts.size() > 1 ? parts[1] : parts[0], avail); // grow toward the max
    }
    if (t.endsWith(QLatin1String("fr"))) {
        const double f = jsParseFloat(t);
        return { Track::Fr, std::isnan(f) ? 1.0 : f };
    }
    if (t.endsWith(QLatin1Char('%'))) {
        const double p = jsParseFloat(t);
        return { Track::Px, std::isnan(p) ? 0.0 : avail * p / 100.0 };
    }
    const double n = calcUnit(t, avail);
    return std::isnan(n) ? Track{ Track::Auto, 0 } : Track{ Track::Px, n };
}

QVector<CssLayoutEngine::Track> CssLayoutEngine::parseTracks(const QString &spec, double avail) const
{
    QVector<Track> tracks;
    if (spec.isEmpty())
        return tracks;
    const QStringList toks = gridSplit(expandRepeat(spec));
    for (const QString &tk : toks)
        tracks.push_back(parseTrack(tk, avail));
    return tracks;
}

QVector<double> CssLayoutEngine::resolveTracks(const QVector<Track> &tracks, double avail, double gap,
                                               const QVector<double> &contentSizes) const
{
    double used = 0, frSum = 0;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks[i].kind == Track::Px) used += tracks[i].value;
        else if (tracks[i].kind == Track::Auto) used += contentSizes.value(i, 0);
        else if (tracks[i].kind == Track::Fr) frSum += tracks[i].value;
    }
    used += gap * std::max(0, static_cast<int>(tracks.size()) - 1);
    const double freeSpace = std::max(0.0, avail - used);
    QVector<double> sizes;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks[i].kind == Track::Px) sizes.push_back(tracks[i].value);
        else if (tracks[i].kind == Track::Auto) sizes.push_back(contentSizes.value(i, 0));
        else sizes.push_back(frSum > 0 ? freeSpace * tracks[i].value / frSum : 0);
    }
    return sizes;
}

QPair<double, double> CssLayoutEngine::layoutGrid(const QList<QQuickItem *> &flow,
                                                  const QVariantMap &rootStyle, double cw, double ch,
                                                  double ox, double oy) const
{
    QString colGapS = styleStr(rootStyle, "column-gap");
    if (colGapS.isEmpty()) colGapS = styleStr(rootStyle, "gap");
    QString rowGapS = styleStr(rootStyle, "row-gap");
    if (rowGapS.isEmpty()) rowGapS = styleStr(rootStyle, "gap");
    const double colGap = colGapS.isEmpty() ? 0 : evalExpr(colGapS, 0);
    const double rowGap = rowGapS.isEmpty() ? 0 : evalExpr(rowGapS, 0);

    QVector<Track> colTracks = parseTracks(styleStr(rootStyle, "grid-template-columns"), cw);
    if (colTracks.isEmpty()) colTracks.push_back({ Track::Fr, 1 });
    const int ncols = colTracks.size();
    const int n = flow.size();
    const int nrows = std::max(1, (n + ncols - 1) / ncols);

    QVector<double> dW(n), dH(n);
    for (int k = 0; k < n; ++k) {
        const QVariantMap ks = flow[k]->property("style").toMap();
        double w = sizeOf(ks, "width", cw);
        double h = sizeOf(ks, "height", ch);
        const double ar = aspectOf(ks);
        if (!std::isnan(ar) && ar > 0) {
            if (!std::isnan(w) && std::isnan(h)) h = w / ar;
            else if (!std::isnan(h) && std::isnan(w)) w = h * ar;
        }
        dW[k] = std::isnan(w) ? flow[k]->implicitWidth() : w;
        dH[k] = std::isnan(h) ? flow[k]->implicitHeight() : h;
    }

    QVector<double> colContent(ncols, 0);
    for (int k = 0; k < n; ++k) { const int c = k % ncols; if (dW[k] > colContent[c]) colContent[c] = dW[k]; }
    const QVector<double> colSizes = resolveTracks(colTracks, cw, colGap, colContent);

    QVector<Track> rowTracks = parseTracks(styleStr(rootStyle, "grid-template-rows"), ch);
    QVector<double> rowContent(nrows, 0);
    for (int k = 0; k < n; ++k) { const int r = k / ncols; if (dH[k] > rowContent[r]) rowContent[r] = dH[k]; }
    QVector<double> rowSizes;
    if (!rowTracks.isEmpty()) {
        while (rowTracks.size() < nrows) rowTracks.push_back({ Track::Auto, 0 });
        rowTracks.resize(nrows);
        rowSizes = resolveTracks(rowTracks, ch, rowGap, rowContent);
    } else {
        rowSizes = rowContent;
    }

    QVector<double> colX(ncols);
    double x = ox;
    for (int c = 0; c < ncols; ++c) { colX[c] = x; x += colSizes[c] + colGap; }
    QVector<double> rowY(nrows);
    double y = oy;
    for (int r = 0; r < nrows; ++r) { rowY[r] = y; y += rowSizes[r] + rowGap; }

    QString justifyItems = styleStr(rootStyle, "justify-items");
    if (justifyItems.isEmpty()) justifyItems = QStringLiteral("stretch");
    QString alignItems = styleStr(rootStyle, "align-items");
    if (alignItems.isEmpty()) alignItems = QStringLiteral("stretch");

    for (int k = 0; k < n; ++k) {
        const QVariantMap ks = flow[k]->property("style").toMap();
        const int col = k % ncols, row = k / ncols;
        const double cellX = colX[col], cellY = rowY[row];
        const double cellW = colSizes[col], cellH = rowSizes[row];
        bool wExplicit = !std::isnan(sizeOf(ks, "width", cw));
        bool hExplicit = !std::isnan(sizeOf(ks, "height", ch));
        if (!std::isnan(aspectOf(ks))) { wExplicit = wExplicit || dW[k] > 0; hExplicit = hExplicit || dH[k] > 0; }
        const double iw2 = (justifyItems == QLatin1String("stretch") && !wExplicit) ? cellW : dW[k];
        const double ih2 = (alignItems == QLatin1String("stretch") && !hExplicit) ? cellH : dH[k];
        double px = cellX, py = cellY;
        if (justifyItems == QLatin1String("center")) px = cellX + (cellW - iw2) / 2;
        else if (justifyItems == QLatin1String("end") || justifyItems == QLatin1String("flex-end")) px = cellX + cellW - iw2;
        if (alignItems == QLatin1String("center")) py = cellY + (cellH - ih2) / 2;
        else if (alignItems == QLatin1String("end") || alignItems == QLatin1String("flex-end")) py = cellY + cellH - ih2;
        place(flow[k], px, py, iw2, ih2);
    }

    double totalW = 0;
    for (int c = 0; c < ncols; ++c) totalW += colSizes[c];
    totalW += colGap * std::max(0, ncols - 1);
    double totalH = 0;
    for (int r = 0; r < nrows; ++r) totalH += rowSizes[r];
    totalH += rowGap * std::max(0, nrows - 1);
    return qMakePair(totalW, totalH);
}

// --- transform-keyframe animation -------------------------------------------------------

QVariantList CssLayoutEngine::buildAnimStops(const QVariantList &frames) const
{
    if (frames.isEmpty())
        return {};
    QVariantList out;
    for (const QVariant &fv : frames) {
        const QVariantMap f = fv.toMap();
        const QVariantMap props = f.value(QStringLiteral("properties")).toMap();
        QVariantMap tr;
        if (props.contains(QStringLiteral("transform")) && m_theme)
            tr = m_theme->parseTransform(props.value(QStringLiteral("transform")).toString());
        QVariantMap stop;
        stop.insert(QStringLiteral("offset"), f.value(QStringLiteral("offset")));
        stop.insert(QStringLiteral("rotate"), tr.value(QStringLiteral("rotate"), 0.0));
        stop.insert(QStringLiteral("scale"), tr.value(QStringLiteral("scale"), 1.0));
        stop.insert(QStringLiteral("tx"), tr.value(QStringLiteral("translateX"), 0.0));
        stop.insert(QStringLiteral("ty"), tr.value(QStringLiteral("translateY"), 0.0));
        out.push_back(stop);
    }
    if (out.first().toMap().value(QStringLiteral("offset")).toDouble() > 0) {
        QVariantMap z;
        z.insert(QStringLiteral("offset"), 0.0);
        z.insert(QStringLiteral("rotate"), 0.0);
        z.insert(QStringLiteral("scale"), 1.0);
        z.insert(QStringLiteral("tx"), 0.0);
        z.insert(QStringLiteral("ty"), 0.0);
        out.prepend(z);
    }
    QVariantMap last = out.last().toMap();
    if (last.value(QStringLiteral("offset")).toDouble() < 1) {
        last.insert(QStringLiteral("offset"), 1.0);
        out.push_back(last);
    }
    return out;
}

void CssLayoutEngine::applyAnim(QQuickItem *root, const QVariantList &stops, qreal p) const
{
    if (!root || stops.size() < 2)
        return;
    QVariantMap a = stops.first().toMap(), b = stops.last().toMap();
    for (int i = 0; i < stops.size() - 1; ++i) {
        const double o0 = stops[i].toMap().value(QStringLiteral("offset")).toDouble();
        const double o1 = stops[i + 1].toMap().value(QStringLiteral("offset")).toDouble();
        if (p >= o0 && p <= o1) { a = stops[i].toMap(); b = stops[i + 1].toMap(); break; }
    }
    const double span = b.value(QStringLiteral("offset")).toDouble() - a.value(QStringLiteral("offset")).toDouble();
    const double t = span > 0 ? (p - a.value(QStringLiteral("offset")).toDouble()) / span : 0;
    const auto lerp = [&](const QString &k) {
        const double av = a.value(k).toDouble(), bv = b.value(k).toDouble();
        return av + (bv - av) * t;
    };
    root->setProperty("_animRotate", lerp(QStringLiteral("rotate")));
    root->setProperty("_animScale", lerp(QStringLiteral("scale")));
    root->setProperty("_animTx", lerp(QStringLiteral("tx")));
    root->setProperty("_animTy", lerp(QStringLiteral("ty")));
}
