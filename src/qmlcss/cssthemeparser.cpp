// CssTheme parsing helpers: comments/at-rules/selectors/declarations/keyframes/font-faces.
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

namespace Detail {

QString stripComments(const QString &css)
{
    static const QRegularExpression re(QStringLiteral(R"(/\*.*?\*/)"),
                                       QRegularExpression::DotMatchesEverythingOption);
    QString result = css;
    result.remove(re);
    return result;
}


// Pull `@media` blocks out of `css` into `media` (condition + inner rules), and DROP the
// at-rules the flat parser can't scope (@supports/@container). Returns the base CSS with all
// of these removed. Brace-balanced. @media rules are applied conditionally by rebuildRules().
QString extractAtRules(const QString &css, QList<RawMediaBlock> &media)
{
    QString out;
    int i = 0;
    while (i < css.length()) {
        if (css.at(i) == QLatin1Char('@')) {
            int j = i + 1;
            while (j < css.length() && css.at(j).isLetter())
                ++j;
            const QString name = css.mid(i + 1, j - i - 1).toLower();
            const bool isMedia = (name == QLatin1String("media"));
            if (isMedia || name == QLatin1String("supports") || name == QLatin1String("container")) {
                const int brace = css.indexOf(QLatin1Char('{'), j);
                if (brace < 0)
                    break; // malformed — drop the rest
                int depth = 0, k = brace;
                for (; k < css.length(); ++k) {
                    if (css.at(k) == QLatin1Char('{'))
                        ++depth;
                    else if (css.at(k) == QLatin1Char('}') && --depth == 0) {
                        ++k;
                        break;
                    }
                }
                if (isMedia) {
                    RawMediaBlock blk;
                    blk.condition = css.mid(j, brace - j).trimmed();
                    blk.body = css.mid(brace + 1, k - brace - 2); // inside the braces
                    media.append(blk);
                }
                i = k; // skip the whole at-rule block
                continue;
            }
        }
        out += css.at(i++);
    }
    return out;
}

// Parse a media condition string ("(max-width: 720px) and (min-width: 400px), print") into
// an OR of AND-groups of feature constraints. Media types (screen/all/print) and unknown
// features are ignored (treated as no constraint). An empty/typeless query always matches.
CssMediaQuery parseMediaCondition(const QString &condition)
{
    CssMediaQuery query;
    const QStringList orParts = condition.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &orPart : orParts) {
        QList<CssMediaFeature> group;
        const QStringList andParts = orPart.split(QLatin1String(" and "), Qt::SkipEmptyParts);
        for (const QString &andPart : andParts) {
            const QString p = andPart.trimmed();
            const int open = p.indexOf(QLatin1Char('('));
            const int colon = p.indexOf(QLatin1Char(':'), open);
            const int close = p.indexOf(QLatin1Char(')'), colon);
            if (open < 0 || colon < 0 || close < 0)
                continue; // a media type (screen/all/print) or `not`/`only` — no constraint
            const QString name = p.mid(open + 1, colon - open - 1).trimmed().toLower();
            double value = 0;
            if (!CssValueParser::parseLengthPx(p.mid(colon + 1, close - colon - 1).trimmed(), &value))
                continue;
            if (name == QLatin1String("min-width") || name == QLatin1String("max-width")
                || name == QLatin1String("min-height") || name == QLatin1String("max-height"))
                group.append({ name, value });
        }
        query.orGroups.append(group);
    }
    return query;
}

bool mediaMatches(const CssMediaQuery &query, qreal width, qreal height)
{
    if (query.orGroups.isEmpty())
        return true;
    for (const QList<CssMediaFeature> &group : query.orGroups) {
        bool all = true;
        for (const CssMediaFeature &f : group) {
            const bool ok = (f.name == QLatin1String("min-width")) ? width >= f.value
                : (f.name == QLatin1String("max-width")) ? width <= f.value
                : (f.name == QLatin1String("min-height")) ? height >= f.value
                : (f.name == QLatin1String("max-height")) ? height <= f.value
                : true;
            if (!ok) { all = false; break; }
        }
        if (all)
            return true;
    }
    return false;
}

