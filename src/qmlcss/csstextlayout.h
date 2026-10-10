#pragma once

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTextLayout>

#include <memory>

namespace QmlCss {

// Everything CssText::applyToText() writes to the composed QQuickText. Compared field by field with the last
// payload APPLIED, so a re-apply with nothing changed writes nothing (no property writes, no QQuickText relayout).
// verticalAlignment is not here: it is constant (AlignVCenter), written once when the label is composed.
struct CssTextPayload {
    QString text;
    QColor color;
    QFont font;
    int lineHeightMode = 0; // QQuickText::ProportionalHeight (0) / FixedHeight (1)
    qreal lineHeight = 0;
    int textFormat = 0; // QQuickText::PlainText (0) / StyledText (4)
    qreal leftPadding = 0, rightPadding = 0, topPadding = 0, bottomPadding = 0;
    int wrapMode = 0; // QTextOption::WrapMode
    int elide = 0; // Qt::TextElideMode
    int horizontalAlignment = 0; // Qt::AlignmentFlag (horizontal)
};

// What a QQuickText configured from a PLAIN-TEXT payload (textFormat PlainText, verticalAlignment AlignVCenter,
// explicit horizontalAlignment, width AND height assigned — CssText's label always has both) reports and draws
// at a given box. Scalars only: nothing here keeps a layout alive.
struct CssTextMetrics {
    qreal implicitWidth = 0;
    qreal implicitHeight = 0;
    int lineCount = 1; // QQuickText::lineCount — visible lines, the elided one included
    bool truncated = false;
    qreal contentWidth = 0; // QQuickText::contentWidth
    qreal contentHeight = 0; // QQuickText::contentHeight
    qreal baselineOffset = 0; // QQuickItem::baselineOffset of the label
    QRectF layedOutTextRect; // QQuickTextPrivate::layedOutTextRect
    qreal lineWidth = 0; // QQuickTextPrivate::lineWidth (the width the lines were aligned in)
    QPointF origin; // where updatePaintNode adds the layout(s): (dx, dy)
    int unelidedLineCount = 0; // lines of the main layout that are drawn
    bool elided = false; // an elide layout (one line) is drawn after them
    bool empty = true; // empty text: nothing is drawn
};

// A configured layout for one box, built on demand: the main layout (its first `metrics.unelidedLineCount`
// lines are drawn) plus, when elided, the one-line elide layout — both at `metrics.origin`, exactly as
// QQuickText::updatePaintNode adds d->layout and d->elideLayout. Glyph caching is on (setCacheEnabled).
struct CssTextBuiltLayout {
    CssTextMetrics metrics;
    std::unique_ptr<QTextLayout> layout;
    std::unique_ptr<QTextLayout> elideLayout;
};

// Pure GUI-side text metrics: a LITERAL port of the QQuickText (Qt 6.11, qquicktext.cpp) code paths a
// CssText label exercises — QQuickText::setFont (0.5pt rounding), QQuickTextPrivate::updateLayout (PlainText
// preprocessing), updateSize, setupTextLayout, setLineGeometry, lineHeightOffset, elidedText, updateBaseline
// and the updatePaintNode offsets. Branches a CssText label can never reach (styled/rich text, inline images,
// fontSizeMode fitting, maximumLineCount, lineLaidOut) are left out and marked. Proven against a real
// QQuickText by the oracle test (tests/test_text.cpp).
//
// `width`/`height` are the LABEL's size (CssText assigns both, so both count as valid). `mirrored` is the
// label's effective LayoutMirroring (it swaps an explicit left/right alignment).
class CssTextLayout {
public:
    static CssTextMetrics metrics(const CssTextPayload &payload, qreal width, qreal height, bool mirrored = false);
    static CssTextBuiltLayout buildLayout(const CssTextPayload &payload, qreal width, qreal height,
                                          bool mirrored = false);
};

} // namespace QmlCss
