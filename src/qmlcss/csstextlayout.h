#pragma once

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
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
    qreal lineHeight = 0;
    qreal leftPadding = 0, rightPadding = 0, topPadding = 0, bottomPadding = 0;
    // The ints together (no padding holes: every CssText keeps one payload inline).
    int lineHeightMode = 0; // QQuickText::ProportionalHeight (0) / FixedHeight (1)
    int textFormat = 0; // QQuickText::PlainText (0) / StyledText (4)
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
    // QQuickTextPrivate state the next geometry change and paint read (QQuickText::geometryChange/updatePaintNode).
    bool widthExceeded = false;
    bool heightExceeded = false;
    int lineHeightOffset = 0; // QQuickTextPrivate::lineHeightOffset() (an int: Qt truncates)
    int effectiveHAlign = 0; // QQuickText::effectiveHAlign() (mirroring applied)
};

// What a QQuickText does when its size changes (QQuickText::geometryChange): nothing, re-place the text it
// already laid out (a new paint node at a new origin), or lay it out again.
enum class CssTextGeometryEffect { None, Repaint, Relayout };

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

    // The label at MOUNT: QQuickText::componentComplete lays the label out before it has a size (width and height
    // invalid; it then follows its implicit size). CssText's label keeps this layout until a size it is given
    // changes it (assigning the implicit size it already has is no geometry change).
    static CssTextMetrics mountMetrics(const CssTextPayload &payload, bool mirrored = false);
    static CssTextBuiltLayout buildMountLayout(const CssTextPayload &payload, bool mirrored = false);

    // QQuickText::updatePaintNode (qquicktext.cpp 2620-2645): where the layout(s) laid out into `metrics` are
    // added for a label of size width x height — the current size, which need not be the one laid out at.
    static QPointF paintOrigin(const CssTextMetrics &metrics, const CssTextPayload &payload, qreal width,
                               qreal height);

    // QQuickText::geometryChange (qquicktext.cpp 2510-2591) for the label laid out into `metrics`, resized
    // from `from` to `to`.
    static CssTextGeometryEffect geometryEffect(const CssTextMetrics &metrics, const CssTextPayload &payload,
                                                QSizeF from, QSizeF to);
};

} // namespace QmlCss
