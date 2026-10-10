#pragma once

// Internal shared declarations for the CssTheme translation units. NOT installed,
// NOT API — consumers use qmlcss/csstheme.h.

#include "csstheme.h"
#include "csslayout.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <atomic>
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
inline int g_namedIdentityReads = 0; // test probe: by-name identity reads (foreign items only)
inline int g_namedStyleReads = 0; // test probe: by-name style reads in the layout (foreign items only)
inline int g_liveContextNodes = 0; // test probe: live CssContextNode instances (4b-2)
inline int g_ownResolveMisses = 0; // test probe: own resolves a CssBox apply actually computed (4b-2)
inline int g_namedInheritReads = 0; // test probe: by-name inherited* reads (foreign ancestors only, 4b-4)
inline int g_textLabelWrites = 0; // test probe: properties written to a CssText's composed label (4b-4)
inline int g_layoutStringParses = 0; // test probe: layout style strings parsed (lengths, boxes, `flex`; 4b-3)
// Lean text (4c) probes: text layouts a lean CssText built on the GUI thread (metrics, and the layout the private
// handoff keeps until sync), on the render thread (public handoff, or a scene graph rebuilt without one), and its
// QSGTextNode (re)builds. The render-thread ones are atomics (threaded render loop).
inline int g_textGuiLayouts = 0;
inline std::atomic<int> g_textRenderLayouts{ 0 };
inline std::atomic<int> g_textNodeBuilds{ 0 };

// Lean text switch, read once from the environment: SQ_LEAN_TEXT=0 keeps every CssText on the composed
// QQuickText. The override is a test probe (-1: the environment). The render-thread handoff is always
// public (the render thread lays out from value state); there used to be a private, GUI-shaped handoff
// mode too (SQ_LEAN_TEXT_HANDOFF) — removed (4c).
inline int g_leanTextOverride = -1; // 0 composed, 1 lean
inline bool leanTextEnabled()
{
    if (g_leanTextOverride >= 0)
        return g_leanTextOverride != 0;
    static const bool enabled = qEnvironmentVariable("SQ_LEAN_TEXT") != QLatin1String("0");
    return enabled;
}

// Step 5a: opt-in phase timers (SQ_PHASE_STATS=1), same shape as ApplyStats above — a plain
// bool checked once per instrumented site, so every call site costs one predictable branch and
// nothing else when disabled (no QElapsedTimer::start()/nsecsElapsed(), no allocation). A bucket
// is either TIMED (time(): accumulates ns + bumps count) or COUNT-ONLY (bump(): no timing at all —
// layout.drains/layout.childVisits are plain counters, not wall-clock samples). Printed once, from
// the destructor at clean exit, one line per bucket (count, total ms); a bucket whose time
// deliberately INCLUDES nested buckets (repeater.completeCreate runs the whole delegate's
// componentComplete cascade, layout.flush runs every layout() of the drain, css.apply runs the
// whole chain/resolve/hover/push apply) is marked "(incl)" in the printed line.
struct PhaseBucket {
    qint64 ns = 0;
    qint64 count = 0;
};
struct PhaseStats {
    bool enabled = qEnvironmentVariableIntValue("SQ_PHASE_STATS") != 0;
    QHash<QByteArray, PhaseBucket> buckets;
    // text.renderBuild samples from updatePaintNode, which a threaded render loop runs off the
    // GUI thread — guard the shared map so that bucket does not race every GUI-thread one. Only
    // taken on the enabled path (a diagnostic build), never while disabled.
    QMutex mutex;
    void time(const char *name, qint64 elapsedNs)
    {
        QMutexLocker locker(&mutex);
        PhaseBucket &b = buckets[QByteArray(name)];
        b.ns += elapsedNs;
        ++b.count;
    }
    void bump(const char *name, qint64 n = 1)
    {
        QMutexLocker locker(&mutex);
        buckets[QByteArray(name)].count += n;
    }
    ~PhaseStats()
    {
        if (!enabled || buckets.isEmpty())
            return;
        static const QSet<QByteArray> incl = { "repeater.completeCreate", "layout.flush", "css.apply" };
        QList<QByteArray> keys = buckets.keys();
        std::sort(keys.begin(), keys.end());
        for (const QByteArray &k : keys) {
            const PhaseBucket &b = buckets.value(k);
            fprintf(stderr, "[phase] %s%s x%lld | %.3fms\n", k.constData(),
                    incl.contains(k) ? " (incl)" : "", static_cast<long long>(b.count), b.ns / 1e6);
        }
    }
};
inline PhaseStats g_phaseStats;

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
