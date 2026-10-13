#pragma once

// Single consumer entry point: pulls in every CSS primitive and registers them all under
// `import qmlcss 1.0` with one call. Classic registration — no qmltyperegistrar machinery.

#include "contrast.h"
#include "cssdropshadow.h"
#include "cssfill.h"
#include "cssfilllayer.h"
#include "csshr.h"
#include "cssicon.h"
#include "cssimage.h"
#include "cssincubator.h"
#include "cssrepeater.h"
#include "cssitem.h"
#include "csskeyframes.h"
#include "csslayout.h"
#include "cssrect.h"
#include "csstext.h"
#include "csstheme.h"
#include "themeiconprovider.h"

#include <QQmlEngine>

namespace QmlCss {

// Process-global; call once before the first `import qmlcss` is resolved.
inline void registerTypes()
{
    qmlRegisterType<CssRect>("qmlcss", 1, 0, "CssRect");
    qmlRegisterType<CssHr>("qmlcss", 1, 0, "CssHr");
    qmlRegisterType<CssImage>("qmlcss", 1, 0, "CssImage");
    qmlRegisterType<CssFill>("qmlcss", 1, 0, "CssFill");
    qmlRegisterType<CssText>("qmlcss", 1, 0, "CssText");
    qmlRegisterType<CssDropShadow>("qmlcss", 1, 0, "CssDropShadow");
    qmlRegisterType<CssKeyframes>("qmlcss", 1, 0, "CssKeyframes");
    qmlRegisterType<CssIcon>("qmlcss", 1, 0, "CssIcon");
    qmlRegisterType<CssFillLayer>("qmlcss", 1, 0, "CssFillLayer");
    qmlRegisterType<CssItem>("qmlcss", 1, 0, "CssItem");
    qmlRegisterType<CssIncubator>("qmlcss", 1, 0, "CssIncubator");
    qmlRegisterType<CssRepeater>("qmlcss", 1, 0, "CssRepeater");
    qmlRegisterSingletonType<Contrast>("qmlcss", 1, 0, "Contrast",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new Contrast(); });
}

// Per-ENGINE (an image provider belongs to one QQmlEngine, unlike the process-global type
// registrations above) — call once per engine, after construction. Completes CssIcon's
// `image://themeicon/<name>|<hexcolor>` resolution; see themeiconprovider.h for what it does and
// does not do (no desktop-specific icon-theme-NAME selection — that's the caller's job).
inline void registerImageProviders(QQmlEngine *engine)
{
    engine->addImageProvider(QStringLiteral("themeicon"), new ThemeIconProvider());
}

} // namespace QmlCss
