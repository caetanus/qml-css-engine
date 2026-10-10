#include "qmlcss/componentcache.h"
#include "qmlcss/csstext.h"

#include "qmlcss/csslayout.h"
#include "qmlcss/csstheme.h"
#include "qmlcss/csstheme_p.h"
#include "qmlcss/valueparser.h"

#include <QFont>
#include <QMatrix4x4>
#include <QMetaProperty>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGTextNode>
#include <QTextOption>
#include <QUrl>

#include <QtQuick/private/qquickitem_p.h>

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

// Everything but the colour: a payload change outside these is a repaint, not a relayout.
bool sameLayoutInputs(const CssTextPayload &a, const CssTextPayload &b)
{
    return a.text == b.text && a.font == b.font && a.lineHeightMode == b.lineHeightMode
        && a.lineHeight == b.lineHeight && a.textFormat == b.textFormat && a.leftPadding == b.leftPadding
        && a.rightPadding == b.rightPadding && a.topPadding == b.topPadding && a.bottomPadding == b.bottomPadding
        && a.wrapMode == b.wrapMode && a.elide == b.elide && a.horizontalAlignment == b.horizontalAlignment;
}

// QQuickText's renderType default: QQuickTextUtil::textRenderType<QQuickText>() (qquicktextutil_p.h 96-108).
QSGTextNode::RenderType windowRenderType()
{
    switch (QQuickWindow::textRenderType()) {
    case QQuickWindow::NativeTextRendering:
        return QSGTextNode::NativeRendering;
    case QQuickWindow::CurveTextRendering:
        return QSGTextNode::CurveRendering;
    case QQuickWindow::QtTextRendering:
        break;
    }
    return QSGTextNode::QtRendering;
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
    if (m_label) {
        putLabel<QString>(m_label, labelProps(m_label).text, !m_payloadApplied, m_text, m_applied.text);
    } else if (m_lean && m_leanMounted) {
        CssTextPayload p = m_applied;
        p.text = m_text;
        applyLeanPayload(p);
    }
    emit textChanged();
}

