#pragma once

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QQuickItem>
#include <QVariant>
#include <QVariantMap>

#include "qmlcss/cssbox.h"
#include "qmlcss/csstextlayout.h" // CssTextPayload, CssTextMetrics

namespace QmlCss {

class CssTheme;
class CssLayoutEngine;

// Native CSS-styled label. Two paths, chosen per label (step 4c):
//   LEAN (plain text, no text-shadow — almost every label): the CssText draws its own QSGTextNode. Its text is
//   laid out by CssTextLayout, a literal port of the QQuickText code paths a label exercises (oracle-tested in
//   tests/test_text.cpp), so the metrics and the glyph positions are QQuickText's. Only scalar metrics are kept
//   between layouts; the render thread builds a QTextLayout from that value state (CssTextLayout::buildLayout)
//   in updatePaintNode, adds it to the QSGTextNode, and drops it immediately after.
//   COMPOSED (styledText, or text-shadow): the description below — a real QQuickText, as before.
// Switching between the two destroys the other path's resources (label, MultiEffect, paint node).
// SQ_LEAN_TEXT=0 keeps every label composed.
//
// The COMPOSED path, translated 1:1 from qml/qmlcss/CssText.qml — BY COMPOSITION, not
// reimplementation. Unlike the QML (whose ROOT *is* a Text), this C++ CssText is a QQuickItem that
// composes a REAL QtQuick `Text` via the Qt type-system and FORWARDS the content plus every
// CSS-driven property onto it. It paints nothing itself; the composed Text does.
//
// Carries the standard CSS signature (cssId/cssAlternateId/cssClass/cssState/cssPrimitive/cssPart/
// style); resolved rules are pushed back into `style` by cssTheme.loadCss(this), and the forwarded
// Text props key off it.
//
// CSS inheritance mirrors CssRect/CssFill: inheritable text props come from this label's own style,
// else from the CSS-inheriting ancestor. The chain walks `parent` (a directly-styled parent whose
// inheritedColor is non-empty) then `parent.parent` (the containing box), reading the inherited*
// getters off whichever ancestor exposes them — so a bare label picks up the container's colour/font.
//
// Forwarded onto the composed Text:
//   text; color<-inheritedColor; font.family/pointSize/weight/letterSpacing/wordSpacing<-inherited*;
//   font.underline/strikeout<-text-decoration; font.capitalization<-text-transform; wrapMode<-white-
//   space; elide<-text-overflow; horizontalAlignment<-text-align; verticalAlignment; textFormat
//   PlainText; lineHeight(Mode)<-line-height; padding<-padding; visible<-display:none;
//   opacity<-visibility/opacity. font.family re-resolves on cssTheme.fontRevision (async @font-face).
//
// text-shadow is composed as a REAL QtQuick.Effects MultiEffect drop-shadow (same idiom as
// the C++ CssDropShadow) sourcing the Text — mirrors the QML `layer.effect: CssDropShadow{}`
// pattern (CssDropShadow is now also a C++ type; CssText inlines the equivalent directly).
//
// Layout participant: implicitWidth/Height mirror the composed Text's; notifyParentLayout on
// implicit/visible/style change (like CssHr).
//
// Equivalent QML this C++ composes:
//   import QtQuick
//   Item {                                    // <- this CssText
//       Text {                                // <- m_text, fills the item
//           text: root.text
//           color: ...; font: ...; wrapMode: ...; elide: ...; textFormat: Text.PlainText
//           horizontalAlignment: ...; verticalAlignment: Text.AlignVCenter; ...padding
//           visible: !(display none); opacity: visibility/opacity
//       }
//       MultiEffect { source: text; shadow* ... }   // <- m_shadow, only when text-shadow present
//   }
class CssText : public CssBox {
    Q_OBJECT
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)

    // The label content (QML: property alias text: root.text — the Text's own text).
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    // Opt-in Text.StyledText for the tiny HTML subset (<u>/<b>/<i>) — desktop affordances like
    // menu mnemonics. Default stays PlainText: transpiled JSX text is literal.
    Q_PROPERTY(bool styledText READ styledText WRITE setStyledText NOTIFY styledTextChanged)

