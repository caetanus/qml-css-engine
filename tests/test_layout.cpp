#include "test_layout.h"

#include "qmlcss/QMLCss.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QScopedPointer>
#include <QTest>

using namespace QmlCss;

namespace {

// A self-contained scene: theme + layout engine + a CssRect container built from QML
// source, children styled purely from CSS. Layout passes are coalesced per event-loop
// turn, so geometry asserts below use QTRY_* to let the pass run.
struct Scene {
    CssTheme theme;
    CssLayoutEngine layout{&theme};
    QQmlEngine engine;
    QScopedPointer<QQuickItem> root;

    Scene(const QString &css, const QString &qmlBody)
    {
        theme.loadFromString(css);
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);
        QQmlComponent component(&engine);
        component.setData(QByteArray("import QtQuick\nimport qmlcss\n") + qmlBody.toUtf8(),
                          QUrl(QStringLiteral("qrc:/layout-test.qml")));
        root.reset(qobject_cast<QQuickItem *>(component.create()));
        if (!root)
            qWarning() << component.errorString();
    }

    QQuickItem *kid(const char *objectName) const
    {
        return root ? root->findChild<QQuickItem *>(QLatin1String(objectName)) : nullptr;
    }
};

// Container with three children a/b/c; per-test CSS drives everything else.
QString threeKids()
{
    return QStringLiteral(R"(
        CssRect {
            width: 300; height: 100
            cssId: "c"
            CssRect { objectName: "a"; cssId: "a" }
            CssRect { objectName: "b"; cssId: "b" }
            CssRect { objectName: "k"; cssId: "k" }
        }
    )");
}

} // namespace

void LayoutTests::initTestCase()
{
    QmlCss::registerTypes();
}

