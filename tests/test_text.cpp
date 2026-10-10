#include "test_text.h"

#include "qmlcss/csstextlayout.h"

#include <QFontMetricsF>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QScopedPointer>
#include <QTest>
#include <QTextLayout>
#include <QTextOption>

#include <QtQuick/private/qquicktext_p.h>
#include <QtQuick/private/qquicktext_p_p.h>

using namespace QmlCss;

namespace {

// One placed line as the scene graph receives it: the line's natural rect (alignment included) moved by the
// offset updatePaintNode adds the layout at, plus the text range it shows.
struct PlacedLine {
    QRectF rect;
    int textStart = 0;
    int textLength = 0;
    bool operator==(const PlacedLine &o) const
    {
        return rect == o.rect && textStart == o.textStart && textLength == o.textLength;
    }
};

QString describe(const PlacedLine &l)
{
    return QStringLiteral("[%1,%2 %3x%4 text %5+%6]")
        .arg(l.rect.x()).arg(l.rect.y()).arg(l.rect.width()).arg(l.rect.height())
        .arg(l.textStart).arg(l.textLength);
}

// QQuickTextUtil::alignedX/alignedY (qquicktextutil.cpp 48-79) — the class is not exported, so the two
// formulas the oracle needs are restated here verbatim.
qreal alignedX(qreal textWidth, qreal itemWidth, int alignment)
{
    switch (alignment) {
    case Qt::AlignRight:
        return itemWidth - textWidth;
    case Qt::AlignHCenter:
        return (itemWidth - textWidth) / 2;
    default:
        return 0;
    }
}
qreal alignedY(qreal textHeight, qreal itemHeight, int alignment)
{
    switch (alignment) {
    case Qt::AlignBottom:
        return itemHeight - textHeight;
    case Qt::AlignVCenter:
        return (itemHeight - textHeight) / 2;
    default:
        return 0;
    }
}

void appendLines(QList<PlacedLine> *out, const QTextLayout *layout, int count, QPointF origin)
{
    for (int i = 0; i < count && i < layout->lineCount(); ++i) {
        const QTextLine line = layout->lineAt(i);
        out->append({ line.naturalTextRect().translated(origin), line.textStart(), line.textLength() });
    }
}

struct Observed {
    qreal implicitWidth = 0, implicitHeight = 0;
    int lineCount = 0;
    bool truncated = false;
    qreal contentWidth = 0, contentHeight = 0;
    qreal baselineOffset = 0;
    QList<PlacedLine> lines;
    bool buildAgrees = true;
};

constexpr qreal kAuto = -1; // height: the label's implicit height at the case's width (what a layout assigns)

struct Case {
    const char *name;
    CssTextPayload payload;
    qreal width;
    qreal height;
    bool mirrored = false;
};

CssTextPayload base(const QString &text)
{
    CssTextPayload p;
    p.text = text;
    p.color = QColor(Qt::black);
    p.font.setFamily(QStringLiteral("Noto Sans"));
    p.font.setPointSizeF(12);
    p.font.setWeight(QFont::Normal);
    p.font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    p.font.setWordSpacing(0);
    p.lineHeightMode = 0;
    p.lineHeight = 1.0;
    p.textFormat = 0;
    p.wrapMode = QTextOption::WordWrap; // CssText's default (white-space other than nowrap)
    p.elide = Qt::ElideNone;
    p.horizontalAlignment = Qt::AlignLeft;
    return p;
}

const QString kLong = QStringLiteral("The quick brown fox jumps over the lazy dog, again and again and again");

QList<Case> cases()
{
    QList<Case> c;
    c.append({ "empty", base(QString()), 100, 30 });
    c.append({ "empty-padded-fixed-lh", [] { auto p = base(QString()); p.lineHeightMode = 1; p.lineHeight = 30;
                                              p.topPadding = 4; p.bottomPadding = 6; p.leftPadding = 3; return p; }(),
               100, 50 });
    c.append({ "single-word", base(QStringLiteral("Hello")), 200, kAuto });
    c.append({ "single-word-taller-box", base(QStringLiteral("Hello")), 200, 60 });
    c.append({ "single-word-shorter-box", base(QStringLiteral("Hello")), 200, 8 });
    c.append({ "two-lines", base(QStringLiteral("first\nsecond")), 200, kAuto });
    c.append({ "wrap-80", base(kLong), 80, kAuto });
    c.append({ "wrap-150", base(kLong), 150, kAuto });
    c.append({ "wrap-400", base(kLong), 400, kAuto });
    c.append({ "wrap-150-short-box-no-elide", base(kLong), 150, 20 });
    c.append({ "nowrap-elide-right", [] { auto p = base(kLong); p.wrapMode = QTextOption::NoWrap;
                                          p.elide = Qt::ElideRight; return p; }(), 60, kAuto });
    c.append({ "nowrap-elide-fits", [] { auto p = base(QStringLiteral("Hi")); p.wrapMode = QTextOption::NoWrap;
                                         p.elide = Qt::ElideRight; return p; }(), 200, kAuto });
    c.append({ "wrap-elide-height-one-line", [] { auto p = base(kLong); p.elide = Qt::ElideRight; return p; }(),
               150, 20 });
    c.append({ "wrap-elide-height-two-lines", [] { auto p = base(kLong); p.elide = Qt::ElideRight; return p; }(),
               150, 40 });
    c.append({ "wrap-elide-newline", [] { auto p = base(QStringLiteral("first line\nsecond line\nthird"));
                                          p.elide = Qt::ElideRight; return p; }(), 200, 40 });
    c.append({ "lh-proportional-1.5", [] { auto p = base(kLong); p.lineHeight = 1.5; return p; }(), 150, kAuto });
    c.append({ "lh-proportional-1.5-single", [] { auto p = base(QStringLiteral("Hello")); p.lineHeight = 1.5;
                                                  return p; }(), 200, 40 });
    // Fractional offsets: lineHeightOffset() returns int (Qt truncates (1 - 1.25) * 22 = -5.5 to -5).
    c.append({ "lh-proportional-1.25", [] { auto p = base(kLong); p.lineHeight = 1.25; return p; }(), 150, kAuto });
    c.append({ "lh-proportional-0.9-box", [] { auto p = base(kLong); p.lineHeight = 0.9; return p; }(), 150, 90 });
    c.append({ "lh-fixed-30", [] { auto p = base(kLong); p.lineHeightMode = 1; p.lineHeight = 30; return p; }(),
               150, kAuto });
    c.append({ "lh-fixed-10-single", [] { auto p = base(QStringLiteral("Hello")); p.lineHeightMode = 1;
                                          p.lineHeight = 10; return p; }(), 200, kAuto });
    c.append({ "font-fractional-10.3pt", [] { auto p = base(kLong); p.font.setPointSizeF(10.3); return p; }(),
               150, kAuto });
    c.append({ "font-fractional-13.8pt", [] { auto p = base(QStringLiteral("Hello")); p.font.setPointSizeF(13.8);
                                              return p; }(), 200, kAuto });
    c.append({ "paddings", [] { auto p = base(kLong); p.leftPadding = 3; p.rightPadding = 7; p.topPadding = 5;
                                p.bottomPadding = 11; return p; }(), 150, kAuto });
    c.append({ "paddings-taller-box", [] { auto p = base(QStringLiteral("Hello")); p.leftPadding = 3;
                                           p.rightPadding = 7; p.topPadding = 5; p.bottomPadding = 11; return p; }(),
               200, 80 });
    c.append({ "align-left", base(QStringLiteral("Hello")), 300, 50 });
    c.append({ "align-center", [] { auto p = base(QStringLiteral("Hello")); p.horizontalAlignment = Qt::AlignHCenter;
                                    return p; }(), 300, 50 });
    c.append({ "align-right", [] { auto p = base(QStringLiteral("Hello")); p.horizontalAlignment = Qt::AlignRight;
                                   return p; }(), 300, 50 });
    c.append({ "align-center-wrapped", [] { auto p = base(kLong); p.horizontalAlignment = Qt::AlignHCenter;
                                            return p; }(), 150, kAuto });
    c.append({ "align-right-padded", [] { auto p = base(QStringLiteral("Hello")); p.horizontalAlignment = Qt::AlignRight;
                                          p.leftPadding = 4; p.rightPadding = 9; return p; }(), 300, 50 });
    c.append({ "mirrored-left", base(QStringLiteral("Hello")), 300, 50, true });
    c.append({ "mirrored-right", [] { auto p = base(QStringLiteral("Hello")); p.horizontalAlignment = Qt::AlignRight;
                                      return p; }(), 300, 50, true });
    c.append({ "mirrored-center", [] { auto p = base(QStringLiteral("Hello")); p.horizontalAlignment = Qt::AlignHCenter;
                                       return p; }(), 300, 50, true });
    c.append({ "multilength-9c-fits", base(QStringLiteral("Short\x9c" "S")), 200, kAuto });
    c.append({ "multilength-9c-elided", [] { auto p = base(QStringLiteral("A rather long label\x9c" "Shorter\x9c" "S"));
                                             p.wrapMode = QTextOption::NoWrap; p.elide = Qt::ElideRight; return p; }(),
               60, kAuto });
    c.append({ "multilength-9c-height", [] { auto p = base(kLong + QStringLiteral("\x9c" "Short one"));
                                             p.elide = Qt::ElideRight; return p; }(), 150, 20 });
    c.append({ "letter-spacing", [] { auto p = base(kLong); p.font.setLetterSpacing(QFont::AbsoluteSpacing, 2);
                                      return p; }(), 150, kAuto });
    c.append({ "word-spacing", [] { auto p = base(kLong); p.font.setWordSpacing(5); return p; }(), 150, kAuto });
    c.append({ "uppercase", [] { auto p = base(kLong); p.font.setCapitalization(QFont::AllUppercase); return p; }(),
               150, kAuto });
    c.append({ "bold-underline", [] { auto p = base(kLong); p.font.setWeight(QFont::Bold); p.font.setUnderline(true);
                                      return p; }(), 150, kAuto });
    c.append({ "rtl-hebrew", base(QStringLiteral(u"שלום עולם, מה שלומך היום")), 300, kAuto });
    c.append({ "rtl-arabic-wrapped", base(QStringLiteral(u"مرحبا بالعالم، كيف حالك اليوم يا صديقي العزيز")), 100,
               kAuto });
    c.append({ "rtl-hebrew-right", [] { auto p = base(QStringLiteral(u"שלום עולם"));
                                        p.horizontalAlignment = Qt::AlignRight; return p; }(), 300, 50 });
    c.append({ "fallback-emoji-cjk", base(QStringLiteral(u"Hi 😀 你好 世界 こんにちは")), 120, kAuto });
    c.append({ "elide-zero-avail-width", [] { auto p = base(kLong); p.elide = Qt::ElideRight; p.leftPadding = 10;
                                              p.rightPadding = 10; return p; }(), 15, 30 });
    return c;
}

Observed observe(QQuickText *label)
{
    Observed r;
    r.implicitWidth = label->implicitWidth();
    r.implicitHeight = label->implicitHeight();
    r.lineCount = label->lineCount();
    r.truncated = label->truncated();
    r.contentWidth = label->contentWidth();
    r.contentHeight = label->contentHeight();
    r.baselineOffset = label->baselineOffset();

    // QQuickText::updatePaintNode (qquicktext.cpp 2601-2660): what it adds, and where.
    QQuickTextPrivate *d = QQuickTextPrivate::get(label);
    if (!d->text.isEmpty() && d->layedOutTextRect.width() > 0) {
        const qreal dy = alignedY(d->layedOutTextRect.height() + d->lineHeightOffset(), d->availableHeight(),
                                  d->vAlign) + label->topPadding();
        const qreal dx = alignedX(d->lineWidth, d->availableWidth(), label->effectiveHAlign()) + label->leftPadding();
        int unelidedLineCount = d->lineCount;
        if (d->elideLayout)
            unelidedLineCount -= 1;
        if (unelidedLineCount > 0)
            appendLines(&r.lines, &d->layout, unelidedLineCount, QPointF(dx, dy));
        if (d->elideLayout)
            appendLines(&r.lines, d->elideLayout.get(), d->elideLayout->lineCount(), QPointF(dx, dy));
    }
    return r;
}

// The REAL QQuickText, configured exactly like CssText does it: verticalAlignment then the payload (in
// applyToText's order) between beginCreate and completeCreate; then CssText::layoutChild at mount (its own
// width still 0: the label takes its implicit size) and finally the box the layout engine assigns.
Observed oracle(QQmlEngine *engine, const Case &c, qreal *heightUsed, Observed *mounted)
{
    QQmlComponent comp(engine);
    comp.setData(c.mirrored ? QByteArray("import QtQuick\nText { LayoutMirroring.enabled: true }")
                            : QByteArray("import QtQuick\nText {}"),
                 QUrl(QStringLiteral("qrc:/oracle-text.qml")));
    QObject *o = comp.beginCreate(engine->rootContext());
    auto *label = qobject_cast<QQuickText *>(o);
    Q_ASSERT(label);
    const CssTextPayload &p = c.payload;
    auto put = [label](const char *name, const QVariant &v) {
        if (!label->setProperty(name, v))
            qFatal("oracle: cannot write Text.%s", name);
    };
    put("verticalAlignment", int(Qt::AlignVCenter));
    put("text", p.text);
    put("color", p.color);
    put("font", QVariant::fromValue(p.font));
    put("lineHeightMode", p.lineHeightMode);
    put("lineHeight", p.lineHeight);
    put("textFormat", p.textFormat);
    put("leftPadding", p.leftPadding);
    put("rightPadding", p.rightPadding);
    put("topPadding", p.topPadding);
    put("bottomPadding", p.bottomPadding);
    put("wrapMode", p.wrapMode);
    put("elide", p.elide);
    put("horizontalAlignment", p.horizontalAlignment);
    comp.completeCreate();
    QScopedPointer<QQuickText> owner(label);

    // CssText::layoutChild at mount (outer width/height 0) + mirrorImplicit.
    label->setX(0);
    label->setY(0);
    label->setWidth(label->implicitWidth());
    label->setHeight(label->implicitHeight());
    // The layout engine's box.
    label->setWidth(c.width);
    const qreal h = c.height == kAuto ? label->implicitHeight() : c.height;
    label->setHeight(h);
    *heightUsed = h;

    // Everything is read as CssText leaves the label; then a wrapMode round-trip (what a CssText re-apply of
    // white-space does) re-runs QQuickTextPrivate::updateLayout + updateSize at the same box. Only
    // baselineOffset may differ between the two — see the test. (forceLayout() would not do: it runs updateSize
    // alone, which re-measures a U+009C label's last abbreviation as if it were the whole text.)
    *mounted = observe(label);
    put("wrapMode", p.wrapMode == QTextOption::NoWrap ? int(QTextOption::WordWrap) : int(QTextOption::NoWrap));
    put("wrapMode", p.wrapMode);
    return observe(label);
}

Observed port(const Case &c, qreal height)
{
    const CssTextMetrics m = CssTextLayout::metrics(c.payload, c.width, height, c.mirrored);
    const CssTextBuiltLayout b = CssTextLayout::buildLayout(c.payload, c.width, height, c.mirrored);
    Observed r;
    r.implicitWidth = m.implicitWidth;
    r.implicitHeight = m.implicitHeight;
    r.lineCount = m.lineCount;
    r.truncated = m.truncated;
    r.contentWidth = m.contentWidth;
    r.contentHeight = m.contentHeight;
    r.baselineOffset = m.baselineOffset;
    if (!m.empty && m.layedOutTextRect.width() > 0) {
        if (b.layout && m.unelidedLineCount > 0)
            appendLines(&r.lines, b.layout.get(), m.unelidedLineCount, m.origin);
        if (m.elided && b.elideLayout)
            appendLines(&r.lines, b.elideLayout.get(), b.elideLayout->lineCount(), m.origin);
    }
    // buildLayout's metrics are the same computation.
    r.buildAgrees = b.metrics.implicitWidth == m.implicitWidth && b.metrics.implicitHeight == m.implicitHeight
        && b.metrics.lineCount == m.lineCount && b.metrics.origin == m.origin;
    return r;
}

// Exact comparison (no fuzzy compare: the port must run the same arithmetic).
bool compare(const Observed &got, const Observed &want, bool baseline)
{
    auto same = [](const char *what, auto g, auto w) {
        if (g == w)
            return true;
        qWarning().noquote() << QStringLiteral("%1: got %2 want %3").arg(QLatin1String(what))
            .arg(QString::number(double(g), 'g', 17), QString::number(double(w), 'g', 17));
        return false;
    };
    bool ok = same("implicitWidth", got.implicitWidth, want.implicitWidth)
        & same("implicitHeight", got.implicitHeight, want.implicitHeight)
        & same("lineCount", got.lineCount, want.lineCount)
        & same("truncated", got.truncated, want.truncated)
        & same("contentWidth", got.contentWidth, want.contentWidth)
        & same("contentHeight", got.contentHeight, want.contentHeight)
        & same("drawn lines", got.lines.size(), want.lines.size());
    if (baseline)
        ok &= same("baselineOffset", got.baselineOffset, want.baselineOffset);
    for (int i = 0; i < qMin(got.lines.size(), want.lines.size()); ++i) {
        if (!(got.lines[i] == want.lines[i])) {
            qWarning().noquote() << QStringLiteral("line %1: got %2 want %3")
                .arg(i).arg(describe(got.lines[i]), describe(want.lines[i]));
            ok = false;
        }
    }
    return ok;
}

} // namespace