    // Fallbacks when the theme leaves the label unstyled (the QML default* props).
    Q_PROPERTY(QColor defaultColor READ defaultColor WRITE setDefaultColor NOTIFY defaultColorChanged)
    Q_PROPERTY(QString defaultFontFamily READ defaultFontFamily WRITE setDefaultFontFamily NOTIFY defaultFontFamilyChanged)
    Q_PROPERTY(qreal defaultFontSize READ defaultFontSize WRITE setDefaultFontSize NOTIFY defaultFontSizeChanged)

    // Inherited text properties (CSS inheritance) — own style wins, else the CSS-inheriting ancestor.
    Q_PROPERTY(QString inheritedColor READ inheritedColor NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontFamily READ inheritedFontFamily NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontSize READ inheritedFontSize NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedFontWeight READ inheritedFontWeight NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedLineHeight READ inheritedLineHeight NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedLetterSpacing READ inheritedLetterSpacing NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedTextTransform READ inheritedTextTransform NOTIFY inheritedChanged)
    Q_PROPERTY(QString inheritedTextAlign READ inheritedTextAlign NOTIFY inheritedChanged)

public:
    explicit CssText(QQuickItem *parent = nullptr);

    QVariantMap style() const { return m_style; }
    void setStyle(const QVariantMap &v);

    QString text() const { return m_text; }
    void setText(const QString &v);

    bool styledText() const { return m_styledText; }
    void setStyledText(bool v);

    QColor defaultColor() const { return m_defaultColor; }
    void setDefaultColor(const QColor &v);
    QString defaultFontFamily() const { return m_defaultFontFamily; }
    void setDefaultFontFamily(const QString &v);
    qreal defaultFontSize() const { return m_defaultFontSize; }
    void setDefaultFontSize(qreal v);

    QString inheritedColor() const;
    QString inheritedFontFamily() const;
    QString inheritedFontSize() const;
    QString inheritedFontWeight() const;
    QString inheritedLineHeight() const;
    QString inheritedLetterSpacing() const;
    QString inheritedTextTransform() const;
    QString inheritedTextAlign() const;
    bool takesPartInInheritance() const override { return true; }

signals:
    void styleChanged();
    void textChanged();
    void styledTextChanged();
    void defaultColorChanged();
    void defaultFontFamilyChanged();
    void defaultFontSizeChanged();
    void inheritedChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;

private slots:
    // An ancestor's inherited text props changed (CSS inheritance): re-apply + re-propagate.
    void onAncestorInheritedChanged();

private:
    // Resolve every CSS-driven value (the QML `readonly property`/binding set) and push it onto the
    // composed Text — the C++ equivalent of all of CssText.qml's Text bindings — or, lean, lay it out.
    void applyToText();
    // Build the payload from the current inputs (`inh` = inheritedText(), walked once by the caller).
    CssTextPayload resolvePayload(const CssInheritedText &inh) const;
    // 5d: whether m_applied is still what resolvePayload would build from the current inputs (`inh`).
    bool payloadCurrent(const CssInheritedText &inh) const;
    // The resolved text-shadow (empty when none); a shadow keeps the label composed.
    QVariantMap shadowSpec() const;

    // --- path selection ---
    bool wantsLean() const;
    // Switch to the path the current styledText/text-shadow call for (no-op when already on it).
    void syncPath();
    void enterLean();
    void enterComposed();
    // Compose the REAL QtQuick Text (configured between beginCreate and completeCreate).
    void composeLabel();

