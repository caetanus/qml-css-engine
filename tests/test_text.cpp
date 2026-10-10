#include "test_text.h"

#include "qmlcss/QMLCss.h"
#include "qmlcss/csstext.h"
#include "qmlcss/csstextlayout.h"
#include "qmlcss/csstheme_p.h"

#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QScopeGuard>
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
    QString text; // the characters the line shows (an elided line: the elided string itself)
    bool operator==(const PlacedLine &o) const
    {
        return rect == o.rect && textStart == o.textStart && textLength == o.textLength && text == o.text;
    }
};

QString describe(const PlacedLine &l)
{
    return QStringLiteral("[%1,%2 %3x%4 text %5+%6 \"%7\"]")
        .arg(l.rect.x()).arg(l.rect.y()).arg(l.rect.width()).arg(l.rect.height())
        .arg(l.textStart).arg(l.textLength).arg(l.text);
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
        out->append({ line.naturalTextRect().translated(origin), line.textStart(), line.textLength(),
                      layout->text().mid(line.textStart(), line.textLength()) });
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
    // Elision (Task 1b: the port elides through public API only). "join" = a wrapped line elided together with
    // the next one because the box holds one line (qquicktext.cpp 919-941); "single" = the only line is too wide
    // (qquicktext.cpp 953).
    auto elided = [](QString text, int wrap) { auto p = base(text); p.wrapMode = wrap; p.elide = Qt::ElideRight;
                                               return p; };
    c.append({ "elide-join-letter-spacing", [&] { auto p = elided(kLong, QTextOption::WordWrap);
                                                  p.font.setLetterSpacing(QFont::AbsoluteSpacing, 2); return p; }(),
               150, 20 });
    c.append({ "elide-join-word-spacing", [&] { auto p = elided(kLong, QTextOption::WordWrap);
                                                p.font.setWordSpacing(5); return p; }(), 150, 20 });
    c.append({ "elide-join-uppercase", [&] { auto p = elided(kLong, QTextOption::WordWrap);
                                             p.font.setCapitalization(QFont::AllUppercase); return p; }(), 150, 20 });
    c.append({ "elide-join-rtl-hebrew", elided(QStringLiteral(u"שלום עולם, מה שלומך היום ומה שלום המשפחה"),
                                               QTextOption::WordWrap), 100, 20 });
    c.append({ "elide-join-rtl-arabic", elided(QStringLiteral(u"مرحبا بالعالم، كيف حالك اليوم يا صديقي العزيز"),
                                               QTextOption::WordWrap), 100, 20 });
    c.append({ "elide-single-rtl-hebrew", elided(QStringLiteral(u"שלום עולם, מה שלומך היום"), QTextOption::NoWrap),
               60, kAuto });
    c.append({ "elide-single-rtl-arabic", elided(QStringLiteral(u"مرحبا بالعالم، كيف حالك اليوم"),
                                                 QTextOption::NoWrap), 60, kAuto });
    c.append({ "elide-newline-one-line-box", elided(QStringLiteral("first\nsecond"), QTextOption::WordWrap), 200, 20 });
    // The joined range ends with the U+2028 the newline became: line 1 wraps, line 2 ends at the newline.
    c.append({ "elide-join-range-has-line-separator",
               elided(QStringLiteral("alpha beta delta\nsecond paragraph"), QTextOption::WordWrap), 90, 20 });
    c.append({ "elide-join-range-has-line-separator-tight", elided(QStringLiteral("ab cd\nx"), QTextOption::WordWrap),
               30, 20 });
    c.append({ "elide-single-letter-spacing", [&] { auto p = elided(kLong, QTextOption::NoWrap);
                                                    p.font.setLetterSpacing(QFont::AbsoluteSpacing, 2); return p; }(),
               60, kAuto });
    c.append({ "elide-single-word-spacing", [&] { auto p = elided(kLong, QTextOption::NoWrap);
                                                  p.font.setWordSpacing(5); return p; }(), 60, kAuto });
    c.append({ "elide-single-uppercase", [&] { auto p = elided(kLong, QTextOption::NoWrap);
                                               p.font.setCapitalization(QFont::AllUppercase); return p; }(), 60, kAuto });
    c.append({ "elide-single-fractional-13.8pt", [&] { auto p = elided(kLong, QTextOption::NoWrap);
                                                       p.font.setPointSizeF(13.8); return p; }(), 73.5, kAuto });
    // WordWrap, one unbreakable word wider than the box: a single line, elided by the 953 path.
    c.append({ "elide-single-unbreakable-word", elided(QStringLiteral("Supercalifragilisticexpialidocious"),
                                                       QTextOption::WordWrap), 90, kAuto });
    c.append({ "elide-single-emoji-cjk", elided(QStringLiteral(u"Hi 😀 你好 世界 こんにちは"), QTextOption::NoWrap),
               70, kAuto });
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

// The label exactly as QQuickText::componentComplete leaves it: the payload written before completion, no size
// ever assigned (it follows its implicit size).
Observed oracleMount(QQmlEngine *engine, const Case &c)
{
    QQmlComponent comp(engine);
    comp.setData(c.mirrored ? QByteArray("import QtQuick\nText { LayoutMirroring.enabled: true }")
                            : QByteArray("import QtQuick\nText {}"),
                 QUrl(QStringLiteral("qrc:/oracle-text-mount.qml")));
    QObject *o = comp.beginCreate(engine->rootContext());
    auto *label = qobject_cast<QQuickText *>(o);
    Q_ASSERT(label);
    const CssTextPayload &p = c.payload;
    label->setProperty("verticalAlignment", int(Qt::AlignVCenter));
    label->setProperty("text", p.text);
    label->setProperty("color", p.color);
    label->setProperty("font", QVariant::fromValue(p.font));
    label->setProperty("lineHeightMode", p.lineHeightMode);
    label->setProperty("lineHeight", p.lineHeight);
    label->setProperty("textFormat", p.textFormat);
    label->setProperty("leftPadding", p.leftPadding);
    label->setProperty("rightPadding", p.rightPadding);
    label->setProperty("topPadding", p.topPadding);
    label->setProperty("bottomPadding", p.bottomPadding);
    label->setProperty("wrapMode", p.wrapMode);
    label->setProperty("elide", p.elide);
    label->setProperty("horizontalAlignment", p.horizontalAlignment);
    comp.completeCreate();
    QScopedPointer<QQuickText> owner(label);
    return observe(label);
}

Observed portMount(const Case &c)
{
    const CssTextMetrics m = CssTextLayout::mountMetrics(c.payload, c.mirrored);
    const CssTextBuiltLayout b = CssTextLayout::buildMountLayout(c.payload, c.mirrored);
    Observed r;
    r.implicitWidth = m.implicitWidth;
    r.implicitHeight = m.implicitHeight;
    r.lineCount = m.lineCount;
    r.truncated = m.truncated;
    r.contentWidth = m.contentWidth;
    r.contentHeight = m.contentHeight;
    r.baselineOffset = m.baselineOffset;
    // Placed where the paint node adds it: at the label's current size (its implicit one).
    const QPointF origin = CssTextLayout::paintOrigin(m, c.payload, m.implicitWidth, m.implicitHeight);
    if (!m.empty && m.layedOutTextRect.width() > 0) {
        if (b.layout && m.unelidedLineCount > 0)
            appendLines(&r.lines, b.layout.get(), m.unelidedLineCount, origin);
        if (m.elided && b.elideLayout)
            appendLines(&r.lines, b.elideLayout.get(), b.elideLayout->lineCount(), origin);
    }
    r.buildAgrees = b.metrics.implicitWidth == m.implicitWidth && b.metrics.implicitHeight == m.implicitHeight
        && b.metrics.lineCount == m.lineCount && origin == m.origin;
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

void TextTests::leanTextMountMetricsMatchQQuickText_data()
{
    leanTextMetricsMatchQQuickText_data();
}

void TextTests::leanTextMountMetricsMatchQQuickText()
{
    QFETCH(int, index);
    const Case c = cases().at(index);
    QQmlEngine engine;
    const Observed want = oracleMount(&engine, c);
    const Observed got = portMount(c);
    QVERIFY2(got.buildAgrees, "buildMountLayout().metrics differs from mountMetrics()");
    QVERIFY(compare(got, want, /*baseline*/ true));
}

// --- Task 2: the lean CssText --------------------------------------------------------------------

namespace {

// Forces the composed (0) or lean (1) path for the CssTexts created while it lives.
struct LeanTextMode {
    int previous;
    explicit LeanTextMode(int mode) : previous(Detail::g_leanTextOverride) { Detail::g_leanTextOverride = mode; }
    ~LeanTextMode() { Detail::g_leanTextOverride = previous; }
};

// A theme + layout engine + QML engine wired the way the app wires them.
struct Scene {
    CssTheme theme;
    CssLayoutEngine layout{ &theme };
    QQmlEngine engine;
    QQuickWindow window;
    QScopedPointer<QObject> root;

    explicit Scene(const QString &css, QSize size = QSize(320, 120))
    {
        theme.loadFromString(css);
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);
        window.resize(size);
        window.setColor(Qt::white);
    }
    QString mount(const QByteArray &qml)
    {
        QQmlComponent c(&engine);
        c.setData(qml, QUrl(QStringLiteral("qrc:/lean-text-test.qml")));
        root.reset(c.create());
        if (!root)
            return c.errorString();
        qobject_cast<QQuickItem *>(root.data())->setParentItem(window.contentItem());
        return QString();
    }
    bool show()
    {
        window.show();
        return QTest::qWaitForWindowExposed(&window);
    }
    QImage grab()
    {
        QCoreApplication::processEvents();
        return window.grabWindow();
    }
    template <typename T = QQuickItem> T *find(const char *name) const
    {
        return root ? root->findChild<T *>(QLatin1String(name)) : nullptr;
    }
};

QQuickItem *composedLabel(QQuickItem *cssText)
{
    for (QQuickItem *k : cssText->childItems())
        if (QByteArray(k->metaObject()->className()).contains("QQuickText"))
            return k;
    return nullptr;
}

QQuickItem *childOfClass(QQuickItem *item, const char *cls)
{
    for (QQuickItem *k : item->childItems())
        if (QByteArray(k->metaObject()->className()).contains(cls))
            return k;
    return nullptr;
}

const char *kTextCss = R"(#t { color: #202020; font-family: "Noto Sans"; font-size: 14px; })";

QImage renderText(const QString &css, const QByteArray &qml, int mode, qreal resizeWidth, QString *error)
{
    LeanTextMode forced(mode);
    Scene scene(QLatin1String(kTextCss) + css);
    *error = scene.mount(qml);
    if (!error->isEmpty())
        return QImage();
    if (!scene.show()) {
        *error = QStringLiteral("window not exposed");
        return QImage();
    }
    if (resizeWidth >= 0) {
        scene.grab(); // the first frame, then the resize (a live label, not a mount)
        if (auto *t = scene.find("t"))
            t->setWidth(resizeWidth);
    }
    auto *t = scene.find("t");
    if (!t) {
        *error = QStringLiteral("no #t");
        return QImage();
    }
    const bool composed = composedLabel(t) != nullptr;
    if (composed != (mode == 0)) {
        *error = QStringLiteral("path: composed=%1 for mode %2").arg(composed).arg(mode);
        return QImage();
    }
    return scene.grab();
}

QByteArray textQml(const QByteArray &props, const QByteArray &rootProps = QByteArray())
{
    return "import QtQuick\nimport qmlcss\nItem { width: 320; height: 120; " + rootProps
        + "\n CssText { objectName: \"t\"; cssId: \"t\"; x: 7; y: 5; " + props + " } }";
}

} // namespace

void TextTests::initTestCase()
{
    QmlCss::registerTypes();
}

void TextTests::leanTextRendersLikeQQuickText_data()
{
    QTest::addColumn<QString>("css");
    QTest::addColumn<QByteArray>("qml");
    QTest::addColumn<qreal>("resizeWidth");
    const QByteArray longText = "text: \"The quick brown fox jumps over the lazy dog, again and again\"; ";
    QTest::newRow("plain") << QString() << textQml("width: 200; height: 30; text: \"Hello world\"") << -1.0;
    QTest::newRow("empty") << QString() << textQml("width: 200; height: 30; text: \"\"") << -1.0;
    QTest::newRow("unmanaged") << QString() << textQml("text: \"Hello world\"") << -1.0;
    QTest::newRow("two-lines") << QString() << textQml("text: \"first\\nsecond\"") << -1.0;
    QTest::newRow("wrap-150") << QString() << textQml("width: 150; " + longText) << -1.0;
    QTest::newRow("wrap-elide-height") << QStringLiteral("#t { text-overflow: ellipsis; }")
                                       << textQml("width: 150; height: 20; " + longText) << -1.0;
    QTest::newRow("nowrap-ellipsis") << QStringLiteral("#t { white-space: nowrap; text-overflow: ellipsis; }")
                                     << textQml("width: 60; height: 24; " + longText) << -1.0;
    QTest::newRow("lh-1.5") << QStringLiteral("#t { line-height: 1.5; }") << textQml("width: 150; " + longText)
                            << -1.0;
    QTest::newRow("lh-30px") << QStringLiteral("#t { line-height: 30px; }") << textQml("width: 150; " + longText)
                             << -1.0;
    QTest::newRow("fractional-size") << QStringLiteral("#t { font-size: 13.7px; }")
                                     << textQml("width: 150; " + longText) << -1.0;
    QTest::newRow("paddings") << QStringLiteral("#t { padding: 5px 7px 11px 3px; }")
                              << textQml("width: 150; " + longText) << -1.0;
    QTest::newRow("align-center") << QStringLiteral("#t { text-align: center; }")
                                  << textQml("width: 300; height: 50; text: \"Hello\"") << -1.0;
    QTest::newRow("align-right") << QStringLiteral("#t { text-align: right; }")
                                 << textQml("width: 300; height: 50; text: \"Hello\"") << -1.0;
    QTest::newRow("align-center-wrapped") << QStringLiteral("#t { text-align: center; }")
                                          << textQml("width: 150; " + longText) << -1.0;
    QTest::newRow("mirrored") << QString()
                              << textQml("width: 300; height: 50; text: \"Hello\"",
                                         "LayoutMirroring.enabled: true; LayoutMirroring.childrenInherit: true")
                              << -1.0;
    QTest::newRow("multilength-9c") << QStringLiteral("#t { white-space: nowrap; text-overflow: ellipsis; }")
                                    << textQml("width: 60; height: 24; text: \"A rather long label\\u009cShorter\\u009cS\"")
                                    << -1.0;
    QTest::newRow("letter-spacing") << QStringLiteral("#t { letter-spacing: 2px; }")
                                    << textQml("width: 150; " + longText) << -1.0;
    QTest::newRow("uppercase") << QStringLiteral("#t { text-transform: uppercase; }")
                               << textQml("width: 150; " + longText) << -1.0;
    QTest::newRow("bold-underline") << QStringLiteral("#t { font-weight: bold; text-decoration: underline; }")
                                    << textQml("width: 150; " + longText) << -1.0;
    QTest::newRow("rtl") << QString() << textQml("width: 300; text: \"שלום עולם\"")
                         << -1.0;
    QTest::newRow("fallback-emoji-cjk") << QString()
                                        << textQml("width: 120; text: \"Hi \U0001F600 你好 世界\"")
                                        << -1.0;
    QTest::newRow("resize-center") << QStringLiteral("#t { text-align: center; }")
                                   << textQml("width: 200; height: 30; text: \"Hello\"") << 260.0;
    QTest::newRow("resize-wrapped-narrower") << QString() << textQml("width: 300; " + longText) << 150.0;
    QTest::newRow("resize-wrapped-wider") << QString() << textQml("width: 150; " + longText) << 300.0;
    QTest::newRow("resize-elided") << QStringLiteral("#t { white-space: nowrap; text-overflow: ellipsis; }")
                                   << textQml("width: 60; height: 24; " + longText) << 120.0;
}

void TextTests::leanTextRendersLikeQQuickText()
{
    QFETCH(QString, css);
    QFETCH(QByteArray, qml);
    QFETCH(qreal, resizeWidth);
    QString error;
    const QImage composed = renderText(css, qml, 0, resizeWidth, &error);
    QVERIFY2(!composed.isNull(), qPrintable(error));
    const QImage lean = renderText(css, qml, 1, resizeWidth, &error);
    QVERIFY2(!lean.isNull(), qPrintable(error));
    QCOMPARE(lean.size(), composed.size());
    if (lean != composed) {
        const QImage a = composed.convertToFormat(QImage::Format_ARGB32);
        const QImage b = lean.convertToFormat(QImage::Format_ARGB32);
        QRect box;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x)
                if (a.pixel(x, y) != b.pixel(x, y))
                    box |= QRect(x, y, 1, 1);
        const QString base = QDir::tempPath() + QStringLiteral("/lean-text-") + QLatin1String(QTest::currentDataTag());
        a.save(base + QStringLiteral("-composed.png"));
        b.save(base + QStringLiteral("-lean.png"));
        QFAIL(qPrintable(QStringLiteral("lean differs from composed in %1,%2 %3x%4 (see %5-*.png)")
                             .arg(box.x()).arg(box.y()).arg(box.width()).arg(box.height()).arg(base)));
    }
}

