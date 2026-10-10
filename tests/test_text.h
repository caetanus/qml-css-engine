#pragma once

#include <QObject>

// Step 4c (lean text): CssTextLayout must report and place text EXACTLY like the QQuickText CssText composes
// today. The oracle is a real QQuickText configured the way CssText::applyToText configures its label;
// QQuickTextPrivate (private headers, test only) supplies what the public API does not expose — the drawn
// lines and the updatePaintNode offset.
class TextTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void leanTextMetricsMatchQQuickText_data();
    void leanTextMetricsMatchQQuickText();
    // The label at mount (laid out before it has a size) — the layout a lean CssText draws until resized.
    void leanTextMountMetricsMatchQQuickText_data();
    void leanTextMountMetricsMatchQQuickText();

    // Step 4c Task 2: the lean CssText draws its own QSGTextNode.
    // Composed (QQuickText) and lean grabs of the same CssText are pixel-identical.
    void leanTextRendersLikeQQuickText_data();
    void leanTextRendersLikeQQuickText();
    // 100 texts: one layout each, one node build each; a pure move rebuilds nothing.
    void leanTextLaysOutOnce();
    // styledText / text-shadow compose the QQuickText; dropping them goes back to lean, cleanly both ways.
    void leanTextFallbackSwitch();
    // background-color paints a box of the outer item's size (there is no label to size it from).
    void leanTextBackgroundSizedWithoutLabel();
    // A theme font registration (fontRevision) re-lays out and rebuilds the glyphs with the new font.
    void leanTextFontRevision();
};
