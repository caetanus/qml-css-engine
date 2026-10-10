#include "qmlcss/cssincubator.h"

#include <QDebug>
#include <QQmlContext>
#include <QTimer>
#include <QVariantAnimation>
#include <QQmlEngine>

namespace QmlCss {

CssIncubator::CssIncubator(QQuickItem *parent)
    : QQuickItem(parent)
{
    // Never a layout participant and never painted: a zero-size bookkeeping node.
    setVisible(false);
    setFlag(ItemHasContents, false);
}

CssIncubator::~CssIncubator()
{
    teardown();
}

void CssIncubator::Incubator::statusChanged(Status s)
{
    if (s == Ready)
        m_owner->ready();
    else if (s == Error) {
        for (const QQmlError &e : errors())
            qWarning().noquote() << "CssIncubator:" << e.toString();
    }
}

// The gap between beginCreate() and completeCreate() (Qt's own wording): parent the page HERE so its
// componentComplete() resolves CSS against its real ancestors, once — not parentless and healed later.
void CssIncubator::Incubator::setInitialState(QObject *o)
{
    m_owner->adopt(o);
}

void CssIncubator::adopt(QObject *o)
{
    o->setParent(this);
    auto *item = qobject_cast<QQuickItem *>(o);
    if (!item)
        return;
    // The Repeater trick: the item becomes a DIRECT child of our parent (the content
    // holder), so the layout engine treats it exactly like a declared child.
    item->setParentItem(parentItem() ? parentItem() : this);
    // ATOMIC reveal: the page is in the scene from here on, while finalize() may stream its bindings and
    // componentComplete() calls over several frames — hold it transparent. The root completes LAST
    // (QQmlObjectCreator::finalize pops the parser-status stack; the root was pushed first) and its CSS
    // resolve rewrites opacity, then Ready follows in the same call: ready() re-asserts the hold with no
    // frame in between (except when nested async incubators make Ready wait — QQmlIncubator waitingFor).
    item->setOpacity(0.0);
}

void CssIncubator::setActive(bool v)
{
    if (m_active == v)
        return;
    m_active = v;
    emit activeChanged();
    if (isComponentComplete())
        sync();
}

void CssIncubator::setSourceComponent(QQmlComponent *c)
{
    if (m_component == c)
        return;
    m_component = c;
    emit sourceComponentChanged();
    if (isComponentComplete())
        sync();
}

void CssIncubator::componentComplete()
{
    QQuickItem::componentComplete();
    sync();
}

void CssIncubator::sync()
{
    if (m_active && m_component) {
        if (m_item || m_incubator)
            return; // already incubating / mounted
        m_incubator = new Incubator(this);
        // Mirror Loader: instantiate in a CHILD of the component's creation context, so
        // bare names (the defining component's properties — __const_* tables, signals)
        // resolve through the enclosing scope chain exactly like a declared child's.
        QQmlContext *creation = m_component->creationContext();
        if (!creation)
            creation = qmlContext(this);
        auto *ctx = new QQmlContext(creation, this);
        // A fresh child context has no base URL of its own — relative asset paths
        // (Image sources) must keep resolving against the defining document.
        ctx->setBaseUrl(creation->baseUrl());
        m_component->create(*m_incubator, ctx);
        // Synchronous outcome (cached + trivial component, or error) resolves immediately
        // via statusChanged; nothing else to do here.
    } else {
        teardown();
    }
}

void CssIncubator::ready()
{
    QQuickItem *item = qobject_cast<QQuickItem *>(m_incubator ? m_incubator->object() : nullptr);
    delete m_incubator;
    m_incubator = nullptr;
    if (!item)
        return;
    if (!m_active) { // deactivated while incubating
        item->setParentItem(nullptr);
        item->deleteLater();
        return;
    }
    m_item = item;
    // The root's componentComplete() just resolved CSS and set the style's own opacity (default 1.0):
    // re-assert the hold (see adopt), then fade the page in once the layout settled (one turn later).
    item->setOpacity(0.0);
    QTimer::singleShot(60, item, [item] {
        auto *fade = new QVariantAnimation(item);
        fade->setStartValue(0.0);
        fade->setEndValue(1.0);
        fade->setDuration(120);
        fade->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(fade, &QVariantAnimation::valueChanged, item,
                         [item](const QVariant &v) { item->setOpacity(v.toReal()); });
        fade->start(QAbstractAnimation::DeleteWhenStopped);
    });
    emit itemChanged();
}

void CssIncubator::teardown()
{
    if (m_incubator) {
        m_incubator->clear(); // cancels a pending incubation
        delete m_incubator;
        m_incubator = nullptr;
    }
    if (m_item) {
        m_item->setParentItem(nullptr);
        m_item->deleteLater();
        m_item = nullptr;
        emit itemChanged();
    }
}

} // namespace QmlCss
