#include "qmlcss/componentcache.h"
#include "qmlcss/csstext.h"

#include "qmlcss/csslayout.h"
#include "qmlcss/csstheme.h"
#include "qmlcss/csstheme_p.h"
#include "qmlcss/valueparser.h"

#include <QFont>
#include <QMetaProperty>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTextOption>
#include <QUrl>

#include <algorithm>

namespace QmlCss {

namespace {

// QML CssText._capitalization(v) -> QFont::Capitalization.
QFont::Capitalization capitalizationFor(const QString &v)
{
    const QString s = v.trimmed().toLower();
    if (s == QLatin1String("uppercase"))
        return QFont::AllUppercase;
    if (s == QLatin1String("lowercase"))
        return QFont::AllLowercase;
    if (s == QLatin1String("capitalize"))
        return QFont::Capitalize;
    return QFont::MixedCase;
}

// The composed label's properties, resolved ONCE per process: the label is always the same QQuickText type, so
// the indices never change and a write skips the by-name lookup of QObject::setProperty.
struct LabelProps {
    QMetaProperty text, color, font, lineHeightMode, lineHeight, textFormat, verticalAlignment;
    QMetaProperty leftPadding, rightPadding, topPadding, bottomPadding, wrapMode, elide, horizontalAlignment;
};

const LabelProps &labelProps(const QQuickItem *label)
{
    static const LabelProps props = [label] {
        const QMetaObject *mo = label->metaObject();
        auto prop = [mo](const char *name) {
            const int index = mo->indexOfProperty(name);
            Q_ASSERT(index >= 0);
            return mo->property(index);
        };
        return LabelProps{ prop("text"), prop("color"), prop("font"), prop("lineHeightMode"), prop("lineHeight"),
                           prop("textFormat"), prop("verticalAlignment"), prop("leftPadding"), prop("rightPadding"),
                           prop("topPadding"), prop("bottomPadding"), prop("wrapMode"), prop("elide"),
                           prop("horizontalAlignment") };
    }();
    return props;
}

// Write one payload field to the label unless it equals the value last applied (or the label is fresh).
template <typename T>
void putLabel(QQuickItem *label, const QMetaProperty &prop, bool fresh, const T &value, T &applied)
{
    if (!fresh && value == applied)
        return;
    prop.write(label, QVariant::fromValue(value));
    applied = value;
    ++Detail::g_textLabelWrites;
}

} // namespace

CssText::CssText(QQuickItem *parent)
    : CssBox(QStringLiteral("text"), parent)
{
}

void CssText::setStyle(const QVariantMap &v)
{
    if (m_style == v)
        return;
    m_style = v;
    emit styleChanged();

    applyToText();
    applyShadow();
    applyBackground();

    // QML: onStyleChanged -> _notifyParent(); pre-mount applies must not trigger layout.
    if (isComponentComplete() && m_layout)
        m_layout->notifyParentLayout(this);
    emit inheritedChanged();
}

void CssText::setText(const QString &v)
{
    if (m_text == v)
        return;
    m_text = v;
    if (m_label)
        putLabel<QString>(m_label, labelProps(m_label).text, !m_payloadApplied, m_text, m_applied.text);
    emit textChanged();
}

void CssText::setStyledText(bool v)
{
    if (m_styledText == v)
        return;
    m_styledText = v;
    if (m_label) // Text.StyledText : PlainText
        putLabel<int>(m_label, labelProps(m_label).textFormat, !m_payloadApplied, m_styledText ? 4 : 0,
                      m_applied.textFormat);
    emit styledTextChanged();
}

void CssText::setDefaultColor(const QColor &v)
{
    if (m_defaultColor == v)
        return;
    m_defaultColor = v;
    emit defaultColorChanged();
    applyToText();
}

void CssText::setDefaultFontFamily(const QString &v)
{
    if (m_defaultFontFamily == v)
        return;
    m_defaultFontFamily = v;
    emit defaultFontFamilyChanged();
    applyToText();
}

void CssText::setDefaultFontSize(qreal v)
{
    if (qFuzzyCompare(m_defaultFontSize, v))
        return;
    m_defaultFontSize = v;
    emit defaultFontSizeChanged();
    applyToText();
}

// --- CSS inheritance chain (QML _cssParent + inherited* getters) -----------------------------

QQuickItem *CssText::cssParentItem() const
{
    // QML: a directly-styled parent (inheritedColor non-empty) wins; else the container at
    // parent.parent (whichever exposes inheritedColor). A CssBox parent answers through the typed walk
    // (same value its inheritedColor getter returns); anything else by name, as before.
    QQuickItem *p = parentItem();
    if (p) {
        auto *b = qobject_cast<const CssBox *>(p);
        if (b && b->takesPartInInheritance()) {
            if (!b->inheritedText().color.isEmpty())
                return p;
        } else {
            const QVariant ic = p->property("inheritedColor");
            if (ic.isValid() && !ic.toString().isEmpty())
                return p;
        }
    }
    QQuickItem *pp = p ? p->parentItem() : nullptr;
    if (pp && pp->metaObject()->indexOfProperty("inheritedColor") >= 0) // existence, not the getter
        return pp;
    return nullptr;
}

QString CssText::inheritedColor() const { return inheritedText().color; }
QString CssText::inheritedFontFamily() const { return inheritedText().fontFamily; }
QString CssText::inheritedFontSize() const { return inheritedText().fontSize; }
QString CssText::inheritedFontWeight() const { return inheritedText().fontWeight; }
QString CssText::inheritedLineHeight() const { return inheritedText().lineHeight; }
QString CssText::inheritedLetterSpacing() const { return inheritedText().letterSpacing; }
QString CssText::inheritedTextTransform() const { return inheritedText().textTransform; }
QString CssText::inheritedTextAlign() const { return inheritedText().textAlign; }

// --- apply resolved CSS onto the composed Text -----------------------------------------------

void CssText::applyToText()
{
    if (!m_label)
        return;

    auto styleStr = [&](const char *k) { return m_style.value(QLatin1String(k)).toString(); };
    CssTextPayload p;

    // text
    p.text = m_text;

    // One typed walk for all eight inherited values (own style value, else the inheriting ancestor's).
    const CssInheritedText inh = inheritedText();

    // color <- inheritedColor, else default.
    const QString &ic = inh.color;
    p.color = (!ic.isEmpty() && m_theme) ? m_theme->parseColor(ic) : m_defaultColor;

    // --- font (family/pointSize/weight/letterSpacing/wordSpacing/capitalization/underline/strike) ---
    QFont &font = p.font;
    const QString &fam = inh.fontFamily;
    font.setFamily(m_theme ? m_theme->resolveFontFamily(fam, m_defaultFontFamily)
                           : (fam.isEmpty() ? m_defaultFontFamily : fam));
    const QString &fs = inh.fontSize;
    const qreal pointSize = (!fs.isEmpty() && m_theme) ? m_theme->parseFontSize(fs, m_defaultFontSize)
                                                       : m_defaultFontSize;
    font.setPointSizeF(pointSize);
    if (m_theme)
        font.setWeight(static_cast<QFont::Weight>(m_theme->fontWeight(inh.fontWeight)));
    const QString &ls = inh.letterSpacing;
    font.setLetterSpacing(QFont::AbsoluteSpacing, (!ls.isEmpty() && m_theme) ? m_theme->parseLength(ls, 0) : 0);
    const QString ws = styleStr("word-spacing");
    font.setWordSpacing((!ws.isEmpty() && m_theme) ? m_theme->parseLength(ws, 0) : 0);
    font.setCapitalization(capitalizationFor(inh.textTransform));
    const QString decoration = styleStr("text-decoration");
    font.setUnderline(decoration.contains(QLatin1String("underline")));
    font.setStrikeOut(decoration.contains(QLatin1String("line-through")));

    // line-height: mode + value (px). QML fallback px = font pixel size (or pointSize*96/72).
    const QString &lh = inh.lineHeight;
    const bool proportional = m_theme ? m_theme->isProportionalLineHeight(lh) : true;
    p.lineHeightMode = proportional ? 0 : 1; // Text.ProportionalHeight : Text.FixedHeight
    const qreal fallbackPx = pointSize * 96.0 / 72.0;
    p.lineHeight = m_theme ? m_theme->parseLineHeight(lh, fallbackPx) : fallbackPx;

    // Transpiled JSX text is literal — PlainText unless the widget opted into StyledText
    // (menu mnemonics underline via <u>).
    p.textFormat = m_styledText ? 4 : 0; // Text.StyledText : Text.PlainText

    // padding (QML boxSideLength side order: left=3, right=1, top=0, bottom=2). Without a theme the
    // padding is never written: carry the applied values so the diff leaves them alone.
    const bool padded = m_theme;
    if (padded) {
        p.leftPadding = m_theme->boxSideLength(m_style, QStringLiteral("padding"), 3);
        p.rightPadding = m_theme->boxSideLength(m_style, QStringLiteral("padding"), 1);
        p.topPadding = m_theme->boxSideLength(m_style, QStringLiteral("padding"), 0);
        p.bottomPadding = m_theme->boxSideLength(m_style, QStringLiteral("padding"), 2);
    } else {
        p.leftPadding = m_applied.leftPadding;
        p.rightPadding = m_applied.rightPadding;
        p.topPadding = m_applied.topPadding;
        p.bottomPadding = m_applied.bottomPadding;
    }

    // white-space: nowrap -> NoWrap, else WordWrap.
    p.wrapMode = static_cast<int>(styleStr("white-space") == QLatin1String("nowrap") ? QTextOption::NoWrap
                                                                                     : QTextOption::WordWrap);
    // text-overflow: ellipsis -> ElideRight, else ElideNone.
    p.elide = static_cast<int>(styleStr("text-overflow") == QLatin1String("ellipsis") ? Qt::ElideRight
                                                                                       : Qt::ElideNone);

    // text-align -> horizontalAlignment.
    const QString &align = inh.textAlign;
    const Qt::Alignment halign = align == QLatin1String("center")
        ? Qt::AlignHCenter
        : (align == QLatin1String("right") ? Qt::AlignRight : Qt::AlignLeft);
    p.horizontalAlignment = static_cast<int>(halign);

    // Write only the fields that differ from the payload last applied (all of them on a fresh label).
    QQuickItem *t = m_label;
    const LabelProps &lp = labelProps(t);
    const bool fresh = !m_payloadApplied;
    putLabel(t, lp.text, fresh, p.text, m_applied.text);
    putLabel(t, lp.color, fresh, p.color, m_applied.color);
    putLabel(t, lp.font, fresh, p.font, m_applied.font);
    putLabel(t, lp.lineHeightMode, fresh, p.lineHeightMode, m_applied.lineHeightMode);
    putLabel(t, lp.lineHeight, fresh, p.lineHeight, m_applied.lineHeight);
    putLabel(t, lp.textFormat, fresh, p.textFormat, m_applied.textFormat);
    if (padded) {
        putLabel(t, lp.leftPadding, fresh, p.leftPadding, m_applied.leftPadding);
        putLabel(t, lp.rightPadding, fresh, p.rightPadding, m_applied.rightPadding);
        putLabel(t, lp.topPadding, fresh, p.topPadding, m_applied.topPadding);
        putLabel(t, lp.bottomPadding, fresh, p.bottomPadding, m_applied.bottomPadding);
    }
    putLabel(t, lp.wrapMode, fresh, p.wrapMode, m_applied.wrapMode);
    putLabel(t, lp.elide, fresh, p.elide, m_applied.elide);
    putLabel(t, lp.horizontalAlignment, fresh, p.horizontalAlignment, m_applied.horizontalAlignment);
    m_payloadApplied = true;

    // display:none never writes `visible` (imperative writes sever author bindings, e.g. the
    // <Show> guard); the layout skips it by style, painting dies via opacity 0.
    if (styleStr("display") == QLatin1String("none") || styleStr("visibility") == QLatin1String("hidden"))
        setOpacity(0);
    else
        setOpacity(m_style.contains(QStringLiteral("opacity")) ? m_style.value(QStringLiteral("opacity")).toReal()
                                                               : 1.0);

    mirrorImplicit();
}

void CssText::applyShadow()
{
    // QML: _dropShadow = (cssTheme.loaded && style["text-shadow"]) ? parseBoxShadow(...) : ({})
    QVariantMap shadow;
    const QString ts = m_style.value(QStringLiteral("text-shadow")).toString();
    if (m_theme && m_theme->isLoaded() && !ts.isEmpty())
        shadow = m_theme->parseBoxShadow(ts);
    const bool hasShadow = shadow.contains(QStringLiteral("color"));

    // Lazy composition: only a label that actually declares text-shadow pays for the effect.
    if (!m_shadow && hasShadow && m_label && isComponentComplete()) {
        if (QQmlEngine *eng = qmlEngine(this)) {
            QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("csstext-81a20bf2"),
                "import QtQuick.Effects\nMultiEffect { autoPaddingEnabled: true; visible: false }");
            if (QObject *o = comp->create(qmlContext(this))) {
                if (QQuickItem *fx = qobject_cast<QQuickItem *>(o)) {
                    fx->setParent(this);
                    fx->setParentItem(this);
                    fx->setProperty("source", QVariant::fromValue(m_label.data()));
                    m_shadow = fx;
                    layoutChild(); // size the fresh effect to the current box
                } else {
                    o->deleteLater();
                }
            }
        }
    }
    if (!m_shadow)
        return;