// Replace `@name` references with values from `vars`; unknown names (e.g.
// `@media`) are left untouched.
QString substituteVars(const QString &text, const QHash<QString, QString> &vars)
{
    static const QRegularExpression refRe(QStringLiteral(R"(@([A-Za-z0-9_-]+))"));
    QString result;
    qsizetype last = 0;
    auto it = refRe.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        result += text.mid(last, m.capturedStart() - last);
        const QString name = m.captured(1);
        result += vars.contains(name) ? vars.value(name) : m.captured(0);
        last = m.capturedEnd();
    }
    result += text.mid(last);
    return result;
}

// Disk-cache location for a remote @import, keyed by a hash of the URL (same scheme as
// the @font-face cache): a launch never re-downloads, and themes work offline once warm.
QString importCachePath(const QString &url)
{
    const QByteArray key = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/webimports");
    QDir().mkpath(cacheDir);
    return cacheDir + QLatin1Char('/') + QString::fromLatin1(key);
}


// Expand GTK-style `@define-color name value;` declarations: collect them,
// resolve references between them, substitute `@name` throughout, then strip
// the declarations themselves.
QString expandDefineColors(const QString &cssIn)
{
    static const QRegularExpression defRe(
        QStringLiteral(R"(@define-color\s+([A-Za-z0-9_-]+)\s+([^;]+);)"));

    QHash<QString, QString> vars;
    auto it = defRe.globalMatch(cssIn);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        vars.insert(m.captured(1).trimmed(), m.captured(2).trimmed());
    }
    if (vars.isEmpty())
        return cssIn;

    // Resolve references between definitions (e.g. `@define-color a @b;`).
    for (int pass = 0; pass < 5; ++pass) {
        bool changed = false;
        for (auto v = vars.begin(); v != vars.end(); ++v) {
            const QString expanded = substituteVars(v.value(), vars);
            if (expanded != v.value()) {
                v.value() = expanded;
                changed = true;
            }
        }
        if (!changed)
            break;
    }

    QString css = cssIn;
    css.remove(defRe);
    return substituteVars(css, vars);
}

CssSimpleSelector parseSimpleSelector(const QString &raw)
{
    CssSimpleSelector sel;
    const QString s = raw.trimmed();

    if (s.isEmpty() || s == QStringLiteral("*")) {
        sel.universal = true;
        return sel;
    }

    int i = 0;
    while (i < s.length()) {
        const QChar c = s[i];
        if (c == QLatin1Char('*')) {
            sel.universal = true;
            ++i;
        } else if (c == QLatin1Char('#')) {
            int j = i + 1;
            while (j < s.length() && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('-') || s[j] == QLatin1Char('_')))
                ++j;
            sel.id = s.mid(i + 1, j - i - 1);
            sel.specificity += 100;
            i = j;
        } else if (c == QLatin1Char('.')) {
            int j = i + 1;
            while (j < s.length() && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('-') || s[j] == QLatin1Char('_')))
                ++j;
            sel.classes.append(s.mid(i + 1, j - i - 1));
            sel.specificity += 10;
            i = j;
        } else if (c == QLatin1Char(':')) {
            // Pseudo-classes (single colon, e.g. :hover) become state classes the
            // widget must supply at resolve time; pseudo-elements (::x) are ignored.
            int j = i + 1;
            bool pseudoElement = false;
            if (j < s.length() && s[j] == QLatin1Char(':')) {
                pseudoElement = true;
                ++j;
            }
            const int nameStart = j;
            while (j < s.length() && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('-')))
                ++j;
            if (!pseudoElement) {
                sel.classes.append(s.mid(nameStart, j - nameStart));
                sel.specificity += 10;
            } else {
                // Pseudo-elements (::before/::after) name a decorative overlay; keep
                // the name so resolve*() can target it instead of dropping it.
                sel.pseudoElement = s.mid(nameStart, j - nameStart);
                sel.specificity += 1;
            }
            i = j;
        } else if (c.isLetter() || c == QLatin1Char('_')) {
            int j = i;
            while (j < s.length() && (s[j].isLetterOrNumber() || s[j] == QLatin1Char('-') || s[j] == QLatin1Char('_')))
                ++j;
            sel.element = s.mid(i, j - i);
            sel.specificity += 1;
            i = j;
        } else {
            ++i;
        }
    }

    return sel;
}


