#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

namespace QmlCss {

class CssTheme;

// The typed home of a CSS element's identity and resolved style. Every engine type that can be
// matched by selectors derives from it (CssRect, CssText, CssFill, CssHr, CssImage, CssItem), so the
// theme and the layout engine read identity/style through one qobject_cast + plain member access
// instead of QObject::property() by name. Members keep the names the subclasses already used and are
// protected: subclass code reads m_cssId/m_cssClass/m_style/m_theme exactly as before.
//
// Not every box is a selector ancestor or a layout participant (CssItem styles its parent; CssIcon is
// a style sink with no identity and does NOT derive from this) — keep those notions separate.
class CssBox : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString cssId READ cssId WRITE setCssId NOTIFY cssIdChanged)
    Q_PROPERTY(QVariant cssAlternateId READ cssAlternateId WRITE setCssAlternateId NOTIFY cssAlternateIdChanged)
    Q_PROPERTY(QVariant cssClass READ cssClass WRITE setCssClass NOTIFY cssClassChanged)
    Q_PROPERTY(QVariant cssState READ cssState WRITE setCssState NOTIFY cssStateChanged)
    Q_PROPERTY(QString cssPrimitive READ cssPrimitive WRITE setCssPrimitive NOTIFY cssPrimitiveChanged)
    Q_PROPERTY(QString cssPart READ cssPart WRITE setCssPart NOTIFY cssPartChanged)
    Q_PROPERTY(bool hasCssIdentity READ hasCssIdentity NOTIFY hasCssIdentityChanged)

public:
    QString cssId() const { return m_cssId; }
    void setCssId(const QString &v);
    QVariant cssAlternateId() const { return m_cssAlternateId; }
    void setCssAlternateId(const QVariant &v);
    QVariant cssClass() const { return m_cssClass; }
    void setCssClass(const QVariant &v);
    QVariant cssState() const { return m_cssState; }
    void setCssState(const QVariant &v);
    QString cssPrimitive() const { return m_cssPrimitive; }
    void setCssPrimitive(const QString &v);
    QString cssPart() const { return m_cssPart; }
    void setCssPart(const QString &v);
    bool hasCssIdentity() const;

    // Typed views, converted ONCE per write (the QML-facing values stay QVariant lists/strings).
    const QStringList &classList() const { return m_classList; }
    const QStringList &stateList() const { return m_stateList; }
    const QStringList &alternateIdList() const { return m_alternateIdList; }
    // The resolved style, read without QVariant boxing or a map copy.
    const QVariantMap &styleMap() const { return m_style; }
    // Push a newly-resolved style into the box. Each class forwards to its own (non-virtual,
    // differently-bodied) setStyle — this is the one virtual seam the theme calls through so it
    // never needs a per-class qobject_cast.
    virtual void applyResolvedStyle(const QVariantMap &style) = 0;

signals:
    void cssIdChanged();
    void cssAlternateIdChanged();
    void cssClassChanged();
    void cssStateChanged();
    void cssPrimitiveChanged();
    void cssPartChanged();
    void hasCssIdentityChanged();

protected:
    CssBox(const QString &defaultPrimitive, QQuickItem *parent);
    // Each type's "resolve if a theme is attached" hook — the creation-time path (componentComplete,
    // scene-join heal) and the fallback of identityChanged() before the box is live.
    virtual void maybeLoadCss() = 0;
    // The ONE place an identity change re-resolves: every identity setter calls it, and so do the
    // live states the cascade reads off the box (CssRect's engine hover, `:focus`). A completed box
    // with a theme tells the theme once (self + registered descendants, one layout batch); before
    // that — creation-time writes, no theme yet — it is the old single-shot maybeLoadCss().
    void identityChanged();

    QString m_cssId;
    QVariant m_cssAlternateId = QVariant::fromValue(QVariantList());
    QVariant m_cssClass = QVariant::fromValue(QVariantList());
    QVariant m_cssState = QVariant::fromValue(QVariantList());
    QString m_cssPrimitive;
    QString m_cssPart;
    QVariantMap m_style;
    QPointer<CssTheme> m_theme; // every class used QPointer (CHECK (a), 2026-10-10)
    // The last resolve ran without a window (truncated ancestors). Set by maybeLoadCss() and by
    // identityChanged(); CssRect/CssText/CssFill heal it with one re-resolve on scene join.
    bool m_scenelessResolve = false;

private:
    QStringList m_classList;
    QStringList m_stateList;
    QStringList m_alternateIdList;
};

} // namespace QmlCss
