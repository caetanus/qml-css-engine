#include "qmlcss/cssbox.h"

#include "csstheme.h"
#include "csstheme_p.h" // cssVariantToStringList (internal declaration; cssbox.cpp lives beside it)

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
    return !m_classList.isEmpty();
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