void TextTests::leanTextLaysOutOnce()
{
    LeanTextMode forced(1);
    Scene scene(QStringLiteral(".col { display: flex; flex-direction: column; } "
                               ".col text { color: #202020; font-family: \"Noto Sans\"; font-size: 12px; }"),
                QSize(300, 2200));
    QByteArray qml = "import QtQuick\nimport qmlcss\nCssRect { objectName: \"col\"; cssClass: [\"col\"]; "
                     "cssPrimitive: \"div\"; width: 300; height: 2200\n";
    for (int i = 0; i < 100; ++i)
        qml += "CssText { cssPrimitive: \"text\"; text: \"row " + QByteArray::number(i) + "\" }\n";
    qml += "}";

    const int gui0 = Detail::g_textGuiLayouts, render0 = Detail::g_textRenderLayouts, nodes0 = Detail::g_textNodeBuilds;
    QVERIFY2(scene.mount(qml).isEmpty(), "mount failed");
    QVERIFY(scene.show());
    scene.grab();

    const QList<CssText *> texts = scene.root->findChildren<CssText *>();
    QCOMPARE(texts.size(), 100);
    for (CssText *t : texts) {
        QVERIFY2(!composedLabel(t), "a plain text composed a QQuickText");
        QVERIFY(t->width() > 0 && t->height() > 0);
    }
    const bool publicHandoff = Detail::leanTextHandoff() == Detail::LeanTextHandoff::Public;
    const int gui = Detail::g_textGuiLayouts - gui0, render = Detail::g_textRenderLayouts - render0;
    const int nodes = Detail::g_textNodeBuilds - nodes0;
    qInfo("100 texts (%s handoff): %d GUI layouts, %d render-thread layouts, %d node builds",
          publicHandoff ? "public" : "private", gui, render, nodes);
    QVERIFY2(gui <= 100, qPrintable(QString::number(gui)));
    QCOMPARE(render, publicHandoff ? 100 : 0);
    QCOMPARE(nodes, 100);

    // A pure move: the container and one text move; nothing is laid out or rebuilt.
    const int gui1 = Detail::g_textGuiLayouts, render1 = Detail::g_textRenderLayouts, nodes1 = Detail::g_textNodeBuilds;
    auto *col = qobject_cast<QQuickItem *>(scene.root.data());
    col->setX(col->x() + 13);
    col->setY(col->y() + 5);
    texts.first()->setX(texts.first()->x() + 3);
    scene.grab();
    QCOMPARE(Detail::g_textGuiLayouts - gui1, 0);
    QCOMPARE(Detail::g_textRenderLayouts - render1, 0);
    QCOMPARE(Detail::g_textNodeBuilds - nodes1, 0);
}

