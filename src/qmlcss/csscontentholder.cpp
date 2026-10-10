#include "qmlcss/csscontentholder.h"

namespace QmlCss {

void CssContentHolder::itemChange(ItemChange change, const ItemChangeData &data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemChildAddedChange)
        emit childAdded(data.item);
    else if (change == ItemChildRemovedChange)
        emit childRemoved(data.item);
}

} // namespace QmlCss
