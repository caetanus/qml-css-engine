#include "csstheme.h"
#include "csstheme_p.h"

#include "csslayout.h"
#include "qmlcss/cssbox.h"
#include "qmlcss/csscontextnode.h"
#include "qmlcss/csstext.h"
#include "cssvar.h"
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
#include <QScopeGuard>
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

namespace {
using FontWalks = FollowerWalks<FontFollowers>;
} // namespace

CssTheme::~CssTheme()
{
    // Same opt-in as the phase timers' own exit print (SQ_PHASE_STATS=1): dump the sharing census
    // once, right before this theme's registered boxes go away, so a bench needs nothing beyond the
    // env var to see both phase timings and the census at exit.
    if (g_phaseStats.enabled)
        dumpCensus();
    // Texts outliving the theme follow nothing (their m_theme QPointer is null from now on).
    for (CssText *t = m_fontFollowers; t;) {
        CssText *next = t->m_fontNext;
        FontWalks::unlinked(t, nullptr);
        t->m_fontPrev = t->m_fontNext = nullptr;
        t->m_fontFollowing = false;
        t = next;
    }
}

void CssTheme::addFontFollower(CssText *text)
{
    if (text->m_fontFollowing)
        return;
    text->m_fontFollowing = true;
    text->m_fontNext = nullptr;
    if (CssText *head = m_fontFollowers) {
        CssText *tail = head->m_fontPrev;
        tail->m_fontNext = text;
        text->m_fontPrev = tail;
        head->m_fontPrev = text;
    } else {
        m_fontFollowers = text;
        text->m_fontPrev = text;
    }
}

void CssTheme::removeFontFollower(CssText *text)
{
    if (!text->m_fontFollowing)
        return;
    FontWalks::unlinked(text, text->m_fontNext);
    CssText *head = m_fontFollowers;
    if (head == text) {
        m_fontFollowers = text->m_fontNext;
        if (text->m_fontNext)
            text->m_fontNext->m_fontPrev = text->m_fontPrev; // the tail
    } else {
        text->m_fontPrev->m_fontNext = text->m_fontNext;
        if (text->m_fontNext)
            text->m_fontNext->m_fontPrev = text->m_fontPrev;
        else
            head->m_fontPrev = text->m_fontPrev; // it was the tail
    }
    text->m_fontPrev = text->m_fontNext = nullptr;
    text->m_fontFollowing = false;
}

void CssTheme::notifyFontFollowers()
{
    FollowerWalk walk{ m_fontFollowers, FontWalks::top };
    FontWalks::top = &walk;
    while (walk.next) {
        auto *text = static_cast<CssText *>(walk.next);
        walk.next = text->m_fontNext;
        text->onFontRevisionChanged();
    }
    FontWalks::top = walk.outer;
}

