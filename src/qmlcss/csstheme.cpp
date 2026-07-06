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




CssTheme::CssTheme(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &CssTheme::onCssFileChanged);
}

QVariantMap CssTheme::resolveMerged(const QList<CssAncestorInfo> &ancestors, const QString &cssId,
                                    const QStringList &alternateIds, const QStringList &classes,
                                    const QString &primitive) const
{
    QVariantMap merged = resolveImpl(ancestors, QString(), classes, QString(), primitive);
    // Waybar-compat aliases first (lowest priority), then the primary id wins on conflict.
    for (const QString &alt : alternateIds) {
        if (alt.isEmpty())
            continue;
        const QVariantMap m = resolveImpl(ancestors, alt, classes, QString(), primitive);
        for (auto it = m.constBegin(); it != m.constEnd(); ++it)
            merged.insert(it.key(), it.value());
    }
    if (!cssId.isEmpty()) {
        const QVariantMap primary = resolveImpl(ancestors, cssId, classes, QString(), primitive);
        for (auto it = primary.constBegin(); it != primary.constEnd(); ++it)
            merged.insert(it.key(), it.value());
    }
    return merged;
}

// One hop up the element tree. Normally the visual parent — but a subtree that Qt reparents
// out of the item tree (Popup/Menu contents live under the window Overlay) severs the visual
// chain, so descendant selectors written against the logical owner (`.wg-select .popup`)
// stop matching. A `property Item cssAncestor: owner` declared on the subtree root re-anchors
// the walk at the owner, restoring the logical chain. The metaobject probe keeps the cost to
// a meta lookup for items that don't declare it.
static QQuickItem *nextCssAncestorHop(QQuickItem *n)
{
    if (n->metaObject()->indexOfProperty("cssAncestor") >= 0) {
        if (auto *redirect = n->property("cssAncestor").value<QQuickItem *>())
            return redirect;
    }
    return n->parentItem();
}

// Walk the item tree upward collecting the CSS identity of every ancestor element (outer→inner).
// Plain Items in between (content holders, transpiler wrappers) carry no CSS signature and are
// skipped — they are not elements.
static QList<CssAncestorInfo> collectCssAncestors(QObject *target)
{
    QList<CssAncestorInfo> chain;
    auto *item = qobject_cast<QQuickItem *>(target);
    if (!item)
        return chain;
    for (QQuickItem *p = nextCssAncestorHop(item); p; p = nextCssAncestorHop(p)) {
        const QMetaObject *mo = p->metaObject();
        if (mo->indexOfProperty("cssPrimitive") < 0 && mo->indexOfProperty("cssClass") < 0)
            continue;
        CssAncestorInfo info;
        info.id = p->property("cssId").toString();
        info.classes = cssVariantToStringList(p->property("cssClass"));
        info.classes += cssVariantToStringList(p->property("cssState"));
        info.primitive = p->property("cssPrimitive").toString();
        chain.prepend(info); // the walk is inner→outer; the chain is outer→inner
    }
    return chain;
}

