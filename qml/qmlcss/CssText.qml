import QtQuick
import "qrc:/qmlcss" as QmlCss

// CssText IS a Text — a CSS-styled label. It carries the CssQmlItem signature; the engine
// pushes the resolved rules into `style` (the reverse slot — reload- and cssClass-aware),
// and the label binds its colour / font / text-shadow to that pushed `style`. Used without
// a cssId it is just a plain Text. Override `color`/`font` to opt a specific label out.
Text {
    id: root

    // --- CssQmlItem signature (read off this object by CssTheme::loadCss) ---
    property string cssId: ""
    property var cssAlternateId: []
    property var cssClass: []
    property string cssPrimitive: "text"
    property string cssPart: ""
    // Engine writes the resolved rules here; the bindings below key off it.
    property var style: ({})

    // Fallbacks when the theme leaves the label unstyled.
    property color defaultColor: "black"
    property string defaultFontFamily: "Sans Serif"
    property real defaultFontSize: 11

    // --- CSS inheritance (mirrors CssRect) ----------------------------------------------
    // Inheritable text props come from this label's own style, else from the CSS-inheriting
    // ancestor (the containing CssRect, at parent.parent). So a bare label with no class
    // picks up the container's colour/font — no need to repeat the class on the label.
    readonly property var _cssParent: (parent && parent.parent && parent.parent.inheritedColor !== undefined)
        ? parent.parent : null
    readonly property string inheritedColor: (style && style["color"]) ? style["color"] : (_cssParent ? _cssParent.inheritedColor : "")
    readonly property string inheritedFontFamily: (style && style["font-family"]) ? style["font-family"] : (_cssParent ? _cssParent.inheritedFontFamily : "")
    readonly property string inheritedFontSize: (style && style["font-size"]) ? style["font-size"] : (_cssParent ? _cssParent.inheritedFontSize : "")
    readonly property string inheritedFontWeight: (style && style["font-weight"]) ? style["font-weight"] : (_cssParent ? _cssParent.inheritedFontWeight : "")
    readonly property string inheritedLetterSpacing: (style && style["letter-spacing"]) ? style["letter-spacing"] : (_cssParent ? _cssParent.inheritedLetterSpacing : "")
    readonly property string inheritedTextTransform: (style && style["text-transform"]) ? style["text-transform"] : (_cssParent ? _cssParent.inheritedTextTransform : "")
    readonly property string inheritedTextAlign: (style && style["text-align"]) ? style["text-align"] : (_cssParent ? _cssParent.inheritedTextAlign : "")

    color: root.inheritedColor ? cssTheme.parseColor(root.inheritedColor) : root.defaultColor
    font.family: root.inheritedFontFamily ? root.inheritedFontFamily : root.defaultFontFamily
    // The engine resolves CSS font-size → points (px→pt, em/rem, pt/bare); no unit math here.
    font.pointSize: root.inheritedFontSize
        ? cssTheme.parseFontSize(root.inheritedFontSize, root.defaultFontSize) : root.defaultFontSize
    font.weight: root._fontWeight(root.inheritedFontWeight)
    font.letterSpacing: root.inheritedLetterSpacing ? cssTheme.parseLength(root.inheritedLetterSpacing, 0) : 0
    // CSS text-transform via QFont capitalization (no need to mutate the text string).
    font.capitalization: root._capitalization(root.inheritedTextTransform)
    verticalAlignment: Text.AlignVCenter

    function _fontWeight(v) {
        var s = String(v).trim().toLowerCase()
        if (s === "" || s === "normal" || s === "400") return Font.Normal
        if (s === "bold" || s === "700") return Font.Bold
        if (s === "bolder") return Font.ExtraBold
        if (s === "lighter") return Font.Light
        var n = parseInt(s)
        if (isNaN(n)) return Font.Normal
        if (n <= 100) return Font.Thin
        if (n <= 200) return Font.ExtraLight
        if (n <= 300) return Font.Light
        if (n <= 400) return Font.Normal
        if (n <= 500) return Font.Medium
        if (n <= 600) return Font.DemiBold
        if (n <= 700) return Font.Bold
        if (n <= 800) return Font.ExtraBold
        return Font.Black
    }
    function _capitalization(v) {
        var s = String(v).trim().toLowerCase()
        if (s === "uppercase") return Font.AllUppercase
        if (s === "lowercase") return Font.AllLowercase
        if (s === "capitalize") return Font.Capitalize
        return Font.MixedCase
    }

    // Layout participant: wrap when a container assigns a width, drop out of layout on
    // `display: none`, and report size changes up so the container re-flows. Geometry (x/y/
    // width) is assigned by the parent CssRect's relayout — this label carries no own size
    // bindings, only its implicit text size.
    wrapMode: Text.WordWrap
    visible: !(style && style["display"] === "none")
    horizontalAlignment: root.inheritedTextAlign === "center"
        ? Text.AlignHCenter
        : (root.inheritedTextAlign === "right" ? Text.AlignRight : Text.AlignLeft)

    function _notifyParent() {
        var holder = root.parent
        if (holder && holder.parent && holder.parent.requestRelayout)
            holder.parent.requestRelayout()
    }
    onImplicitWidthChanged: root._notifyParent()
    onImplicitHeightChanged: root._notifyParent()
    onStyleChanged: root._notifyParent()
    onVisibleChanged: root._notifyParent()

    // CSS `text-shadow` → drop-shadow layer.
    readonly property var _dropShadow: (cssTheme && cssTheme.loaded && style && style["text-shadow"])
        ? cssTheme.parseBoxShadow(style["text-shadow"]) : ({})
    layer.enabled: root._dropShadow.color !== undefined
    layer.effect: QmlCss.CssDropShadow { shadow: root._dropShadow }

    function hasCssIdentity() {
        return root.cssId.length > 0
            || root.cssPart.length > 0
            || (Array.isArray(root.cssClass) ? root.cssClass.length > 0 : String(root.cssClass).length > 0)
    }

    Component.onCompleted: {
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
    }

    onCssIdChanged: {
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
    }

    onCssClassChanged: {
        if (cssTheme && root.hasCssIdentity())
            cssTheme.loadCss(root)
    }
}