namespace {
QStringList *g_warnings = nullptr;
QtMessageHandler g_previousHandler = nullptr;
void recordWarnings(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (type != QtDebugMsg && type != QtInfoMsg && g_warnings)
        g_warnings->append(msg);
    if (g_previousHandler)
        g_previousHandler(type, ctx, msg);
}
} // namespace

void TextTests::leanTextFallbackSwitch()
{
    QStringList warnings;
    g_warnings = &warnings;
    g_previousHandler = qInstallMessageHandler(recordWarnings);
    auto restore = qScopeGuard([] { qInstallMessageHandler(g_previousHandler); g_warnings = nullptr; });

    LeanTextMode forced(1);
    Scene scene(QLatin1String(kTextCss) + QStringLiteral(" .sh { text-shadow: 1px 1px 2px #000000; }"));
    QVERIFY(scene.mount(textQml("width: 200; height: 30; text: \"Switch <b>me</b>\"; styledText: true")).isEmpty());
    QVERIFY(scene.show());
    auto *t = scene.find<CssText>("t");
    QVERIFY(t);
    scene.grab();

    // styledText composes the QQuickText.
    QVERIFY(composedLabel(t));
    QVERIFY(!(t->flags() & QQuickItem::ItemHasContents));
    // ... dropping it goes lean: no label left, the item paints.
    t->setStyledText(false);
    scene.grab();
    QVERIFY(!composedLabel(t));
    QVERIFY(t->flags() & QQuickItem::ItemHasContents);
    // ... and back.
    t->setStyledText(true);
    scene.grab();
    QVERIFY(composedLabel(t));
    QVERIFY(!(t->flags() & QQuickItem::ItemHasContents));
    t->setStyledText(false);
    scene.grab();
    QVERIFY(!composedLabel(t));

    // text-shadow likewise (a MultiEffect sources the label).
    t->setProperty("cssClass", QStringList{ QStringLiteral("sh") });
    scene.grab();
    QVERIFY(composedLabel(t));
    QVERIFY(childOfClass(t, "MultiEffect"));
    t->setProperty("cssClass", QStringList());
    scene.grab();
    QVERIFY(!composedLabel(t));
    QVERIFY(!childOfClass(t, "MultiEffect"));
    QVERIFY(t->flags() & QQuickItem::ItemHasContents);
    t->setProperty("cssClass", QStringList{ QStringLiteral("sh") });
    scene.grab();
    QVERIFY(composedLabel(t));
    QVERIFY(childOfClass(t, "MultiEffect"));
    t->setProperty("cssClass", QStringList());
    scene.grab();
    QVERIFY(!composedLabel(t));
    scene.root.reset();
    scene.grab();

    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join(QLatin1Char('\n'))));
}

