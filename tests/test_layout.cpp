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

QTEST_MAIN(LayoutTests)
