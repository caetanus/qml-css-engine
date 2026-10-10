#include "qmlcss/csstextlayout.h"

#include <QFontMetricsF>
#include <QQuickWindow>
#include <QTextOption>
#include <QtMath>

#include <QtGui/private/qfixed_p.h>
#include <QtGui/private/qtextengine_p.h>

#include <cfloat>
#include <climits>

// Literal port of QQuickText (qtdeclarative v6.11.1, src/quick/items/qquicktext.cpp). Every block names the
// source lines it ports. The math is NOT simplified: the oracle test (tests/test_text.cpp) holds it to the
// real QQuickText. The configuration is the one CssText gives its label:
//   - textFormat PlainText (StyledText labels stay on the composed QQuickText);
//   - verticalAlignment AlignVCenter; horizontalAlignment always written (hAlignImplicit == false);
//   - width AND height assigned (widthValid/heightValid) once the label is laid out;
//   - implicitWidth read (requireImplicitSize), so the implicit size is always computed;
//   - fontSizeMode FixedSize, maximumLineCount unset (INT_MAX), no lineLaidOut handler, no inline images.
// Branches that configuration never reaches are left out and marked "(unreachable: ...)".

namespace QmlCss {

namespace {

constexpr QChar kElideChar = QChar(0x2026); // QQuickTextPrivate::elideChar (qquicktext.cpp 42)

enum { ProportionalHeight = 0, FixedHeight = 1 }; // QQuickText::LineHeightMode

// The QQuickTextPrivate state setupTextLayout() reads and writes, for one label.
struct QuickTextPort {
    // --- configuration (QQuickText properties) ---
    QString text; // d->text
    QFont font; // d->font
    int lineHeightMode = ProportionalHeight;
    qreal lineHeight = 1.0;
    qreal leftPadding = 0, rightPadding = 0, topPadding = 0, bottomPadding = 0;
    int wrapMode = QTextOption::NoWrap;
    int elideMode = Qt::ElideNone;
    int effectiveHAlign = Qt::AlignLeft; // QQuickText::effectiveHAlign()
    bool useDesignMetrics = true; // renderType != NativeRendering

    // --- item geometry (QQuickItem) ---
    qreal width = 0, height = 0;
    bool widthValid = true, heightValid = true;

    // --- QQuickTextPrivate flags ---
    bool requireImplicitSize = true;
    bool implicitWidthValid = true;
    bool implicitHeightValid = true;

    // --- results ---
    std::unique_ptr<QTextLayout> layout = std::make_unique<QTextLayout>();
    std::unique_ptr<QTextLayout> elideLayout;
    int multilengthEos = -1;
    qreal lineWidth = 0;
    int lineCount = 1;
    bool truncated = false;
    bool widthExceeded = false, heightExceeded = false;
    qreal implicitWidth = 0, implicitHeight = 0;
    QRectF layedOutTextRect;
    qreal baselineOffset = 0;
    bool earlyElidedReturn = false; // setupTextLayout took the "zero width or height" exit (implicit size kept)

    // QQuickTextPrivate::availableWidth/availableHeight (qquicktext.cpp 131-141)
    qreal availableWidth() const { return width - leftPadding - rightPadding; }
    qreal availableHeight() const { return height - topPadding - bottomPadding; }

    // QQuickItem::setImplicitSize/setImplicitHeight: an item without an explicit size follows its implicit one.
    void setImplicitSize(qreal w, qreal h)
    {
        implicitWidth = w;
        implicitHeight = h;
        if (!widthValid)
            width = w;
        if (!heightValid)
            height = h;
    }
    void setImplicitHeight(qreal h)
    {
        implicitHeight = h;
        if (!heightValid)
            height = h;
    }

    // QQuickTextPrivate::lineHeightOffset (qquicktext.cpp 1367-1373) — note the int return: Qt truncates.
    int lineHeightOffset() const
    {
        QFontMetricsF fm(font);
        qreal fontHeight = qCeil(fm.height()); // QScriptLine and therefore QTextLine rounds up
        return lineHeightMode == FixedHeight ? fontHeight - lineHeight : (1.0 - lineHeight) * fontHeight;
    }