void TextTests::leanTextMetricsMatchQQuickText_data()
{
    QTest::addColumn<int>("index");
    const QList<Case> all = cases();
    for (int i = 0; i < all.size(); ++i)
        QTest::newRow(all[i].name) << i;
}

void TextTests::leanTextMetricsMatchQQuickText()
{
    QFETCH(int, index);
    const Case c = cases().at(index);
    QQmlEngine engine;
    qreal h = 0;
    Observed mounted;
    const Observed settled = oracle(&engine, c, &h, &mounted);
    const Observed got = port(c, h);
    QVERIFY2(got.buildAgrees, "buildLayout().metrics differs from metrics()");

    // (1) Against the label exactly as CssText leaves it — every metric but baselineOffset. QQuickText's
    // baselineOffset is HISTORY-dependent: it is only refreshed by updateSize(), and (a) assigning a height equal
    // to the implicit one the item already follows is no geometry change (no relayout), so a label sized to its
    // implicit box keeps the baseline computed while its height was still invalid (no AlignVCenter shift);
    // (b) QQuickText::geometryChange returns early for empty text, so an empty label never re-centres it.
    // Nothing draws from it (updatePaintNode recomputes its offsets) and CssText does not read it.
    QVERIFY(compare(got, mounted, /*baseline*/ false));
    // (2) Against the same label once a property round-trip re-ran its layout at the same box: everything,
    // baseline too.
    QVERIFY(compare(got, settled, /*baseline*/ true));
}

QTEST_MAIN(TextTests)
