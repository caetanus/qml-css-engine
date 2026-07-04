#pragma once

// Internal shared declarations for the CssTheme translation units. NOT installed,
// NOT API — consumers use qmlcss/csstheme.h.

#include "csstheme.h"
#include "csslayout.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>

namespace QmlCss {

// Coerce a CssQmlItem identity property (cssAlternateId / cssClass) to a string list.
QStringList cssVariantToStringList(const QVariant &v);

namespace Detail {

struct RawMediaBlock {
    QString condition; // the part between `@media` and `{`
    QString body;      // the rules inside
};

struct FullSelectorParse {
    QList<CssSimpleSelector> ancestors;
    CssSimpleSelector subject;
};

struct ApplyStats {
    qint64 chainNs = 0, resolveNs = 0, hoverNs = 0, pushNs = 0; int count = 0;
    int fromLoad = 0, fromSender = 0, fromDescendant = 0, fromAll = 0;
    QHash<QByteArray, int> byClass;
    bool enabled = qEnvironmentVariableIntValue("SQ_MOUNT_STATS") != 0;
    ~ApplyStats() {
        if (enabled && count > 0) {
            fprintf(stderr, "[apply-stats] x%d | chain: %.1fms | resolve: %.1fms | hover: %.1fms | push: %.1fms\n",
                    count, chainNs / 1e6, resolveNs / 1e6, hoverNs / 1e6, pushNs / 1e6);
            fprintf(stderr, "[apply-origin] load: %d | sender: %d | descendant: %d | reapplyAll: %d\n",
                    fromLoad, fromSender, fromDescendant, fromAll);
            for (auto it = byClass.constBegin(); it != byClass.constEnd(); ++it)
                fprintf(stderr, "[apply-class] %s x%d\n", it.key().constData(), it.value());
        }
    }
};
inline ApplyStats g_applyStats;

// Exception-safe layout hibernation: open the batch on construction, single flush on scope
// exit. Null-safe so callers work before any CssLayoutEngine registered.
struct LayoutBatchGuard {
    CssLayoutEngine *eng;
    explicit LayoutBatchGuard(CssLayoutEngine *e) : eng(e) { if (eng) eng->beginBatch(); }
    ~LayoutBatchGuard() { if (eng) eng->endBatch(); }
};

// Shared parse/matching helpers (formerly csstheme.cpp anonymous namespace); split across
// cssthemeparser.cpp / cssthemeloader.cpp / cssthemevalues.cpp / csstheme.cpp.
QString stripComments(const QString &css);
QString extractAtRules(const QString &css, QList<RawMediaBlock> &media);
CssMediaQuery parseMediaCondition(const QString &condition);
bool mediaMatches(const CssMediaQuery &query, qreal width, qreal height);
QString substituteVars(const QString &text, const QHash<QString, QString> &vars);
QString importCachePath(const QString &url);
QString expandDefineColors(const QString &cssIn);
CssSimpleSelector parseSimpleSelector(const QString &raw);
FullSelectorParse parseFullSelector(const QString &fullSelector);
bool ancestorSelectorMatches(const CssSimpleSelector &sel, const CssAncestorInfo &info);
bool ancestorChainMatches(const QList<CssSimpleSelector> &required, const QList<CssAncestorInfo> &chain);
QVariantMap parseDeclarationBlock(const QString &block, QVariantMap *importantOut = nullptr);
QVariantList parseKeyframeBlock(const QString &block);
QString extractKeyframes(const QString &css, QHash<QString, QVariantList> &out);
CssFontFace parseFontFace(const QString &body);
QString extractFontFaces(const QString &css, QList<CssFontFace> &out);
QList<CssRule> parseCss(const QString &css);
bool selectorMatches(const CssSimpleSelector &sel, bool hasAncestorContext, const QString &id,
                     const QStringList &classes, const QString &pseudoElement, const QString &primitive);
double cssSideToAngle(const QString &sideRaw);

} // namespace Detail
} // namespace QmlCss
