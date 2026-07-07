#pragma once

#include <QObject>

// Pins the DOCUMENTED contract of CssLayoutEngine (docs/css-support.md, "Box model
// and layout"): flex, grid, box model, viewport units, absolute positioning. Each
// case builds a real QML scene (CssRect container + CssRect children) styled purely
// from CSS and asserts the resulting child geometry.
class LayoutTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    // Flexbox
    void flexRowPlacesAndGaps();
    void flexJustifySpaceBetween();
    void flexColumnCentersCrossAxis();
    void flexGrowAbsorbsRemainder();

    // Grid
    void gridFrTracksSplitWidth();
    void gridRepeatWrapsRows();

    // Box model
    void paddingInsetsContent();
    void borderInsetsContentBox();
    void childMarginOffsets();
    void percentAndCalcWidths();
    void viewportUnitsResolve();
    void aspectRatioDerivesHeight();

    // Absolute positioning
    void absoluteAndInsetPlacement();
};
