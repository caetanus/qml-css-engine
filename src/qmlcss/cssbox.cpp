#include "qmlcss/cssbox.h"

#include "csstheme.h"
#include "csstheme_p.h" // cssVariantToStringList (internal declaration; cssbox.cpp lives beside it)
#include "cssvar.h"

#include <QJSValue>
#include <QSet>

#include <cmath>

#include <QtQuick/private/qquickitem_p.h>
#include <QtQuick/private/qquickitemchangelistener_p.h>

namespace QmlCss {

// The one QQuickItem change listener every watching box shares (watchOwnLayoutChanges): a box's own
// implicit-size change, typed, without a connection (and a signal vector) per item. Never destroyed: an
// item that outlives static destruction would still call itemDestroyed() on it from ~QQuickItem.
struct CssOwnLayoutListener final : QQuickItemChangeListener {
    void itemImplicitWidthChanged(QQuickItem *item) override { static_cast<CssBox *>(item)->ownImplicitSizeChanged(); }
    void itemImplicitHeightChanged(QQuickItem *item) override { static_cast<CssBox *>(item)->ownImplicitSizeChanged(); }
};

namespace {
QQuickItemChangeListener *ownLayoutListener()
{
    static CssOwnLayoutListener *const listener = new CssOwnLayoutListener;
    return listener;
}
using InheritWalks = Detail::FollowerWalks<Detail::InheritFollowers>;
} // namespace

CssBox::CssBox(const QString &defaultPrimitive, QQuickItem *parent)
    : QQuickItem(parent)
    , m_cssPrimitive(defaultPrimitive)
{
}

CssBox::~CssBox()
{
    if (m_inheritSource)
        unfollowInheritance();
    // Our followers outlive us (children die after us): they follow nothing from now on.
    for (CssBox *f = m_inheritFollowers; f;) {
        CssBox *next = f->m_inheritNext;
        InheritWalks::unlinked(f, nullptr); // a walk of OUR list (we died inside it) stops here
        f->m_inheritSource = f->m_inheritPrev = f->m_inheritNext = nullptr;
        f = next;
    }
    if (m_themeRegistered && m_theme)
        m_theme->forgetRegistered(this);
}

void CssBox::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemVisibleHasChanged) {
        m_visibilityDelivered = data.boolValue;
        if (m_watchingOwnLayout)
            ownVisibilityChanged();
    }
}

void CssBox::watchOwnLayoutChanges()
{
    if (m_watchingOwnLayout)
        return;
    m_watchingOwnLayout = true;
    QQuickItemPrivate::get(this)->addItemChangeListener(
        ownLayoutListener(), QQuickItemPrivate::ImplicitWidth | QQuickItemPrivate::ImplicitHeight);
}

void CssBox::followInheritanceOf(QQuickItem *ancestor)
{
    if (!ancestor || m_inheritSource)
        return;
    auto *source = qobject_cast<CssBox *>(ancestor);
    if (!source || !source->takesPartInInheritance()) {
        // A foreign element declaring the inherited* contract: its own signal is the only report.
        connect(ancestor, SIGNAL(inheritedChanged()), this, SLOT(onForeignInheritedChanged()));
        return;
    }
    // Append (follow order = the order the old connections were made in).
    m_inheritSource = source;
    m_inheritNext = nullptr;
    if (CssBox *head = source->m_inheritFollowers) {
        CssBox *tail = head->m_inheritPrev;
        tail->m_inheritNext = this;
        m_inheritPrev = tail;
        head->m_inheritPrev = this;
    } else {
        source->m_inheritFollowers = this;
        m_inheritPrev = this;
    }
}

void CssBox::unfollowInheritance()
{
    CssBox *source = m_inheritSource;
    InheritWalks::unlinked(this, m_inheritNext);
    CssBox *head = source->m_inheritFollowers;
    if (head == this) {
        source->m_inheritFollowers = m_inheritNext;
        if (m_inheritNext)
            m_inheritNext->m_inheritPrev = m_inheritPrev; // the tail
    } else {
        m_inheritPrev->m_inheritNext = m_inheritNext;
        if (m_inheritNext)
            m_inheritNext->m_inheritPrev = m_inheritPrev;
        else
            head->m_inheritPrev = m_inheritPrev; // we were the tail
    }
    m_inheritSource = m_inheritPrev = m_inheritNext = nullptr;
}