    // Mirror CssDropShadow.qml onto the composed MultiEffect (sourcing the Text).
    m_shadow->setProperty("shadowEnabled", hasShadow);
    m_shadow->setProperty("shadowColor", hasShadow ? shadow.value(QStringLiteral("color")).value<QColor>()
                                                    : QColor(Qt::transparent));
    m_shadow->setProperty("shadowHorizontalOffset", hasShadow ? shadow.value(QStringLiteral("x")).toReal() : 0.0);
    m_shadow->setProperty("shadowVerticalOffset", hasShadow ? shadow.value(QStringLiteral("y")).toReal() : 0.0);
    // MultiEffect.shadowBlur is 0..1 over blurMax (32px default).
    m_shadow->setProperty("shadowBlur",
                          hasShadow ? std::min<qreal>(1.0, shadow.value(QStringLiteral("blur")).toReal() / 32.0) : 0.0);

    // The MultiEffect draws the Text (plus shadow) when active — ABOVE the still-visible label
    // (same glyphs, so no double vision). Hiding the label empties the effect's source texture
    // on the RHI path, blanking the text entirely.
    m_shadow->setVisible(hasShadow);
    if (m_label)
        m_label->setVisible(true);
}

void CssText::layoutChild()
{
    if (!m_label)
        return;
    m_label->setX(0);
    m_label->setY(0);
    // When a width/height is assigned (by the layout engine), fill it so wrap/elide/valign apply;
    // otherwise let the Text keep its own implicit size so a standalone label never collapses.
    const qreal w = width();
    m_label->setWidth(w > 0.5 ? w : m_label->implicitWidth());
    const qreal h = height();
    m_label->setHeight(h > 0.5 ? h : m_label->implicitHeight());

    if (m_shadow) {
        m_shadow->setX(0);
        m_shadow->setY(0);
        m_shadow->setWidth(m_label->width());
        m_shadow->setHeight(m_label->height());
    }
    if (m_bg) {
        m_bg->setX(0);
        m_bg->setY(0);
        m_bg->setWidth(m_label->width());
        m_bg->setHeight(m_label->height());
    }
}