// Split a full selector on combinators and extract the subject (last token)
// plus the full ancestor chain (outer→inner) from the leading parts.
FullSelectorParse parseFullSelector(const QString &fullSelector)
{
    static const QRegularExpression combinatorRe(QStringLiteral(R"(\s*[>+~]\s*|\s+)"));
    const QStringList parts = fullSelector.trimmed().split(combinatorRe, Qt::SkipEmptyParts);

    FullSelectorParse result;
    if (parts.isEmpty()) {
        result.subject.universal = true;
        return result;
    }

    result.subject = parseSimpleSelector(parts.last());

    for (int i = 0; i + 1 < parts.size(); ++i) {
        const CssSimpleSelector ancestor = parseSimpleSelector(parts.at(i));
        result.subject.specificity += ancestor.specificity;
        result.ancestors.append(ancestor);
    }

    return result;
}

// One requirement of a rule's ancestor chain against one real ancestor element.
bool ancestorSelectorMatches(const CssSimpleSelector &sel, const CssAncestorInfo &info)
{
    if (!sel.id.isEmpty() && sel.id != info.id)
        return false;
    if (!sel.element.isEmpty() && sel.element != info.primitive)
        return false;
    for (const QString &cls : sel.classes) {
        if (!info.classes.contains(cls))
            return false;
    }
    return true;
}

// Descendant-combinator semantics: the rule's ancestors (outer→inner) must match a
// subsequence of the element's real ancestor chain (outer→inner), in order.
bool ancestorChainMatches(const QList<CssSimpleSelector> &required, const QList<CssAncestorInfo> &chain)
{
    int c = 0;
    for (const CssSimpleSelector &sel : required) {
        for (;; ++c) {
            if (c >= chain.size())
                return false;
            if (ancestorSelectorMatches(sel, chain.at(c))) {
                ++c;
                break;
            }
        }
    }
    return true;
}

QVariantMap parseDeclarationBlock(const QString &block, QVariantMap *importantOut)
{
    QVariantMap props;
    int i = 0;
    while (i < block.length()) {
        int end = block.indexOf(QLatin1Char(';'), i);
        if (end < 0)
            end = block.length();
        const QString decl = block.mid(i, end - i).trimmed();
        const int colon = decl.indexOf(QLatin1Char(':'));
        if (colon > 0) {
            const QString prop = decl.left(colon).trimmed().toLower();
            QString val = decl.mid(colon + 1).trimmed();
            // CSS `!important`: strip a trailing `!important` (optional space) and flag it,
            // so the cascade can let it win over higher-specificity non-important rules.
            bool important = false;
            {
                const int bang = val.toLower().lastIndexOf(QLatin1String("!important"));
                if (bang >= 0 && val.mid(bang + 10).trimmed().isEmpty()) {
                    val = val.left(bang).trimmed();
                    important = true;
                }
            }
            // Normalize CSS color keywords that Qt cannot parse to an explicit
            // fully-transparent hex, in the engine, so every consumer gets a usable
            // value — a raw QML `color:` binding or a Canvas fillStyle, not just
            // parseColor(). Qt's QColor rejects `transparent`/`none`/`inherit` on those
            // raw paths and falls back to black; `#00000000` reads as transparent black
            // under both Qt's #AARRGGBB and CSS's #RRGGBBAA. Whole-value only: a gradient
            // keeps its inner `transparent` stop (parseGradient routes it through
            // parseColor) and substrings inside url()/font-family are never touched.
            // `transparent` is always a colour keyword so it normalizes on any property;
            // `none`/`inherit` mean "absent" on non-colour shorthands (border/box-shadow),
            // so they only fold on colour properties.
            const QString low = val.toLower();
            const bool colorProp = prop.endsWith(QLatin1String("color"))
                || prop == QLatin1String("fill")
                || prop == QLatin1String("background");
            if (low == QLatin1String("transparent")
                || (colorProp && (low == QLatin1String("none") || low == QLatin1String("inherit"))))
                val = QStringLiteral("#00000000");
            if (!prop.isEmpty()) {
                props.insert(prop, val);
                if (important && importantOut != nullptr)
                    importantOut->insert(prop, val);
            }
        }
        i = end + 1;
    }
    return props;
}