void CssBox::propagateInheritedChange()
{
    Detail::FollowerWalk walk{ m_inheritFollowers, InheritWalks::top };
    InheritWalks::top = &walk;
    while (walk.next) {
        auto *follower = static_cast<CssBox *>(walk.next);
        walk.next = follower->m_inheritNext;
        follower->inheritSourceChanged();
    }
    InheritWalks::top = walk.outer;
}

void CssBox::identityChanged()
{
    m_contextDirty = true; // first: the re-apply below must intern a fresh node
    // No hasCssIdentity() gate here: a registered box that LOSES its identity (a text node's class
    // emptied) must still drop the stale style — the theme decides (see CssTheme::restyle).
    if (m_theme && isComponentComplete()) {
        m_theme->restyle(this);
        m_scenelessResolve = window() == nullptr; // as maybeLoadCss: healed on scene join
    } else {
        maybeLoadCss();
    }
}

QExplicitlySharedDataPointer<CssContextNode> CssBox::contextNode(const CssTheme *theme)
{
    // Re-validated on every apply (contract: frozen snapshots must not defeat live ancestor reads): the parent
    // node is recomputed first — an ancestor whose identity changed, a reparent, a cssAncestor re-anchor all
    // change the parent pointer — and our node is reused only if nothing of ours changed either.
    QExplicitlySharedDataPointer<CssContextNode> parent;
    if (QQuickItem *anc = theme->nearestCssElementOf(this))
        parent = theme->contextNodeFor(anc);
    if (m_contextNode && !m_contextDirty && m_contextNode->generation == theme->ruleGeneration()
        && m_contextNode->parent == parent)
        return m_contextNode;
    CssAncestorInfo self;
    self.id = m_cssId;
    self.classes = m_classList;
    self.classes += m_stateList;
    self.primitive = m_cssPrimitive;
    m_contextNode = theme->internContext(parent, self);
    m_contextDirty = false;
    return m_contextNode;
}

bool CssBox::resolvedUnderCurrentAncestry(const CssTheme *theme) const
{
    if (!m_contextNode || m_contextDirty)
        return false;
    // Interned nodes: the same ancestry (every identity up the chain, the rule generation) is the same
    // node, so a pointer compare decides.
    return m_contextNode->parent == theme->contextNodeFor(theme->nearestCssElementOf(const_cast<CssBox *>(this)));
}

CssInheritedText CssBox::inheritedText() const
{
    CssInheritedText r;
    if (QQuickItem *anc = inheritanceParent()) {
        auto *b = qobject_cast<const CssBox *>(anc);
        if (b && b->takesPartInInheritance()) {
            r = b->inheritedText();
        } else { // a foreign element declaring the inherited* getters
            ++Detail::g_namedInheritReads;
            r.color = anc->property("inheritedColor").toString();
            r.fontFamily = anc->property("inheritedFontFamily").toString();
            r.fontSize = anc->property("inheritedFontSize").toString();
            r.fontWeight = anc->property("inheritedFontWeight").toString();
            r.lineHeight = anc->property("inheritedLineHeight").toString();
            r.letterSpacing = anc->property("inheritedLetterSpacing").toString();
            r.textTransform = anc->property("inheritedTextTransform").toString();
            r.textAlign = anc->property("inheritedTextAlign").toString();
        }
    }
    // Static keys: a QLatin1String key would build (allocate) a QString per lookup on every walk (5d reads this
    // walk on each text re-apply to tell whether anything inherited changed).
    static const QString kColor = QStringLiteral("color"), kFontFamily = QStringLiteral("font-family"),
                         kFontSize = QStringLiteral("font-size"), kFontWeight = QStringLiteral("font-weight"),
                         kLineHeight = QStringLiteral("line-height"),
                         kLetterSpacing = QStringLiteral("letter-spacing"),
                         kTextTransform = QStringLiteral("text-transform"), kTextAlign = QStringLiteral("text-align");
    auto own = [this](const QString &k, QString &slot) {
        const auto it = m_style.constFind(k);
        if (it == m_style.constEnd())
            return;
        QString v = it->toString();
        if (!v.isEmpty())
            slot = std::move(v);
    };
    own(kColor, r.color);
    own(kFontFamily, r.fontFamily);
    own(kFontSize, r.fontSize);
    own(kFontWeight, r.fontWeight);
    own(kLineHeight, r.lineHeight);
    own(kLetterSpacing, r.letterSpacing);
    own(kTextTransform, r.textTransform);
    own(kTextAlign, r.textAlign);
    return r;
}

