#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "qmlcss/cssbox.h"

namespace QmlCss {

class CssTheme;

// Non-visual CSS enabler, translated 1:1 from qml/qmlcss/CssItem.qml — BY COMPOSITION. Drop a
// CssItem into ANY QtQuick builtin to make it CSS-styled — the styling analogue of MouseArea: a
// non-visual child (visible:false, 0×0) that carries the CssQmlItem identity signature, registers
// itself with cssTheme.loadCss, and applies the resolved rules imperatively onto its parentItem().
//
// The parent's type is inferred from its properties:
//   text + font  → "text"  — applies color, font-family, font-size
//   radius + color → "rect" — applies background-color, border-radius, border-color/width
//   else          → "item"  — nothing applied (no mappable visual props)
//
// The engine re-pushes on theme reload and cssId/cssClass change (the reverse slot), so the
// imperative apply never goes stale.
//
// Example:
//   Text { text: "hi"; CssItem { cssId: "foo" } }
class CssItem : public CssBox {
    Q_OBJECT
    // Engine writes the resolved rules here; apply() pushes them onto the parent.
    Q_PROPERTY(QVariantMap style READ style WRITE setStyle NOTIFY styleChanged)

public:
    explicit CssItem(QQuickItem *parent = nullptr);

    QVariantMap style() const { return m_style; }
    void setStyle(const QVariantMap &v);

signals:
    void styleChanged();

protected:
    void componentComplete() override;

private:
    // Infer parent primitive type from its properties (QML _inferPrimitive()).
    QString inferPrimitive() const;
    // Push resolved style rules onto parentItem() (QML _apply()).
    void apply();
    // Re-style via the engine's reverse slot when identity changes (QML onCssId/ClassChanged).
    void maybeLoadCss() override;
};

} // namespace QmlCss