void CssTheme::applyCssTo(QObject *target) const
{
    QElapsedTimer t;
    if (g_applyStats.enabled) {
        t.start();
        if (target)
            ++g_applyStats.byClass[QByteArray(target->metaObject()->className())];
    }
    if (!target)
        return;

    // Identity is read straight off the CssQmlItem target (re-read every apply, so a
    // later cssClass change is reflected on the next apply / reload).
    const QString cssId = target->property("cssId").toString();
    // Authored classes + runtime state classes (`cssState`: hover/active/focus/…) so
    // pseudo-class rules like `.counter:hover` resolve when the widget reports the state.
    QStringList classes = cssVariantToStringList(target->property("cssClass"));
    classes += cssVariantToStringList(target->property("cssState"));
    // Engine-tracked hover (a composed HoverHandler on elements with an applicable :hover
    // rule — the web hovers ANY element, not just interactive ones).
    if (target->property("cssEngineHover").toBool())
        classes << QStringLiteral("hover");
    const QString cssPart = target->property("cssPart").toString();
    // Type selectors (`button {}`) match this element's primitive.
    const QString cssPrimitive = target->property("cssPrimitive").toString();

    // A part target (`#tray.item`, `#cpu.graph`) resolves ONLY that part — excluding the
    // bare `#id` base — so a sub-element doesn't inherit the container's own background.
    // Otherwise merge the waybar alias(es) under the primary id.
    QVariantMap style;
    if (!cssPart.isEmpty()) {
        // A `cssPart` resolves two ways, merged: the class-part `#id.part`/`.part` (waybar
        // sub-elements), and the pseudo-element `#id::part`/`::part` (a decorative overlay, e.g.
        // the keyboard tab ring's `::tab-stop`). Pseudo-element wins on conflict — it names the
        // decoration itself, not a container alias.
        style = resolvePart(cssId, cssPart, classes);
        const QVariantMap pseudo = resolveExact(cssId, classes, cssPart);
        for (auto it = pseudo.constBegin(); it != pseudo.constEnd(); ++it)
            style.insert(it.key(), it.value());
    } else {
        const QStringList alternateIds = cssVariantToStringList(target->property("cssAlternateId"));
        const qint64 t0 = g_applyStats.enabled ? t.nsecsElapsed() : 0;
        const QList<CssAncestorInfo> ancestors = collectCssAncestors(target);
        const qint64 t1 = g_applyStats.enabled ? t.nsecsElapsed() : 0;
        style = resolveMerged(ancestors, cssId, alternateIds, classes, cssPrimitive);
        const qint64 t2 = g_applyStats.enabled ? t.nsecsElapsed() : 0;

        // An applicable `:hover` rule makes the element hover-track ITSELF (it composes a
        // HoverHandler behind cssHoverStyled) — the web hovers any element, and only
        // interactive elements get transpiler-wired tracking. Compare base (hover stripped)
        // against base+hover, so the answer is stable WHILE hovering.
        if (target->metaObject()->indexOfProperty("cssHoverStyled") >= 0) {
            QStringList off = classes;
            off.removeAll(QStringLiteral("hover"));
            QStringList on = off;
            on << QStringLiteral("hover");
            const QVariantMap offStyle = (off.size() == classes.size())
                ? style : resolveMerged(ancestors, cssId, alternateIds, off, cssPrimitive);
            const bool hoverStyled =
                resolveMerged(ancestors, cssId, alternateIds, on, cssPrimitive) != offStyle;
            target->setProperty("cssHoverStyled", hoverStyled);
        }
        if (g_applyStats.enabled) {
            const qint64 t3 = t.nsecsElapsed();
            g_applyStats.chainNs += t1 - t0;
            g_applyStats.resolveNs += t2 - t1;
            g_applyStats.hoverNs += t3 - t2;
        }
    }

    // Push the resolved map into the target's `style` sink; its renderer keys off it
    // (gradient/box-shadow/bevel with the alpha-fix a plain Rectangle cannot do).
    const qint64 tp = g_applyStats.enabled ? t.nsecsElapsed() : 0;
    target->setProperty("style", style);
    if (g_applyStats.enabled) {
        g_applyStats.pushNs += t.nsecsElapsed() - tp;
        ++g_applyStats.count;
    }
}

void CssTheme::loadCss(QObject *target)
{
    if (!target)
        return;

    // The engine's one guarantee: the target must carry the CssQmlItem signature
    // (identity `cssId` + a `style` sink). Without it loadCss would resolve nothing and
    // style nothing — i.e. "quebra o brinquedo silenciosamente". So fail LOUDLY instead.
    const QMetaObject *mo = target->metaObject();
    if (mo->indexOfProperty("cssId") < 0 || mo->indexOfProperty("style") < 0) {
        qWarning().noquote() << "CssTheme::loadCss: target" << mo->className()
                             << "lacks the CssQmlItem signature (needs cssId + style) — refusing to style it.";
        return;
    }

    if (g_applyStats.enabled) ++g_applyStats.fromLoad;
    // Register once; the reverse slot re-reads the object's identity on every (re)load.
    if (!m_bindings.contains(QPointer<QObject>(target))) {
        m_bindings.append(QPointer<QObject>(target));
        // The reverse slot must never dangle: drop the registration when the object dies.
        connect(target, &QObject::destroyed, this, [this](QObject *obj) {
            m_bindings.removeIf([obj](const QPointer<QObject> &p) { return p.isNull() || p == obj; });
        });
        // Observe the registered cssClass AND cssState: a state change (focused/urgent/
        // hover/active) must re-style automatically, so the applet registers once and never
        // re-calls loadCss.
        const QMetaMethod reapplySlot = staticMetaObject.method(
            staticMetaObject.indexOfSlot("reapplyForSender()"));
        for (const char *stateProp : { "cssClass", "cssState" }) {
            const int idx = mo->indexOfProperty(stateProp);
            if (idx < 0)
                continue;
            const QMetaProperty prop = mo->property(idx);
            if (prop.hasNotifySignal())
                connect(target, prop.notifySignal(), this, reapplySlot);
        }
    }

    applyCssTo(target);
}