// Parse a `@keyframes` body — `0% { ... } 50% { ... } 100% { ... }` (also `from`/`to`,
// and comma-separated offsets) — into a list of { offset (0..1), properties }, sorted by
// offset. Reuses parseDeclarationBlock for each frame's declarations.
QVariantList parseKeyframeBlock(const QString &block)
{
    QVariantList frames;
    int i = 0;
    while (i < block.length()) {
        const int brace = block.indexOf(QLatin1Char('{'), i);
        if (brace < 0)
            break;
        const QString selector = block.mid(i, brace - i).trimmed();
        const int close = block.indexOf(QLatin1Char('}'), brace + 1);
        if (close < 0)
            break;
        const QVariantMap props = parseDeclarationBlock(block.mid(brace + 1, close - brace - 1));
        const QStringList sels = selector.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &s : sels) {
            const QString t = s.trimmed().toLower();
            double offset = -1.0;
            if (t == QLatin1String("from")) {
                offset = 0.0;
            } else if (t == QLatin1String("to")) {
                offset = 1.0;
            } else if (t.endsWith(QLatin1Char('%'))) {
                bool ok = false;
                const double pct = t.left(t.length() - 1).trimmed().toDouble(&ok);
                if (ok)
                    offset = pct / 100.0;
            }
            if (offset >= 0.0) {
                QVariantMap frame;
                frame.insert(QStringLiteral("offset"), offset);
                frame.insert(QStringLiteral("properties"), props);
                frames.append(frame);
            }
        }
        i = close + 1;
    }
    std::sort(frames.begin(), frames.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("offset")).toDouble()
            < b.toMap().value(QStringLiteral("offset")).toDouble();
    });
    return frames;
}

// Extract `@keyframes <name> { ... }` blocks (brace-matched, since they nest — which the
// flat parseCss cannot handle) into `out`, and return the CSS with them removed so the
// ordinary rule parser never sees them.
QString extractKeyframes(const QString &css, QHash<QString, QVariantList> &out)
{
    QString result;
    int i = 0;
    while (i < css.length()) {
        const int at = css.indexOf(QStringLiteral("@keyframes"), i, Qt::CaseInsensitive);
        if (at < 0) {
            result += css.mid(i);
            break;
        }
        result += css.mid(i, at - i);
        int j = at + 10; // past "@keyframes"
        while (j < css.length() && css[j].isSpace())
            ++j;
        const int nameStart = j;
        while (j < css.length() && css[j] != QLatin1Char('{') && !css[j].isSpace())
            ++j;
        const QString name = css.mid(nameStart, j - nameStart).trimmed();
        while (j < css.length() && css[j] != QLatin1Char('{'))
            ++j;
        if (j >= css.length())
            break;
        int depth = 0;
        int k = j;
        for (; k < css.length(); ++k) {
            if (css[k] == QLatin1Char('{')) {
                ++depth;
            } else if (css[k] == QLatin1Char('}')) {
                --depth;
                if (depth == 0) {
                    ++k;
                    break;
                }
            }
        }
        const QString inner = css.mid(j + 1, k - j - 2);
        if (!name.isEmpty())
            out.insert(name, parseKeyframeBlock(inner));
        i = k;
    }
    return result;
}