    // QQuickTextPrivate::updateLayout, PlainText branch (qquicktext.cpp 240-248): text up to the first U+009C,
    // '\n' -> QChar::LineSeparator.
    void updateLayoutText()
    {
        QString tmp = text;
        multilengthEos = tmp.indexOf(QLatin1Char('\x9c'));
        if (multilengthEos != -1)
            tmp = tmp.mid(0, multilengthEos);
        tmp.replace(QLatin1Char('\n'), QChar::LineSeparator);
        layout->setText(tmp);
    }

    // QQuickTextPrivate::setLineGeometry (qquicktext.cpp 1296-1362). Without inline images both of its branches
    // reduce to the same placement; this is the extra-allocated branch (1301-1305).
    void setLineGeometry(QTextLine &line, qreal lineWidth, qreal &height) const
    {
        line.setLineWidth(lineWidth);
        line.setPosition(QPointF(line.position().x(), height));
        height += (lineHeightMode == FixedHeight) ? lineHeight : line.height() * lineHeight;
    }

    // QQuickTextPrivate::elidedText (qquicktext.cpp 739-762), PlainText (!styledText).
    QString elidedText(qreal lineWidth, const QTextLine &line, const QTextLine *nextLine = nullptr) const
    {
        if (nextLine) {
            return layout->engine()->elidedText(Qt::TextElideMode(elideMode), QFixed::fromReal(lineWidth), 0,
                                                line.textStart(), line.textLength() + nextLine->textLength());
        } else {
            QString elideText = layout->text().mid(line.textStart(), line.textLength());
            // QFontMetrics won't help eliding styled text.
            elideText[elideText.size() - 1] = kElideChar;
            // Appending the elide character may push the line over the maximum width
            // in which case the elided text will need to be elided.
            QFontMetricsF metrics(layout->font());
            if (metrics.horizontalAdvance(kElideChar) + line.naturalTextWidth() >= lineWidth)
                elideText = metrics.elidedText(elideText, Qt::TextElideMode(elideMode), lineWidth);
            return elideText;
        }
    }