bool CssBox::hasCssIdentity() const
{
    if (!m_cssId.isEmpty() || !m_cssPart.isEmpty() || !m_cssPrimitive.isEmpty())
        return true;
    // An inline style alone is enough to be styled (nothing else would resolve it).
    return !m_classList.isEmpty() || m_inline;
}

// --- inline style ------------------------------------------------------------------------------------------

namespace {

// React DOM's unitless properties (kebab-case): a bare number there is NOT a length, so it gets no `px`.
bool isUnitlessProperty(const QString &prop)
{
    static const QSet<QString> unitless = {
        QStringLiteral("animation-iteration-count"), QStringLiteral("aspect-ratio"),
        QStringLiteral("border-image-outset"), QStringLiteral("border-image-slice"),
        QStringLiteral("border-image-width"), QStringLiteral("column-count"), QStringLiteral("columns"),
        QStringLiteral("flex"), QStringLiteral("flex-grow"), QStringLiteral("flex-shrink"),
        QStringLiteral("flex-order"), QStringLiteral("flex-positive"), QStringLiteral("flex-negative"),
        QStringLiteral("grid-area"), QStringLiteral("grid-row"), QStringLiteral("grid-row-end"),
        QStringLiteral("grid-row-span"), QStringLiteral("grid-row-start"), QStringLiteral("grid-column"),
        QStringLiteral("grid-column-end"), QStringLiteral("grid-column-span"), QStringLiteral("grid-column-start"),
        QStringLiteral("font-weight"), QStringLiteral("line-clamp"), QStringLiteral("line-height"),
        QStringLiteral("opacity"), QStringLiteral("order"), QStringLiteral("orphans"), QStringLiteral("scale"),
        QStringLiteral("tab-size"), QStringLiteral("widows"), QStringLiteral("z-index"), QStringLiteral("zoom"),
        QStringLiteral("fill-opacity"), QStringLiteral("flood-opacity"), QStringLiteral("stop-opacity"),
        QStringLiteral("stroke-dasharray"), QStringLiteral("stroke-dashoffset"),
        QStringLiteral("stroke-miterlimit"), QStringLiteral("stroke-opacity"), QStringLiteral("stroke-width"),
    };
    return unitless.contains(prop);
}

// `backgroundColor` -> `background-color`, `WebkitTransform` -> `-webkit-transform`, `msTransform` ->
// `-ms-transform` (React DOM's rule). Kebab-case and custom properties pass through.
QString kebabProperty(const QString &key)
{
    if (key.startsWith(QLatin1String("--")))
        return key;
    QString out;
    out.reserve(key.size() + 4);
    if (key.startsWith(QLatin1String("ms")) && key.size() > 2 && key.at(2).isUpper())
        out += QLatin1Char('-');
    for (const QChar c : key) {
        if (c.isUpper()) {
            out += QLatin1Char('-');
            out += c.toLower();
        } else {
            out += c;
        }
    }
    return Detail::normalizeDeclarationProperty(out);
}

// The longhands a shorthand resets. An inline shorthand must beat a stylesheet longhand (the web resets
// the longhands), but the consumers prefer a longhand over its shorthand whatever their origin — so the
// overlay erases the cascaded longhands first.
const QStringList *longhandsOf(const QString &prop)
{
    static const QHash<QString, QStringList> table = [] {
        QHash<QString, QStringList> t;
        const QStringList sides = {QStringLiteral("top"), QStringLiteral("right"), QStringLiteral("bottom"),
                                   QStringLiteral("left")};
        auto sided = [&](const QString &prefix, const QString &suffix) {
            QStringList l;
            for (const QString &s : sides)
                l << prefix + s + suffix;
            return l;
        };
        t.insert(QStringLiteral("padding"), sided(QStringLiteral("padding-"), QString()));
        t.insert(QStringLiteral("margin"), sided(QStringLiteral("margin-"), QString()));
        t.insert(QStringLiteral("inset"), sides);
        t.insert(QStringLiteral("border-width"), sided(QStringLiteral("border-"), QStringLiteral("-width")));
        t.insert(QStringLiteral("border-style"), sided(QStringLiteral("border-"), QStringLiteral("-style")));
        t.insert(QStringLiteral("border-color"), sided(QStringLiteral("border-"), QStringLiteral("-color")));
        QStringList border = {QStringLiteral("border-width"), QStringLiteral("border-style"),
                              QStringLiteral("border-color")};
        for (const QString &s : sides) {
            const QString side = QStringLiteral("border-") + s;
            const QStringList parts = {side + QStringLiteral("-width"), side + QStringLiteral("-style"),
                                       side + QStringLiteral("-color")};
            t.insert(side, parts);
            border << side << parts;
        }
        t.insert(QStringLiteral("border"), border);
        t.insert(QStringLiteral("border-radius"),
                 {QStringLiteral("border-top-left-radius"), QStringLiteral("border-top-right-radius"),
                  QStringLiteral("border-bottom-right-radius"), QStringLiteral("border-bottom-left-radius")});
        t.insert(QStringLiteral("background"),
                 {QStringLiteral("background-color"), QStringLiteral("background-image"),
                  QStringLiteral("background-position"), QStringLiteral("background-size"),
                  QStringLiteral("background-repeat"), QStringLiteral("background-attachment"),
                  QStringLiteral("background-clip"), QStringLiteral("background-origin")});
        t.insert(QStringLiteral("flex"),
                 {QStringLiteral("flex-grow"), QStringLiteral("flex-shrink"), QStringLiteral("flex-basis")});
        t.insert(QStringLiteral("flex-flow"), {QStringLiteral("flex-direction"), QStringLiteral("flex-wrap")});
        t.insert(QStringLiteral("gap"), {QStringLiteral("row-gap"), QStringLiteral("column-gap")});
        t.insert(QStringLiteral("overflow"), {QStringLiteral("overflow-x"), QStringLiteral("overflow-y")});
        t.insert(QStringLiteral("font"),
                 {QStringLiteral("font-family"), QStringLiteral("font-size"), QStringLiteral("font-weight"),
                  QStringLiteral("font-style"), QStringLiteral("font-variant"), QStringLiteral("line-height")});
        t.insert(QStringLiteral("outline"),
                 {QStringLiteral("outline-width"), QStringLiteral("outline-style"), QStringLiteral("outline-color")});
        t.insert(QStringLiteral("text-decoration"),
                 {QStringLiteral("text-decoration-line"), QStringLiteral("text-decoration-color"),
                  QStringLiteral("text-decoration-style")});
        t.insert(QStringLiteral("transition"),
                 {QStringLiteral("transition-property"), QStringLiteral("transition-duration"),
                  QStringLiteral("transition-timing-function"), QStringLiteral("transition-delay")});
        t.insert(QStringLiteral("animation"),
                 {QStringLiteral("animation-name"), QStringLiteral("animation-duration"),
                  QStringLiteral("animation-timing-function"), QStringLiteral("animation-delay"),
                  QStringLiteral("animation-iteration-count"), QStringLiteral("animation-direction"),
                  QStringLiteral("animation-fill-mode")});
        t.insert(QStringLiteral("grid-template"),
                 {QStringLiteral("grid-template-rows"), QStringLiteral("grid-template-columns"),
                  QStringLiteral("grid-template-areas")});
        t.insert(QStringLiteral("place-items"), {QStringLiteral("align-items"), QStringLiteral("justify-items")});
        t.insert(QStringLiteral("place-content"),
                 {QStringLiteral("align-content"), QStringLiteral("justify-content")});
        t.insert(QStringLiteral("place-self"), {QStringLiteral("align-self"), QStringLiteral("justify-self")});
        return t;
    }();
    const auto it = table.constFind(prop);
    return it == table.constEnd() ? nullptr : &*it;
}

// One map value as a declaration value, or a null QString when the entry declares nothing (null,
// undefined, false, empty — React DOM drops those).
QString inlineValueString(const QString &prop, const QVariant &v)
{
    switch (v.typeId()) {
    case QMetaType::Int: case QMetaType::UInt: case QMetaType::LongLong: case QMetaType::ULongLong:
    case QMetaType::Double: case QMetaType::Float: {
        const double d = v.toDouble();
        if (!std::isfinite(d))
            return QString();
        QString n = d == std::floor(d) && std::abs(d) < 1e15 ? QString::number(qint64(d))
                                                            : QString::number(d, 'g', 15);
        if (d != 0 && !CssVar::isCustomProperty(prop) && !isUnitlessProperty(prop))
            n += QLatin1String("px");
        return n;
    }
    case QMetaType::Bool:
    case QMetaType::UnknownType:
    case QMetaType::Nullptr:
        return QString();
    default:
        break;
    }
    const QString s = v.toString();
    if (s.trimmed().isEmpty())
        return QString();
    return Detail::normalizeDeclarationValue(prop, s);
}

} // namespace