// Parse a single `@font-face { ... }` body into { family, url }. Reuses parseDeclarationBlock for
// the declarations; pulls the family name (quotes stripped) and the FIRST url() listed in `src`.
CssFontFace parseFontFace(const QString &body)
{
    CssFontFace face;
    const QVariantMap props = parseDeclarationBlock(body);

    QString family = props.value(QStringLiteral("font-family")).toString().trimmed();
    if ((family.startsWith(QLatin1Char('"')) && family.endsWith(QLatin1Char('"')))
        || (family.startsWith(QLatin1Char('\'')) && family.endsWith(QLatin1Char('\''))))
        family = family.mid(1, family.size() - 2).trimmed();
    face.family = family;

    const QString src = props.value(QStringLiteral("src")).toString();
    static const QRegularExpression urlRe(QStringLiteral(R"(url\(\s*['"]?([^'")]+)['"]?\s*\))"));
    const QRegularExpressionMatch m = urlRe.match(src);
    if (m.hasMatch())
        face.url = m.captured(1).trimmed();
    return face;
}

// Extract `@font-face { ... }` blocks (brace-matched) into `out` and return the CSS with them
// removed, so the ordinary rule parser never sees them (their `@font-face` "selector" would
// otherwise be glued onto the following rule). Mirrors extractKeyframes.
QString extractFontFaces(const QString &css, QList<CssFontFace> &out)
{
    QString result;
    int i = 0;
    while (i < css.length()) {
        const int at = css.indexOf(QStringLiteral("@font-face"), i, Qt::CaseInsensitive);
        if (at < 0) {
            result += css.mid(i);
            break;
        }
        result += css.mid(i, at - i);
        int j = at + 10; // past "@font-face"
        while (j < css.length() && css[j] != QLatin1Char('{'))
            ++j;
        if (j >= css.length())
            break;
        int depth = 0, k = j;
        for (; k < css.length(); ++k) {
            if (css[k] == QLatin1Char('{')) {
                ++depth;
            } else if (css[k] == QLatin1Char('}')) {
                --depth;
                if (depth == 0) {
                    ++k;
                    break;
                }
            }
        }
        const CssFontFace face = parseFontFace(css.mid(j + 1, k - j - 2));
        if (!face.family.isEmpty() && !face.url.isEmpty())
            out.append(face);
        i = k;
    }
    return result;
}

QList<CssRule> parseCss(const QString &css)
{
    QList<CssRule> rules;
    const QString cleaned = expandDefineColors(stripComments(css));

    int i = 0;
    while (i < cleaned.length()) {
        const int blockStart = cleaned.indexOf(QLatin1Char('{'), i);
        if (blockStart < 0)
            break;

        const int blockEnd = cleaned.indexOf(QLatin1Char('}'), blockStart + 1);
        if (blockEnd < 0)
            break;

        QString selectorPart = cleaned.mid(i, blockStart - i);
        // Discard any preceding at-rule statements (e.g. "@define-color name #hex;"),
        // which have no {} block of their own and would otherwise be glued onto the
        // next rule's selector text. Selectors never contain ';', so only keep what
        // follows the last one.
        const int lastSemicolon = selectorPart.lastIndexOf(QLatin1Char(';'));
        if (lastSemicolon >= 0)
            selectorPart = selectorPart.mid(lastSemicolon + 1);
        selectorPart = selectorPart.trimmed();
        const QString blockContent = cleaned.mid(blockStart + 1, blockEnd - blockStart - 1);

        if (!selectorPart.isEmpty()) {
            QVariantMap important;
            const QVariantMap props = parseDeclarationBlock(blockContent, &important);
            if (!props.isEmpty()) {
                const QStringList selectors = selectorPart.split(QLatin1Char(','), Qt::SkipEmptyParts);
                for (const QString &sel : selectors) {
                    FullSelectorParse parsed = parseFullSelector(sel.trimmed());
                    rules.append({std::move(parsed.ancestors), parsed.subject, props, important});
                }
            }
        }

        i = blockEnd + 1;
    }

    return rules;
}

bool selectorMatches(const CssSimpleSelector &sel, bool hasAncestorContext, const QString &id,
                     const QStringList &classes, const QString &pseudoElement, const QString &primitive)
{
    // A pseudo-element rule (`::before`) only matches when that overlay is requested,
    // and an ordinary rule only matches when no overlay is requested.
    if (sel.pseudoElement != pseudoElement)
        return false;
    // A type selector (`button`, `input`) matches the element's primitive — the component
    // exposes `cssPrimitive`, so the engine matches against it directly (no class needed).
    if (!sel.element.isEmpty() && sel.element != primitive)
        return false;
    // A selector that constrains nothing at the top level (no id, class, or type) is too broad
    // for component styling; allow it only with an ancestor context (i.e. via resolveWith).
    if (!sel.universal && sel.id.isEmpty() && sel.classes.isEmpty() && sel.element.isEmpty()
        && !hasAncestorContext)
        return false;
    if (!sel.id.isEmpty() && sel.id != id)
        return false;
    for (const QString &cls : sel.classes) {
        if (!classes.contains(cls))
            return false;
    }
    return true;
}

// CSS gradient side keywords → angle (0deg points up, increasing clockwise).
double cssSideToAngle(const QString &sideRaw)
{
    const QString side = sideRaw.toLower().simplified();
    if (side == QLatin1String("top")) return 0.0;
    if (side == QLatin1String("right")) return 90.0;
    if (side == QLatin1String("bottom")) return 180.0;
    if (side == QLatin1String("left")) return 270.0;
    if (side == QLatin1String("top right") || side == QLatin1String("right top")) return 45.0;
    if (side == QLatin1String("bottom right") || side == QLatin1String("right bottom")) return 135.0;
    if (side == QLatin1String("bottom left") || side == QLatin1String("left bottom")) return 225.0;
    if (side == QLatin1String("top left") || side == QLatin1String("left top")) return 315.0;
    return 180.0;
}

} // namespace Detail

} // namespace QmlCss
