#pragma once

#include <QObject>

class QmlCssTests final : public QObject {
    Q_OBJECT

private slots:
    void cssRectLoadsAndRestyles();
    void cssTextUsesStandaloneDefaults();
    void cssItemAppliesToParent();
};