void LayoutTests::flexRowPlacesAndGaps()
{
    Scene s(QStringLiteral("#c { display: flex; flex-direction: row; gap: 10px; }"
                           "#a, #b, #k { width: 50px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("a")->x(), 0.0);
    QTRY_COMPARE(s.kid("b")->x(), 60.0);
    QTRY_COMPARE(s.kid("k")->x(), 120.0);
    // align-items defaults to stretch: unsized cross axis fills the container.
    QTRY_COMPARE(s.kid("a")->height(), 100.0);
}

void LayoutTests::flexJustifySpaceBetween()
{
    Scene s(QStringLiteral("#c { display: flex; justify-content: space-between; }"
                           "#a, #b, #k { width: 50px; height: 10px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("a")->x(), 0.0);
    QTRY_COMPARE(s.kid("b")->x(), 125.0);
    QTRY_COMPARE(s.kid("k")->x(), 250.0);
}

void LayoutTests::flexColumnCentersCrossAxis()
{
    Scene s(QStringLiteral("#c { display: flex; flex-direction: column; align-items: center; }"
                           "#a, #b, #k { width: 50px; height: 20px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("a")->x(), 125.0); // (300 - 50) / 2
    QTRY_COMPARE(s.kid("b")->y(), 20.0);
    QTRY_COMPARE(s.kid("k")->y(), 40.0);
}

void LayoutTests::flexGrowAbsorbsRemainder()
{
    Scene s(QStringLiteral("#c { display: flex; }"
                           "#a { width: 50px; height: 10px; }"
                           "#b { flex-grow: 1; height: 10px; }"
                           "#k { width: 50px; height: 10px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("b")->width(), 200.0); // 300 - 50 - 50
    QTRY_COMPARE(s.kid("k")->x(), 250.0);
}

void LayoutTests::gridFrTracksSplitWidth()
{
    Scene s(QStringLiteral("#c { display: grid; grid-template-columns: 1fr 2fr; }"
                           "#a, #b { height: 10px; }"),
            QStringLiteral(R"(
                CssRect {
                    width: 300; height: 100
                    cssId: "c"
                    CssRect { objectName: "a"; cssId: "a" }
                    CssRect { objectName: "b"; cssId: "b" }
                }
            )"));
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("a")->width(), 100.0);
    QTRY_COMPARE(s.kid("b")->width(), 200.0);
    QTRY_COMPARE(s.kid("b")->x(), 100.0);
}

void LayoutTests::gridRepeatWrapsRows()
{
    Scene s(QStringLiteral("#c { display: grid; grid-template-columns: repeat(2, 100px); "
                           "     row-gap: 8px; }"
                           "#a, #b, #k { height: 10px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("b")->x(), 100.0);
    // Third item wraps to the second row, first column.
    QTRY_COMPARE(s.kid("k")->x(), 0.0);
    QTRY_VERIFY(s.kid("k")->y() >= 18.0); // 10px row + 8px gap
}

void LayoutTests::paddingInsetsContent()
{
    Scene s(QStringLiteral("#c { display: flex; padding: 10px; }"
                           "#a, #b, #k { width: 20px; height: 20px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("a")->x(), 10.0);
    QTRY_COMPARE(s.kid("a")->y(), 10.0);
}

void LayoutTests::borderInsetsContentBox()
{
    // The content holder is the PADDING BOX (web box model): children live inside the border, the
    // content area shrinks by the border widths, and an auto-sized box's implicit size includes the
    // border on both axes. Child x/y stays padding-relative — the holder absorbs the border offset.
    Scene s(QStringLiteral("#c { display: flex; padding: 4px; border: 3px solid #000; }"
                           "#a, #b, #k { width: 20px; height: 20px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("a")->x(), 4.0);
    QTRY_COMPARE(s.kid("a")->mapToItem(s.root.data(), 0, 0).x(), 7.0); // border 3 + padding 4
    QTRY_COMPARE(s.kid("a")->mapToItem(s.root.data(), 0, 0).y(), 7.0);

    // Auto-sized box: implicit = content + padding + border.
    Scene s2(QStringLiteral("#c { display: flex; padding: 4px; border: 3px solid #000; }"
                            "#a { width: 20px; height: 20px; }"),
             QStringLiteral(R"(
                 CssRect {
                     cssId: "c"
                     CssRect { objectName: "a"; cssId: "a" }
                 }
             )"));
    QVERIFY(s2.root);
    QTRY_COMPARE(s2.root->implicitWidth(), 34.0);  // 20 + 4+4 + 3+3
    QTRY_COMPARE(s2.root->implicitHeight(), 34.0);
}

void LayoutTests::childMarginOffsets()
{
    Scene s(QStringLiteral("#c { display: flex; }"
                           "#a { width: 20px; height: 20px; margin-left: 5px; }"
                           "#b, #k { width: 20px; height: 20px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("a")->x(), 5.0);
    QTRY_COMPARE(s.kid("b")->x(), 25.0);
}

void LayoutTests::percentAndCalcWidths()
{
    Scene s(QStringLiteral("#c { display: flex; flex-direction: column; }"
                           "#a { width: 50%; height: 10px; }"
                           "#b { width: calc(100% - 40px); height: 10px; }"
                           "#k { width: 10px; height: 10px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("a")->width(), 150.0);
    QTRY_COMPARE(s.kid("b")->width(), 260.0);
}

void LayoutTests::viewportUnitsResolve()
{
    Scene s(QStringLiteral("#c { display: flex; }"
                           "#a { width: 10vw; height: 10vh; }"
                           "#b, #k { width: 1px; height: 1px; }"),
            threeKids());
    QVERIFY(s.root);
    s.theme.setViewport(1000, 500);
    QTRY_COMPARE(s.kid("a")->width(), 100.0);
    QTRY_COMPARE(s.kid("a")->height(), 50.0);
}

void LayoutTests::aspectRatioDerivesHeight()
{
    Scene s(QStringLiteral("#c { display: flex; }"
                           "#a { width: 100px; aspect-ratio: 2; }"
                           "#b, #k { width: 1px; height: 1px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("a")->height(), 50.0);
}

void LayoutTests::absoluteAndInsetPlacement()
{
    Scene s(QStringLiteral("#c { display: flex; }"
                           "#a { position: absolute; left: 20px; top: 30px; width: 10px; height: 10px; }"
                           "#b { position: absolute; inset: 5px; }"
                           "#k { width: 40px; height: 10px; }"),
            threeKids());
    QVERIFY(s.root);
    QTRY_COMPARE(s.kid("a")->x(), 20.0);
    QTRY_COMPARE(s.kid("a")->y(), 30.0);
    // inset: 5px pins all four edges → size follows the container minus 10.
    QTRY_COMPARE(s.kid("b")->width(), 290.0);
    QTRY_COMPARE(s.kid("b")->height(), 90.0);
    // absolute children leave the flow: the flow child starts at the origin.
    QTRY_COMPARE(s.kid("k")->x(), 0.0);
}

// --- golden flex geometry (step 4b-3 Task 1) --------------------------------------------

namespace {

// A child, styled directly via an inline `style` object literal (no theme/CSS cascade — the
// same idiom test_qml_css.cpp's flex unit tests use), so every case is self-contained and the
// geometry below depends on nothing but layoutFlex/place/clampSize/placeAbsolute.
QByteArray goldenKid(const char *tag, const char *name, const QByteArray &styleBody)
{
    return QByteArray(tag) + " { objectName: \"" + name + "\"; cssPrimitive: \""
        + (QByteArray(tag) == "CssText" ? "text" : "") + "\"; style: (" + styleBody + ") }\n";
}

QByteArray goldenScene(const QByteArray &containerStyle, double cw, double ch, const QByteArray &kidsQml)
{
    return QByteArray("import QtQuick\nimport qmlcss\nCssRect {\n    cssPrimitive: \"\"\n    width: ")
        + QByteArray::number(cw) + "; height: " + QByteArray::number(ch) + "\n    style: ("
        + containerStyle + ")\n" + kidsQml + "}\n";
}

// The CssRect/CssFill contentHolder that owns `child` — its immediate parent item (same helper
// as test_qml_css.cpp's contentHolderOf, duplicated here since that one is file-local).
QQuickItem *goldenContentHolderOf(QQuickItem *child)
{
    return child ? child->parentItem() : nullptr;
}

struct Geom { double x, y, w, h; };

struct FlexCase {
    const char *name;
    QByteArray containerStyle;
    // Default shape: three CssRect children "a"/"b"/"k", one style body each. Leave all three
    // empty and set `extraQml` instead for a case that needs a different tree (text primitive,
    // mixed flow+absolute).
    QByteArray styleA, styleB, styleK;
    double cw = 300, ch = 100;
    double vw = 0, vh = 0; // 0 => leave the theme's default viewport alone
    QByteArray extraQml;   // non-empty overrides the default a/b/k children entirely
    QVector<QByteArray> kids = { "a", "b", "k" };
    QVector<Geom> expect;
    // Theme CSS rules (cssId selectors), for the one case (text primitive) that cannot use an
    // inline `style` object directly: a NON-empty cssPrimitive gives CssBox::hasCssIdentity()
    // true, so CssText's componentComplete calls theme->loadCss() and OVERWRITES the inline
    // style with whatever the theme resolves (empty, if none loaded) — exactly why the plain
    // CssRect cases above all set cssPrimitive:"" (identity-less, loadCss never fires, the
    // inline style stands). Here the primitive must stay "text" for isTextPrimitive() to see
    // it, so the width is supplied through the real cascade instead.
    QByteArray themeCss;
};

} // namespace

void LayoutTests::flexGeometryGolden()
{
    const QVector<FlexCase> cases = {
        // --- direction -------------------------------------------------------------------
        { "row_default", R"({"display":"flex"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 100, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 50, 20 }, { 100, 0, 50, 20 } } },
        { "column_default", R"({"display":"flex","flex-direction":"column"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 100, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 0, 20, 50, 20 }, { 0, 40, 50, 20 } } },

        // --- wrap (default align-content == stretch) -------------------------------------
        { "wrap_overflow", R"({"display":"flex","flex-wrap":"wrap"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 120, 200, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 50, 20 }, { 0, 100, 50, 20 } } },

        // --- justify-content ---------------------------------------------------------------
        { "justify_flex_start", R"({"display":"flex","justify-content":"flex-start"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 60, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 50, 20 }, { 100, 0, 50, 20 } } },
        { "justify_center", R"({"display":"flex","justify-content":"center"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 60, 0, 0, {}, { "a", "b", "k" },
          { { 75, 0, 50, 20 }, { 125, 0, 50, 20 }, { 175, 0, 50, 20 } } },
        { "justify_flex_end", R"({"display":"flex","justify-content":"flex-end"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 60, 0, 0, {}, { "a", "b", "k" },
          { { 150, 0, 50, 20 }, { 200, 0, 50, 20 }, { 250, 0, 50, 20 } } },
        { "justify_end", R"({"display":"flex","justify-content":"end"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 60, 0, 0, {}, { "a", "b", "k" },
          { { 150, 0, 50, 20 }, { 200, 0, 50, 20 }, { 250, 0, 50, 20 } } },
        { "justify_space_between", R"({"display":"flex","justify-content":"space-between"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 60, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 125, 0, 50, 20 }, { 250, 0, 50, 20 } } },
        { "justify_space_around", R"({"display":"flex","justify-content":"space-around"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 60, 0, 0, {}, { "a", "b", "k" },
          { { 25, 0, 50, 20 }, { 125, 0, 50, 20 }, { 225, 0, 50, 20 } } },
        { "justify_space_evenly", R"({"display":"flex","justify-content":"space-evenly"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 60, 0, 0, {}, { "a", "b", "k" },
          { { 37.5, 0, 50, 20 }, { 125, 0, 50, 20 }, { 212.5, 0, 50, 20 } } },
        { "justify_bogus", R"({"display":"flex","justify-content":"not-a-real-value"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 60, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 50, 20 }, { 100, 0, 50, 20 } } },

        // --- align-items (no explicit height so stretch/center/end are all observable) ----
        { "align_stretch", R"({"display":"flex","align-items":"stretch"})",
          R"({"width":"50px"})", R"({"width":"50px"})", R"({"width":"50px"})", 300, 100, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 100 }, { 50, 0, 50, 100 }, { 100, 0, 50, 100 } } },
        { "align_center", R"({"display":"flex","align-items":"center"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 100, 0, 0, {}, { "a", "b", "k" },
          { { 0, 40, 50, 20 }, { 50, 40, 50, 20 }, { 100, 40, 50, 20 } } },
        { "align_flex_end", R"({"display":"flex","align-items":"flex-end"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 100, 0, 0, {}, { "a", "b", "k" },
          { { 0, 80, 50, 20 }, { 50, 80, 50, 20 }, { 100, 80, 50, 20 } } },
        { "align_flex_start", R"({"display":"flex","align-items":"flex-start"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 100, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 50, 20 }, { 100, 0, 50, 20 } } },
        { "align_bogus", R"({"display":"flex","align-items":"not-a-real-value"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 100, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 50, 20 }, { 100, 0, 50, 20 } } },
        { "align_self_override", R"({"display":"flex","align-items":"flex-start"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px","align-self":"center"})",
          R"({"width":"50px","height":"20px"})", 300, 100, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 40, 50, 20 }, { 100, 0, 50, 20 } } },

        // --- flex-grow / the `flex` shorthand ----------------------------------------------
        { "flex_grow", R"({"display":"flex"})",
          R"({"width":"50px","height":"20px"})", R"({"flex-grow":"1","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 200, 20 }, { 250, 0, 50, 20 } } },
        { "flex_shorthand_1", R"({"display":"flex"})",
          R"({"width":"50px","height":"20px"})", R"({"flex":"1","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 200, 20 }, { 250, 0, 50, 20 } } },
        { "flex_shorthand_1_0", R"({"display":"flex"})",
          R"({"width":"50px","height":"20px"})", R"({"flex":"1 0","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 200, 20 }, { 250, 0, 50, 20 } } },
        // 3rd token ("10px") is NOT read as flex-basis by today's engine — only grow+shrink are
        // parsed from the shorthand; this pins that today the token is silently ignored.
        { "flex_shorthand_2_1_10px", R"({"display":"flex"})",
          R"({"width":"30px","height":"20px"})", R"({"flex":"2 1 10px","height":"20px"})",
          R"({"width":"70px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 30, 20 }, { 30, 0, 200, 20 }, { 230, 0, 70, 20 } } },

        // --- flex-shrink on overflow ---------------------------------------------------------
        { "flex_shrink_overflow", R"({"display":"flex"})",
          R"({"width":"100px","height":"20px","flex-shrink":"1"})",
          R"({"width":"100px","height":"20px","flex-shrink":"2"})",
          R"({"width":"100px","height":"20px","flex-shrink":"0"})", 150, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 0, 20 }, { 50, 0, 100, 20 } } },

        // --- flex-basis ------------------------------------------------------------------
        { "flex_basis_px", R"({"display":"flex"})",
          R"({"width":"50px","height":"20px","flex-basis":"80px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 80, 20 }, { 80, 0, 50, 20 }, { 130, 0, 50, 20 } } },
        { "flex_basis_percent", R"({"display":"flex"})",
          R"({"height":"20px","flex-basis":"40%"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 120, 20 }, { 120, 0, 50, 20 }, { 170, 0, 50, 20 } } },

        // --- margins (shorthand 1/2/3/4 values + a side override) ------------------------
        { "margin_shorthand_1", R"({"display":"flex"})",
          R"({"width":"50px","margin":"5px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 30, 0, 0, {}, { "a", "b", "k" },
          { { 5, 5, 50, 20 }, { 60, 0, 50, 20 }, { 110, 0, 50, 20 } } },
        { "margin_shorthand_2", R"({"display":"flex"})",
          R"({"width":"50px","margin":"5px 10px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 30, 0, 0, {}, { "a", "b", "k" },
          { { 10, 5, 50, 20 }, { 70, 0, 50, 20 }, { 120, 0, 50, 20 } } },
        { "margin_shorthand_3", R"({"display":"flex"})",
          R"({"width":"50px","margin":"4px 10px 16px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 30, 0, 0, {}, { "a", "b", "k" },
          { { 10, 4, 50, 10 }, { 70, 0, 50, 20 }, { 120, 0, 50, 20 } } },
        { "margin_shorthand_4", R"({"display":"flex"})",
          R"({"width":"50px","margin":"3px 6px 9px 12px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 30, 0, 0, {}, { "a", "b", "k" },
          { { 12, 3, 50, 18 }, { 68, 0, 50, 20 }, { 118, 0, 50, 20 } } },
        { "margin_side_override", R"({"display":"flex"})",
          R"({"width":"50px","margin":"5px","margin-left":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 30, 0, 0, {}, { "a", "b", "k" },
          { { 20, 5, 50, 20 }, { 75, 0, 50, 20 }, { 125, 0, 50, 20 } } },

        // --- padding/border + box-sizing (on the CHILD, content-box vs border-box) --------
        { "child_box_sizing_content_box", R"({"display":"flex"})",
          R"({"width":"20px","height":"20px","padding":"5px","border":"2px solid #000"})",
          R"({"width":"20px","height":"20px"})", R"({"width":"20px","height":"20px"})",
          300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 34, 34 }, { 34, 0, 20, 20 }, { 54, 0, 20, 20 } } },
        { "child_box_sizing_border_box", R"({"display":"flex"})",
          R"({"width":"20px","height":"20px","padding":"5px","border":"2px solid #000","box-sizing":"border-box"})",
          R"({"width":"20px","height":"20px"})", R"({"width":"20px","height":"20px"})",
          300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 20, 20 }, { 20, 0, 20, 20 }, { 40, 0, 20, 20 } } },

        // --- min-/max-width clamping -------------------------------------------------------
        { "max_width_clamp", R"({"display":"flex"})",
          R"({"width":"200px","max-width":"50px","height":"20px"})", R"({"width":"20px","height":"20px"})",
          R"({"width":"20px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 20, 20 }, { 70, 0, 20, 20 } } },
        { "min_width_clamp", R"({"display":"flex"})",
          R"({"width":"20px","min-width":"80px","height":"20px"})", R"({"width":"20px","height":"20px"})",
          R"({"width":"20px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 80, 20 }, { 80, 0, 20, 20 }, { 100, 0, 20, 20 } } },

        // --- aspect-ratio ------------------------------------------------------------------
        { "aspect_ratio_number", R"({"display":"flex"})",
          R"({"width":"100px","aspect-ratio":"2"})", R"({"width":"20px","height":"20px"})",
          R"({"width":"20px","height":"20px"})", 300, 60, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 100, 50 }, { 100, 0, 20, 20 }, { 120, 0, 20, 20 } } },
        { "aspect_ratio_fraction", R"({"display":"flex"})",
          R"({"width":"160px","aspect-ratio":"16/9"})", R"({"width":"20px","height":"20px"})",
          R"({"width":"20px","height":"20px"})", 300, 60, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 160, 90 }, { 160, 0, 20, 20 }, { 180, 0, 20, 20 } } },

        // --- width: % / vw / calc / min / clamp --------------------------------------------
        { "width_percent", R"({"display":"flex"})",
          R"({"width":"50%","height":"20px"})", R"({"width":"20px","height":"20px"})",
          R"({"width":"20px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 150, 20 }, { 150, 0, 20, 20 }, { 170, 0, 20, 20 } } },
        { "width_vw", R"({"display":"flex"})",
          R"({"width":"10vw","height":"20px"})", R"({"width":"20px","height":"20px"})",
          R"({"width":"20px","height":"20px"})", 300, 20, 1000, 600, {},
          { "a", "b", "k" }, { { 0, 0, 100, 20 }, { 100, 0, 20, 20 }, { 120, 0, 20, 20 } } },
        // NOTE: these three use a named raw-string delimiter (R"CSS(...)CSS") because the value
        // itself ends in ')' immediately followed by the JSON closing quote — with the default
        // empty delimiter that ")\"" sequence closes the raw string early.
        { "width_calc", R"({"display":"flex"})",
          R"CSS({"width":"calc(50% - 10px)","height":"20px"})CSS", R"({"width":"20px","height":"20px"})",
          R"({"width":"20px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 140, 20 }, { 140, 0, 20, 20 }, { 160, 0, 20, 20 } } },
        { "width_min", R"({"display":"flex"})",
          R"CSS({"width":"min(200px, 50%)","height":"20px"})CSS", R"({"width":"20px","height":"20px"})",
          R"({"width":"20px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 150, 20 }, { 150, 0, 20, 20 }, { 170, 0, 20, 20 } } },
        { "width_clamp", R"({"display":"flex"})",
          R"CSS({"width":"clamp(50px, 20%, 100px)","height":"20px"})CSS", R"({"width":"20px","height":"20px"})",
          R"({"width":"20px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 60, 20 }, { 60, 0, 20, 20 }, { 80, 0, 20, 20 } } },

        // --- gap / row-gap / column-gap -----------------------------------------------------
        { "gap_basic", R"({"display":"flex","gap":"10px"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 60, 0, 50, 20 }, { 120, 0, 50, 20 } } },
        { "gap_row_gap_fallback", R"({"display":"flex","row-gap":"15px"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 65, 0, 50, 20 }, { 130, 0, 50, 20 } } },
        { "gap_column_gap_fallback", R"({"display":"flex","column-gap":"8px"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 300, 20, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 58, 0, 50, 20 }, { 116, 0, 50, 20 } } },

        // --- align-content across 2 wrapped lines -----------------------------------------
        { "align_content_center", R"({"display":"flex","flex-wrap":"wrap","align-content":"center"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 120, 200, 0, 0, {}, { "a", "b", "k" },
          { { 0, 80, 50, 20 }, { 50, 80, 50, 20 }, { 0, 100, 50, 20 } } },
        { "align_content_space_between", R"({"display":"flex","flex-wrap":"wrap","align-content":"space-between"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 120, 200, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 50, 20 }, { 0, 180, 50, 20 } } },
        { "align_content_stretch", R"({"display":"flex","flex-wrap":"wrap","align-content":"stretch"})",
          R"({"width":"50px","height":"20px"})", R"({"width":"50px","height":"20px"})",
          R"({"width":"50px","height":"20px"})", 120, 200, 0, 0, {}, { "a", "b", "k" },
          { { 0, 0, 50, 20 }, { 50, 0, 50, 20 }, { 0, 100, 50, 20 } } },

        // --- a text primitive centered (isTextPrimitive path) -----------------------------
        // The text's own (font-dependent) natural height is deliberately not what's asserted:
        // align-items:center + isTextPrimitive grows csz to crossInner whenever the natural
        // height is smaller than it (true for any ordinary font in a 200px-tall row), making
        // the PLACED geometry deterministic regardless of the actual font metrics.
        { "text_primitive_centered", R"({"display":"flex","align-items":"center"})",
          {}, {}, {}, 200, 200, 0, 0,
          QByteArray("CssText { objectName: \"t\"; cssId: \"t\" }\n")
              + goldenKid("CssRect", "b", R"({"width":"30px","height":"30px"})")
              + goldenKid("CssRect", "k", R"({"width":"30px","height":"30px"})"),
          { "t", "b", "k" },
          { { 0, 0, 60, 200 }, { 60, 85, 30, 30 }, { 90, 85, 30, 30 } },
          R"(#t { width: 60px; })" },

        // --- an absolutely positioned child with inset/left/top (here: right+bottom) ------
        { "absolute_right_bottom", R"({"display":"flex"})", {}, {}, {}, 300, 100, 0, 0,
          goldenKid("CssRect", "a", R"({"position":"absolute","right":"20px","bottom":"10px","width":"40px","height":"25px"})")
              + goldenKid("CssRect", "k", R"({"width":"40px","height":"10px"})"),
          { "a", "k" },
          { { 240, 65, 40, 25 }, { 0, 0, 40, 10 } } },
    };

    for (const FlexCase &c : cases) {
        QByteArray kidsQml;
        if (!c.extraQml.isEmpty())
            kidsQml = c.extraQml;
        else
            kidsQml = goldenKid("CssRect", "a", c.styleA) + goldenKid("CssRect", "b", c.styleB)
                + goldenKid("CssRect", "k", c.styleK);
        const QByteArray qml = goldenScene(c.containerStyle, c.cw, c.ch, kidsQml);

        CssTheme theme;
        if (!c.themeCss.isEmpty())
            theme.loadFromString(QString::fromUtf8(c.themeCss));
        CssLayoutEngine layoutEngine(&theme);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
        if (c.vw > 0 || c.vh > 0)
            theme.setViewport(c.vw, c.vh);

        QQmlComponent component(&engine);
        component.setData(qml, QUrl(QStringLiteral("qrc:/golden-%1.qml").arg(c.name)));
        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(QStringLiteral("%1: %2").arg(c.name, component.errorString())));
        auto *root = qobject_cast<QQuickItem *>(object.data());
        QVERIFY2(root, qPrintable(QStringLiteral("%1: root is not an Item").arg(c.name)));

        QVector<QQuickItem *> kidItems;
        for (const QByteArray &kn : c.kids) {
            QQuickItem *k = root->findChild<QQuickItem *>(QString::fromUtf8(kn));
            QVERIFY2(k, qPrintable(QStringLiteral("%1: missing child %2").arg(c.name, QString::fromUtf8(kn))));
            kidItems.push_back(k);
        }
        QVERIFY2(!kidItems.isEmpty(), qPrintable(QStringLiteral("%1: no children to check").arg(c.name)));

        // ONE forced, synchronous layout pass — same idiom as the existing direct-style flex
        // tests in test_qml_css.cpp (layoutEngine.layout(root, contentHolderOf(child))).
        layoutEngine.layout(root, goldenContentHolderOf(kidItems.first()));

        QCOMPARE(kidItems.size(), c.expect.size());
        for (int i = 0; i < kidItems.size(); ++i) {
            QQuickItem *k = kidItems[i];
            const Geom &g = c.expect[i];
            const QByteArray ctx = c.name + QByteArray(":") + c.kids[i];
            QVERIFY2(k->x() == g.x, qPrintable(QStringLiteral("%1 x: got %2 want %3")
                .arg(QString::fromUtf8(ctx)).arg(k->x()).arg(g.x)));
            QVERIFY2(k->y() == g.y, qPrintable(QStringLiteral("%1 y: got %2 want %3")
                .arg(QString::fromUtf8(ctx)).arg(k->y()).arg(g.y)));
            QVERIFY2(k->width() == g.w, qPrintable(QStringLiteral("%1 width: got %2 want %3")
                .arg(QString::fromUtf8(ctx)).arg(k->width()).arg(g.w)));
            QVERIFY2(k->height() == g.h, qPrintable(QStringLiteral("%1 height: got %2 want %3")
                .arg(QString::fromUtf8(ctx)).arg(k->height()).arg(g.h)));
        }
    }
}

QTEST_MAIN(LayoutTests)
