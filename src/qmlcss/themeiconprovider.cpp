#include "qmlcss/themeiconprovider.h"

#include <QIcon>

namespace QmlCss {

ThemeIconProvider::ThemeIconProvider()
    : QQuickImageProvider(QQuickImageProvider::Pixmap)
{
}

QPixmap ThemeIconProvider::requestPixmap(const QString &id, QSize *size, const QSize &requestedSize)
{
    // id = "<icon-name>|<hexcolor>" (CssIcon::resolveIconSource); the colour is CssIcon's own
    // MultiEffect tint key (see themeiconprovider.h) — split it off and ignore it here.
    const QString name = id.section(QLatin1Char('|'), 0, 0);

    // `requestedSize` already carries the caller's devicePixelRatio: Qt Quick's own pixmap-loading
    // pipeline (QQuickPixmap, used by every "image://" source — this is standard behaviour, not
    // specific to this provider) multiplies a bound `sourceSize` by the window's
    // effectiveDevicePixelRatio() before calling requestPixmap, exactly like the "@2x" convention
    // for file-based images. A SYMBOLIC icon's underlying asset is an SVG (QIcon::fromTheme uses
    // Qt's SVG icon engine for one), so asking for the scaled size rasterizes it sharp at that
    // size directly — no upscale-and-blur.
    const QSize wanted = requestedSize.isValid() && !requestedSize.isEmpty() ? requestedSize : QSize(16, 16);
    QPixmap pm = QIcon::fromTheme(name).pixmap(wanted);
    if (pm.isNull()) {
        // Never return a null pixmap — an Image.source pointed at a provider that returns one
        // reports Image.Error and renders nothing at all; a transparent placeholder at least
        // keeps the layout's reserved space intact (and is visibly "a blank icon", not a crash).
        pm = QPixmap(wanted);
        pm.fill(Qt::transparent);
    }
    if (size)
        *size = pm.size();
    return pm;
}

} // namespace QmlCss