void CssTheme::forgetRegistered(QObject *obj)
{
    m_registered.remove(obj);
    // Never resize m_bindings while reapplyAll() (below) is range-for'ing over it — applyCssTo()
    // could, in principle, destroy a registered object synchronously (today everything uses
    // deleteLater, so this hasn't been observed, but the guard is cheap and the alternative is
    // UB). Compaction just waits for the next death or reapplyAll's own prune.
    ++m_deadBindings;
    if (!m_walkingBindings && m_deadBindings > m_bindings.size() / 2) {
        // From ~CssBox the dying box's QPointer is not null yet (QObject clears it later): drop it by address.
        m_bindings.removeIf([obj](const QPointer<QObject> &p) { return p.isNull() || p.data() == obj; });
        m_deadBindings = 0;
    }
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
// a meta lookup for items that don't declare it — memoised per metaobject (a metaobject never gains
// properties), so the per-hop cost is a hash lookup instead of indexOfProperty's string scan over every
// property of the class chain. A plain static is fine: items live on the single GUI thread. QML types get
// dynamic metaobjects that can be freed (engine/type teardown) and their address reused by another type,
// so a hit is confirmed by className (QML type names carry a process-unique _QMLTYPE_n suffix).
static bool declaresCssAncestor(const QMetaObject *mo)
{
    struct Probe { QByteArray className; bool has; };
    static QHash<const QMetaObject *, Probe> cache;
    auto it = cache.find(mo);
    if (it != cache.end() && qstrcmp(it->className.constData(), mo->className()) == 0)
        return it->has;
    const bool has = mo->indexOfProperty("cssAncestor") >= 0;
    cache.insert(mo, Probe{QByteArray(mo->className()), has});
    return has;
}

static QQuickItem *nextCssAncestorHop(QQuickItem *n)
{
    if (declaresCssAncestor(n->metaObject())) {
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
        CssAncestorInfo info;
        if (auto *box = qobject_cast<CssBox *>(p)) {
            // A CssBox always qualifies as "an element": every box has cssPrimitive/cssClass,
            // the same probe the by-name path below runs for foreign items.
            info.id = box->cssId();
            info.classes = box->classList();
            info.classes += box->stateList();
            info.primitive = box->cssPrimitive();
        } else {
            const QMetaObject *mo = p->metaObject();
            if (mo->indexOfProperty("cssPrimitive") < 0 && mo->indexOfProperty("cssClass") < 0)
                continue;
            ++Detail::g_namedIdentityReads;
            info.id = p->property("cssId").toString();
            info.classes = cssVariantToStringList(p->property("cssClass"));
            info.classes += cssVariantToStringList(p->property("cssState"));
            info.primitive = p->property("cssPrimitive").toString();
        }
        chain.prepend(info); // the walk is inner→outer; the chain is outer→inner
    }
    return chain;
}

// The ancestor chain (outer→inner) a context node stands for: the node and its parents, each one's `self` —
// the same identities collectCssAncestors() reads live off the item tree.
static QList<CssAncestorInfo> ancestorChainOf(const CssContextNode *n)
{
    QList<CssAncestorInfo> chain;
    for (; n; n = n->parent.data())
        chain.prepend(n->self);
    return chain;
}

// Does `p` count as an element of the selector chain? Every CssBox does; a foreign item does when it
// declares a cssPrimitive or cssClass — the same test collectCssAncestors() applies.
static bool isCssElement(QQuickItem *p)
{
    if (qobject_cast<CssBox *>(p))
        return true;
    const QMetaObject *mo = p->metaObject();
    return mo->indexOfProperty("cssPrimitive") >= 0 || mo->indexOfProperty("cssClass") >= 0;
}

CssContextNode::~CssContextNode()
{
    --Detail::g_liveContextNodes;
    // Erase our intern entry ONLY if it still points at us: after rebuildRules() cleared the table, a
    // fresh node of the new generation may own the same key.
    if (owner) {
        auto it = owner->m_contextNodes.find(internKey);
        if (it != owner->m_contextNodes.end() && it.value() == this)
            owner->m_contextNodes.erase(it);
    }
    // `parent` is released after this body: a parent whose last child just died dies next, and erases
    // its own entry the same way.
}

QExplicitlySharedDataPointer<CssContextNode> CssTheme::internContext(
    const QExplicitlySharedDataPointer<CssContextNode> &parent, const CssAncestorInfo &self) const
{
    // The parent's ADDRESS is part of the key: safe, because every node holds a strong ref to its parent —
    // a key's parent cannot be freed (and its address reused) while the keyed node exists.
    QString key;
    key.reserve(64);
    key += QString::number(quintptr(parent.data()), 16);
    key += QLatin1Char('\x1d');
    key += self.id;
    key += QLatin1Char('\x1e');
    key += self.classes.join(QLatin1Char('\x1e'));
    key += QLatin1Char('\x1e');
    key += self.primitive;
    if (CssContextNode *n = m_contextNodes.value(key))
        return QExplicitlySharedDataPointer<CssContextNode>(n);
    auto *n = new CssContextNode;
    ++Detail::g_liveContextNodes;
    n->parent = parent;
    n->self = self;
    n->generation = m_generation;
    n->owner = this;
    n->internKey = key;
    m_contextNodes.insert(key, n);
    return QExplicitlySharedDataPointer<CssContextNode>(n);
}

QQuickItem *CssTheme::nearestCssElementOf(QQuickItem *item) const
{
    if (!item)
        return nullptr;
    for (QQuickItem *p = nextCssAncestorHop(item); p; p = nextCssAncestorHop(p)) {
        if (isCssElement(p))
            return p;
    }
    return nullptr;
}

QExplicitlySharedDataPointer<CssContextNode> CssTheme::contextNodeFor(QQuickItem *item) const
{
    if (!item)
        return {};
    if (auto *box = qobject_cast<CssBox *>(item))
        return box->contextNode(this);
    if (!isCssElement(item))
        return {};
    // A foreign element: no cache to validate, so its node is re-interned by name on every walk (the
    // same reads collectCssAncestors does for it).
    ++Detail::g_namedIdentityReads;
    CssAncestorInfo self;
    self.id = item->property("cssId").toString();
    self.classes = cssVariantToStringList(item->property("cssClass"));
    self.classes += cssVariantToStringList(item->property("cssState"));
    self.primitive = item->property("cssPrimitive").toString();
    return internContext(contextNodeFor(nearestCssElementOf(item)), self);
}

void CssTheme::applyCssTo(QObject *target) const
{
    QElapsedTimer t;
    if (g_applyStats.enabled || g_phaseStats.enabled) {
        t.start();
        if (target && g_applyStats.enabled)
            ++g_applyStats.byClass[QByteArray(target->metaObject()->className())];
    }
    if (!target)
        return;

    // Identity is read straight off the CssQmlItem target (re-read every apply, so a
    // later cssClass change is reflected on the next apply / reload). Typed for a CssBox
    // (one qobject_cast, then member access); by name only for a foreign item.
    auto *box = qobject_cast<CssBox *>(target);
    QString cssId;
    // Authored classes + runtime state classes (`cssState`: hover/active/focus/…) so
    // pseudo-class rules like `.counter:hover` resolve when the widget reports the state.
    QStringList classes;
    QString cssPart;
    // Type selectors (`button {}`) match this element's primitive.
    QString cssPrimitive;
    if (box) {
        cssId = box->cssId();
        classes = box->classList();
        classes += box->stateList();
        cssPart = box->cssPart();
        cssPrimitive = box->cssPrimitive();
    } else {
        ++Detail::g_namedIdentityReads;
        cssId = target->property("cssId").toString();
        classes = cssVariantToStringList(target->property("cssClass"));
        classes += cssVariantToStringList(target->property("cssState"));
        cssPart = target->property("cssPart").toString();
        cssPrimitive = target->property("cssPrimitive").toString();
    }
    // The box's selector-context node, re-validated on every apply (identity, ancestry, rule generation). It
    // stands for the whole ancestor chain and memoises the box's own resolves.
    QExplicitlySharedDataPointer<CssContextNode> node;
    if (box) {
        const qint64 tn = g_applyStats.enabled ? t.nsecsElapsed() : 0;
        node = box->contextNode(this);
        if (g_applyStats.enabled)
            g_applyStats.chainNs += t.nsecsElapsed() - tn;
    }

    // Engine-tracked hover (a composed HoverHandler on elements with an applicable :hover
    // rule — the web hovers ANY element, not just interactive ones). CssRect-only, stays
    // by name on purpose (out of scope here) — not counted: it is a live render state, not
    // an identity field.
    if (target->property("cssEngineHover").toBool())
        classes << QStringLiteral("hover");
    // `:focus` is a LIVE item state, not something an author sets: read it straight off the
    // item's native activeFocus, exactly as `:hover` rides the composed HoverHandler above.
    // Widgets therefore never have to hand-declare a "focus" cssState — every focusable box gets
    // `:focus` (and the default focus ring) for free, including ones added later.
    const bool hasFocus = box ? box->hasActiveFocus() : target->property("activeFocus").toBool();
    if (hasFocus && !classes.contains(QStringLiteral("focus")))
        classes << QStringLiteral("focus");

    // A part target (`#tray.item`, `#cpu.graph`) resolves ONLY that part — excluding the
    // bare `#id` base — so a sub-element doesn't inherit the container's own background.
    // Otherwise merge the waybar alias(es) under the primary id.
    // A `cssPart` resolves two ways, merged: the class-part `#id.part`/`.part` (waybar
    // sub-elements), and the pseudo-element `#id::part`/`::part` (a decorative overlay, e.g.
    // the keyboard tab ring's `::tab-stop`). Pseudo-element wins on conflict — it names the
    // decoration itself, not a container alias.
    auto resolvePartMerged = [&](const QStringList &cls) {
        QVariantMap r = resolvePartRaw(cssId, cssPart, cls, QString());
        const QVariantMap pseudo = resolveExactRaw(cssId, cls, cssPart, QString());
        for (auto it = pseudo.constBegin(); it != pseudo.constEnd(); ++it)
            r.insert(it.key(), it.value());
        return r;
    };
    // An applicable `:hover` rule makes the element hover-track ITSELF (it composes a
    // HoverHandler behind cssHoverStyled) — the web hovers any element, and only
    // interactive elements get transpiler-wired tracking. Compare base (hover stripped)
    // against base+hover, so the answer is stable WHILE hovering.
    auto probeHover = [&](const QVariantMap &style, auto &&resolveClasses) {
        if (target->metaObject()->indexOfProperty("cssHoverStyled") < 0)
            return;
        QStringList off = classes;
        off.removeAll(QStringLiteral("hover"));
        QStringList on = off;
        on << QStringLiteral("hover");
        const QVariantMap offStyle = (off.size() == classes.size()) ? style : resolveClasses(off);
        target->setProperty("cssHoverStyled", resolveClasses(on) != offStyle);
    };
    QVariantMap style;
    if (box) {
        // Own-only inputs key the node's memo; own id/primitive live in node->self and the ancestor chain is the
        // node's identity. The part path reads neither ancestors, alternates nor primitive — cssPart in the key
        // also separates the two modes.
        const QStringList &alternateIds = box->alternateIdList();
        auto ownResolve = [&](const QStringList &cls) -> QVariantMap {
            QString key;
            key.reserve(64);
            key += cls.join(QLatin1Char('\x1e'));
            key += QLatin1Char('\x1f');
            key += alternateIds.join(QLatin1Char('\x1e'));
            key += QLatin1Char('\x1f');
            key += cssPart;
            if (auto it = node->ownResolves.constFind(key); it != node->ownResolves.constEnd())
                return *it;
            ++Detail::g_ownResolveMisses;
            QVariantMap r;
            if (!cssPart.isEmpty()) {
                r = resolvePartMerged(cls);
            } else {
                const QList<CssAncestorInfo> ancestors = ancestorChainOf(node->parent.data());
                r = resolveMerged(ancestors, cssId, alternateIds, cls, cssPrimitive);
            }
            // var() substituted before the map is memoised or pushed: downstream caches see final values.
            // Guarded HERE, not only inside applyVars: varsOf() resolves every ancestor node's own vars.
            if (m_usesVars)
                applyVars(r, varsOf(node->parent.data()));
            node->ownResolves.insert(key, r);
            return r;
        };
        const qint64 t1 = g_applyStats.enabled ? t.nsecsElapsed() : 0;
        style = ownResolve(classes);
        if (cssPart.isEmpty()) {
            const qint64 t2 = g_applyStats.enabled ? t.nsecsElapsed() : 0;
            probeHover(style, ownResolve);
            if (g_applyStats.enabled) {
                g_applyStats.resolveNs += t2 - t1;
                g_applyStats.hoverNs += t.nsecsElapsed() - t2;
            }
        }
    } else if (!cssPart.isEmpty()) {
        style = resolvePartMerged(classes);
        if (m_usesVars)
            applyVars(style, varsForChain(collectCssAncestors(target)));
    } else {
        const QStringList alternateIds = cssVariantToStringList(target->property("cssAlternateId"));
        const qint64 t0 = g_applyStats.enabled ? t.nsecsElapsed() : 0;
        const QList<CssAncestorInfo> ancestors = collectCssAncestors(target);
        const qint64 t1 = g_applyStats.enabled ? t.nsecsElapsed() : 0;
        style = resolveMerged(ancestors, cssId, alternateIds, classes, cssPrimitive);
        const qint64 t2 = g_applyStats.enabled ? t.nsecsElapsed() : 0;
        probeHover(style, [&](const QStringList &cls) {
            return resolveMerged(ancestors, cssId, alternateIds, cls, cssPrimitive);
        });
        applyVars(style, varsForChain(ancestors));   // after the probe: it compares raw cascades
        if (g_applyStats.enabled) {
            const qint64 t3 = t.nsecsElapsed();
            g_applyStats.chainNs += t1 - t0;
            g_applyStats.resolveNs += t2 - t1;
            g_applyStats.hoverNs += t3 - t2;
        }
    }

    // Push the resolved map into the target's `style` sink; its renderer keys off it
    // (gradient/box-shadow/bevel with the alpha-fix a plain Rectangle cannot do). Typed for a
    // CssBox (one virtual call into its own setStyle, no by-name property dispatch); by name only
    // for a foreign item.
    // The inline style (style="…") over the whole cascade, :hover/:focus included — AFTER the node's
    // memo was filled, so two boxes of one identity never share an inline overlay. One pointer test
    // for a box without one.
    if (box) {
        if (const CssInlineStyle *in = box->inlineStyleData())
            overlayInlineStyle(style, *in, node.data());
    }

    const qint64 tp = g_applyStats.enabled ? t.nsecsElapsed() : 0;
    if (box)
        box->applyResolvedStyle(style);
    else
        target->setProperty("style", style);
    if (g_applyStats.enabled) {
        g_applyStats.pushNs += t.nsecsElapsed() - tp;
        ++g_applyStats.count;
    }
    if (g_phaseStats.enabled)
        g_phaseStats.time("css.apply", t.nsecsElapsed());
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
    if (!m_registered.contains(target)) {
        m_registered.insert(target);
        m_bindings.append(QPointer<QObject>(target));
        // The reverse slot must never dangle: forget the object when it dies — O(1); compact once the
        // dead outnumber the live (amortised O(1)). A box of ours tells us from its destructor (no
        // connection per box); anything else through `destroyed`.
        auto *box = qobject_cast<CssBox *>(target);
        if (box && box->m_theme == this)
            box->m_themeRegistered = true;
        else
            connect(target, &QObject::destroyed, this, &CssTheme::forgetRegistered);
        // Observe a FOREIGN target's cssClass AND cssState: a state change (focused/urgent/
        // hover/active) must re-style automatically, so the applet registers once and never
        // re-calls loadCss. A CssBox is not observed: its identity setters call restyle()
        // themselves — observing it too resolved every class/state write twice.
        if (!box) {
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
    }

    applyCssTo(target);
}

void CssTheme::reapplyForSender()
{
    restyleTarget(sender());
}

void CssTheme::restyle(CssBox *box)
{
    if (!box)
        return;
    if (!m_registered.contains(box)) {
        // Never styled yet (it completed without an identity): register + apply on its first
        // identity, exactly as the setter's loadCss did; still nothing to do without one.
        if (box->hasCssIdentity())
            loadCss(box);
        return;
    }
    restyleTarget(box);
}

void CssTheme::applyInlineStyle(CssBox *box)
{
    if (!box)
        return;
    if (!m_registered.contains(box)) {
        if (box->hasCssIdentity())
            loadCss(box);
        return;
    }
    if (g_applyStats.enabled) ++g_applyStats.fromSender;
    const LayoutBatchGuard batch(m_layoutEngine);
    applyCssTo(box);
}

void CssTheme::restyleTarget(QObject *target)
{
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
    // Walk the target's VISUAL subtree: exactly the set the old `isAncestorOf()` test selected out of
    // EVERY registered object (O(registered x depth) per class/state change — i.e. per hover), now
    // O(subtree). isAncestorOf never followed `cssAncestor` re-anchoring, and neither does this.
    // childItems() returns the implicitly shared list (no allocation).
    // Order does not matter: collectCssAncestors() reads each ancestor's identity live off the item,
    // never off a previous applyCssTo(). Raw pointers on the stack rely on the same convention as
    // m_bindings (csstheme.h): composed items are destroyed with deleteLater(), never synchronously.
    QVarLengthArray<QQuickItem *, 64> stack;
    for (QQuickItem *k : item->childItems())
        stack.append(k);
    while (!stack.isEmpty()) {
        QQuickItem *k = stack.last();
        stack.removeLast();
        if (m_registered.contains(k)) {
            if (g_applyStats.enabled) ++g_applyStats.fromDescendant;
            applyCssTo(k);
        }
        for (QQuickItem *c : k->childItems())
            stack.append(c);
    }
}

void CssTheme::reapplyAll()
{
    const LayoutBatchGuard batch(m_layoutEngine); // N applies, ONE layout flush
    ++m_reapplyEpoch;
    // Re-entrant call (applyCssTo() below triggers another reapplyAll(), e.g. a reload mid-apply):
    // the OUTER call already has m_walkingBindings set and is range-for'ing over m_bindings, so this
    // prune must not run (it would resize the list the outer loop is iterating) and this walk must
    // not touch m_bindings' iterators either — copy it. QList is implicitly shared, so the copy is
    // O(1) until the first mutation (the outer loop never mutates it, so this stays cheap); the
    // common, non-nested path below is unchanged and allocation-free.
    if (m_walkingBindings) {
        const QList<QPointer<QObject>> snapshot = m_bindings;
        for (const QPointer<QObject> &p : snapshot) {
            if (p) {
                if (g_applyStats.enabled) ++g_applyStats.fromAll;
                applyCssTo(p);
            }
        }
        return;
    }
    m_bindings.removeIf([](const QPointer<QObject> &p) { return p.isNull(); });
    m_deadBindings = 0;
    // Guard the walk below: a destroyed-object lambda firing mid-loop (applyCssTo() destroying a
    // registered object synchronously) must not resize m_bindings out from under the range-for.
    m_walkingBindings = true;
    const auto resetWalking = qScopeGuard([this] { m_walkingBindings = false; });
    for (const QPointer<QObject> &p : std::as_const(m_bindings)) {
        if (p) {
            if (g_applyStats.enabled) ++g_applyStats.fromAll;
            applyCssTo(p);
        }
    }
}

QVariantMap CssTheme::resolve(const QString &id, const QStringList &classes, const QString &pseudoElement,
                              const QString &primitive) const
{
    QVariantMap r = resolveImpl({}, id, classes, pseudoElement, primitive);
    applyVars(r, m_rootVars);
    return r;
}

QVariantMap CssTheme::resolveWith(const QString &contextId, const QString &id, const QStringList &classes,
                                  const QString &pseudoElement) const
{
    // The context id becomes a one-element ancestor chain, so `#workspaces button` matches.
    // The single `id` doubles as the primitive: `resolveWith("workspaces", "button")` must
    // match `#workspaces button` (a type selector) as well as `#workspaces #button`.
    CssAncestorInfo context;
    context.id = contextId;
    QVariantMap r = resolveImpl({context}, id, classes, pseudoElement, id);
    applyVars(r, varsForChain({context}));
    return r;
}

QVariantMap CssTheme::resolveWithAncestors(const QList<CssAncestorInfo> &ancestors, const QString &id,
                                           const QStringList &classes, const QString &pseudoElement,
                                           const QString &primitive) const
{
    QVariantMap r = resolveImpl(ancestors, id, classes, pseudoElement, primitive);
    applyVars(r, varsForChain(ancestors));
    return r;
}

QVariantMap CssTheme::resolveImpl(const QList<CssAncestorInfo> &ancestors, const QString &id,
                                  const QStringList &classes, const QString &pseudoElement,
                                  const QString &primitive) const
{
    // Uncached: a CssBox apply memoises its own resolves on its selector-context node (CssContextNode).
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
    return result;
}

QVariantMap CssTheme::resolveExact(const QString &id, const QStringList &classes, const QString &pseudoElement,
                                   const QString &requiredClass) const
{
    QVariantMap r = resolveExactRaw(id, classes, pseudoElement, requiredClass);
    applyVars(r, m_rootVars);   // no ancestry here: the root environment
    return r;
}

QVariantMap CssTheme::resolveExactRaw(const QString &id, const QStringList &classes, const QString &pseudoElement,
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

QVariantMap CssTheme::resolvePartRaw(const QString &id, const QString &part, const QStringList &classes,
                                     const QString &pseudoElement) const
{
    return resolveExactRaw(id, classes, pseudoElement, part);
}

void CssTheme::overlayInlineStyle(QVariantMap &style, const CssInlineStyle &in, const CssContextNode *node) const
{
    // An inline shorthand resets the longhands a stylesheet set (the consumers would prefer them).
    for (const QString &l : in.shadowedLonghands)
        style.remove(l);
    if (!in.usesVars) {
        for (auto it = in.decls.constBegin(); it != in.decls.constEnd(); ++it)
            style.insert(it.key(), it.value());
        return;
    }
    // var() against the box's own environment (its ancestors' and its own stylesheet custom properties)
    // plus the inline custom properties; those are consumed here, as the cascade's are.
    QVariantMap decls = in.decls;
    CssVar::substitute(decls, CssVar::overlay(varsOf(node), decls));
    for (auto it = decls.constBegin(); it != decls.constEnd(); ++it)
        style.insert(it.key(), it.value());
}

// --- custom properties -------------------------------------------------------------------------------------

const QVariantMap &CssTheme::varsOf(const CssContextNode *n) const
{
    if (!n)
        return m_rootVars;
    if (n->varsReady)
        return n->vars;
    // What the element declares as an ANCESTOR (its identity with cssState, no hover — the same inputs the
    // node is interned by), over what its parent passes down. Memoised on the node: same inputs, same map.
    const QVariantMap own = resolveImpl(ancestorChainOf(n->parent.data()), n->self.id, n->self.classes,
                                        QString(), n->self.primitive);
    n->vars = CssVar::overlay(varsOf(n->parent.data()), own);
    n->varsReady = true;
    return n->vars;
}

QVariantMap CssTheme::varsForChain(const QList<CssAncestorInfo> &ancestors) const
{
    QVariantMap env = m_rootVars;
    if (!m_usesVars)
        return env;
    for (int k = 0; k < ancestors.size(); ++k) {
        const CssAncestorInfo &a = ancestors.at(k);
        env = CssVar::overlay(env, resolveImpl(ancestors.mid(0, k), a.id, a.classes, QString(), a.primitive));
    }
    return env;
}

void CssTheme::applyVars(QVariantMap &style, const QVariantMap &inherited) const
{
    if (!m_usesVars)
        return;
    CssVar::substitute(style, CssVar::overlay(inherited, style));
}

} // namespace QmlCss

