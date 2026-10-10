#pragma once

#include <QQuickItem>

namespace QmlCss {

class CssBox;

// The plain item that hosts a box's declared children (CssRect/CssFill `contentHolder`). It reports
// each child as it is added or removed, so the box handles THAT child — instead of reacting to the
// argument-less `childrenChanged` by rescanning every sibling (O(N) per change, O(N^2) per list build).
// The owning box hears it through a typed call (CssBox::contentChildAdded/Removed), not a connection;
// the signals stay for other observers.
class CssContentHolder : public QQuickItem {
    Q_OBJECT

public:
    explicit CssContentHolder(CssBox *owner);
    // The owner is going away (its destructor): report nothing more.
    void clearOwner() { m_owner = nullptr; }

signals:
    void childAdded(QQuickItem *child);
    void childRemoved(QQuickItem *child);

protected:
    void itemChange(ItemChange change, const ItemChangeData &data) override;

private:
    CssBox *m_owner;
};

} // namespace QmlCss