void TextTests::leanTextBackgroundSizedWithoutLabel()
{
    LeanTextMode forced(1);
    Scene scene(QLatin1String(kTextCss) + QStringLiteral(" .bg { background-color: #ff0000; }"));
    QVERIFY(scene.mount("import QtQuick\nimport qmlcss\nItem { width: 320; height: 120\n"
                        " CssText { objectName: \"t\"; cssId: \"t\"; cssClass: [\"bg\"]; x: 7; y: 5; width: 120; "
                        "height: 40; text: \"Hi\" }\n"
                        " CssText { objectName: \"u\"; cssId: \"t\"; cssClass: [\"bg\"]; x: 7; y: 60; text: \"Hi\" } }")
                .isEmpty());
    QVERIFY(scene.show());
    auto *t = scene.find("t");
    auto *u = scene.find("u");
    QVERIFY(t && u);
    QVERIFY(!composedLabel(t) && !composedLabel(u));
    auto *bg = t->findChild<QQuickItem *>(QStringLiteral("cssTextBg"));
    QVERIFY2(bg, "no background underlay");
    QCOMPARE(QSizeF(bg->width(), bg->height()), QSizeF(120, 40));
    // Unmanaged: the box is the text's implicit size.
    auto *ubg = u->findChild<QQuickItem *>(QStringLiteral("cssTextBg"));
    QVERIFY(ubg);
    QVERIFY(u->implicitWidth() > 0 && u->implicitHeight() > 0);
    QCOMPARE(QSizeF(ubg->width(), ubg->height()), QSizeF(u->implicitWidth(), u->implicitHeight()));
    // A resize follows.
    t->setWidth(150);
    t->setHeight(44);
    QCOMPARE(QSizeF(bg->width(), bg->height()), QSizeF(150, 44));
    const QImage img = scene.grab();
    QCOMPARE(QColor(img.pixel(7 + 2, 5 + 2)), QColor(Qt::red));
    QCOMPARE(QColor(img.pixel(7 + 148, 5 + 42)), QColor(Qt::red));
}

