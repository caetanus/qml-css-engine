#include "qmlcss/csscontentholder.h"

#include "qmlcss/cssbox.h"

namespace QmlCss {

CssContentHolder::CssContentHolder(CssBox *owner)
    : QQuickItem(owner)
    , m_owner(owner)
{
}

void CssContentHolder::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemChildAddedChange) {
        if (m_owner)
            m_owner->contentChildAdded(data.item);
        emit childAdded(data.item);
    } else if (change == ItemChildRemovedChange) {
        if (m_owner)
            m_owner->contentChildRemoved(data.item);
        emit childRemoved(data.item);
    }
}

} // namespace QmlCss
