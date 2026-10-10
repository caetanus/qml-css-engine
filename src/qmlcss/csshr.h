#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QVariant>
#include <QVariantMap>

#include "qmlcss/cssbox.h"

namespace QmlCss {

class CssTheme;
class CssLayoutEngine;

// Native <hr>, translated 1:1 from qml/qmlcss/CssHr.qml — BY COMPOSITION, not reimplementation.
// A CSS-addressable Qt Quick item carrying the standard CSS signature (cssId/cssAlternateId/
// cssClass/cssState/cssPrimitive/cssPart/style) that paints only its top border. It does NOT
// paint anything itself: it instantiates a REAL QtQuick Shape via the Qt type-system and
// manages that child's geometry (the C++ equivalent of the QML anchors top/left/right). The
// Shape (a ShapePath filling a PathRectangle) is what draws the border line — Rectangle is
// avoided per the owner's rule (it is capped for several effects); Shape is the general path.
//
// Equivalent QML this C++ composes:
//   import QtQuick
//   import QtQuick.Shapes
//   Item {                              // <- this CssHr
//       Shape {                         // <- m_shape, full-width, pinned to the top
//           width: root.width; height: Math.max(1, root.line.width)
//           visible: root.line.visible
//           ShapePath {
//               strokeWidth: 0; strokeColor: "transparent"
//               fillColor: root.line.color
//               PathRectangle { x: 0; y: 0; width: shape.width; height: shape.height }
//           }
//       }
//   }
class CssHr : public CssBox {
    Q_OBJECT
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)
    Q_PROPERTY(QVariantMap line READ line NOTIFY lineChanged)

public:
    explicit CssHr(QQuickItem *parent = nullptr);

    QVariantMap style() const { return m_style; }
    void setStyle(const QVariantMap &v);

    QVariantMap line() const { return m_line; }

signals:
    void styleChanged();
    void lineChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    // Re-run cssTheme.borderSide(style, "top", 1, "#ededed"), push into the Rectangle child,
    // refresh implicitHeight, and emit lineChanged. Faithful to the QML `line` binding.
    void recomputeLine();
    // top/left/right anchor equivalent: keep the child pinned full-width at the top.
    void layoutChild();
    // Re-style through the reverse-slot engine path when identity changes (QML onCss*Changed).
    void maybeLoadCss() override;

    QVariantMap m_line;

    QPointer<CssLayoutEngine> m_layout;
    QPointer<QQuickItem> m_shape; // the REAL QtQuick Shape (ShapePath + PathRectangle), via the type-system
};

} // namespace QmlCss
