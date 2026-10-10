#include "cssvar.h"

namespace QmlCss::CssVar {

namespace {

constexpr int kMaxDepth = 32; // a reference chain deeper than this is treated as a cycle

// The index of the ')' closing the '(' at `open`, or -1.
int matchingParen(const QString &s, int open)
{
    int depth = 0;
    for (int i = open; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('('))
            ++depth;
        else if (c == QLatin1Char(')') && --depth == 0)
            return i;
    }
    return -1;
}

// The first top-level ',' in s[from, to), or -1.
int topLevelComma(const QString &s, int from, int to)
{
    int depth = 0;
    for (int i = from; i < to; ++i) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('('))
            ++depth;
        else if (c == QLatin1Char(')'))
            --depth;
        else if (c == QLatin1Char(',') && depth == 0)
            return i;
    }
    return -1;
}

bool expandAt(const QString &value, const QVariantMap &env, QString &out, int depth)
{
    if (depth > kMaxDepth)
        return false;
    out.clear();
    out.reserve(value.size());
    int i = 0;
    while (i < value.size()) {
        const int at = value.indexOf(QLatin1String("var("), i);
        if (at < 0) {
            out += QStringView(value).mid(i);
            break;
        }
        out += QStringView(value).mid(i, at - i);
        const int open = at + 3;
        const int close = matchingParen(value, open);
        if (close < 0)
            return false;
        const int comma = topLevelComma(value, open + 1, close);
        const QString name = value.mid(open + 1, (comma < 0 ? close : comma) - open - 1).trimmed();
        if (!isCustomProperty(name))
            return false;
        QString piece;
        const auto hit = env.constFind(name);
        bool ok = false;
        if (hit != env.constEnd())
            ok = expandAt(hit->toString(), env, piece, depth + 1);
        if (!ok && comma >= 0)   // CSS: the fallback is used when the variable is missing or invalid
            ok = expandAt(value.mid(comma + 1, close - comma - 1).trimmed(), env, piece, depth + 1);
        if (!ok)
            return false;
        out += piece;
        i = close + 1;
    }
    out = out.trimmed();
    return true;
}

} // namespace

bool expand(const QString &value, const QVariantMap &env, QString &out)
{
    return expandAt(value, env, out, 0);
}

QVariantMap overlay(const QVariantMap &base, const QVariantMap &style)
{
    QVariantMap env = base;
    QStringList withRefs;
    for (auto it = style.constBegin(); it != style.constEnd(); ++it) {
        if (!isCustomProperty(it.key()))
            continue;
        env.insert(it.key(), it.value());
        if (hasReference(it.value().toString()))
            withRefs.append(it.key());
    }
    // A custom property's references resolve where it is DECLARED (a descendant redefining what it refers
    // to does not change it): expand them against this element's environment, then hand the values down
    // final. One that cannot resolve is invalid — unset, as if undeclared.
    if (withRefs.isEmpty())
        return env;
    const QVariantMap raw = env;
    for (const QString &k : std::as_const(withRefs)) {
        QString out;
        if (expand(raw.value(k).toString(), raw, out))
            env.insert(k, out);
        else
            env.remove(k);
    }
    return env;
}

void substitute(QVariantMap &style, const QVariantMap &env)
{
    for (auto it = style.begin(); it != style.end();) {
        if (isCustomProperty(it.key())) {
            it = style.erase(it);
            continue;
        }
        if (it->typeId() == QMetaType::QString) {
            const QString v = it->toString();
            if (hasReference(v)) {
                QString out;
                if (!expand(v, env, out)) {
                    it = style.erase(it);
                    continue;
                }
                // A whole-value `transparent` reached through a variable gets the parser's normalisation.
                *it = out.compare(QLatin1String("transparent"), Qt::CaseInsensitive) == 0
                    ? QStringLiteral("#00000000") : out;
            }
        }
        ++it;
    }
}

} // namespace QmlCss::CssVar