    // QQuickTextPrivate::setupTextLayout (qquicktext.cpp 777-1294).
    QRectF setupTextLayout(qreal *const baseline)
    {
        // 781-784 (maximumLineCountValid is false)
        bool singlelineElide = elideMode != Qt::ElideNone && widthValid;
        bool multilineElide = elideMode == Qt::ElideRight && widthValid && heightValid;

        // 788-812: elided with a zero width or height.
        if ((!requireImplicitSize || (implicitWidthValid && implicitHeightValid))
                && ((singlelineElide && availableWidth() <= 0.)
                    || (multilineElide && heightValid && availableHeight() <= 0.))) {
            // we are elided and we have a zero width or height
            widthExceeded = widthValid && availableWidth() <= 0.;
            heightExceeded = heightValid && availableHeight() <= 0.;
            truncated = true;
            lineCount = 0;
            if (qFuzzyIsNull(width))
                layout->setText(QString());
            earlyElidedReturn = true;

            QFontMetricsF fm(font);
            qreal height = (lineHeightMode == FixedHeight) ? lineHeight : qCeil(fm.height()) * lineHeight;
            *baseline = fm.ascent();
            return QRectF(0, 0, 0, height);
        }

        // 814-829 (visibleImgTags: no inline images)
        const bool shouldUseDesignMetrics = useDesignMetrics;
        layout->setCacheEnabled(true);
        QTextOption textOption = layout->textOption();
        if (textOption.alignment() != Qt::Alignment(effectiveHAlign)
                || textOption.wrapMode() != QTextOption::WrapMode(wrapMode)
                || textOption.useDesignMetrics() != shouldUseDesignMetrics) {
            textOption.setAlignment(Qt::Alignment(effectiveHAlign));
            textOption.setWrapMode(QTextOption::WrapMode(wrapMode));
            textOption.setUseDesignMetrics(shouldUseDesignMetrics);
            layout->setTextOption(textOption);
        }
        if (layout->font() != font)
            layout->setFont(font);

        // 830-833
        lineWidth = (widthValid || implicitWidthValid) && width > 0 ? width : FLT_MAX;
        qreal maxHeight = heightValid ? availableHeight() : FLT_MAX;

        // 835-838 (customLayout: no lineLaidOut handler)
        bool canWrap = wrapMode != QTextOption::NoWrap && widthValid;

        // 840-856: horizontalFit/verticalFit are false (fontSizeMode FixedSize); the font-size search state
        // they drive is left out (unreachable: FixedSize).
        QString layoutText = layout->text();
        const int maxLineCount = INT_MAX; // maximumLineCount() without extra->maximumLineCount set

        // 857-874
        bool widthChanged = false;
        widthExceeded = availableWidth() <= 0 && (singlelineElide || canWrap);
        heightExceeded = availableHeight() <= 0 && multilineElide;

        QRectF br;

        int visibleCount = 0;
        bool elide;
        qreal height = 0;
        QString elideText;
        bool once = true;
        int elideStart = 0;
        // elideEnd is only read for ElideLeft/ElideMiddle formats (rich/styled text) — kept for the port's shape.
        int elideEnd = 0;
        Q_UNUSED(elideEnd);
        // noBreakLastLine (872): only switches the wrap mode at visibleCount == maxLineCount (unreachable: INT_MAX).

        int eos = multilengthEos;

        // 879-1199: repeated layouts with abbreviated strings may be required if the text doesn't fit within the
        // item dimensions, or a changed width/height invalidates the first pass.
        for (;;) {
            // 880-887: !once rescales the font (unreachable: FixedSize keeps scaledFontSize == the font size).

            layout->beginLayout();

            // 891-904
            bool wrapped = false;
            truncated = false;
            elide = false;
            int unwrappedLineCount = 1;
            height = 0;
            qreal naturalHeight = 0;
            qreal previousHeight = 0;
            br = QRectF();

            QRectF unelidedRect;
            QTextLine line;
            for (visibleCount = 1; ; ++visibleCount) {
                line = layout->createLine();

                // 905-915 (no custom line geometry; noBreakLastLine unreachable)
                setLineGeometry(line, lineWidth, naturalHeight);

                unelidedRect = br.united(line.naturalTextRect());

                // 919-941: elide the previous line if the accumulated height of the text exceeds the height
                // of the element.
                if (multilineElide && naturalHeight > maxHeight && visibleCount > 1) {
                    elide = true;
                    heightExceeded = true;
                    if (eos != -1)  // There's an abbreviated string available, skip the rest as it's
                        break;      // all going to be discarded.

                    truncated = true;

                    visibleCount -= 1;

                    const QTextLine previousLine = layout->lineAt(visibleCount - 1);
                    elideText = layoutText.at(line.textStart() - 1) != QChar::LineSeparator
                            ? elidedText(line.width(), previousLine, &line)
                            : elidedText(line.width(), previousLine);
                    elideStart = previousLine.textStart();
                    // elideEnd isn't required for right eliding.

                    height = previousHeight;
                    break;
                }

                // 943-966
                const bool isLastLine = line.textStart() + line.textLength() >= layoutText.size();
                if (isLastLine) {
                    if (singlelineElide && visibleCount == 1 && line.naturalTextWidth() > line.width()) {
                        // Elide a single previousLine of text if its width exceeds the element width.
                        elide = true;
                        widthExceeded = true;
                        if (eos != -1) // There's an abbreviated string available.
                            break;

                        truncated = true;
                        elideText = layout->engine()->elidedText(
                                Qt::TextElideMode(elideMode),
                                QFixed::fromReal(line.width()),
                                0,
                                line.textStart(),
                                line.textLength());
                        elideStart = line.textStart();
                        elideEnd = elideStart + line.textLength();
                    } else {
                        br = unelidedRect;
                        height = naturalHeight;
                    }
                    break;
                } else {
                    // 967-1002
                    const bool wrappedLine = layoutText.at(line.textStart() + line.textLength() - 1) != QChar::LineSeparator;
                    wrapped |= wrappedLine;

                    if (!wrappedLine)
                        ++unwrappedLineCount;

                    // 973-1001: stop at maxLineCount (unreachable: INT_MAX).
                }
                br = unelidedRect;
                previousHeight = height;
                height = naturalHeight;
            }
            widthExceeded |= wrapped;

            // 1009-1093: save the implicit size of the text on the first layout only.
            if (once) {
                once = false;

                // If implicit sizes are required layout any additional lines up to the maximum line count.
                if ((requireImplicitSize) && line.isValid() && unwrappedLineCount < maxLineCount) {
                    // Layout the remainder of the wrapped lines up to maxLineCount to get the implicit height.
                    for (int lineCount = layout->lineCount(); lineCount < maxLineCount; ++lineCount) {
                        line = layout->createLine();
                        if (!line.isValid())
                            break;
                        if (layoutText.at(line.textStart() - 1) == QChar::LineSeparator)
                            ++unwrappedLineCount;
                        setLineGeometry(line, lineWidth, naturalHeight);
                    }

                    // Create the remainder of the unwrapped lines up to maxLineCount to get the implicit width.
                    const int eol = line.isValid()
                            ? line.textStart() + line.textLength()
                            : layoutText.size();
                    if (eol < layoutText.size() && layoutText.at(eol) != QChar::LineSeparator)
                        line = layout->createLine();
                    for (; line.isValid() && unwrappedLineCount < maxLineCount; ++unwrappedLineCount)
                        line = layout->createLine();
                }

                layout->endLayout();

                const qreal naturalWidth = layout->maximumWidth();

                setImplicitSize(naturalWidth + leftPadding + rightPadding,
                                naturalHeight + qMax(lineHeightOffset(), 0) + topPadding + bottomPadding);

                // Update any variables that are dependent on the validity of the width or height.
                singlelineElide = elideMode != Qt::ElideNone && widthValid;
                multilineElide = elideMode == Qt::ElideRight && widthValid && heightValid;
                canWrap = wrapMode != QTextOption::NoWrap && widthValid;

                const qreal oldWidth = lineWidth;
                const qreal oldHeight = maxHeight;

                const qreal availWidth = availableWidth();
                const qreal availHeight = availableHeight();

                lineWidth = widthValid && width > 0 ? availWidth : naturalWidth;
                maxHeight = heightValid ? availHeight : FLT_MAX;

                // If the width of the item has changed and it's possible the result of wrapping,
                // eliding, scaling has changed, or the text is not left aligned do another layout.
                if ((!qFuzzyCompare(lineWidth, oldWidth) || (widthExceeded && lineWidth > oldWidth))
                        && (singlelineElide || multilineElide || canWrap
                            || effectiveHAlign != Qt::AlignLeft)) {
                    widthChanged = true;
                    widthExceeded = lineWidth >= qMin(oldWidth, naturalWidth);
                    heightExceeded = false;
                    continue;
                }

                // If the height of the item has changed and it's possible the result of eliding,
                // line count truncation or scaling has changed, do another layout.
                if ((maxHeight < qMin(oldHeight, naturalHeight) || (heightExceeded && maxHeight > oldHeight))
                        && multilineElide) { // (canWrap && maximumLineCountValid): unreachable
                    widthExceeded = false;
                    heightExceeded = false;
                    continue;
                }

                // If the horizontal alignment is not left and the width was not valid we need to relayout
                // now that we know the maximum line width.
                if (!widthValid && !implicitWidthValid && unwrappedLineCount > 1
                        && effectiveHAlign != Qt::AlignLeft) {
                    widthExceeded = false;
                    heightExceeded = false;
                    continue;
                }
            } else if (widthChanged) {
                // 1094-1129
                widthChanged = false;
                if (line.isValid()) {
                    for (int lineCount = layout->lineCount(); lineCount < maxLineCount; ++lineCount) {
                        line = layout->createLine();
                        if (!line.isValid())
                            break;
                        setLineGeometry(line, lineWidth, naturalHeight);
                    }
                }
                layout->endLayout();

                setImplicitHeight(naturalHeight + qMax(lineHeightOffset(), 0) + topPadding + bottomPadding);

                multilineElide = elideMode == Qt::ElideRight && widthValid && heightValid;

                const qreal oldHeight = maxHeight;
                maxHeight = heightValid ? availableHeight() : FLT_MAX;
                // If the height of the item has changed and it's possible the result of eliding,
                // line count truncation or scaling has changed, do another layout.
                if ((maxHeight < qMin(oldHeight, naturalHeight) || (heightExceeded && maxHeight > oldHeight))
                        && multilineElide) { // (canWrap && maximumLineCountValid): unreachable
                    widthExceeded = false;
                    heightExceeded = false;
                    continue;
                }
            } else {
                layout->endLayout();
            }

            // 1131-1141: if the text needs to be elided and there's an abbreviated string available go back and
            // do another layout with the abbreviated string.
            if (eos != -1 && elide) {
                int start = eos + 1;
                eos = text.indexOf(QLatin1Char('\x9c'), start);
                layoutText = text.mid(start, eos != -1 ? eos - start : -1);
                layoutText.replace(QLatin1Char('\n'), QChar::LineSeparator);
                layout->setText(layoutText);
                continue;
            }

            // 1143
            br.moveTop(0);

            // 1145-1153: the advance is only read by QQuickText's advance property — not a CssText metric.

            // 1155-1156
            break;
            // 1158-1198: font-size fitting (unreachable: FixedSize).
        }

        // 1201-1202
        implicitWidthValid = true;
        implicitHeightValid = true;

        // 1215-1216
        if (eos != multilengthEos)
            truncated = true;

        // 1220-1273
        if (elide) {
            if (!elideLayout) {
                elideLayout = std::make_unique<QTextLayout>();
                elideLayout->setCacheEnabled(true);
            }
            // 1226-1251: format ranges (unreachable: PlainText has none).
            elideLayout->setFont(layout->font());
            elideLayout->setTextOption(layout->textOption());
            elideLayout->setText(elideText);
            elideLayout->beginLayout();

            QTextLine elidedLine = elideLayout->createLine();
            elidedLine.setPosition(QPointF(0, height));
            setLineGeometry(elidedLine, lineWidth, height);
            elideLayout->endLayout();

            br = br.united(elidedLine.naturalTextRect());

            if (visibleCount == 1)
                layout->clearLayout();
        } else {
            elideLayout.reset();
        }

        // 1275-1279
        QTextLine firstLine = visibleCount == 1 && elideLayout
                ? elideLayout->lineAt(0)
                : layout->lineAt(0);
        if (firstLine.isValid())
            *baseline = firstLine.y() + firstLine.ascent();

        // 1281-1282
        br.setHeight(height);

        // 1284-1288
        lineCount = visibleCount;

        return br;
    }