QVariant CssBox::inlineStyle() const
{
    return m_inline ? QVariant(m_inline->decls) : QVariant(QVariantMap());
}

void CssBox::setInlineStyle(const QVariant &in)
{
    QVariant v = in;
    if (v.metaType() == QMetaType::fromType<QJSValue>())
        v = v.value<QJSValue>().toVariant();

    QVariantMap decls;
    if (v.typeId() == QMetaType::QString) {
        // The stylesheet's own declaration parser: identical normalisation (folding, colour keywords).
        // `!important` is accepted and ignored — an inline declaration already wins.
        const QVariantMap parsed = Detail::parseDeclarationBlock(v.toString());
        for (auto it = parsed.constBegin(); it != parsed.constEnd(); ++it)
            if (!it.value().toString().isEmpty())
                decls.insert(it.key(), it.value());
    } else if (v.canConvert<QVariantMap>() && v.typeId() != QMetaType::UnknownType) {
        const QVariantMap map = v.toMap();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            const QString prop = kebabProperty(it.key());
            if (prop.isEmpty())
                continue;
            const QString value = inlineValueString(prop, it.value());
            if (!value.isNull())
                decls.insert(prop, value);
        }
    }

    const bool had = bool(m_inline);
    if (decls.isEmpty() ? !had : (had && m_inline->decls == decls))
        return; // a re-bind producing the same declarations: no restyle

    if (decls.isEmpty()) {
        m_inline.reset();
    } else {
        auto next = std::make_unique<CssInlineStyle>();
        for (auto it = decls.constBegin(); it != decls.constEnd(); ++it) {
            if (const QStringList *longhands = longhandsOf(it.key()))
                for (const QString &l : *longhands)
                    if (!decls.contains(l))
                        next->shadowedLonghands << l;
            if (!next->usesVars)
                next->usesVars = CssVar::isCustomProperty(it.key()) || CssVar::hasReference(it.value().toString());
        }
        next->decls = std::move(decls);
        m_inline = std::move(next);
    }
    emit inlineStyleChanged();
    if (had != bool(m_inline))
        emit hasCssIdentityChanged();

    // Only THIS box re-applies: an inline style changes no identity, so neither the context node nor any
    // descendant's selector match moves (inherited text values follow through the usual propagation).
    if (m_theme && isComponentComplete()) {
        m_theme->applyInlineStyle(this);
        if (!window())
            m_scenelessResolve = true; // as identityChanged: healed on scene join
    } else {
        maybeLoadCss();
    }
}