void CssText::mirrorImplicit()
{
    if (!m_label)
        return;
    // We are the layout participant; our implicit size is the composed Text's.
    setImplicitWidth(m_label->implicitWidth());
    setImplicitHeight(m_label->implicitHeight());
    // Keep the Text at its natural size while unmanaged.
    if (width() <= 0.5)
        m_label->setWidth(m_label->implicitWidth());
    if (height() <= 0.5)
        m_label->setHeight(m_label->implicitHeight());
}

void CssText::maybeLoadCss()
{
    // QML: if (cssTheme && hasCssIdentity()) cssTheme.loadCss(root)
    if (m_theme && hasCssIdentity()) {
        m_theme->loadCss(this);
        // A truncated resolve: container-created delegates (a SplitView handle, incubated rows)
        // complete BEFORE they are parented, so ancestor-scoped rules miss. Remembered here and
        // healed by itemChange once the item joins a window.
        m_scenelessResolve = window() == nullptr;
    }
}

void CssText::componentComplete()
{
    QQuickItem::componentComplete();

    if (QQmlContext *ctx = qmlContext(this)) {
        m_theme = qobject_cast<CssTheme *>(ctx->contextProperty(QStringLiteral("cssTheme")).value<QObject *>());
        m_layout = qobject_cast<CssLayoutEngine *>(ctx->contextProperty(QStringLiteral("cssLayout")).value<QObject *>());
    }

    // SINGLE-SHOT mount: resolve + set the style before the Text exists (the apply helpers
    // no-op on a null label); the composed Text's first configuration is already final.
    if (m_theme && hasCssIdentity()) {
        m_theme->loadCss(this);
        m_scenelessResolve = window() == nullptr; // healed by itemChange on scene attach
    }

    if (QQmlEngine *eng = qmlEngine(this)) {
        // The REAL QtQuick Text, via the Qt type-system (we forward everything onto it).
        // Configured BETWEEN beginCreate and completeCreate: a QQuickText that is not yet complete
        // defers its text layout, so the ~20 property writes of the first applyToText() cost ONE
        // layout (and one font match) at completion instead of one per write.
        {
            QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("csstext-bf5e2923"),
                "import QtQuick\nText {}");
            if (QObject *o = comp->beginCreate(qmlContext(this))) {
                if (QQuickItem *label = qobject_cast<QQuickItem *>(o)) {
                    label->setParent(this); // C++ ownership: beginCreate() means C++ owns it
                    label->setParentItem(this);
                    m_label = label;
                    m_payloadApplied = false; // a fresh label: the first apply writes the whole payload
                    // Constant for every label, so written once here instead of in the payload.
                    labelProps(label).verticalAlignment.write(label, static_cast<int>(Qt::AlignVCenter));
                    ++Detail::g_textLabelWrites;
                    applyToText();
                    comp->completeCreate();
                } else {
                    comp->completeCreate();
                    o->deleteLater();
                }
            } else {
                qWarning("CssText: failed to compose Text: %s", qPrintable(comp->errorString()));
            }
        }

        // The text-shadow MultiEffect is composed LAZILY in applyShadow(): a MultiEffect
        // drags in ShaderEffectSources + blur items even while invisible, and most labels
        // never declare text-shadow.
    }

    if (m_label) {
        // The composed Text's implicit size drives ours; re-flow the parent on change.
        connect(m_label, &QQuickItem::implicitWidthChanged, this, [this]() {
            mirrorImplicit();
            layoutChild();
        });
        connect(m_label, &QQuickItem::implicitHeightChanged, this, [this]() {
            mirrorImplicit();
            layoutChild();
        });
    }

    // notifyParentLayout on implicit/visible change (QML on*Changed -> _notifyParent), like CssHr.
    connect(this, &QQuickItem::implicitWidthChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });
    connect(this, &QQuickItem::implicitHeightChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });
    connect(this, &QQuickItem::visibleChanged, this, [this]() {
        if (m_layout)
            m_layout->notifyParentLayout(this);
    });
    // font.family re-resolves once a downloaded @font-face registers (QML leading fontRevision dep).
    if (m_theme)
        connect(m_theme, &CssTheme::fontRevisionChanged, this, [this]() {
            applyToText();
            applyShadow();
        });
    // An ancestor's inherited text props re-propagate to us (CSS inheritance).
    if (QQuickItem *anc = cssParentItem())
        connect(anc, SIGNAL(inheritedChanged()), this, SLOT(onAncestorInheritedChanged()));

    layoutChild();
    applyToText();
    applyShadow();
    applyBackground(); // a pre-completion style may already carry a background

    // (style already resolved+registered above — single-shot mount)
    if (m_layout)
        m_layout->notifyParentLayout(this);
}

