#pragma once

// Shared desktop-scroll composition for CssRect AND CssFill (owner directive 2026-07-06: "cssfill
// *deveria* ter scroll"). `overflow(-y): auto/scroll` turns the content area into a real Flickable
// with a mouse-wheel handler (discrete steps, no fling), a draggable Controls-free scrollbar, and
// focus-follows-scroll. Both container types host declared children in a plain `contentHolder`, so
// the composition is identical — kept here so there is exactly one copy.

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <algorithm>

#include "qmlcss/componentcache.h"

namespace QmlCss {

// Shared Controls-free desktop scrollbar for CSS containers and external Flickables.
// The minimum thumb size shortens travel: position and drag use the same range.
inline QQuickItem *composeScrollBar(QQuickItem *flick)
{
    if (!flick || !qmlEngine(flick))
        return nullptr;
    auto *comp = QmlCss::cachedComponent(qmlEngine(flick), QStringLiteral("css-scroll-bar"), R"(
import QtQuick
    Rectangle {
        id: sbTrack
        required property var flick
        objectName: "cssScrollTrack"
        readonly property real maxY: Math.max(0, flick.contentHeight - flick.height)
        z: 1000
        x: flick.width - width
        y: 0
        width: 12
        height: flick.height
        color: "transparent"
        visible: flick.contentHeight > flick.height + 1
        MouseArea {
            anchors.fill: parent
            onPressed: (m) => {
                if (m.y < sbThumb.y) flick.contentY = Math.max(0, flick.contentY - flick.height * 0.9)
                else if (m.y > sbThumb.y + sbThumb.height) flick.contentY = Math.min(sbTrack.maxY, flick.contentY + flick.height * 0.9)
            }
        }
        Rectangle {
            id: sbThumb
            objectName: "cssScrollThumb"
            x: 3
            width: 6
            radius: 3
            color: thumbArea.pressed ? "#99000000" : (thumbArea.containsMouse ? "#77000000" : "#55000000")
            height: Math.min(sbTrack.height, Math.max(24, flick.visibleArea.heightRatio * sbTrack.height))
            y: sbTrack.maxY > 0 ? Math.max(0, Math.min(1, flick.contentY / sbTrack.maxY)) * (sbTrack.height - height) : 0
            MouseArea {
                id: thumbArea
                anchors.fill: parent
                hoverEnabled: true
                preventStealing: true
                property real __grab: 0
                onPressed: (m) => __grab = m.y
                onPositionChanged: (m) => {
                    if (!pressed) return
                    var maxY = sbTrack.height - sbThumb.height
                    if (maxY <= 0) return
                    var ny = sbThumb.y + (m.y - __grab)
                    var frac = Math.max(0, Math.min(ny / maxY, 1))
                    flick.contentY = frac * sbTrack.maxY
                }
            }
        }
    }
)");
    auto *bar = qobject_cast<QQuickItem *>(comp->createWithInitialProperties(
        {{QStringLiteral("flick"), QVariant::fromValue(flick)}}, qmlContext(flick)));
    if (!bar) {
        qWarning("css scroll: failed to compose scrollbar: %s", qPrintable(comp->errorString()));
        return nullptr;
    }
    bar->setParent(flick);
    bar->setParentItem(flick);
    return bar;
}

// Compose the scroll Flickable, parent it under `owner`, and move `contentHolder` into its
// contentItem. Returns the Flickable (or nullptr on failure).
inline QQuickItem *composeScrollFlickable(QQuickItem *owner, QQuickItem *contentHolder)
{
    if (!owner || !contentHolder)
        return nullptr;
    QQmlEngine *eng = qmlEngine(owner);
    if (!eng)
        return nullptr;
    QQmlComponent *comp = QmlCss::cachedComponent(eng, QStringLiteral("css-scroll-flickable"),
        "import QtQuick\n"
        "Flickable {\n"
        "    id: flick\n"
        "    clip: true\n"
        "    boundsBehavior: Flickable.StopAtBounds\n"
        "    flickableDirection: Flickable.VerticalFlick\n"
        // Desktop scroll, not touch: the wheel moves in discrete steps with no kinetic momentum
        // (StopAtBounds). Trackpad pixelDelta maps 1:1; a mouse notch (120) moves ~3 lines. We drive
        // contentY directly and clamp, so there is no flick fling.
        "    property real __maxY: Math.max(0, contentHeight - height)\n"
        "    WheelHandler {\n"
        "        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad\n"
        "        onWheel: (ev) => {\n"
        "            var dy = ev.pixelDelta.y !== 0 ? ev.pixelDelta.y : (ev.angleDelta.y / 120) * 54\n"
        "            flick.contentY = Math.max(0, Math.min(flick.contentY - dy, flick.__maxY))\n"
        "        }\n"
        "    }\n"
        // Keyboard focus follows scroll (tab-focus study §6): bring a focused inner control into view.
        "    Connections {\n"
        "        target: flick.Window.window\n"
        "        function onActiveFocusItemChanged() {\n"
        "            var fi = flick.Window.window ? flick.Window.window.activeFocusItem : null\n"
        "            if (!fi) return\n"
        "            var p = fi, inside = false\n"
        "            while (p) { if (p === flick.contentItem) { inside = true; break } p = p.parent }\n"
        "            if (!inside) return\n"
        "            var pos = fi.mapToItem(flick.contentItem, 0, 0)\n"
        "            var top = pos.y, bottom = pos.y + fi.height\n"
        "            if (top < flick.contentY) flick.contentY = Math.max(0, top - 8)\n"
        "            else if (bottom > flick.contentY + flick.height) flick.contentY = Math.min(flick.__maxY, bottom - flick.height + 8)\n"
        "        }\n"
        "    }\n"
        "}");
    QObject *o = comp->create(qmlContext(owner));
    QQuickItem *flick = qobject_cast<QQuickItem *>(o);
    if (!flick) {
        if (o)
            o->deleteLater();
        qWarning("css scroll: failed to compose Flickable: %s", qPrintable(comp->errorString()));
        return nullptr;
    }
    flick->setParent(owner);
    flick->setParentItem(owner);
    QQuickItem *contentItem = flick->property("contentItem").value<QQuickItem *>();
    contentHolder->setParentItem(contentItem ? contentItem : flick);
    composeScrollBar(flick);
    return flick;
}

// Push the laid-out content extent into the Flickable's contentHeight. The extent [top, bottom] comes
// from the layout pass (it just placed every child), NOT from QQuickItem::childrenRect(): Qt's
// childrenRect tracker recomputes non-incrementally (QQuickContents calls calcHeight() without the
// changed item), i.e. it rescans ALL children on every child geometry change — clearing a scrolled
// list became O(N^3). `bottomPad` is the box's padding-bottom so the last child breathes at max scroll.
inline void syncScrollExtent(QQuickItem *flick, QQuickItem *contentHolder, qreal extentTop,
                             qreal extentBottom, qreal ownerHeight, qreal ownerWidth,
                             qreal bottomPad = 0.0)
{
    if (!flick || !contentHolder)
        return;
    const qreal pad = std::max({0.0, extentTop, bottomPad});
    const qreal contentH = std::max(ownerHeight, extentBottom + pad);
    contentHolder->setHeight(contentH);
    flick->setProperty("contentWidth", ownerWidth);
    flick->setProperty("contentHeight", contentH);
}

} // namespace QmlCss
