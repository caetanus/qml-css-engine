#pragma once

#include <QString>
#include <QVariantMap>

// CSS custom properties: `--name: value` declarations and `var(--name, fallback)` references.
//
// Custom properties inherit along the selector-context chain (CssContextNode: the element's CSS ancestry,
// cssAncestor re-anchoring included). An element's environment is its parent's environment overlaid with its
// own `--*` declarations; the root environment comes from `:root` / `html` rules. References are substituted
// at computed-value time, before a resolved map leaves the resolver, so every downstream cache sees final
// values. A reference that cannot be resolved (unknown name, no fallback, or a cycle) makes its declaration
// invalid at computed-value time: the property is dropped, as if it had not been declared.
namespace QmlCss::CssVar {

inline bool isCustomProperty(const QString &name) { return name.startsWith(QLatin1String("--")); }
inline bool hasReference(const QString &value) { return value.contains(QLatin1String("var(")); }

// `value` with every var() replaced from `env` (custom-property name -> raw value; values may reference
// other custom properties). False when a reference cannot be resolved; `out` is then unspecified.
bool expand(const QString &value, const QVariantMap &env, QString &out);

// `base` overlaid with the custom properties declared in `style`, their own references resolved against the
// result (a reference resolves where the property is declared).
QVariantMap overlay(const QVariantMap &base, const QVariantMap &style);

// Substitute every reference in `style` from `env` (already holding `style`'s own custom properties), drop the
// declarations that fail, and drop the custom properties themselves (consumers never see them).
void substitute(QVariantMap &style, const QVariantMap &env);

} // namespace QmlCss::CssVar