void CssText::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    layoutChild();
}

// QML equivalent (what the underlay would look like declared in CssText.qml):
//   Shape {
//       z: -1; visible: bgColor.a > 0
//       ShapePath {
//           strokeColor: "transparent"; strokeWidth: 0; fillColor: bgColor
//           PathRectangle { width: parent.width; height: parent.height; radius: bgRadius }
//       }
//   }
void CssText::applyBackground()
{
    // `background-color` wins; a plain-colour `background` shorthand also paints (gradient
    // strings parse invalid and are skipped — text gradients are not mapped).
    QString bgValue = m_style.value(QStringLiteral("background-color")).toString().trimmed();
    if (bgValue.isEmpty())
        bgValue = m_style.value(QStringLiteral("background")).toString().trimmed();
    const QColor color = bgValue.isEmpty() ? QColor() : CssValueParser::parseColor(bgValue);

    if (!color.isValid() || color.alpha() == 0) {
        if (m_bg)
            m_bg->setProperty("bgColor", QColor(Qt::transparent));
        return;
    }

    if (!m_bg && isComponentComplete()) {
        if (QQmlEngine *eng = qmlEngine(this)) {
            QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("csstext-f477b5b7"),
                R"(import QtQuick
import QtQuick.Shapes
Shape {
    id: bgroot
    objectName: "cssTextBg"
    property color bgColor: "transparent"
    property real bgRadius: 0
    z: -1
    visible: bgColor.a > 0
    preferredRendererType: Shape.CurveRenderer
    ShapePath {
        strokeColor: "transparent"
        strokeWidth: 0
        fillColor: bgroot.bgColor
        PathRectangle { x: 0; y: 0; width: bgroot.width; height: bgroot.height; radius: bgroot.bgRadius }
    }
})");
            if (QObject *o = comp->create(qmlContext(this))) {
                if (QQuickItem *bg = qobject_cast<QQuickItem *>(o)) {
                    bg->setParent(this); // C++ ownership (mirrors CssRect's composed scene)
                    bg->setParentItem(this);
                    m_bg = bg;
                } else {
                    o->deleteLater();
                }
            } else {
                qWarning("CssText: failed to compose background Shape: %s", qPrintable(comp->errorString()));
            }
        }
    }
    if (!m_bg)
        return;

    double radius = 0;
    CssValueParser::parseLengthPx(m_style.value(QStringLiteral("border-radius")).toString(), &radius);
    m_bg->setProperty("bgColor", color);
    m_bg->setProperty("bgRadius", radius);
    layoutChild(); // size the fresh underlay to the current box
}

void CssText::onAncestorInheritedChanged()
{
    applyToText();
    applyShadow();
    emit inheritedChanged();
}

} // namespace QmlCss

void QmlCss::CssText::itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);
    // Heal the truncated-ancestor resolve (see maybeLoadCss): one re-resolve when the item
    // actually joins a window — zero cost for elements built inside the live scene.
    if (change == ItemSceneChange && data.window && m_scenelessResolve && isComponentComplete()) {
        m_scenelessResolve = false;
        maybeLoadCss();
    }
}