void CssTheme::reapplyForSender()
{
    QObject *target = sender();
    if (!target)
        return;
    if (g_applyStats.enabled) ++g_applyStats.fromSender;
    const LayoutBatchGuard batch(m_layoutEngine); // self + descendants, one flush
    applyCssTo(target);
    // Ancestor-scoped rules (`.nav button.active text`) read ANCESTOR identity: a class/state
    // change on `target` can restyle registered DESCENDANTS, which only observe themselves.
    auto *item = qobject_cast<QQuickItem *>(target);
    if (!item)
        return;
    for (const QPointer<QObject> &p : std::as_const(m_bindings)) {
        if (!p || p == target)
            continue;
        auto *child = qobject_cast<QQuickItem *>(p.data());
        if (child && item->isAncestorOf(child)) {
            if (g_applyStats.enabled) ++g_applyStats.fromDescendant;
            applyCssTo(child);
        }
    }
}

void CssTheme::reapplyAll()
{
    const LayoutBatchGuard batch(m_layoutEngine); // N applies, ONE layout flush
    m_bindings.removeIf([](const QPointer<QObject> &p) { return p.isNull(); });
    for (const QPointer<QObject> &p : m_bindings) {
        if (p) {
            if (g_applyStats.enabled) ++g_applyStats.fromAll;
            applyCssTo(p);
        }
    }
}

QVariantMap CssTheme::resolve(const QString &id, const QStringList &classes, const QString &pseudoElement,
                              const QString &primitive) const
{
    return resolveImpl({}, id, classes, pseudoElement, primitive);
}

QVariantMap CssTheme::resolveWith(const QString &contextId, const QString &id, const QStringList &classes,
                                  const QString &pseudoElement) const
{
    // The context id becomes a one-element ancestor chain, so `#workspaces button` matches.
    // The single `id` doubles as the primitive: `resolveWith("workspaces", "button")` must
    // match `#workspaces button` (a type selector) as well as `#workspaces #button`.
    CssAncestorInfo context;
    context.id = contextId;
    return resolveImpl({context}, id, classes, pseudoElement, id);
}

QVariantMap CssTheme::resolveWithAncestors(const QList<CssAncestorInfo> &ancestors, const QString &id,
                                           const QStringList &classes, const QString &pseudoElement,
                                           const QString &primitive) const
{
    return resolveImpl(ancestors, id, classes, pseudoElement, primitive);
}

