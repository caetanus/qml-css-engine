#pragma once

#include <QObject>

// Step 4c (lean text): CssTextLayout must report and place text EXACTLY like the QQuickText CssText composes
// today. The oracle is a real QQuickText configured the way CssText::applyToText configures its label;
// QQuickTextPrivate (private headers, test only) supplies what the public API does not expose — the drawn
// lines and the updatePaintNode offset.
class TextTests : public QObject {
    Q_OBJECT

private slots:
    void leanTextMetricsMatchQQuickText_data();
    void leanTextMetricsMatchQQuickText();
};