void CssBox::setCssId(const QString &v)
{
    if (m_cssId == v)
        return;
    m_cssId = v;
    emit cssIdChanged();
    emit hasCssIdentityChanged();
    identityChanged();
}

void CssBox::setCssAlternateId(const QVariant &v)
{
    if (m_cssAlternateId == v)
        return;
    m_cssAlternateId = v;
    m_alternateIdList = cssVariantToStringList(v);
    emit cssAlternateIdChanged();
    identityChanged();
}

void CssBox::setCssClass(const QVariant &v)
{
    // Value-compare: QML re-binds hand a FRESH array each evaluation; an equal
    // list must not trigger a restyle (it double-applied every element on mount).
    const QStringList list = cssVariantToStringList(v);
    if (m_cssClass == v || m_classList == list)
        return;
    m_cssClass = v;
    m_classList = list;
    emit cssClassChanged();
    emit hasCssIdentityChanged();
    identityChanged();
}

void CssBox::setCssState(const QVariant &v)
{
    // Value-compare: see setCssClass.
    const QStringList list = cssVariantToStringList(v);
    if (m_cssState == v || m_stateList == list)
        return;
    m_cssState = v;
    m_stateList = list;
    emit cssStateChanged();
    identityChanged();
}

void CssBox::setCssPrimitive(const QString &v)
{
    if (m_cssPrimitive == v)
        return;
    m_cssPrimitive = v;
    emit cssPrimitiveChanged();
    emit hasCssIdentityChanged();
    identityChanged();
}

void CssBox::setCssPart(const QString &v)
{
    if (m_cssPart == v)
        return;
    m_cssPart = v;
    emit cssPartChanged();
    emit hasCssIdentityChanged();
    identityChanged();
}

} // namespace QmlCss