    // QQuickTextPrivate::updateBaseline (qquicktext.cpp 404-417); vAlign is AlignVCenter.
    void updateBaseline(qreal baseline, qreal dy)
    {
        qreal yoff = 0;
        if (heightValid)
            yoff = dy / 2;
        baselineOffset = baseline + yoff + topPadding;
    }

    // QQuickTextPrivate::updateSize (qquicktext.cpp 434-589), non-rich text.
    void updateSize()
    {
        const qreal hPadding = leftPadding + rightPadding;
        const qreal vPadding = topPadding + bottomPadding;

        // 456-482: empty text has the font's height.
        if (text.isEmpty()) {
            // How much more expensive is it to just do a full layout on an empty string here?
            QFontMetricsF fm(font);
            qreal fontHeight = qCeil(fm.height()); // QScriptLine and therefore QTextLine rounds up
            fontHeight = lineHeightMode == FixedHeight ? lineHeight : fontHeight * lineHeight;
            updateBaseline(fm.ascent(), height - fontHeight - vPadding);
            setImplicitSize(hPadding, fontHeight + qMax(lineHeightOffset(), 0) + vPadding);
            layedOutTextRect = QRectF(0, 0, 0, fontHeight);
            lineCount = 1;
            truncated = false;
            return;
        }

        // 486-497
        qreal baseline = 0;
        QRectF textRect = setupTextLayout(&baseline);
        layedOutTextRect = textRect;
        const QSizeF size = textRect.size();
        updateBaseline(baseline, height - size.height() - vPadding);
    }
};

// QQuickText::setFont (qquicktext.cpp 1739-1756): 0.5pt resolution; antialiasing stays on (the default).
QFont roundedFont(const QFont &source)
{
    QFont font = source;
    if (font.pointSizeF() != -1) {
        // 0.5pt resolution
        qreal size = qRound(font.pointSizeF() * 2.0);
        font.setPointSizeF(size / 2.0);
    }
    return font;
}

// QQuickText::effectiveHAlign (qquicktext.cpp 2069-2086) for an EXPLICIT alignment (CssText always writes one).
int effectiveHAlign(int hAlign, bool mirrored)
{
    if (mirrored) {
        if (hAlign == Qt::AlignLeft)
            return Qt::AlignRight;
        if (hAlign == Qt::AlignRight)
            return Qt::AlignLeft;
    }
    return hAlign;
}

// QQuickTextUtil::alignedX/alignedY (qquicktextutil.cpp 48-79).
qreal alignedX(qreal textWidth, qreal itemWidth, int alignment)
{
    qreal x = 0;
    switch (alignment) {
    case Qt::AlignLeft:
    case Qt::AlignJustify:
        break;
    case Qt::AlignRight:
        x = itemWidth - textWidth;
        break;
    case Qt::AlignHCenter:
        x = (itemWidth - textWidth) / 2;
        break;
    }
    return x;
}
qreal alignedY(qreal textHeight, qreal itemHeight, int alignment)
{
    qreal y = 0;
    switch (alignment) {
    case Qt::AlignTop:
        break;
    case Qt::AlignBottom:
        y = itemHeight - textHeight;
        break;
    case Qt::AlignVCenter:
        y = (itemHeight - textHeight) / 2;
        break;
    }
    return y;
}

QuickTextPort configure(const CssTextPayload &p, qreal width, qreal height, bool mirrored)
{
    QuickTextPort t;
    t.text = p.text;
    t.font = roundedFont(p.font);
    // QQuickText::setLineHeight ignores a negative value (qquicktext.cpp 2743).
    t.lineHeightMode = p.lineHeightMode;
    t.lineHeight = p.lineHeight < 0.0 ? 1.0 : p.lineHeight;
    t.leftPadding = p.leftPadding;
    t.rightPadding = p.rightPadding;
    t.topPadding = p.topPadding;
    t.bottomPadding = p.bottomPadding;
    t.wrapMode = p.wrapMode;
    t.elideMode = p.elide;
    t.effectiveHAlign = effectiveHAlign(p.horizontalAlignment, mirrored);
    // QQuickTextPrivate::renderType defaults to QQuickTextUtil::textRenderType<QQuickText>() (the window-wide
    // text render type); design metrics are off only for NativeRendering (qquicktext.cpp 814).
    t.useDesignMetrics = QQuickWindow::textRenderType() != QQuickWindow::NativeTextRendering;
    t.width = width;
    t.height = height;
    t.updateLayoutText();
    return t;
}

CssTextBuiltLayout run(const CssTextPayload &p, qreal width, qreal height, bool mirrored)
{
    QuickTextPort t = configure(p, width, height, mirrored);
    t.updateSize();

    CssTextBuiltLayout out;
    CssTextMetrics &m = out.metrics;
    if (t.earlyElidedReturn) {
        // The zero-size elided exit keeps the implicit size of the previous full layout: the one the label got
        // at mount, at its own implicit size (width/height not yet assigned by a layout).
        QuickTextPort mount = configure(p, 0, 0, mirrored);
        mount.widthValid = false;
        mount.heightValid = false;
        mount.implicitWidthValid = false;
        mount.implicitHeightValid = false;
        mount.updateSize();
        t.implicitWidth = mount.implicitWidth;
        t.implicitHeight = mount.implicitHeight;
    }
    m.implicitWidth = t.implicitWidth;
    m.implicitHeight = t.implicitHeight;
    m.lineCount = t.lineCount;
    m.truncated = t.truncated;
    // QQuickText::contentWidth/contentHeight (qquicktext.cpp 2706-2722)
    m.contentWidth = t.layedOutTextRect.width();
    m.contentHeight = t.layedOutTextRect.height() + qMax(t.lineHeightOffset(), 0);
    m.baselineOffset = t.baselineOffset;
    m.layedOutTextRect = t.layedOutTextRect;
    m.lineWidth = t.lineWidth;
    m.empty = t.text.isEmpty();

    // QQuickText::updatePaintNode (qquicktext.cpp 2620-2652): where the layouts are added.
    const qreal dy = alignedY(t.layedOutTextRect.height() + t.lineHeightOffset(), t.availableHeight(),
                              Qt::AlignVCenter) + t.topPadding;
    const qreal dx = alignedX(t.lineWidth, t.availableWidth(), t.effectiveHAlign) + t.leftPadding;
    m.origin = QPointF(dx, dy);
    int unelidedLineCount = t.lineCount;
    if (t.elideLayout)
        unelidedLineCount -= 1;
    m.unelidedLineCount = qMax(unelidedLineCount, 0);
    m.elided = t.elideLayout != nullptr;

    out.layout = std::move(t.layout);
    out.elideLayout = std::move(t.elideLayout);
    return out;
}

} // namespace

CssTextMetrics CssTextLayout::metrics(const CssTextPayload &payload, qreal width, qreal height, bool mirrored)
{
    return run(payload, width, height, mirrored).metrics;
}

CssTextBuiltLayout CssTextLayout::buildLayout(const CssTextPayload &payload, qreal width, qreal height, bool mirrored)
{
    return run(payload, width, height, mirrored);
}

} // namespace QmlCss