QVariantMap CssTheme::resolveImpl(const QList<CssAncestorInfo> &ancestors, const QString &id,
                                  const QStringList &classes, const QString &pseudoElement,
                                  const QString &primitive) const
{
    // Memoized: page creation resolves IDENTICAL inputs over and over (every feed row, every
    // menu item shares classes/primitive/ancestry). Keyed by the full input signature;
    // cleared whenever the active rules change (rebuildRules).
    QString key;
    key.reserve(96);
    key += id;
    key += QLatin1Char('\x1f');
    key += classes.join(QLatin1Char('\x1e'));
    key += QLatin1Char('\x1f');
    key += pseudoElement;
    key += QLatin1Char('\x1f');
    key += primitive;
    for (const CssAncestorInfo &a : ancestors) {
        key += QLatin1Char('\x1d');
        key += a.id;
        key += QLatin1Char('\x1e');
        key += a.classes.join(QLatin1Char('\x1e'));
        key += QLatin1Char('\x1e');
        key += a.primitive;
    }
    const auto cached = m_resolveCache.constFind(key);
    if (cached != m_resolveCache.constEnd())
        return *cached;

    struct Match {
        int specificity;
        int sourceOrder;
        QVariantMap props;
        QVariantMap important;
    };

    // Candidate rules: union of the element's class buckets + element bucket + id bucket +
    // the unkeyed rules — instead of scanning the whole rule list per resolve.
    QVarLengthArray<int, 64> candidates;
    for (const QString &cls : classes)
        for (auto it = m_rulesByClass.constFind(cls); it != m_rulesByClass.constEnd() && it.key() == cls; ++it)
            candidates.append(it.value());
    if (!primitive.isEmpty())
        for (auto it = m_rulesByElement.constFind(primitive); it != m_rulesByElement.constEnd() && it.key() == primitive; ++it)
            candidates.append(it.value());
    if (!id.isEmpty())
        for (auto it = m_rulesById.constFind(id); it != m_rulesById.constEnd() && it.key() == id; ++it)
            candidates.append(it.value());
    for (int i : m_rulesUnkeyed)
        candidates.append(i);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    QList<Match> matches;
    for (int i : candidates) {
        const CssRule &rule = m_rules.at(i);
        // A descendant rule only matches when the element's real ancestor chain satisfies the
        // selector's ancestor chain. Rules with no ancestor requirement match in all contexts.
        if (!rule.ancestors.isEmpty() && !ancestorChainMatches(rule.ancestors, ancestors))
            continue;
        if (selectorMatches(rule.selector, !ancestors.isEmpty(), id, classes, pseudoElement, primitive))
            matches.append({rule.selector.specificity, i, rule.properties, rule.importantProperties});
    }

    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        return a.specificity != b.specificity ? a.specificity < b.specificity : a.sourceOrder < b.sourceOrder;
    });

    QVariantMap result;
    for (const Match &m : matches) {
        for (auto it = m.props.constBegin(); it != m.props.constEnd(); ++it)
            result.insert(it.key(), it.value());
    }
    // `!important` overlay: re-apply important declarations (in specificity order) on top,
    // so they win over any non-important declaration regardless of its specificity.
    for (const Match &m : matches) {
        for (auto it = m.important.constBegin(); it != m.important.constEnd(); ++it)
            result.insert(it.key(), it.value());
    }
    m_resolveCache.insert(key, result);
    return result;
}

QVariantMap CssTheme::resolveExact(const QString &id, const QStringList &classes, const QString &pseudoElement,
                                   const QString &requiredClass) const
{
    struct Match {
        int specificity;
        int sourceOrder;
        QVariantMap props;
        QVariantMap important;
    };

    QList<Match> matches;
    for (int i = 0; i < m_rules.size(); ++i) {
        const CssRule &rule = m_rules.at(i);
        if (!rule.ancestors.isEmpty())
            continue;
        const CssSimpleSelector &sel = rule.selector;
        if (sel.id != id || sel.pseudoElement != pseudoElement)
            continue;
        // `requiredClass` (used by resolvePart) restricts to rules carrying that class,
        // e.g. `#cpu.graph`, so the bare `#cpu` base is excluded.
        if (!requiredClass.isEmpty() && !sel.classes.contains(requiredClass))
            continue;
        // Every other class the selector demands must be a supplied state (the required
        // class is implicit), so `#cpu.graph:active` matches only when "active" is asked.
        bool ok = true;
        for (const QString &cls : sel.classes)
            if (cls != requiredClass && !classes.contains(cls)) { ok = false; break; }
        if (!ok)
            continue;
        matches.append({sel.specificity, i, rule.properties, rule.importantProperties});
    }

    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) {
        return a.specificity != b.specificity ? a.specificity < b.specificity : a.sourceOrder < b.sourceOrder;
    });

    QVariantMap result;
    for (const Match &m : matches)
        for (auto it = m.props.constBegin(); it != m.props.constEnd(); ++it)
            result.insert(it.key(), it.value());
    for (const Match &m : matches) // !important overlay (see resolveImpl)
        for (auto it = m.important.constBegin(); it != m.important.constEnd(); ++it)
            result.insert(it.key(), it.value());
    return result;
}

QVariantMap CssTheme::resolvePart(const QString &id, const QString &part, const QStringList &classes,
                                  const QString &pseudoElement) const
{
    // A "part" is an exact-match query that REQUIRES the part class — see resolveExact.
    return resolveExact(id, classes, pseudoElement, part);
}

} // namespace QmlCss

