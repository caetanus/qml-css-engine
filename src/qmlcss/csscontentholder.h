#pragma once

#include <QQuickItem>

namespace QmlCss {

// The plain item that hosts a box's declared children (CssRect/CssFill `contentHolder`). It reports
// each child as it is added or removed, so the box handles THAT child — instead of reacting to the
// argument-less `childrenChanged` by rescanning every sibling (O(N) per change, O(N^2) per list build).
class CssContentHolder : public QQuickItem {
    Q_OBJECT

public:
    using QQuickItem::QQuickItem;

signals:
    void childAdded(QQuickItem *child);
    void childRemoved(QQuickItem *child);

protected:
    void itemChange(ItemChange change, const ItemChangeData &data) override;
};

} // namespace QmlCss
