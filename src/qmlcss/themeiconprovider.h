#pragma once

#include <QQuickImageProvider>

namespace QmlCss {

// Completes CssIcon's `image://themeicon/<name>|<hexcolor>` resolution (cssicon.cpp,
// resolveIconSource()): registered under the `themeicon` scheme, this looks the icon NAME up in
// the system icon theme (QIcon::fromTheme) and rasterizes it at the requested size. The trailing
// `|<hexcolor>` is NOT used for tinting here — CssIcon already recolours the loaded pixmap via a
// REAL QtQuick.Effects MultiEffect (`colorizationColor`), so re-tinting the source pixmap too
// would be redundant; the suffix only keeps Qt's per-request pixmap cache key unique across
// different colourings of the same icon name (so a light-mode and a dark-mode tint of the same
// icon aren't treated as cache hits of each other's STALE result).
//
// Theme NAME resolution is the CALLER's job, not this provider's: QIcon::fromTheme() uses
// whatever QIcon::themeName() currently is, which this generic engine does not set (it has no
// desktop-specific knowledge — see apps/solidterm/native/systemtheme.cpp for why a Qt default
// can't be trusted on this machine, and how to read the REAL desktop icon theme explicitly).
class ThemeIconProvider : public QQuickImageProvider {
public:
    ThemeIconProvider();

    QPixmap requestPixmap(const QString &id, QSize *size, const QSize &requestedSize) override;
};

} // namespace QmlCss
