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

// Compose the scroll Flickable, parent it under `owner`, and move `contentHolder` into its
// contentItem. Returns the Flickable (or nullptr on failure). Caller connects
// contentHolder->childrenRectChanged to its own sync slot and re-runs layout.
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
        // Controls-free desktop scrollbar on the VIEWPORT layer (parent: flick keeps it out of the
        // reparented content). Persistent while content overflows; the thumb is draggable and the
        // track pages toward a click. Thumb y is a one-way binding off visibleArea (no feedback loop).
        "    Rectangle {\n"
        "        id: sbTrack\n"
        "        parent: flick\n"
        "        x: flick.width - width\n"
        "        y: 0\n"
        "        width: 12\n"
        "        height: flick.height\n"
        "        color: \"transparent\"\n"
        "        visible: flick.contentHeight > flick.height + 1\n"
        "        MouseArea {\n"
        "            anchors.fill: parent\n"
        "            onPressed: (m) => {\n"
        "                if (m.y < sbThumb.y) flick.contentY = Math.max(0, flick.contentY - flick.height * 0.9)\n"
        "                else if (m.y > sbThumb.y + sbThumb.height) flick.contentY = Math.min(flick.__maxY, flick.contentY + flick.height * 0.9)\n"
        "            }\n"
        "        }\n"
        "        Rectangle {\n"
        "            id: sbThumb\n"
        "            x: 3\n"
        "            width: 6\n"
        "            radius: 3\n"
        "            color: thumbArea.pressed ? \"#99000000\" : (thumbArea.containsMouse ? \"#77000000\" : \"#55000000\")\n"
        "            height: Math.max(24, flick.visibleArea.heightRatio * sbTrack.height)\n"
        "            y: flick.visibleArea.yPosition * sbTrack.height\n"
        "            MouseArea {\n"
        "                id: thumbArea\n"
        "                anchors.fill: parent\n"
        "                hoverEnabled: true\n"
        "                preventStealing: true\n"
        "                property real __grab: 0\n"
        "                onPressed: (m) => __grab = m.y\n"
        "                onPositionChanged: (m) => {\n"
        "                    if (!pressed) return\n"
        "                    var maxY = sbTrack.height - sbThumb.height\n"
        "                    if (maxY <= 0) return\n"
        "                    var ny = sbThumb.y + (m.y - __grab)\n"
        "                    var frac = Math.max(0, Math.min(ny / maxY, 1))\n"
        "                    flick.contentY = frac * flick.__maxY\n"
        "                }\n"
        "            }\n"
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
    return flick;
}

// Push the laid-out content extent into the Flickable's contentHeight (with bottom breathing).
inline void syncScrollExtent(QQuickItem *flick, QQuickItem *contentHolder, qreal ownerHeight, qreal ownerWidth)
{
    if (!flick || !contentHolder)
        return;
    const QRectF r = contentHolder->childrenRect();
    const qreal pad = std::max<qreal>(0.0, r.y());
    const qreal contentH = std::max(ownerHeight, r.y() + r.height() + pad);
    contentHolder->setHeight(contentH);
    flick->setProperty("contentWidth", ownerWidth);
    flick->setProperty("contentHeight", contentH);
}

} // namespace QmlCss