void CssText::setStyledText(bool v)
{
    if (m_styledText == v)
        return;
    m_styledText = v;
    // Styled text keeps the composed QQuickText (its tag subset); plain text goes lean.
    if (isComponentComplete())
        syncPath();
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

CssTextPayload CssText::resolvePayload() const
{
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
    if (m_theme) {
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
    return p;
}

void CssText::applyToText()
{
    if (isComponentComplete())
        syncPath();
    if (!m_label && !m_lean)
        return;

    const CssTextPayload p = resolvePayload();
    if (m_lean) {
        applyLeanPayload(p);
    } else {
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
        if (m_theme) {
            putLabel(t, lp.leftPadding, fresh, p.leftPadding, m_applied.leftPadding);
            putLabel(t, lp.rightPadding, fresh, p.rightPadding, m_applied.rightPadding);
            putLabel(t, lp.topPadding, fresh, p.topPadding, m_applied.topPadding);
            putLabel(t, lp.bottomPadding, fresh, p.bottomPadding, m_applied.bottomPadding);
        }
        putLabel(t, lp.wrapMode, fresh, p.wrapMode, m_applied.wrapMode);
        putLabel(t, lp.elide, fresh, p.elide, m_applied.elide);
        putLabel(t, lp.horizontalAlignment, fresh, p.horizontalAlignment, m_applied.horizontalAlignment);
        m_payloadApplied = true;
    }

    // display:none never writes `visible` (imperative writes sever author bindings, e.g. the
    // <Show> guard); the layout skips it by style, painting dies via opacity 0.
    const QString display = m_style.value(QStringLiteral("display")).toString();
    const QString visibility = m_style.value(QStringLiteral("visibility")).toString();
    if (display == QLatin1String("none") || visibility == QLatin1String("hidden"))
        setOpacity(0);
    else
        setOpacity(m_style.contains(QStringLiteral("opacity")) ? m_style.value(QStringLiteral("opacity")).toReal()
                                                               : 1.0);

    if (!m_lean)
        mirrorImplicit();
}

void CssText::applyShadow()
{
    const QVariantMap shadow = shadowSpec();
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
    // The box the text occupies: ours when a width/height is assigned (by the layout engine), else the text's
    // implicit size so a standalone label never collapses. The shadow and the background take it from the outer
    // item's box (contract 9), never from the label.
    QSizeF box;
    if (m_label) {
        m_label->setX(0);
        m_label->setY(0);
        // Fill an assigned size so wrap/elide/valign apply.
        const qreal w = width();
        m_label->setWidth(w > 0.5 ? w : m_label->implicitWidth());
        const qreal h = height();
        m_label->setHeight(h > 0.5 ? h : m_label->implicitHeight());
        box = m_label->size();
    } else if (m_lean) {
        box = m_box;
    } else {
        return;
    }

    if (m_shadow) {
        m_shadow->setX(0);
        m_shadow->setY(0);
        m_shadow->setSize(box);
    }
    if (m_bg) {
        m_bg->setX(0);
        m_bg->setY(0);
        m_bg->setSize(box);
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

    // Plain text draws itself (lean); styled text composes the REAL QtQuick Text. The text-shadow
    // MultiEffect is composed LAZILY in applyShadow(): a MultiEffect drags in ShaderEffectSources + blur
    // items even while invisible, and most labels never declare text-shadow.
    if (wantsLean())
        enterLean();
    else
        composeLabel();

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
            const CssTextPayload before = m_applied;
            applyToText();
            applyShadow();
            // The same family name may now resolve to the registered font: lay out again even if the payload
            // did not change (applyToText re-laid out when it did).
            if (m_lean && m_leanMounted && sameLayoutInputs(before, m_applied)) {
                leanRelayout();
                leanResize();
                leanPublishImplicit();
                layoutChild();
            }
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
    if (m_lean) {
        // A move needs nothing (the item's transform node moves the glyphs); a resize is the label's.
        if (newGeometry.size() == oldGeometry.size() || !m_leanMounted)
            return;
        leanResize();
        leanPublishImplicit();
    }
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

QVariantMap CssText::shadowSpec() const
{
    // QML: _dropShadow = (cssTheme.loaded && style["text-shadow"]) ? parseBoxShadow(...) : ({})
    QVariantMap shadow;
    const QString ts = m_style.value(QStringLiteral("text-shadow")).toString();
    if (m_theme && m_theme->isLoaded() && !ts.isEmpty())
        shadow = m_theme->parseBoxShadow(ts);
    return shadow;
}

// --- path selection (contract 1/4: QQuickText fallback for styledText and text-shadow) --------------

bool CssText::wantsLean() const
{
    return Detail::leanTextEnabled() && !m_styledText && !shadowSpec().contains(QStringLiteral("color"));
}

void CssText::syncPath()
{
    // Before componentComplete picks the first path there is nothing to switch from.
    if (!m_lean && !m_label)
        return;
    const bool lean = wantsLean();
    if (lean == m_lean)
        return;
    if (lean)
        enterLean();
    else
        enterComposed();
}

void CssText::enterLean()
{
    // Drop the composed path: the MultiEffect first, its source detached while the label still lives.
    if (m_shadow) {
        m_shadow->setProperty("source", QVariant::fromValue<QQuickItem *>(nullptr));
        delete m_shadow.data();
    }
    if (m_label) {
        disconnect(m_label, nullptr, this, nullptr);
        delete m_label.data();
    }
    m_payloadApplied = false;

    m_lean = true;
    m_leanMounted = false; // a fresh virtual label: its first layout is a mount
    setFlag(ItemHasContents, true);
    applyToText();
    layoutChild();
}

void CssText::enterComposed()
{
    // Drop the lean path: its layouts, and its paint node (removed at the next sync once the item no longer has
    // contents — update() first, it only marks items that have them).
    m_lean = false;
    m_leanMounted = false;
    m_nodeDirty = false;
    if (flags() & ItemHasContents) {
        update();
        setFlag(ItemHasContents, false);
    }
    composeLabel();
    layoutChild();
    applyShadow();
}

void CssText::composeLabel()
{
    QQmlEngine *eng = qmlEngine(this);
    if (!eng)
        return;
    // The REAL QtQuick Text, via the Qt type-system (we forward everything onto it).
    // Configured BETWEEN beginCreate and completeCreate: a QQuickText that is not yet complete
    // defers its text layout, so the ~20 property writes of the first applyToText() cost ONE
    // layout (and one font match) at completion instead of one per write.
    QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("csstext-bf5e2923"), "import QtQuick\nText {}");
    QObject *o = comp->beginCreate(qmlContext(this));
    if (!o) {
        qWarning("CssText: failed to compose Text: %s", qPrintable(comp->errorString()));
        return;
    }
    QQuickItem *label = qobject_cast<QQuickItem *>(o);
    if (!label) {
        comp->completeCreate();
        o->deleteLater();
        return;
    }
    label->setParent(this); // C++ ownership: beginCreate() means C++ owns it
    label->setParentItem(this);
    m_label = label;
    m_payloadApplied = false; // a fresh label: the first apply writes the whole payload
    // Constant for every label, so written once here instead of in the payload.
    labelProps(label).verticalAlignment.write(label, static_cast<int>(Qt::AlignVCenter));
    ++Detail::g_textLabelWrites;
    applyToText();
    comp->completeCreate();

    // The composed Text's implicit size drives ours; re-flow the parent on change.
    connect(m_label, &QQuickItem::implicitWidthChanged, this, [this]() {
        mirrorImplicit();
        layoutChild();
    });
    connect(m_label, &QQuickItem::implicitHeightChanged, this, [this]() {
        mirrorImplicit();
        layoutChild();
    });
    mirrorImplicit();
}

// --- lean path ------------------------------------------------------------------------------------------
//
// The lean CssText runs a VIRTUAL QQuickText label: the same layouts at the same moments as the composed one
// (mount layout, then QQuickText::geometryChange deciding on each resize), so what it draws is what the composed
// label would draw (tests/test_text.cpp leanTextRendersLikeQQuickText).

void CssText::applyLeanPayload(const CssTextPayload &p)
{
    if (!m_leanMounted) {
        // QQuickText::componentComplete: the label is laid out before it has a size and follows its implicit
        // one; CssText::layoutChild then gives it ours (assigning the implicit size it has is no change).
        m_applied = p;
        m_payloadApplied = true;
        leanRelayout();
        m_leanMounted = true;
        m_box = QSizeF(m_metrics.implicitWidth, m_metrics.implicitHeight);
        leanResize();
        leanPublishImplicit();
        return;
    }
    if (sameLayoutInputs(p, m_applied)) {
        if (p.color != m_applied.color) { // QQuickText::setColor: a repaint
            m_applied.color = p.color;
            markNodeDirty();
        }
        return;
    }
    // A QQuickText setter: updateLayout at the label's current size.
    m_applied = p;
    leanRelayout();
    leanResize();
    leanPublishImplicit();
}

void CssText::leanRelayout()
{
    m_mirrored = labelMirrored();
    m_layoutMount = !m_leanMounted;
    m_layoutBox = m_box;
    ++Detail::g_textGuiLayouts;
    // Scalars only; the render thread lays out again from this value state in updatePaintNode.
    m_metrics = m_layoutMount ? CssTextLayout::mountMetrics(m_applied, m_mirrored)
                              : CssTextLayout::metrics(m_applied, m_box.width(), m_box.height(), m_mirrored);
    markNodeDirty();
}

void CssText::leanResize()
{
    // CssText::layoutChild gives the label our width, then our height — each one a QQuickText::geometryChange;
    // an unassigned dimension is the label's implicit one, which a relayout may change (bounded: a size that
    // keeps moving is QQuickText's own behaviour, and three rounds cover a wrap that changes both).
    auto resizeTo = [this](QSizeF to) {
        if (to == m_box)
            return;
        const QSizeF from = m_box;
        m_box = to;
        switch (CssTextLayout::geometryEffect(m_metrics, m_applied, from, to)) {
        case CssTextGeometryEffect::Relayout:
            leanRelayout();
            break;
        case CssTextGeometryEffect::Repaint:
            markNodeDirty();
            break;
        case CssTextGeometryEffect::None:
            break;
        }
    };
    for (int round = 0; round < 3; ++round) {
        const QSizeF before = m_box;
        resizeTo(QSizeF(width() > 0.5 ? width() : m_metrics.implicitWidth, m_box.height()));
        resizeTo(QSizeF(m_box.width(), height() > 0.5 ? height() : m_metrics.implicitHeight));
        if (m_box == before)
            break;
    }
}

void CssText::leanPublishImplicit()
{
    // We are the layout participant; our implicit size is the label's.
    setImplicitSize(m_metrics.implicitWidth, m_metrics.implicitHeight);
    layoutChild();
}

void CssText::markNodeDirty()
{
    m_nodeDirty = true;
    update();
}

bool CssText::labelMirrored() const
{
    // A composed label inherits our children's mirroring (QQuickItemPrivate::setImplicitLayoutMirror): its
    // effective LayoutMirroring is our inheritedLayoutMirror.
    return QQuickItemPrivate::get(this)->inheritedLayoutMirror;
}

QSGNode *CssText::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    // QQuickText::updatePaintNode (qquicktext.cpp 2601-2677) for a plain-text label.
    if (!m_lean || !leanDrawable() || !window()) {
        delete oldNode;
        m_nodeDirty = false;
        return nullptr;
    }
    if (oldNode && !m_nodeDirty)
        return oldNode;

    auto *node = static_cast<QSGTextNode *>(oldNode);
    if (!node) {
        node = window()->createTextNode();
        if (!node)
            return nullptr;
    }

    // Lay out here, from value state (the render thread never keeps a GUI-shaped layout around).
    const CssTextPayload payload = m_applied;
    CssTextBuiltLayout built = m_layoutMount
        ? CssTextLayout::buildMountLayout(payload, m_mirrored)
        : CssTextLayout::buildLayout(payload, m_layoutBox.width(), m_layoutBox.height(), m_mirrored);
    ++Detail::g_textRenderLayouts;
    std::unique_ptr<QTextLayout> layout = std::move(built.layout);
    std::unique_ptr<QTextLayout> elideLayout = std::move(built.elideLayout);

    const QSGTextNode::RenderType renderType = windowRenderType();
    node->setFiltering(smooth() ? QSGTexture::Linear : QSGTexture::Nearest);
    node->setTextStyle(QSGTextNode::Normal);
    node->setRenderType(renderType);
    node->setRenderTypeQuality(-1); // QQuickText::DefaultRenderTypeQuality
    node->clear();
    node->setMatrix(QMatrix4x4());
    node->setColor(m_applied.color);
    node->setViewport(QRectF());

    // The origin at the label's CURRENT size, from the scalars of its last layout.
    const QPointF origin = CssTextLayout::paintOrigin(m_metrics, m_applied, m_box.width(), m_box.height());
    if (m_metrics.unelidedLineCount > 0 && layout)
        node->addTextLayout(origin, layout.get(), -1, -1, 0, m_metrics.unelidedLineCount);
    if (m_metrics.elided && elideLayout)
        node->addTextLayout(origin, elideLayout.get());
    // The glyph nodes hold their own glyph runs: the layouts die here (contract 6/7).
    layout.reset();
    elideLayout.reset();

    m_unscalableGlyphs = renderType == QSGTextNode::NativeRendering;
    m_nodeDirty = false;
    ++Detail::g_textNodeBuilds;
    return node;
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
    if (!m_lean || !m_leanMounted)
        return;
    switch (change) {
    case ItemSceneChange:
        // A new window has a new scene graph: the node is built again there.
        if (data.window)
            markNodeDirty();
        break;
    case ItemDevicePixelRatioHasChanged:
        // QQuickText::itemChange: native glyphs are laid out for one pixel grid — lay out again.
        if (m_unscalableGlyphs) {
            leanRelayout();
            leanResize();
            leanPublishImplicit();
            layoutChild();
        }
        break;
    case ItemParentHasChanged:
        // The inherited LayoutMirroring may differ under the new parent.
        if (labelMirrored() != m_mirrored) {
            leanRelayout();
            leanResize();
            leanPublishImplicit();
            layoutChild();
        }
        break;
    default:
        break;
    }
}