    // --- lean path: a virtual QQuickText label (QQuickText's own state machine, ported) ---
    void applyLeanPayload(const CssTextPayload &p);
    // QQuickTextPrivate::updateLayout at the label's current size (at mount: before it has one).
    void leanRelayout();
    // Give the label the size CssText::layoutChild would (ours, else its implicit one), width then height,
    // re-laying out or re-placing it as QQuickText::geometryChange would.
    void leanResize();
    void leanPublishImplicit();
    // The glyph node must be rebuilt: update() flags it for the next sync.
    void markNodeDirty();
    bool leanDrawable() const { return !m_metrics.empty && m_metrics.layedOutTextRect.width() > 0; }
    // The LayoutMirroring a composed label would inherit from us.
    bool labelMirrored() const;

    // Recompute + push the text-shadow drop-shadow onto the composed MultiEffect.
    void applyShadow();
    // Paint `background-color` (+ border-radius) behind the Text — a lazily-composed Shape
    // underlay (the web paints backgrounds on any element, text included).
    void applyBackground();
    // Keep the composed Text (and shadow) sized to us; when unmanaged (no assigned size), let the
    // Text use its own implicit size so it never collapses.
    void layoutChild();
    // Mirror the composed Text's implicit size onto us (we are the layout participant).
    void mirrorImplicit();
    // Re-style through the reverse-slot engine path when identity changes (QML onCss*Changed).
    void maybeLoadCss() override;
    void applyResolvedStyle(const QVariantMap &style) override { setStyle(style); }
    QQuickItem *inheritanceParent() const override { return cssParentItem(); }

protected:
    // Protected so C++ widget subclasses (the widgets-to-cpp port) can chain their own
    // item-change handling through the scene-attach resolve heal.
    void itemChange(ItemChange change, const ItemChangeData &data) override;

private:
    // The CSS-inheriting ancestor (QML `_cssParent`): a directly-styled parent, else parent.parent.
    QQuickItem *cssParentItem() const;

    QString m_text;
    bool m_styledText = false;

    QColor m_defaultColor = QColor(Qt::black);
    QString m_defaultFontFamily = QStringLiteral("Sans Serif");
    qreal m_defaultFontSize = 11;

    QPointer<CssLayoutEngine> m_layout;
    QPointer<QQuickItem> m_label; // the REAL QtQuick Text, via the type-system
    CssTextPayload m_applied; // what m_label was last given (valid while m_payloadApplied)
    bool m_payloadApplied = false; // false until the first apply onto the current m_label
    // 5d: the inputs m_applied was last resolved from. A resolve is a pure function of m_style, the inherited
    // values, the defaults, styledText, text, the theme and its fontRevision (resolveFontFamily's answer can only
    // change when a font registers); every apply leaves m_applied equal to the payload it resolved, and setText
    // keeps m_applied.text current. So equal inputs reuse m_applied instead of building it again. m_style is
    // compared by identity (any setStyle that changes it replaces the map), the inherited values by value; the
    // defaults/styledText/theme setters invalidate instead of being stored.
    QVariantMap m_resolvedStyle;
    CssInheritedText m_resolvedInherited;
    int m_resolvedFontRevision = 0;
    bool m_resolvedValid = false;
    QPointer<QQuickItem> m_shadow; // the REAL QtQuick.Effects MultiEffect drop-shadow (text-shadow)
    QPointer<QQuickItem> m_bg; // background underlay Shape, composed on first background-color

    // Lean path state (m_applied is the lean payload too). Scalars only — the render thread builds its own
    // QTextLayout(s) from this value state in updatePaintNode and drops them once added to the QSGTextNode.
    bool m_lean = false;
    bool m_leanMounted = false; // the virtual label has had its mount layout
    bool m_layoutMount = false; // m_metrics comes from the mount layout (no size yet), not from m_layoutBox
    bool m_mirrored = false; // the LayoutMirroring m_metrics was laid out with
    bool m_nodeDirty = false; // the glyph node must be (re)built at the next sync
    bool m_unscalableGlyphs = false; // native rendering: a DPR change needs a relayout (QQuickText)
    CssTextMetrics m_metrics;
    QSizeF m_box; // the virtual label's size
    QSizeF m_layoutBox; // the size m_metrics was laid out at
};

} // namespace QmlCss
