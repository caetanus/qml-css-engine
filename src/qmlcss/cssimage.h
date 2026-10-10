#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include "qmlcss/cssbox.h"

namespace QmlCss {

class CssTheme;
class CssLayoutEngine;

// Native <img>, translated 1:1 from qml/qmlcss/CssImage.qml — BY COMPOSITION, not reimplementation.
// A CSS-addressable Qt Quick item that shows a source image. It does NOT paint anything itself:
// it instantiates the REAL QtQuick Image (via the Qt type-system) and, for rounded corners, the
// REAL QtQuick.Effects MultiEffect (the cross-backend shader/mask idiom) whose mask source is a
// REAL QtQuick Shape rounded-rect (Rectangle is avoided per the owner's rule). `object-fit` maps
// to Image.fillMode; a non-zero `border-radius` rounds the image through the MultiEffect mask
// (so `border-radius: 50%` yields a real circle). Without radius, the Image draws direct.
//
// Carries the standard CSS signature (cssId/cssAlternateId/cssClass/cssState/cssPrimitive/
// cssPart/style); resolved rules are pushed back into `style` by cssTheme.loadCss(this).
//
// Equivalent QML this C++ composes:
//   import QtQuick
//   import QtQuick.Effects
//   import QtQuick.Shapes
//   Item {                                        // <- this CssImage
//       Image {                                   // <- m_image
//           anchors.fill: parent
//           fillMode: root._fillMode              // object-fit
//           asynchronous: true; cache: true
//           visible: !root._rounded
//           layer.enabled: root._rounded
//       }
//       MultiEffect {                             // <- m_effect (only when rounded)
//           anchors.fill: img; source: img
//           visible: root._rounded
//           maskEnabled: true
//           maskSource: mask
//       }
//       Item {                                    // <- m_maskHolder (layered, hidden)
//           id: mask; anchors.fill: img; layer.enabled: true; visible: false
//           Shape {                               // <- m_mask, rounded-rect mask
//               anchors.fill: parent
//               ShapePath {
//                   strokeWidth: 0; fillColor: "white"
//                   PathRectangle { width: mask.width; height: mask.height; radius: root._radius }
//               }
//           }
//       }
//   }
class CssImage : public CssBox {
    Q_OBJECT
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)
    // The image URL (QML: property alias source: img.source).
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)

public:
    explicit CssImage(QQuickItem *parent = nullptr);

    QVariantMap style() const { return m_style; }
    void setStyle(const QVariantMap &v);

    QUrl source() const { return m_source; }
    void setSource(const QUrl &v);

signals:
    void styleChanged();
    void sourceChanged();

protected:
    void componentComplete() override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    // Recompute fillMode (object-fit), radius/rounded (border-radius) and push them onto the
    // composed Image / MultiEffect / mask Rectangle — the C++ equivalent of the QML bindings.
    void recompute();
    // anchors.fill equivalent: keep every composed child sized to us.
    void layoutChildren();
    // Re-style through the reverse-slot engine path when identity changes (QML onCss*Changed).
    void maybeLoadCss() override;

    QUrl m_source;

    QPointer<QQuickItem> m_image;  // REAL QtQuick Image
    QPointer<QQuickItem> m_effect; // REAL QtQuick.Effects MultiEffect (rounded-corner mask)
    QPointer<QQuickItem> m_mask;   // REAL QtQuick Shape (rounded-rect) used as the MultiEffect maskSource
};

} // namespace QmlCss
