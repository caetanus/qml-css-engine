#include "qmlcss/cssbox.h"

#include "csstheme.h"
#include "csstheme_p.h" // cssVariantToStringList (internal declaration; cssbox.cpp lives beside it)

namespace QmlCss {

CssBox::CssBox(const QString &defaultPrimitive, QQuickItem *parent)
    : QQuickItem(parent)
    , m_cssPrimitive(defaultPrimitive)
{
}

void CssBox::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemVisibleHasChanged)
        m_visibilityDelivered = data.boolValue;
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