void TextTests::leanTextFontRevision()
{
    const QString family = QStringLiteral("SqTest Serif");
    if (QFontDatabase::families().contains(family))
        QSKIP("the test font is already registered");
    const QString fontPath = QStringLiteral(SQ_TEST_DATA_DIR "/SqTestSerif-Regular.ttf");
    QVERIFY2(QFile::exists(fontPath), qPrintable(fontPath));

    LeanTextMode forced(1);
    const QString css = QStringLiteral(R"(#t { color: #202020; font-family: "SqTest Serif", "Noto Sans"; font-size: 20px; })");
    const QByteArray qml = textQml("width: 300; height: 40; text: \"Glyphs change\"");
    Scene scene(css);
    QVERIFY(scene.mount(qml).isEmpty());
    QVERIFY(scene.show());
    const QImage before = scene.grab();

    const int nodes0 = Detail::g_textNodeBuilds;
    const int revision0 = scene.theme.fontRevision();
    scene.theme.loadFromString(css + QStringLiteral(" @font-face { font-family: \"SqTest Serif\"; src: url(\"%1\"); }")
                                         .arg(fontPath));
    QCOMPARE(scene.theme.fontRevision(), revision0 + 1);
    const QImage after = scene.grab();
    QVERIFY2(Detail::g_textNodeBuilds > nodes0, "the glyph node was not rebuilt");
    QVERIFY2(after != before, "the registered font did not change the glyphs");

    // No stale font: a text mounted with the font already registered draws the same pixels.
    Scene fresh(css);
    QVERIFY(fresh.mount(qml).isEmpty());
    QVERIFY(fresh.show());
    QCOMPARE(fresh.grab(), after);
}

QTEST_MAIN(TextTests)
