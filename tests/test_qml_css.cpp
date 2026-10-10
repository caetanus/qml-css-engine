#include "test_qml_css.h"

#include "qmlcss/QMLCss.h"
#include "qmlcss/csscontentholder.h"
#include "qmlcss/cssbox.h"
#include "qmlcss/csstheme_p.h"

#include <QtQml/qqml.h>

#include <QColor>
#include <QImage>
#include <QPair>
#include <QPointer>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QFont>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QTextOption>
#include <QUrl>

#include <cmath>

using namespace QmlCss;

namespace {

// Locate the CssRect/CssFill contentHolder that owns `child` (the Item whose children are the
// layout participants): it is the child's immediate parent item. The layout root is one level up.
QQuickItem *contentHolderOf(QQuickItem *child)
{
    return child ? child->parentItem() : nullptr;
}

// The REAL QtQuick Text a CssText composes (it forwards color/font/wrap/align onto it).
QQuickItem *composedText(QObject *cssText)
{
    auto *item = qobject_cast<QQuickItem *>(cssText);
    if (!item)
        return nullptr;
    for (QQuickItem *k : item->childItems()) {
        if (QByteArray(k->metaObject()->className()).contains("QQuickText"))
            return k;
    }
    return nullptr;
}

// Keeps the CssTexts created while it lives on the COMPOSED QQuickText (the styledText/text-shadow fallback).
// The tests that read the forwarded payload off the composed label use it: the payload is resolved once for both
// paths, and the lean path is held to the composed one pixel for pixel (tests/test_text.cpp).
struct ComposedTextScope {
    int previous = Detail::g_leanTextOverride;
    ComposedTextScope() { Detail::g_leanTextOverride = 0; }
    ~ComposedTextScope() { Detail::g_leanTextOverride = previous; }
};

// Walk the VISUAL tree (childItems(), recursively) rather than the QObject tree: a part an
// engine type composes via QQmlComponent::create/beginCreate is always a visual (setParentItem)
// child, but — before the Step 3 Task 1 fix — some of them were NOT also a QObject child, so
// findChildren() alone would miss exactly the ones this test exists to catch.
void collectComposedDescendants(QQuickItem *item, QList<QPair<QByteArray, QPointer<QObject>>> &out)
{
    for (QQuickItem *k : item->childItems()) {
        out.append({ QByteArray(k->metaObject()->className()), QPointer<QObject>(k) });
        collectComposedDescendants(k, out);
    }
}

} // namespace

void QmlCssTests::initTestCase()
{
    // Process-global, so every test's `import qmlcss` (and the CssFill shim that composes
    // CssRect) resolves. Same call consumers make.
    QmlCss::registerTypes();
}

void QmlCssTests::cssRectLoadsAndRestyles()
    {
        CssTheme theme;
        theme.loadFromString(QStringLiteral("#box { background-color: #112233; border-radius: 7px; }"));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import qmlcss

            CssRect {
                width: 24
                height: 24
                cssId: "box"
            }
        )", QUrl());

        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        const QVariantMap initial = object->property("style").toMap();
        QCOMPARE(initial.value(QStringLiteral("background-color")).toString(), QStringLiteral("#112233"));
        QCOMPARE(initial.value(QStringLiteral("border-radius")).toString(), QStringLiteral("7px"));

        theme.loadFromString(QStringLiteral("#box { background-color: #445566; }"));
        const QVariantMap reloaded = object->property("style").toMap();
        QCOMPARE(reloaded.value(QStringLiteral("background-color")).toString(), QStringLiteral("#445566"));
}

void QmlCssTests::cssTextUsesStandaloneDefaults()
    {
        ComposedTextScope composed;
        CssTheme theme;
        theme.loadFromString(QStringLiteral("#label { color: rgba(10, 20, 30, 0.5); font-size: 18px; }"));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import qmlcss

            CssText {
                cssId: "label"
                text: "Label"
            }
        )", QUrl());

        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        QCOMPARE(object->property("style").toMap().value(QStringLiteral("font-size")).toString(),
                 QStringLiteral("18px"));
        // color is forwarded onto the composed Text.
        QQuickItem *label = composedText(object.data());
        QVERIFY2(label, "CssText did not compose a Text");
        const QColor color = label->property("color").value<QColor>();
        QCOMPARE(color.red(), 10);
        QCOMPARE(color.green(), 20);
        QCOMPARE(color.blue(), 30);
}

void QmlCssTests::cssItemAppliesToParent()
    {
        CssTheme theme;
        theme.loadFromString(QStringLiteral("#plain { background-color: #abcdef; border-radius: 5px; }"));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

        QQmlComponent component(&engine);
        // `import qmlcss` proves the C++ registration (not the removed qrc QML file).
        component.setData(R"(
            import QtQuick
            import qmlcss

            Rectangle {
                width: 24
                height: 24

                CssItem {
                    cssId: "plain"
                }
            }
        )", QUrl());

        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        const QColor color = object->property("color").value<QColor>();
        QCOMPARE(color, QColor(QStringLiteral("#abcdef")));
        QCOMPARE(object->property("radius").toReal(), 5.0);
}

// --- Layout (geometry) tests ----------------------------------------------------------------
//
// These instantiate a CssRect container with CssRect children, register BOTH the CssTheme (as
// `cssTheme`) and a CssLayoutEngine (as `cssLayout`), then drive the box model synchronously via
// CssLayoutEngine::layout(root, contentHolder) and read back each child's assigned geometry.
// Children set `cssPrimitive: ""` so the theme registry never overwrites their explicit `style`.

void QmlCssTests::layoutFlexColumnStacks()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    // A flex-column whose content (3×30 = 90) exceeds the container's 50px main size. The items must
    // STACK at their natural height and overflow — NOT shrink to fit. This guards the regression that
    // a default flex-shrink:1 introduced (auto-sized columns collapsing their content) which the
    // earlier unit tests missed — the reason "tests must also show in the app".
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 100; height: 50
            style: ({ "display": "flex", "flex-direction": "column", "gap": "0px" })

            CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "height": "30px" }) }
            CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "height": "30px" }) }
            CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "height": "30px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    // Each keeps its 30px height (no shrink), stacked down the column: a@0, b@30, c@60 (overflowing).
    QVERIFY2(std::abs(a->height() - 30.0) < 0.5, qPrintable(QString::number(a->height())));
    QVERIFY2(std::abs(b->height() - 30.0) < 0.5, qPrintable(QString::number(b->height())));
    QVERIFY2(std::abs(c->height() - 30.0) < 0.5, qPrintable(QString::number(c->height())));
    QVERIFY2(std::abs(a->y() - 0.0) < 0.5, qPrintable(QString::number(a->y())));
    QVERIFY2(std::abs(b->y() - 30.0) < 0.5, qPrintable(QString::number(b->y())));
    QVERIFY2(std::abs(c->y() - 60.0) < 0.5, qPrintable(QString::number(c->y())));
}

void QmlCssTests::layoutClampsMaxAndMinWidth()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 300; height: 60
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect {
                objectName: "capped"; cssPrimitive: ""
                style: ({ "width": "200px", "max-width": "50px" })
            }
            CssRect {
                objectName: "floored"; cssPrimitive: ""
                style: ({ "width": "20px", "min-width": "80px" })
            }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *capped = root->findChild<QQuickItem *>(QStringLiteral("capped"));
    auto *floored = root->findChild<QQuickItem *>(QStringLiteral("floored"));
    QVERIFY(capped && floored);

    layoutEngine.layout(root, contentHolderOf(capped));

    // max-width clamps the 200px request down to 50; min-width floors the 20px request up to 80.
    QVERIFY2(std::abs(capped->width() - 50.0) < 0.5, qPrintable(QString::number(capped->width())));
    QVERIFY2(std::abs(floored->width() - 80.0) < 0.5, qPrintable(QString::number(floored->width())));
    // The floored item follows the capped one: x = 50 (capped's clamped width, gap 0).
    QVERIFY2(std::abs(floored->x() - 50.0) < 0.5, qPrintable(QString::number(floored->x())));
}

void QmlCssTests::layoutHonoursFlexOrder()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 300; height: 40
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "width": "30px", "order": "2" }) }
            CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "width": "30px", "order": "0" }) }
            CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "width": "30px", "order": "1" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    // Visual order follows ascending `order`: b(0) @0, c(1) @30, a(2) @60.
    QVERIFY2(std::abs(b->x() - 0.0) < 0.5, qPrintable(QString::number(b->x())));
    QVERIFY2(std::abs(c->x() - 30.0) < 0.5, qPrintable(QString::number(c->x())));
    QVERIFY2(std::abs(a->x() - 60.0) < 0.5, qPrintable(QString::number(a->x())));
    QVERIFY(b->x() < c->x());
    QVERIFY(c->x() < a->x());
}

void QmlCssTests::layoutAppliesFlexBasis()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 400; height: 40
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "flex-basis": "40px" }) }
            CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "flex-basis": "80px" }) }
            CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "flex-basis": "120px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    // Each item's base main-axis size is its flex-basis (no flex-grow, so no free-space growth).
    QVERIFY2(std::abs(a->width() - 40.0) < 0.5, qPrintable(QString::number(a->width())));
    QVERIFY2(std::abs(b->width() - 80.0) < 0.5, qPrintable(QString::number(b->width())));
    QVERIFY2(std::abs(c->width() - 120.0) < 0.5, qPrintable(QString::number(c->width())));
    // Laid out left-to-right, so x offsets accumulate the bases.
    QVERIFY2(std::abs(a->x() - 0.0) < 0.5, qPrintable(QString::number(a->x())));
    QVERIFY2(std::abs(b->x() - 40.0) < 0.5, qPrintable(QString::number(b->x())));
    QVERIFY2(std::abs(c->x() - 120.0) < 0.5, qPrintable(QString::number(c->x())));
}

void QmlCssTests::layoutVisibilityHiddenKeepsSpace()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 300; height: 60
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect {
                objectName: "ghost"; cssPrimitive: ""
                style: ({ "width": "50px", "visibility": "hidden" })
            }
            CssRect { objectName: "sibling"; cssPrimitive: ""; style: ({ "width": "30px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *ghost = root->findChild<QQuickItem *>(QStringLiteral("ghost"));
    auto *sibling = root->findChild<QQuickItem *>(QStringLiteral("sibling"));
    QVERIFY(ghost && sibling);

    layoutEngine.layout(root, contentHolderOf(ghost));

    // visibility:hidden → painted invisible and inert, but STILL occupies its box in the flow.
    QCOMPARE(ghost->property("opacity").toReal(), 0.0);
    QCOMPARE(ghost->property("enabled").toBool(), false);
    QVERIFY2(ghost->width() > 0.5, qPrintable(QString::number(ghost->width())));
    // The sibling does NOT recede into the hidden item's space: it starts after the 50px box.
    QVERIFY2(std::abs(sibling->x() - 50.0) < 0.5, qPrintable(QString::number(sibling->x())));
}

void QmlCssTests::layoutGridTemplateAreas()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 300; height: 200
            style: ({
                "display": "grid",
                "grid-template-columns": "100px 100px",
                "grid-template-rows": "40px 40px",
                "grid-template-areas": '"head head" "side main"',
                "gap": "10px"
            })

            CssRect { objectName: "head"; cssPrimitive: ""; style: ({ "grid-area": "head" }) }
            CssRect { objectName: "side"; cssPrimitive: ""; style: ({ "grid-area": "side" }) }
            CssRect { objectName: "main"; cssPrimitive: ""; style: ({ "grid-area": "main" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *head = root->findChild<QQuickItem *>(QStringLiteral("head"));
    auto *side = root->findChild<QQuickItem *>(QStringLiteral("side"));
    auto *main = root->findChild<QQuickItem *>(QStringLiteral("main"));
    QVERIFY(head && side && main);

    layoutEngine.layout(root, contentHolderOf(head));

    // `head` spans both columns: width = 100 + 10(gap) + 100 = 210, anchored at (0, 0).
    QVERIFY2(std::abs(head->width() - 210.0) < 0.5, qPrintable(QString::number(head->width())));
    QVERIFY2(std::abs(head->x() - 0.0) < 0.5, qPrintable(QString::number(head->x())));
    QVERIFY2(std::abs(head->y() - 0.0) < 0.5, qPrintable(QString::number(head->y())));
    QVERIFY2(std::abs(head->height() - 40.0) < 0.5, qPrintable(QString::number(head->height())));

    // Second row (y = 40 + 10 = 50): side @ col0, main @ col1 (x = 100 + 10 = 110).
    QVERIFY2(std::abs(side->x() - 0.0) < 0.5, qPrintable(QString::number(side->x())));
    QVERIFY2(std::abs(side->y() - 50.0) < 0.5, qPrintable(QString::number(side->y())));
    QVERIFY2(std::abs(side->width() - 100.0) < 0.5, qPrintable(QString::number(side->width())));

    QVERIFY2(std::abs(main->x() - 110.0) < 0.5, qPrintable(QString::number(main->x())));
    QVERIFY2(std::abs(main->y() - 50.0) < 0.5, qPrintable(QString::number(main->y())));
    QVERIFY2(std::abs(main->width() - 100.0) < 0.5, qPrintable(QString::number(main->width())));
}

void QmlCssTests::layoutFlexShrinkOverflow()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 100; height: 40
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "width": "60px", "flex-shrink": "1" }) }
            CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "width": "60px", "flex-shrink": "1" }) }
            CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "width": "60px", "flex-shrink": "1" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    // 3×60 = 180 overflows the 100 container; with default flex-shrink:1 each item deflates by the
    // same scaled factor to 100/3 ≈ 33.33, so together they exactly fit the container.
    QVERIFY2(a->width() < 60.0, qPrintable(QString::number(a->width())));
    QVERIFY2(std::abs(a->width() - 100.0 / 3.0) < 0.5, qPrintable(QString::number(a->width())));
    const double sum = a->width() + b->width() + c->width();
    QVERIFY2(std::abs(sum - 100.0) < 0.5, qPrintable(QString::number(sum)));
    // Positions accumulate the shrunk widths (no gaps): a@0, b@~33.33, c@~66.67.
    QVERIFY2(std::abs(b->x() - a->width()) < 0.5, qPrintable(QString::number(b->x())));
    QVERIFY2(std::abs(c->x() - (a->width() + b->width())) < 0.5, qPrintable(QString::number(c->x())));
}

void QmlCssTests::layoutAlignContentCenter()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 100; height: 200
            style: ({
                "display": "flex", "flex-direction": "row", "flex-wrap": "wrap",
                "gap": "0px", "align-content": "center"
            })

            CssRect { objectName: "a"; cssPrimitive: ""; style: ({ "width": "60px", "height": "20px" }) }
            CssRect { objectName: "b"; cssPrimitive: ""; style: ({ "width": "60px", "height": "20px" }) }
            CssRect { objectName: "c"; cssPrimitive: ""; style: ({ "width": "60px", "height": "20px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    // Each 60px item overflows the 100px row → three wrapped lines of 20px height each (total 60).
    // align-content: center centres the 3-line block in the 200px cross box: free = 200-60 = 140,
    // so the first line starts at 70.
    QVERIFY2(std::abs(a->y() - 70.0) < 0.5, qPrintable(QString::number(a->y())));
    QVERIFY2(a->y() > 0.5, qPrintable(QString::number(a->y())));
    // Lines stack downward by their natural height (20 each): b@90, c@110.
    QVERIFY2(std::abs(b->y() - 90.0) < 0.5, qPrintable(QString::number(b->y())));
    QVERIFY2(std::abs(c->y() - 110.0) < 0.5, qPrintable(QString::number(c->y())));
}

void QmlCssTests::layoutBoxSizingBorderBox()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 400; height: 60
            style: ({ "display": "flex", "flex-direction": "row", "gap": "0px" })

            CssRect {
                objectName: "border"; cssPrimitive: ""
                style: ({ "width": "100px", "padding": "10px", "box-sizing": "border-box" })
            }
            CssRect {
                objectName: "content"; cssPrimitive: ""
                style: ({ "width": "100px", "padding": "10px", "box-sizing": "content-box" })
            }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *borderBox = root->findChild<QQuickItem *>(QStringLiteral("border"));
    auto *contentBox = root->findChild<QQuickItem *>(QStringLiteral("content"));
    QVERIFY(borderBox && contentBox);

    layoutEngine.layout(root, contentHolderOf(borderBox));

    // border-box: the 100px width INCLUDES the 2×10 padding, so the laid-out box stays 100.
    QVERIFY2(std::abs(borderBox->width() - 100.0) < 0.5, qPrintable(QString::number(borderBox->width())));
    // content-box (CSS default): the 100px is the content box, so the border box = 100 + 2×10 = 120.
    QVERIFY2(std::abs(contentBox->width() - 120.0) < 0.5, qPrintable(QString::number(contentBox->width())));
    // The content-box item follows the 100px border-box item (gap 0).
    QVERIFY2(std::abs(contentBox->x() - 100.0) < 0.5, qPrintable(QString::number(contentBox->x())));
}

void QmlCssTests::buttonLabelInheritsColor()
{
    ComposedTextScope composed;
    CssTheme theme;
    theme.loadFromString(QStringLiteral("button { color: #ffffff; }"));

    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssFill {
            cssPrimitive: "button"
            width: 100; height: 40

            CssText { objectName: "label"; text: "Click" }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    // The button's resolved style carries the white foreground colour.
    QCOMPARE(root->property("inheritedColor").toString(), QStringLiteral("#ffffff"));

    auto *label = root->findChild<QQuickItem *>(QStringLiteral("label"));
    QVERIFY(label);
    // The label has no colour of its own, so it inherits white from the button parent —
    // forwarded onto its composed Text.
    QQuickItem *labelText = composedText(label);
    QVERIFY2(labelText, "CssText did not compose a Text");
    const QColor color = labelText->property("color").value<QColor>();
    QCOMPARE(color, QColor(255, 255, 255));
}

void QmlCssTests::deepInheritanceIsLinear()
{
    ComposedTextScope composed;
    // A label 22 boxes below the only element that sets a colour. Resolving an inherited value
    // must walk that chain ONCE: probing each ancestor by calling its getter (which climbs the
    // rest of the chain) and then climbing again doubled the work per level — 2^22 here.
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".top { color: #ff0000; font-family: Serif; }"));
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    constexpr int depth = 22;
    QString qml = QStringLiteral("import QtQuick\nimport qmlcss\nCssRect { cssClass: \"top\"\n");
    for (int i = 0; i < depth; ++i)
        qml += QStringLiteral("CssRect { cssClass: \"b%1\"\n").arg(i);
    qml += QStringLiteral("CssText { objectName: \"label\"; text: \"deep\" }\n");
    for (int i = 0; i <= depth; ++i)
        qml += QStringLiteral("}\n");

    QElapsedTimer timer;
    timer.start();
    QQmlComponent component(&engine);
    component.setData(qml.toUtf8(), QUrl());
    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *label = object->findChild<QQuickItem *>(QStringLiteral("label"));
    QVERIFY(label);
    QQuickItem *labelText = composedText(label);
    QVERIFY(labelText);
    QCOMPARE(labelText->property("color").value<QColor>(), QColor(255, 0, 0));
    QCOMPARE(labelText->property("font").value<QFont>().family(), QStringLiteral("Serif"));
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QStringLiteral("deep inheritance took %1 ms").arg(timer.elapsed())));
}

void QmlCssTests::fillImageLayersAreLazy()
{
    // A CssFill composes its solid + Image layers only once a url() background appears; a plain
    // box carries just the renderer. When the image arrives the layers sit BELOW the renderer and
    // the declared children, and the image shows the resolved source.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString png = dir.filePath(QStringLiteral("bg.png"));
    QImage img(4, 4, QImage::Format_ARGB32);
    img.fill(Qt::red);
    QVERIFY(img.save(png));

    CssTheme theme;
    theme.loadFromString(QStringLiteral(".plain { background-color: #123456; }\n"
                                        ".pic { background: url(\"%1\"); background-size: cover; }")
                             .arg(QUrl::fromLocalFile(png).toString()));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssFill {
            objectName: "fill"; cssClass: "plain"; width: 80; height: 40
            Item { objectName: "kid"; width: 10; height: 10 }
        }
    )", QUrl());
    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *fill = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(fill);

    const auto imageLayer = [fill]() -> QQuickItem * {
        for (QQuickItem *k : fill->childItems())
            if (QByteArray(k->metaObject()->className()).contains("QQuickImage"))
                return k;
        return nullptr;
    };
    const auto indexOfClass = [fill](const char *cls) {
        const QList<QQuickItem *> kids = fill->childItems();
        for (int i = 0; i < kids.size(); ++i)
            if (QByteArray(kids[i]->metaObject()->className()).contains(cls))
                return i;
        return -1;
    };
    QCOMPARE(imageLayer(), nullptr); // plain box: no image layers composed
    QVERIFY(indexOfClass("CssRect") >= 0);

    fill->setProperty("cssClass", QStringLiteral("pic"));
    QQuickItem *image = imageLayer();
    QVERIFY2(image, "url() background did not compose the Image layer");
    QVERIFY(image->isVisible());
    QCOMPARE(image->property("source").toUrl(), QUrl::fromLocalFile(png));
    QCOMPARE(image->size(), QSizeF(80, 40));
    // Painted order: image under the renderer, the renderer under the declared children.
    auto *kid = fill->findChild<QQuickItem *>(QStringLiteral("kid"));
    QVERIFY(kid);
    const int holder = fill->childItems().indexOf(contentHolderOf(kid));
    QVERIFY(holder >= 0);
    QVERIFY(indexOfClass("QQuickImage") < indexOfClass("CssRect"));
    QVERIFY(indexOfClass("CssRect") < holder);
}

// --- New CSS property mapping tests ---------------------------------------------------------

void QmlCssTests::cssRectZIndex()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#zbox { z-index: 5; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            width: 24; height: 24
            cssId: "zbox"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    QCOMPARE(object->property("z").toReal(), 5.0);
}

void QmlCssTests::cssRectOverflowHidden()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#clipped { overflow: hidden; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            width: 24; height: 24
            cssId: "clipped"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    QCOMPARE(object->property("clip").toBool(), true);
}

void QmlCssTests::cssTextDecorationUnderline()
{
    ComposedTextScope composed;
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#uline { text-decoration: underline; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssText {
            cssId: "uline"
            text: "Hello"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    QQuickItem *label = composedText(object.data());
    QVERIFY2(label, "CssText did not compose a Text");
    const QFont font = label->property("font").value<QFont>();
    QCOMPARE(font.underline(), true);
    QCOMPARE(font.strikeOut(), false);
}

void QmlCssTests::cssTextWhiteSpaceNowrap()
{
    ComposedTextScope composed;
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#nowrap { white-space: nowrap; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssText {
            cssId: "nowrap"
            text: "Hello world"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    QQuickItem *label = composedText(object.data());
    QVERIFY2(label, "CssText did not compose a Text");
    // Text.NoWrap maps to QTextOption::NoWrap == 0
    QCOMPARE(label->property("wrapMode").toInt(), static_cast<int>(QTextOption::NoWrap));
}

void QmlCssTests::cssTextOverflowEllipsis()
{
    ComposedTextScope composed;
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#ellipsis { text-overflow: ellipsis; white-space: nowrap; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssText {
            cssId: "ellipsis"
            text: "A very long piece of text that should be elided"
            width: 50
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    QQuickItem *label = composedText(object.data());
    QVERIFY2(label, "CssText did not compose a Text");
    // Text.ElideRight maps to Qt::ElideRight == 1
    QCOMPARE(label->property("elide").toInt(), static_cast<int>(Qt::ElideRight));
    // The composed Text filled the assigned 50px width (so it actually elides).
    QCOMPARE(label->width(), 50.0);
}

// --- C++ CssRect (composition translation of CssRect.qml) ------------------------------------
//
// Proves the C++ type registers via `import qmlcss`, resolves its style through cssTheme.loadCss,
// composes a REAL QtQuick Shape for the box (background / border / radius pushed onto it), AND is
// a layout container: a declared child lands in the inner contentHolder and is laid out by the
// C++ box model.
void QmlCssTests::cssRectCppComposesShapeAndContains()
{
    CssTheme theme;
    // A gradient forces the SHAPE path of the Shape x Rectangle policy (a plain solid box
    // now takes the cheap Rectangle path — covered by rectanglePolicyFastPathAndSwap).
    theme.loadFromString(QStringLiteral(
        "#box { background: linear-gradient(180deg, #2244aa, #112255); "
        "border: 2px solid #ffcc00; border-radius: 8px; "
        "display: flex; flex-direction: row; gap: 0px; }"));

    CssLayoutEngine layout(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    // `import qmlcss` proves the C++ registration (not a qrc QML file).
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            width: 120
            height: 40
            cssId: "box"

            CssRect { objectName: "kid"; cssPrimitive: ""; style: ({ "width": "40px", "height": "20px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *rect = qobject_cast<CssRect *>(object.data());
    QVERIFY(rect);

    // loadCss pushed the resolved style.
    const QVariantMap style = rect->property("style").toMap();
    QCOMPARE(style.value(QStringLiteral("background")).toString(),
             QStringLiteral("linear-gradient(180deg, #2244aa, #112255)"));
    QCOMPARE(style.value(QStringLiteral("border-radius")).toString(), QStringLiteral("8px"));

    // The composed render subtree root carries the resolved CSS values (bg/border/radius) that
    // drive the real Shape fill — identified as the CssRect child exposing `radiusStr`.
    QQuickItem *renderRoot = nullptr;
    for (QQuickItem *k : rect->childItems()) {
        if (k->property("radiusStr").isValid()) {
            renderRoot = k;
            break;
        }
    }
    QVERIFY2(renderRoot, "composed render subtree missing");
    QCOMPARE(renderRoot->property("radiusStr").toString(), QStringLiteral("8px"));
    QCOMPARE(renderRoot->property("borderWidth").toReal(), 2.0);
    QCOMPARE(renderRoot->property("borderColorOpaque").value<QColor>(), QColor(QStringLiteral("#ffcc00")));
    // Gradient path (the policy sends solid boxes to the Rectangle fast path instead).
    QVERIFY(renderRoot->property("hasGradient").toBool());
    const QVariantMap gradient = renderRoot->property("gradient").toMap();
    QCOMPARE(gradient.value(QStringLiteral("stops")).toList().size(), 2);

    // The box fill is a genuine QtQuick Shape (NOT a Rectangle), somewhere under the render root.
    bool hasShape = false;
    bool hasRectangle = false;
    for (QQuickItem *d : renderRoot->findChildren<QQuickItem *>()) {
        const QByteArray cls(d->metaObject()->className());
        if (cls.contains("QQuickShape") && !cls.contains("QQuickShapePath"))
            hasShape = true;
        if (cls.contains("QQuickRectangle"))
            hasRectangle = true;
    }
    QVERIFY2(hasShape, "no composed QtQuick Shape fill");
    QVERIFY2(!hasRectangle, "composition used a Rectangle (owner's rule: Shape only)");

    // Container: the declared child lands in the inner contentHolder (child.parent.parent == rect),
    // and the C++ box model laid it out at its 40px width.
    auto *kid = rect->findChild<QQuickItem *>(QStringLiteral("kid"));
    QVERIFY(kid);
    QQuickItem *holder = kid->parentItem();
    QVERIFY(holder);
    QCOMPARE(holder->parentItem(), rect);
    QVERIFY2(std::abs(kid->width() - 40.0) < 0.5, qPrintable(QString::number(kid->width())));
}

// border-style: dotted/dashed — the uniform border ring becomes a DASHED Shape stroke
// (strokeStyle DashLine, dashPattern in strokeWidth units), and non-solid styles force the
// Shape path of the Rectangle policy. none hides the border entirely.
void QmlCssTests::cssRectBorderStyleDottedStrokes()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#ring { border: 1px dotted #333333; border-radius: 3px; }"
        "#off  { border: 2px solid #333333; border-style: none; }"));

    CssLayoutEngine layout(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        Item {
            CssRect { objectName: "ring"; cssId: "ring"; width: 60; height: 30 }
            CssRect { objectName: "off";  cssId: "off";  width: 60; height: 30 }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto renderRootOf = [](QQuickItem *box) -> QQuickItem * {
        for (QQuickItem *k : box->childItems())
            if (k->property("radiusStr").isValid())
                return k;
        return nullptr;
    };

    auto *ring = root->findChild<QQuickItem *>(QStringLiteral("ring"));
    QVERIFY(ring);
    QQuickItem *shell = renderRootOf(ring);
    QVERIFY2(shell, "dotted border did not compose the Shape shell (needsShape policy)");
    QCOMPARE(shell->property("borderStyle").toString(), QStringLiteral("dotted"));
    QVERIFY(shell->property("borderVisible").toBool());

    // The stroke is genuinely dashed: a ShapePath with strokeStyle DashLine and the dot pattern.
    bool dashed = false;
    for (QObject *d : shell->findChildren<QObject *>()) {
        if (!QByteArray(d->metaObject()->className()).contains("QQuickShapePath"))
            continue;
        if (d->property("strokeColor").value<QColor>() != QColor(QStringLiteral("#333333")))
            continue; // fill/shadow paths
        QCOMPARE(d->property("strokeStyle").toInt(), 2); // ShapePath.DashLine (Qt::PenStyle)
        const QVariantList pat = d->property("dashPattern").toList();
        QCOMPARE(pat.size(), 2);
        QCOMPARE(pat.at(0).toReal(), 1.0);
        QCOMPARE(pat.at(1).toReal(), 2.0);
        dashed = true;
    }
    QVERIFY2(dashed, "no dashed border ShapePath found");

    // border-style: none kills the ring even with width+color set.
    auto *off = root->findChild<QQuickItem *>(QStringLiteral("off"));
    QVERIFY(off);
    QQuickItem *offShell = renderRootOf(off);
    if (offShell)
        QVERIFY(!offShell->property("borderVisible").toBool());
}

// Container-created delegates (a SplitView handle, incubated view subtrees) complete BEFORE
// they are parented into the scene: the first resolve walks a truncated ancestor chain and
// ancestor-scoped rules miss. The item re-resolves ONCE when it joins a window.
void QmlCssTests::scenelessResolveHealsOnSceneAttach()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".scope .kid { background: #123456; }"));
    CssLayoutEngine layout(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    // A detached element (no parent chain): the scoped rule cannot match yet.
    QQmlComponent kidComp(&engine);
    kidComp.setData("import QtQuick\nimport qmlcss\nCssRect { cssPrimitive: \"div\"; cssClass: [\"kid\"] }", QUrl());
    QScopedPointer<QObject> kidObj(kidComp.create());
    QVERIFY2(kidObj, qPrintable(kidComp.errorString()));
    auto *kid = qobject_cast<QQuickItem *>(kidObj.data());
    QVERIFY(kid);
    QVERIFY(!kid->property("style").toMap().contains(QStringLiteral("background")));

    // A window hosting a .scope ancestor; parenting the kid attaches it to the scene.
    QQmlComponent scopeComp(&engine);
    scopeComp.setData("import QtQuick\nimport qmlcss\nCssRect { cssPrimitive: \"div\"; cssClass: [\"scope\"] }", QUrl());
    QScopedPointer<QObject> scopeObj(scopeComp.create());
    QVERIFY2(scopeObj, qPrintable(scopeComp.errorString()));
    auto *scope = qobject_cast<QQuickItem *>(scopeObj.data());
    QVERIFY(scope);

    QQuickWindow window;
    scope->setParentItem(window.contentItem());
    kid->setParentItem(scope);

    QTRY_COMPARE(kid->property("style").toMap().value(QStringLiteral("background")).toString(),
                 QStringLiteral("#123456"));
}

// --- C++ CssHr (composition translation of CssHr.qml) ----------------------------------------
//
// Proves the C++ type registers via the classic API, instantiates from QML as `qmlcss.CssHr`,
// resolves its top border through cssTheme.borderSide, and — crucially — that the child it
// composes is the REAL QtQuick Shape (ShapePath + PathRectangle), carrying the resolved
// colour/height full-width (not a repaint, and not a Rectangle).

void QmlCssTests::cssHrCppComposesRealShape()
{
    // Classic one-line registration — no qmltyperegistrar / QML_ELEMENT machinery.
    qmlRegisterType<CssHr>("qmlcss", 1, 0, "CssHr");

    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#rule { border-top: 3px solid #ff8800; }"));

    QQmlEngine engine;
    CssLayoutEngine layout(&theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    // `import qmlcss` proves the C++ registration (not the qrc QML file).
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssHr {
            width: 120
            cssId: "rule"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    auto *hr = qobject_cast<CssHr *>(object.data());
    QVERIFY(hr);

    // loadCss pushed the resolved style; the `line` binding read the top border off it.
    const QVariantMap style = hr->property("style").toMap();
    QCOMPARE(style.value(QStringLiteral("border-top")).toString(), QStringLiteral("3px solid #ff8800"));

    const QVariantMap line = hr->property("line").toMap();
    QCOMPARE(line.value(QStringLiteral("width")).toReal(), 3.0);
    QCOMPARE(line.value(QStringLiteral("color")).value<QColor>(), QColor(QStringLiteral("#ff8800")));
    QCOMPARE(line.value(QStringLiteral("visible")).toBool(), true);

    // implicitHeight tracks the line width (max(1, width)).
    QCOMPARE(hr->implicitHeight(), 3.0);

    // The composed child must be a genuine QtQuick Shape (NOT a Rectangle), styled with the line
    // colour/height and pinned full-width (the C++ anchor equivalent).
    const QList<QQuickItem *> kids = hr->childItems();
    QCOMPARE(kids.size(), 1);
    QQuickItem *shape = kids.first();
    QVERIFY(shape);
    const QByteArray cls(shape->metaObject()->className());
    QVERIFY2(cls.contains("QQuickShape"), cls);
    QVERIFY2(!cls.contains("QQuickRectangle"), cls);
    // fillColor / rectHeight are the aliases the composed Shape exposes onto its ShapePath /
    // PathRectangle — the real fill colour and rounded-rect height.
    QCOMPARE(shape->property("fillColor").value<QColor>(), QColor(QStringLiteral("#ff8800")));
    QCOMPARE(shape->property("rectHeight").toReal(), 3.0);
    QCOMPARE(shape->property("rectWidth").toReal(), 120.0);
    QCOMPARE(shape->height(), 3.0);
    QCOMPARE(shape->property("visible").toBool(), true);
    QCOMPARE(shape->width(), 120.0); // followed the parent's width
    QCOMPARE(shape->x(), 0.0);
    QCOMPARE(shape->y(), 0.0);

    // The ShapePath really carries a PathRectangle child (proves it is a genuine vector fill).
    QObject *shapePath = shape->findChild<QObject *>();
    QVERIFY(shapePath);
    QVERIFY2(QByteArray(shapePath->metaObject()->className()).contains("QQuickShapePath"),
             shapePath->metaObject()->className());
}

// --- C++ CssImage (composition translation of CssImage.qml) ----------------------------------
//
// Helper: classify the composed children of a CssImage by their real Qt class names.
namespace {
struct ImageParts {
    QQuickItem *image = nullptr;  // QQuickImage
    QQuickItem *effect = nullptr; // QQuickMultiEffect
    QQuickItem *shape = nullptr;  // QQuickShape (rounded-rect mask)
};
ImageParts partsOf(QQuickItem *root)
{
    ImageParts p;
    const QList<QQuickItem *> kids = root->childItems();
    for (QQuickItem *k : kids) {
        const QByteArray cls(k->metaObject()->className());
        if (cls.contains("QQuickImage"))
            p.image = k;
        else if (cls.contains("QQuickMultiEffect"))
            p.effect = k;
        else if (cls.contains("QQuickShape") && !cls.contains("QQuickShapePath"))
            p.shape = k;
    }
    return p;
}
} // namespace

// Rounded case: a non-zero border-radius must compose a REAL Image + MultiEffect + Shape mask,
// with the effect masking (maskEnabled) and the Image hidden (drawn through the effect).
void QmlCssTests::cssImageCppComposesImageEffectAndMask()
{
    qmlRegisterType<CssImage>("qmlcss", 1, 0, "CssImage");

    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#avatar { border-radius: 12px; object-fit: cover; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssImage {
            width: 100
            height: 100
            cssId: "avatar"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *img = qobject_cast<CssImage *>(object.data());
    QVERIFY(img);

    // loadCss pushed the resolved style.
    const QVariantMap style = img->property("style").toMap();
    QCOMPARE(style.value(QStringLiteral("border-radius")).toString(), QStringLiteral("12px"));

    const ImageParts p = partsOf(img);
    QVERIFY2(p.image, "composed Image missing");
    QVERIFY2(p.effect, "composed MultiEffect missing");
    QVERIFY2(p.shape, "composed Shape mask missing");

    // object-fit: cover -> Image.PreserveAspectCrop (2).
    QCOMPARE(p.image->property("fillMode").toInt(), 2);
    // Rounded -> the Image is hidden (the MultiEffect draws it through the mask).
    QCOMPARE(p.image->isVisible(), false);

    // The MultiEffect masks, is visible, and points at the REAL composed Image / Shape.
    QCOMPARE(p.effect->property("maskEnabled").toBool(), true);
    QCOMPARE(p.effect->isVisible(), true);
    QCOMPARE(p.effect->property("source").value<QQuickItem *>(), p.image);
    QCOMPARE(p.effect->property("maskSource").value<QQuickItem *>(), p.shape);

    // The mask's rounded-rect radius follows the resolved border-radius and fills the item.
    QCOMPARE(p.shape->property("maskRadius").toReal(), 12.0);
    QCOMPARE(p.shape->property("maskWidth").toReal(), 100.0);
    QCOMPARE(p.shape->property("maskHeight").toReal(), 100.0);
}

// No-radius case: the Image is shown DIRECT, and the MultiEffect mask is disabled/hidden.
void QmlCssTests::cssImageCppNoRadiusShowsImageDirect()
{
    qmlRegisterType<CssImage>("qmlcss", 1, 0, "CssImage");

    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#plain { object-fit: contain; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssImage {
            width: 80
            height: 40
            cssId: "plain"
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *img = qobject_cast<CssImage *>(object.data());
    QVERIFY(img);

    const ImageParts p = partsOf(img);
    QVERIFY2(p.image, "composed Image missing");
    QVERIFY2(p.effect, "composed MultiEffect missing");

    // object-fit: contain -> Image.PreserveAspectFit (1).
    QCOMPARE(p.image->property("fillMode").toInt(), 1);
    // No radius -> the Image draws directly, the effect is off.
    QCOMPARE(p.image->isVisible(), true);
    QCOMPARE(p.effect->isVisible(), false);
    QCOMPARE(p.effect->property("maskEnabled").toBool(), false);
}

// --- C++ CssText (composition translation of CssText.qml) ------------------------------------
//
// Proves CssText registers via `import qmlcss`, composes a REAL QtQuick Text (not a Rectangle,
// not a repaint), forwards the CSS-driven props (color/font/wrap/align) onto it, and inherits the
// container's colour/font/align through the CSS-inheritance chain.
void QmlCssTests::cssTextCppComposesRealText()
{
    ComposedTextScope composed;
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#box { color: #ff0000; font-size: 20px; text-align: center; } "
        "#label { white-space: nowrap; }"));

    CssLayoutEngine layout(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    // `import qmlcss` proves the C++ registration (not the removed qrc QML file).
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            width: 200; height: 40
            cssId: "box"

            CssText { objectName: "label"; cssId: "label"; text: "Hi" }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *box = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(box);

    auto *label = box->findChild<CssText *>(QStringLiteral("label"));
    QVERIFY(label);

    // loadCss pushed the label's own style (white-space).
    QCOMPARE(label->property("style").toMap().value(QStringLiteral("white-space")).toString(),
             QStringLiteral("nowrap"));

    // The composed child is a genuine QtQuick Text (NOT a Rectangle).
    const QList<QQuickItem *> kids = label->childItems();
    QQuickItem *text = nullptr;
    for (QQuickItem *k : kids) {
        const QByteArray cls(k->metaObject()->className());
        QVERIFY2(!cls.contains("QQuickRectangle"), cls);
        if (cls.contains("QQuickText"))
            text = k;
    }
    QVERIFY2(text, "CssText did not compose a real Text");

    // Forwarded content + literal text format.
    QCOMPARE(text->property("text").toString(), QStringLiteral("Hi"));
    QCOMPARE(text->property("textFormat").toInt(), 0); // Text.PlainText

    // Inherited from the container: colour (red), font-size (20px -> 15pt) and centre alignment.
    QCOMPARE(text->property("color").value<QColor>(), QColor(255, 0, 0));
    const QFont font = text->property("font").value<QFont>();
    QVERIFY2(std::abs(font.pointSizeF() - 15.0) < 0.5, qPrintable(QString::number(font.pointSizeF())));
    QCOMPARE(text->property("horizontalAlignment").toInt(), static_cast<int>(Qt::AlignHCenter));

    // The label's own white-space: nowrap forwarded onto the Text.
    QCOMPARE(text->property("wrapMode").toInt(), static_cast<int>(QTextOption::NoWrap));
}

// --- C++ CssFill (composition translation of CssFill.qml) ------------------------------------
//
// Proves CssFill (registered via the classic API, instantiated as `qmlcss.CssFill`) composes,
// for a url() background: a REAL solid-colour Shape behind, a REAL QtQuick Image, and a REAL
// CssRect renderer (our own type, cssPrimitive "" and fill forced transparent so the image shows
// through while the border still frames) — AND that it hosts a declared child in its contentHolder,
// laid out by the C++ box model.
void QmlCssTests::cssFillCppComposesImageRectAndHostsChildren()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "#panel { background-color: #204080; background-image: url(qrc:/nope.png); "
        "background-size: contain; border: 2px solid #ffcc00; "
        "display: flex; flex-direction: row; gap: 0px; }"));

    CssLayoutEngine layout(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    // `import qmlcss` proves the C++ registration (not the removed qrc QML file).
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssFill {
            width: 120
            height: 60
            cssId: "panel"

            CssRect { objectName: "kid"; cssPrimitive: ""; style: ({ "width": "40px", "height": "20px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *fill = qobject_cast<CssFill *>(object.data());
    QVERIFY(fill);

    // loadCss pushed the resolved style.
    const QVariantMap style = fill->property("style").toMap();
    QCOMPARE(style.value(QStringLiteral("background-color")).toString(), QStringLiteral("#204080"));
    QCOMPARE(style.value(QStringLiteral("background-image")).toString(), QStringLiteral("url(qrc:/nope.png)"));

    // Classify the composed layers by their real Qt class names.
    QQuickItem *image = nullptr;
    QQuickItem *rect = nullptr;
    QQuickItem *solidShape = nullptr;
    for (QQuickItem *k : fill->childItems()) {
        const QByteArray cls(k->metaObject()->className());
        if (cls.contains("QQuickImage"))
            image = k;
        else if (cls.contains("CssRect"))
            rect = k;
        else if (cls.contains("QQuickShape") && !cls.contains("QQuickShapePath"))
            solidShape = k;
    }
    QVERIFY2(image, "composed Image missing");
    QVERIFY2(rect, "composed CssRect renderer missing");
    QVERIFY2(solidShape, "composed solid-background Shape missing");

    // The Image carries the resolved url() source, is visible, and object-fit maps background-size:
    // contain -> Image.PreserveAspectFit (1).
    QCOMPARE(image->property("source").toUrl(), QUrl(QStringLiteral("qrc:/nope.png")));
    QCOMPARE(image->isVisible(), true);
    QCOMPARE(image->property("fillMode").toInt(), 1);

    // The solid Shape shows the background-color behind the image.
    QCOMPARE(solidShape->isVisible(), true);
    QCOMPARE(solidShape->property("fillColor").value<QColor>(), QColor(QStringLiteral("#204080")));

    // The CssRect renderer got the same style, and its fill is transparent (image present) while the
    // border is still resolved — proven through its own composed render root.
    QCOMPARE(rect->property("cssPrimitive").toString(), QString());
    QCOMPARE(rect->property("defaultColor").value<QColor>(), QColor(Qt::transparent));
    QQuickItem *renderRoot = nullptr;
    for (QQuickItem *k : rect->childItems()) {
        if (k->property("radiusStr").isValid()) {
            renderRoot = k;
            break;
        }
    }
    QVERIFY2(renderRoot, "CssRect render subtree missing");
    QCOMPARE(renderRoot->property("borderWidth").toReal(), 2.0);
    QCOMPARE(renderRoot->property("borderColorOpaque").value<QColor>(), QColor(QStringLiteral("#ffcc00")));

    // Container: the declared child lands in CssFill's inner contentHolder (child.parent.parent ==
    // fill), and the C++ box model laid it out at its 40px width — NOT inside the renderer CssRect.
    auto *kid = fill->findChild<QQuickItem *>(QStringLiteral("kid"));
    QVERIFY(kid);
    QQuickItem *holder = kid->parentItem();
    QVERIFY(holder);
    QCOMPARE(holder->parentItem(), fill);
    QVERIFY2(std::abs(kid->width() - 40.0) < 0.5, qPrintable(QString::number(kid->width())));
}

// --- C++ CssDropShadow (composition translation of CssDropShadow.qml) -----------------------
//
// Proves CssDropShadow registers via `import qmlcss`, composes a REAL QtQuick.Effects
// MultiEffect as its only child, and correctly drives the shadow* props (including the
// shadowBlur 0..1 normalisation over 32 px) from a QVariantMap shadow descriptor.
void QmlCssTests::cssDropShadowCppComposesMultiEffect()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssDropShadow {
            width: 100; height: 40
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *ds = qobject_cast<CssDropShadow *>(object.data());
    QVERIFY(ds);

    // Initially: no shadow descriptor → hasShadow false.
    QCOMPARE(ds->hasShadow(), false);

    // Find the composed REAL MultiEffect child.
    QQuickItem *effect = nullptr;
    for (QQuickItem *k : ds->childItems()) {
        if (QByteArray(k->metaObject()->className()).contains("QQuickMultiEffect"))
            effect = k;
    }
    QVERIFY2(effect, "composed MultiEffect missing");
    QCOMPARE(effect->property("shadowEnabled").toBool(), false);

    // Set a shadow descriptor and verify it is pushed onto the MultiEffect.
    QVariantMap shadow;
    shadow[QStringLiteral("x")]     = 2.0;
    shadow[QStringLiteral("y")]     = 3.0;
    shadow[QStringLiteral("blur")]  = 16.0;         // blur/32 = 0.5
    shadow[QStringLiteral("color")] = QColor(QStringLiteral("#ff8800"));
    ds->setShadow(shadow);

    QCOMPARE(ds->hasShadow(), true);
    QCOMPARE(effect->property("shadowEnabled").toBool(), true);
    QCOMPARE(effect->property("shadowColor").value<QColor>(), QColor(QStringLiteral("#ff8800")));
    QCOMPARE(effect->property("shadowHorizontalOffset").toReal(), 2.0);
    QCOMPARE(effect->property("shadowVerticalOffset").toReal(), 3.0);
    // QML: shadowBlur = Math.min(1, 16 / 32) = 0.5
    QVERIFY2(std::abs(effect->property("shadowBlur").toReal() - 0.5) < 1e-6,
             qPrintable(QString::number(effect->property("shadowBlur").toReal())));

    // autoPaddingEnabled must be true (matches the original QML).
    QCOMPARE(effect->property("autoPaddingEnabled").toBool(), true);

    // Clearing the shadow (no color key) resets shadowEnabled.
    ds->setShadow({});
    QCOMPARE(ds->hasShadow(), false);
    QCOMPARE(effect->property("shadowEnabled").toBool(), false);
}

// --- C++ CssKeyframes (composition translation of CssKeyframes.qml) --------------------------
//
// Proves CssKeyframes registers via `import qmlcss`, composes a REAL QtQuick NumberAnimation,
// and correctly interpolates the keyframe value onto target[animatedProperty] when progress is
// set directly (testing _numAt without requiring the animation event loop to tick).
void QmlCssTests::cssKeyframesCppInterpolatesTarget()
{
    QQmlEngine engine;

    // A target QQuickItem whose `opacity` we'll animate.
    QQmlComponent targetComp(&engine);
    targetComp.setData("import QtQuick\nItem { width: 1; height: 1 }", QUrl());
    QScopedPointer<QObject> targetObj(targetComp.create());
    QVERIFY2(targetObj, qPrintable(targetComp.errorString()));
    auto *target = qobject_cast<QQuickItem *>(targetObj.data());
    QVERIFY(target);

    // Instantiate CssKeyframes from QML to exercise the full componentComplete path.
    QQmlComponent kfComp(&engine);
    kfComp.setData(R"(
        import QtQuick
        import qmlcss
        CssKeyframes {}
    )", QUrl());
    QScopedPointer<QObject> kfObj(kfComp.create());
    QVERIFY2(kfObj, qPrintable(kfComp.errorString()));

    // CssKeyframes is invisible and 0x0 (matching the QML original).
    auto *kf = qobject_cast<QQuickItem *>(kfObj.data());
    QVERIFY(kf);
    QCOMPARE(kf->isVisible(), false);
    QCOMPARE(kf->width(), 0.0);
    QCOMPARE(kf->height(), 0.0);

    // Build two keyframes: opacity 0->1.
    QVariantMap f0, f1, p0, p1;
    p0[QStringLiteral("opacity")] = QStringLiteral("0.0");
    p1[QStringLiteral("opacity")] = QStringLiteral("1.0");
    f0[QStringLiteral("offset")]     = 0.0;
    f0[QStringLiteral("properties")] = p0;
    f1[QStringLiteral("offset")]     = 1.0;
    f1[QStringLiteral("properties")] = p1;
    QVariantList frames;
    frames << f0 << f1;

    kfObj->setProperty("frames", frames);
    kfObj->setProperty("animatedProperty", QStringLiteral("opacity"));
    kfObj->setProperty("target", QVariant::fromValue(target));

    // Directly set progress (bypasses the NumberAnimation event loop) and check _numAt.
    kfObj->setProperty("progress", 0.0);
    QVERIFY2(std::abs(target->property("opacity").toReal() - 0.0) < 0.01,
             qPrintable(QString::number(target->property("opacity").toReal())));

    kfObj->setProperty("progress", 0.5);
    QVERIFY2(std::abs(target->property("opacity").toReal() - 0.5) < 0.01,
             qPrintable(QString::number(target->property("opacity").toReal())));

    kfObj->setProperty("progress", 1.0);
    QVERIFY2(std::abs(target->property("opacity").toReal() - 1.0) < 0.01,
             qPrintable(QString::number(target->property("opacity").toReal())));

    // Quarter-way: 0.25 -> opacity 0.25 (linear).
    kfObj->setProperty("progress", 0.25);
    QVERIFY2(std::abs(target->property("opacity").toReal() - 0.25) < 0.01,
             qPrintable(QString::number(target->property("opacity").toReal())));
}

// --- C++ CssRect @keyframes animation driver -----------------------------------------------
//
// Proves that CssRect's live @keyframes driver is wired: a CssRect with
// `animation: spin 200ms linear infinite` (and a matching @keyframes block in the theme)
// actually ticks animTick via a REAL NumberAnimation and drives rotation() through applyAnim.
//
// Two cases:
//  1. Positive: animation active → rotation() moves away from 0 within 2s.
//  2. Negative: static transform only (no animation key) → rotation() is the parsed value.
//
// Style is injected directly via setProperty("style", map) to bypass the selector-resolution
// path (which has a pre-existing failure unrelated to this driver — parsesKeyframesAndAnimation).
void QmlCssTests::cssRectCppKeyframesDriverLive()
{
    // --- shared setup ---
    // Load @keyframes spin (0deg → 180deg) into the theme.  We don't need a selector rule —
    // we inject style directly, so only keyframes lookup matters here.
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        "@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(180deg); } }"));

    CssLayoutEngine layout(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    // --- positive: animation is live, rotation leaves 0 ---
    {
        QQmlComponent comp(&engine);
        comp.setData(R"(
            import QtQuick
            import qmlcss
            CssRect { width: 60; height: 60 }
        )", QUrl());
        QScopedPointer<QObject> obj(comp.create());
        QVERIFY2(obj, qPrintable(comp.errorString()));
        auto *rect = qobject_cast<CssRect *>(obj.data());
        QVERIFY(rect);

        // Inject style with `animation: spin 200ms linear infinite` and no static transform.
        // This triggers recompute() → buildAnimStops (2 stops) → _animActive=true → NumberAnimation
        // starts → tick fires → applyAnim writes _animRotate → applyTransform sets rotation().
        QVariantMap animStyle;
        animStyle.insert(QStringLiteral("animation"), QStringLiteral("spin 200ms linear infinite"));
        rect->setProperty("style", animStyle);

        // Within 2s (>>200ms animation period) rotation() must leave 0. QTRY_VERIFY pumps the
        // event loop, allowing the NumberAnimation to fire its first tick.
        QTRY_VERIFY_WITH_TIMEOUT(rect->rotation() > 0.5, 2000);
    }

    // --- negative: static transform, no animation — rotation() is the CSS value ---
    {
        QQmlComponent comp(&engine);
        comp.setData(R"(
            import QtQuick
            import qmlcss
            CssRect { width: 60; height: 60 }
        )", QUrl());
        QScopedPointer<QObject> obj(comp.create());
        QVERIFY2(obj, qPrintable(comp.errorString()));
        auto *rect = qobject_cast<CssRect *>(obj.data());
        QVERIFY(rect);

        // Static transform only — _animActive=false, static branch in applyTransform.
        QVariantMap staticStyle;
        staticStyle.insert(QStringLiteral("transform"), QStringLiteral("rotate(45deg)"));
        rect->setProperty("style", staticStyle);

        QVERIFY2(std::abs(rect->rotation() - 45.0) < 0.5,
                 qPrintable(QString::number(rect->rotation())));
    }
}

// --- C++ CssIcon (composition translation of CssIcon.qml) ------------------------------------
//
// Proves CssIcon registers via `import qmlcss`, composes a REAL QtQuick Image and a REAL
// QtQuick.Effects MultiEffect, and correctly drives the icon source URL from a QVariantMap style
// (icon-name key → themeicon URL) and a fallbackSource, as well as the colorization props.
void QmlCssTests::cssIconCppComposesImageAndEffect()
{
    QQmlEngine engine;

    // 1. Basic construction: compose Image + MultiEffect.
    QQmlComponent comp(&engine);
    comp.setData(R"(
        import QtQuick
        import qmlcss
        CssIcon {
            width: 48; height: 48
        }
    )", QUrl());

    QScopedPointer<QObject> object(comp.create());
    QVERIFY2(object, qPrintable(comp.errorString()));
    auto *icon = qobject_cast<CssIcon *>(object.data());
    QVERIFY(icon);

    // Find the composed REAL Image and MultiEffect children.
    QQuickItem *image = nullptr;
    QQuickItem *effect = nullptr;
    for (QQuickItem *k : icon->childItems()) {
        const QByteArray cls(k->metaObject()->className());
        if (cls.contains("QQuickImage"))
            image = k;
        else if (cls.contains("QQuickMultiEffect"))
            effect = k;
    }
    QVERIFY2(image, "composed Image missing");
    QVERIFY2(effect, "composed MultiEffect missing");

    // Image starts invisible (effect is the renderer); effect also invisible (no source loaded yet).
    QCOMPARE(image->isVisible(), false);
    // Initially no source is set, so effect is hidden too.
    QCOMPARE(effect->isVisible(), false);

    // Image fillMode == Image.PreserveAspectFit (1).
    QCOMPARE(image->property("fillMode").toInt(), 1);

    // Effect source points at the composed image.
    QCOMPARE(effect->property("source").value<QQuickItem *>(), image);

    // 2. Colorization props: default colorize=true, color=white.
    QVERIFY2(std::abs(effect->property("colorization").toReal() - 1.0) < 1e-6,
             qPrintable(QString::number(effect->property("colorization").toReal())));
    QCOMPARE(effect->property("colorizationColor").value<QColor>(), QColor(Qt::white));

    // 3. setColorize(false) → colorization 0.
    icon->setColorize(false);
    QVERIFY2(std::abs(effect->property("colorization").toReal() - 0.0) < 1e-6,
             qPrintable(QString::number(effect->property("colorization").toReal())));

    // 4. setColor → colorizationColor updated.
    icon->setColorize(true);
    icon->setColor(QColor(QStringLiteral("#ff4400")));
    QCOMPARE(effect->property("colorizationColor").value<QColor>(), QColor(QStringLiteral("#ff4400")));

    // 5. fallbackSource: setFallbackSource drives the Image source URL.
    icon->setFallbackSource(QStringLiteral("qrc:/icons/fallback.svg"));
    const QString fbSrc = image->property("source").toUrl().toString();
    QCOMPARE(fbSrc, QStringLiteral("qrc:/icons/fallback.svg"));

    // 6. icon-name CSS key → "image://themeicon/<name>|<hexcolor-without-#>".
    //    The color is #ff4400 (set above), hex = "ff4400".
    QVariantMap style;
    style[QStringLiteral("icon-name")] = QStringLiteral("search");
    icon->setStyle(style);
    // Qt encodes the pipe '|' as '%7C' when converting to QUrl internally; decode first.
    const QString themeUrl = image->property("source").toUrl().toString();
    const QString themeDecoded = QUrl::fromPercentEncoding(themeUrl.toUtf8());
    QVERIFY2(themeDecoded.startsWith(QLatin1String("image://themeicon/search|")),
             qPrintable(themeDecoded));
    // The hex part after '|' must be the color without '#': ff4400.
    QVERIFY2(themeDecoded.endsWith(QLatin1String("ff4400")),
             qPrintable(themeDecoded));

    // 7. icon CSS key overrides fallbackSource; sourceFromCssUrl strips url(...).
    QVariantMap style2;
    style2[QStringLiteral("icon")] = QStringLiteral("url(qrc:/icons/star.png)");
    icon->setStyle(style2);
    const QString urlSrc = image->property("source").toUrl().toString();
    QCOMPARE(urlSrc, QStringLiteral("qrc:/icons/star.png"));

    // 8. iconSize auto-derives from geometry (width=height=48, no explicit set).
    // icon was created 48×48 — auto iconSize should be 48.
    QCOMPARE(icon->iconSize(), 48);
}

// --- C++ CssFillLayer (composition translation of CssFillLayer.qml) -------------------------
//
// Proves CssFillLayer registers via `import qmlcss`, composes a REAL QtQuick Shape as its only
// child, and correctly drives the Shape's opacity from peakAlpha (solid color alpha or max stop
// alpha), so the fill renders opaque and alpha is carried on opacity — exactly as the QML did.
void QmlCssTests::cssFillLayerCppComposesShape()
{
    QQmlEngine engine;

    // --- Test 1: solid colour with alpha 0.5 → composed Shape opacity == 0.5 ---------------
    QQmlComponent comp1(&engine);
    comp1.setData(R"(
        import QtQuick
        import qmlcss
        CssFillLayer {
            width: 50; height: 50
            spec: ({ type: "color", color: Qt.rgba(1, 0, 0, 0.5) })
            radii: [0, 0, 0, 0]
        }
    )", QUrl());

    QScopedPointer<QObject> o1(comp1.create());
    QVERIFY2(o1, qPrintable(comp1.errorString()));
    auto *layer1 = qobject_cast<QQuickItem *>(o1.data());
    QVERIFY(layer1);

    // Exactly one composed child: the real Shape.
    const QList<QQuickItem *> kids1 = layer1->childItems();
    QCOMPARE(kids1.size(), 1);
    QQuickItem *shape1 = kids1.first();
    QVERIFY(shape1);
    QVERIFY2(QByteArray(shape1->metaObject()->className()).contains("QQuickShape"),
             shape1->metaObject()->className());
    QVERIFY2(!QByteArray(shape1->metaObject()->className()).contains("QQuickRectangle"),
             shape1->metaObject()->className());

    // peakAlpha = solid color alpha = 0.5 → Shape opacity == 0.5 (alpha on opacity, fill opaque).
    QVERIFY2(std::abs(shape1->property("opacity").toReal() - 0.5) < 0.01,
             qPrintable(QString::number(shape1->property("opacity").toReal())));

    // --- Test 2: linear gradient, stops with alpha 1.0 and 0.8 → peakAlpha == 1.0 ----------
    QQmlComponent comp2(&engine);
    comp2.setData(R"(
        import QtQuick
        import qmlcss
        CssFillLayer {
            width: 60; height: 20
            spec: ({
                type: "linear", angle: 90,
                stops: [
                    { position: 0.0, color: Qt.rgba(1, 0, 0, 1.0) },
                    { position: 1.0, color: Qt.rgba(0, 0, 1, 0.8) }
                ]
            })
            radii: [0, 0, 0, 0]
        }
    )", QUrl());

    QScopedPointer<QObject> o2(comp2.create());
    QVERIFY2(o2, qPrintable(comp2.errorString()));
    auto *layer2 = qobject_cast<QQuickItem *>(o2.data());
    QVERIFY(layer2);

    const QList<QQuickItem *> kids2 = layer2->childItems();
    QCOMPARE(kids2.size(), 1);
    QQuickItem *shape2 = kids2.first();
    QVERIFY(shape2);
    QVERIFY2(QByteArray(shape2->metaObject()->className()).contains("QQuickShape"),
             shape2->metaObject()->className());

    // peakAlpha = max(1.0, 0.8) = 1.0.
    QVERIFY2(std::abs(shape2->property("opacity").toReal() - 1.0) < 0.01,
             qPrintable(QString::number(shape2->property("opacity").toReal())));
}

// --- C++ CssItem — migrated from cssItemAppliesToParent, now uses `import qmlcss` ----------
//
// Proves CssItem registers via `import qmlcss`, infers "rect" from its Rectangle parent (which
// has both `radius` and `color` properties), and correctly applies background-color and
// border-radius onto that parent — same assertions as the original QML-based test.
void QmlCssTests::cssItemCppAppliesToParent()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral("#item2 { background-color: #112233; border-radius: 8px; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    // `import qmlcss` proves the C++ registration (not the removed qrc QML file).
    component.setData(R"(
        import QtQuick
        import qmlcss

        Rectangle {
            width: 30
            height: 30

            CssItem {
                cssId: "item2"
            }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));

    // CssItem inferred "rect" from the Rectangle parent and applied the CSS props.
    const QColor color = object->property("color").value<QColor>();
    QCOMPARE(color, QColor(QStringLiteral("#112233")));
    QCOMPARE(object->property("radius").toReal(), 8.0);
}

// --- C++ Contrast (ported from Contrast.js) --------------------------------------------------
//
// (a) Pure C++ validation: contrastRatio(white, black) ≈ 21, toHex red, blendOver at 0.5,
//     needsDarkIcon on light/dark bg, naturalContrastColor already passing the target.
void QmlCssTests::contrastCppPure()
{
    Contrast c;

    // contrastRatio(white, black) must be close to 21 (WCAG maximum).
    const QColor white = QColor::fromRgbF(1.0, 1.0, 1.0, 1.0);
    const QColor black = QColor::fromRgbF(0.0, 0.0, 0.0, 1.0);
    const double ratio = c.contrastRatio(white, black);
    QVERIFY2(std::abs(ratio - 21.0) < 0.5, qPrintable(QString::number(ratio)));

    // toHex of pure red (#ff0000).
    const QColor red = QColor::fromRgbF(1.0, 0.0, 0.0, 1.0);
    QCOMPARE(c.toHex(red), QStringLiteral("#ff0000"));

    // blendOver: semi-transparent white over black → grey ~(0.5, 0.5, 0.5, 1).
    const QColor semiWhite = QColor::fromRgbF(1.0, 1.0, 1.0, 0.5);
    const QColor blended = c.blendOver(semiWhite, black);
    QVERIFY2(std::abs(blended.redF()   - 0.5) < 0.01, qPrintable(QString::number(blended.redF())));
    QVERIFY2(std::abs(blended.greenF() - 0.5) < 0.01, qPrintable(QString::number(blended.greenF())));
    QVERIFY2(std::abs(blended.blueF()  - 0.5) < 0.01, qPrintable(QString::number(blended.blueF())));
    QCOMPARE(blended.alphaF(), 1.0);

    // needsDarkIcon: white bg is light (luminance ≥ 0.5) → true; black bg → false.
    QCOMPARE(c.needsDarkIcon(white), true);
    QCOMPARE(c.needsDarkIcon(black), false);

    // naturalContrastColor: fg already satisfying the target is returned unchanged.
    // white vs black contrast ≈ 21; target 4.5 → should return white as-is.
    const QColor result = c.naturalContrastColor(white, black, 4.5);
    QCOMPARE(result, white);
}

// (b) QML singleton validation: `import qmlcss` + `Contrast.toHex(...)` — proves registration.
void QmlCssTests::contrastQmlSingleton()
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            property string hexRed: Contrast.toHex(Qt.rgba(1, 0, 0, 1))
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    QCOMPARE(object->property("hexRed").toString(), QStringLiteral("#ff0000"));
}

// The QML original duck-typed: `if (holder.parent.requestRelayout) holder.parent.requestRelayout()`.
// A plain-Item grandparent (window content, transpiler wrapper, ...) has no requestRelayout();
// it must be skipped silently — invokeMethod-by-name warns "No such method" on every notify.
void QmlCssTests::layoutNotifyParentSkipsNonBoxAncestors()
{
    static QStringList warnings;
    warnings.clear();
    const auto previous = qInstallMessageHandler(
        [](QtMsgType type, const QMessageLogContext &, const QString &msg) {
            if (type == QtWarningMsg)
                warnings.append(msg);
        });

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {            // plain grandparent — no requestRelayout()
            Item {        // holder
                CssText { text: "hi" }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> object(component.create());

    CssTheme theme;
    CssLayoutEngine layout(&theme);
    auto *grandparent = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(grandparent);
    QQuickItem *holder = grandparent->childItems().value(0);
    QVERIFY(holder);
    QQuickItem *text = holder->childItems().value(0);
    QVERIFY(text);

    layout.notifyParentLayout(text);

    qInstallMessageHandler(previous);
    for (const QString &w : std::as_const(warnings))
        QVERIFY2(!w.contains(QLatin1String("No such method")),
                 qPrintable(QStringLiteral("notifyParentLayout warned: ") + w));
}

// End-to-end web-parity scoping: `.nav button` must style ONLY buttons under a `.nav`
// element (the chain is collected from the item tree by applyCssTo), and toggling a class
// on the ANCESTOR (`button.active` here) must restyle scoped DESCENDANT rules
// (`.nav button.active text`) — the text child itself never changes its own identity.
void QmlCssTests::ancestorScopedRulesStyleTheTree()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        text { color: #101010; }
        .nav button { background-color: #112233; }
        .nav button.active text { color: #ffffff; }
    )"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            width: 400; height: 200
            CssRect {
                objectName: "nav"
                cssClass: ["nav"]
                cssPrimitive: "div"
                CssFill {
                    objectName: "inside"
                    cssPrimitive: "button"
                    CssText { objectName: "label"; cssPrimitive: "text"; text: "hi" }
                }
            }
            CssFill {
                objectName: "outside"
                cssPrimitive: "button"
            }
        }
    )", QUrl());

    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QObject *inside = root->findChild<QObject *>(QStringLiteral("inside"));
    QObject *outside = root->findChild<QObject *>(QStringLiteral("outside"));
    QObject *label = root->findChild<QObject *>(QStringLiteral("label"));
    QVERIFY(inside && outside && label);

    // Scoped: only the button under .nav gets the background.
    QCOMPARE(inside->property("style").toMap().value(QStringLiteral("background-color")).toString(),
             QStringLiteral("#112233"));
    QVERIFY(!outside->property("style").toMap().contains(QStringLiteral("background-color")));

    // Label starts at the bare `text {}` colour.
    QCOMPARE(label->property("style").toMap().value(QStringLiteral("color")).toString(),
             QStringLiteral("#101010"));

    // Turning the BUTTON active must restyle the LABEL (ancestor-scoped rule).
    inside->setProperty("cssClass", QVariant(QStringList{QStringLiteral("active")}));
    QCOMPARE(label->property("style").toMap().value(QStringLiteral("color")).toString(),
             QStringLiteral("#ffffff"));
}

// vh heights must follow setViewport without any item geometry change: the layout engine
// re-queues every known root on viewportChanged (single-event tiling resizes went stale).
void QmlCssTests::viewportChangeRelayoutsVhHeights()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".page { height: calc(100vh - 40px); }"));
    theme.setViewport(800, 400);

    QQmlEngine engine;
    CssLayoutEngine layout(&theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssPrimitive: ""
            width: 800; height: 1000
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRect {
                objectName: "page"
                cssClass: ["page"]
                cssPrimitive: "div"
            }
        }
    )", QUrl());

    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QObject *page = root->findChild<QObject *>(QStringLiteral("page"));
    QVERIFY(page);
    QTRY_COMPARE(page->property("height").toReal(), 360.0); // 400 - 40

    // Height-only viewport change (the tiling case): no item geometry changes, ONLY the
    // viewport — the page must still follow.
    theme.setViewport(800, 600);
    QTRY_COMPARE(page->property("height").toReal(), 560.0); // 600 - 40
}

// A viewport change re-queues EVERY known root (vh resolves at layout time). Without a
// batch, each requestLayout() outside a batch flushes synchronously on its own — R
// separate flush() calls for R known roots, and any cascade that bubbles back up to an
// ancestor WITHIN one root's own flush can re-lay that ancestor again when relayoutKnownRoots
// reaches it explicitly later. Wrapping the whole loop in one batch collapses the redundant
// entries (m_pending is keyed by root) into a single flush.
void QmlCssTests::viewportChangeIsOneFlush()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10vh; }"));
    CssLayoutEngine layoutEngine(&theme);
    theme.setViewport(800, 400);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssPrimitive: ""
            width: 200; height: 400
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRepeater {
                model: 100
                delegate: Component {
                    CssRect { cssClass: ["row"]; cssPrimitive: "div"
                        CssRect { cssPrimitive: "div" } }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    const quint64 before = layoutEngine.childVisits();
    theme.setViewport(800, 600);
    const quint64 visits = layoutEngine.childVisits() - before;
    // Measured baseline: 301 visits (the container's 101 children + each row visited TWICE — its own
    // pending entry and the parent's cascade, which must first fix the row's cross size — + leaves).
    // Batching the viewport loop removed R-1 flush calls, not visits; 402 still catches a regression
    // that re-lays the boxes once more.
    QVERIFY2(visits <= 2 * 201, qPrintable(QStringLiteral("viewport visits %1").arg(visits)));
}

// Popup/Menu contents are reparented by Qt to the window Overlay, severing the visual
// parent chain that descendant selectors are matched against. The declared
// `property Item cssAncestor: owner` must re-anchor the walk at the logical owner so
// `.wg-select .popup` still styles the popup — and rules NOT scoped to the owner must
// keep NOT matching.
void QmlCssTests::cssAncestorReanchorsDetachedSubtree()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .wg-select .popup { background-color: #ffffff; }
        .other .popup { background-color: #ff0000; }
    )"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            width: 400; height: 200
            CssFill {
                id: sel
                objectName: "select"
                cssClass: ["wg-select"]
                cssPrimitive: "select"
            }
            Item {
                objectName: "overlay" // plain Item: simulates the window Overlay (no CSS identity)
                CssFill {
                    objectName: "popup"
                    cssPrimitive: "div"
                    cssClass: ["popup"]
                    property Item cssAncestor: sel
                }
                CssFill {
                    objectName: "orphan"
                    cssPrimitive: "div"
                    cssClass: ["popup"] // same class, NO re-anchor: scoped rules must not match
                }
            }
        }
    )", QUrl());

    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QObject *popup = root->findChild<QObject *>(QStringLiteral("popup"));
    QObject *orphan = root->findChild<QObject *>(QStringLiteral("orphan"));
    QVERIFY(popup && orphan);

    QCOMPARE(popup->property("style").toMap().value(QStringLiteral("background-color")).toString(),
             QStringLiteral("#ffffff"));
    QVERIFY(!orphan->property("style").toMap().contains(QStringLiteral("background-color")));
}

// text-shadow must not blank the label: MultiEffect sources the composed Text, and a
// visible:false source yields an EMPTY texture on the RHI path (the caption vanished).
// The label stays visible; the effect paints the same glyphs + shadow above it.
void QmlCssTests::cssTextShadowKeepsLabelVisible()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        ".cap { text-shadow: 0 2px 8px rgba(0, 0, 0, 0.65); color: #ffffff; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssText { cssClass: ["cap"]; cssPrimitive: "text"; text: "hi" }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QQuickItem *label = composedText(root.data());
    QVERIFY(label);
    QVERIFY2(label->isVisible(), "text-shadow hid the composed Text (empty-texture source)");
}

// The web paints `background-color` behind ANY element, text included — CssText composes a
// Shape underlay (never Rectangle) sized to its box. No background declared → underlay hidden.
void QmlCssTests::cssTextCppPaintsBackground()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        ".tile { background-color: #313244; border-radius: 4px; color: #cdd6f4; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            width: 300; height: 60
            CssText { objectName: "tiled"; cssClass: ["tile"]; cssPrimitive: "text"; text: "hi"; width: 260; height: 20 }
            CssText { objectName: "bare"; cssPrimitive: "text"; text: "plain" }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QObject *tiled = root->findChild<QObject *>(QStringLiteral("tiled"));
    QObject *bare = root->findChild<QObject *>(QStringLiteral("bare"));
    QVERIFY(tiled && bare);

    QObject *bg = tiled->findChild<QObject *>(QStringLiteral("cssTextBg"));
    QVERIFY2(bg, "CssText did not compose the background underlay");
    QCOMPARE(bg->property("bgColor").value<QColor>(), QColor(QStringLiteral("#313244")));
    QCOMPARE(bg->property("bgRadius").toReal(), 4.0);
    QVERIFY(bg->property("visible").toBool());
    // The underlay tracks the item's box.
    QCOMPARE(bg->property("width").toReal(), 260.0);
    QCOMPARE(bg->property("height").toReal(), 20.0);

    // No background declared → the underlay stays hidden (or is never created).
    QObject *bareBg = bare->findChild<QObject *>(QStringLiteral("cssTextBg"));
    QVERIFY(!bareBg || !bareBg->property("visible").toBool());
}

// The transpiler's <Show> gate is a `visible:` BINDING on the delegate root. CssRect used to
// run setVisible(display != "none") on EVERY style apply — an imperative write that severs the
// binding, so every <Show> inside a styled tree leaked visible. Only a declared `display` may
// drive visible.
void QmlCssTests::styleApplyPreservesVisibleBinding()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".badge { background-color: #41cd52; }"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            width: 100; height: 40
            property bool showIt: false
            CssRect { objectName: "badge"; cssClass: ["badge"]; cssPrimitive: "div"; visible: parent.showIt }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    auto *badge = root->findChild<QQuickItem *>(QStringLiteral("badge"));
    QVERIFY(badge);
    // The style (no `display`) was applied on completion; the binding must have survived.
    QCOMPARE(badge->isVisible(), false);
    root->setProperty("showIt", true);
    QCOMPARE(badge->isVisible(), true);

    // And `display: none` still hides — by PAINT (opacity 0) and input (disabled), with
    // `visible` (and its binding) untouched; the layout skips it by style.
    theme.loadFromString(QStringLiteral(".badge { display: none; }"));
    theme.loadCss(badge);
    QCOMPARE(badge->opacity(), 0.0);
    QVERIFY(!badge->isEnabled());
    QCOMPARE(badge->isVisible(), true); // author binding still owns `visible`

    // Removing display:none restores paint AND the binding still works.
    theme.loadFromString(QStringLiteral(".badge { background-color: #41cd52; }"));
    theme.loadCss(badge);
    QCOMPARE(badge->opacity(), 1.0);
    root->setProperty("showIt", false);
    QCOMPARE(badge->isVisible(), false); // the <Show> guard survived the round-trip
}

// `transition: opacity <ms>` must ANIMATE the item's opacity on restyle, not snap — the
// carousel crossfade depends on it. Sampled mid-flight with generous margins (CI-safe).
void QmlCssTests::transitionAnimatesItemOpacity()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .slide { opacity: 1; transition: opacity 400ms linear; }
        .slide.off { opacity: 0; }
    )"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { objectName: "s"; cssClass: ["slide"]; cssPrimitive: "div"; width: 100; height: 50 }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *s = qobject_cast<QQuickItem *>(root.data());
    QVERIFY(s);
    QCOMPARE(s->opacity(), 1.0);

    s->setProperty("cssClass", QVariant(QStringList{QStringLiteral("slide"), QStringLiteral("off")}));
    QTest::qWait(120); // ~30% through a 400ms linear fade
    const qreal mid = s->opacity();
    QVERIFY2(mid > 0.02 && mid < 0.98, qPrintable(QStringLiteral("opacity snapped: %1").arg(mid)));
    QTRY_VERIFY_WITH_TIMEOUT(s->opacity() < 0.02, 2000); // settles at the target
}

// `transition: width <ms>` must animate the layout-applied width — the sidebar collapse.
void QmlCssTests::transitionAnimatesLayoutWidth()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .side { width: 200px; transition: width 400ms linear; }
        .side.collapsed { width: 60px; }
    )"));
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssPrimitive: ""
            width: 400; height: 100
            style: ({ "display": "flex", "flex-direction": "row" })
            CssRect { objectName: "side"; cssClass: ["side"]; cssPrimitive: "div" }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *side = root->findChild<QQuickItem *>(QStringLiteral("side"));
    QVERIFY(side);
    QTRY_COMPARE_WITH_TIMEOUT(side->width(), 200.0, 2000);

    side->setProperty("cssClass", QVariant(QStringList{QStringLiteral("side"), QStringLiteral("collapsed")}));
    QTest::qWait(120);
    const qreal mid = side->width();
    QVERIFY2(mid < 198.0 && mid > 62.0, qPrintable(QStringLiteral("width snapped: %1").arg(mid)));
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(side->width() - 60.0) < 1.0, 2000);
}

// A `.card:hover` rule must flip cssHoverStyled on a matching CssRect (which then composes
// its own HoverHandler), and the engine-tracked hover state must restyle: forcing the
// internal flag resolves the :hover declarations.
void QmlCssTests::hoverRuleEnablesEngineHoverTracking()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .card { background-color: #ffffff; }
        .card:hover { background-color: #ff0000; }
        .plain { background-color: #eeeeee; }
    )"));

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        Item {
            CssRect { objectName: "card"; cssClass: ["card"]; cssPrimitive: "div"; width: 50; height: 50 }
            CssRect { objectName: "plain"; cssClass: ["plain"]; cssPrimitive: "div"; width: 50; height: 50 }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QObject *card = root->findChild<QObject *>(QStringLiteral("card"));
    QObject *plain = root->findChild<QObject *>(QStringLiteral("plain"));
    QVERIFY(card && plain);
    QVERIFY2(card->property("cssHoverStyled").toBool(), "matching :hover rule not detected");
    QVERIFY(!plain->property("cssHoverStyled").toBool());

    // Simulate the tracked state (the composed HoverHandler writes the same property).
    card->setProperty("cssEngineHover", true);
    QCOMPARE(card->property("style").toMap().value(QStringLiteral("background-color")).toString(),
             QStringLiteral("#ff0000"));
    card->setProperty("cssEngineHover", false);
    QCOMPARE(card->property("style").toMap().value(QStringLiteral("background-color")).toString(),
             QStringLiteral("#ffffff"));
}

// `overflow-y: auto` (or scroll) turns the box's content area into a REAL Flickable —
// wheel/drag scrolling comes from QtQuick, not a reimplementation. contentHeight tracks the
// laid-out children so it scrolls exactly the overflow.
void QmlCssTests::overflowScrollComposesFlickable()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .list { overflow-y: auto; display: flex; flex-direction: column; }
        .row { height: 60px; }
    )"));
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            objectName: "list"
            cssClass: ["list"]
            cssPrimitive: "div"
            width: 200; height: 150
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    // 5 x 60px rows in a 150px viewport -> a Flickable with ~300px of content.
    QQuickItem *flick = nullptr;
    const auto kids = root->findChildren<QQuickItem *>();
    for (QQuickItem *k : kids) {
        if (QString::fromLatin1(k->metaObject()->className()).contains(QLatin1String("Flickable"))) {
            flick = k;
            break;
        }
    }
    QVERIFY2(flick, "overflow-y: auto did not compose a Flickable");
    QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentHeight").toReal() >= 295.0, 2000);
    QVERIFY(flick->clip());

    // It actually scrolls: content offset moves.
    flick->setProperty("contentY", 120.0);
    QCOMPARE(flick->property("contentY").toReal(), 120.0);
}

// Flyweight <For>: reorder must reuse the SAME delegate items (moved, not recreated).
void QmlCssTests::cssRepeaterReordersWithoutRecreating()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssPrimitive: ""
            width: 200; height: 200
            property var order: ["a", "b", "c"]
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRepeater {
                model: box.order
                delegate: Component {
                    CssRect { objectName: "row-" + modelData; cssClass: ["row"]; cssPrimitive: "div" }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    auto rows = [&]() {
        QHash<QString, QQuickItem *> out;
        for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
            if (k->objectName().startsWith(QLatin1String("row-")))
                out.insert(k->objectName(), k);
        }
        return out;
    };
    const auto before = rows();
    QCOMPARE(before.size(), 3);
    QQuickItem *a = before.value(QStringLiteral("row-a"));
    QQuickItem *c = before.value(QStringLiteral("row-c"));
    QVERIFY(a && c);

    // Reorder: c moves first — the INSTANCES must survive.
    root->setProperty("order", QVariant(QStringList{QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("b")}));
    const auto after = rows();
    QCOMPARE(after.size(), 3);
    QCOMPARE(after.value(QStringLiteral("row-a")), a); // same pointer: reused, not recreated
    QCOMPARE(after.value(QStringLiteral("row-c")), c);

    // Layout follows the new order after the flush: c above a.
    QTRY_VERIFY_WITH_TIMEOUT(c->y() < a->y(), 2000);

    // Removal destroys only the removed row; the rest survive.
    root->setProperty("order", QVariant(QStringList{QStringLiteral("c"), QStringLiteral("a")}));
    QTRY_COMPARE_WITH_TIMEOUT(rows().size(), 2, 2000);
    QCOMPARE(rows().value(QStringLiteral("row-a")), a);
}

// A transition animates a CHANGE, never the first style. The fast Rectangle received
// transitionMs before its first colours, so every new element with a `transition` faded in from
// the defaults — a freshly created button "lit up" (and a border came in black). The first style
// must land at once; a later class change must still animate. The Shape shell never had the bug
// (its first cssIn lands before its Behaviors are live); its case stays as the guarantee.
void QmlCssTests::transitionSkipsFirstStyle()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .btn { background-color: #2244aa; border: 1px solid #ffcc00; border-radius: 4px;
               transition: background-color 400ms linear; }
        .btn.on { background-color: #aa4422; }
    )"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { cssClass: ["btn"]; cssPrimitive: "div"; width: 80; height: 24 }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QQuickItem *rect = nullptr;
    for (QQuickItem *k : root->findChildren<QQuickItem *>())
        if (QByteArray(k->metaObject()->className()).contains("QQuickRectangle")) rect = k;
    QVERIFY2(rect, "a solid box should take the fast Rectangle path");

    // First style: final colours at once, not mid-fade from the defaults.
    QCOMPARE(rect->property("color").value<QColor>(), QColor(QStringLiteral("#2244aa")));
    QObject *border = qvariant_cast<QObject *>(rect->property("border"));
    QVERIFY(border);
    QCOMPARE(border->property("color").value<QColor>(), QColor(QStringLiteral("#ffcc00")));

    // A change still animates: not there at once, there after the transition.
    root->setProperty("cssClass", QVariant(QStringList{QStringLiteral("btn"), QStringLiteral("on")}));
    QVERIFY(rect->property("color").value<QColor>() != QColor(QStringLiteral("#aa4422")));
    QTRY_COMPARE_WITH_TIMEOUT(rect->property("color").value<QColor>(), QColor(QStringLiteral("#aa4422")), 2000);

    // The Shape shell (a per-side border forces it): its fill path is at the final colour at birth.
    QQmlComponent shellComponent(&engine);
    shellComponent.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { cssClass: ["shell"]; cssPrimitive: "div"; width: 80; height: 24 }
    )", QUrl());
    theme.loadFromString(QStringLiteral(R"(
        .shell { background-color: #2244aa; border-left: 3px solid #ffcc00;
                 transition: background-color 400ms linear; }
    )"));
    QScopedPointer<QObject> shell(shellComponent.create());
    QVERIFY2(shell, qPrintable(shellComponent.errorString()));
    QObject *fillPath = nullptr;
    for (QObject *k : shell->findChildren<QObject *>())
        if (!fillPath && QByteArray(k->metaObject()->className()).contains("QQuickShapePath")) fillPath = k;
    QVERIFY2(fillPath, "a per-side border should take the Shape path");
    QCOMPARE(fillPath->property("fillColor").value<QColor>(), QColor(QStringLiteral("#2244aa")));
}

// The Shape x Rectangle policy: a rectangle-safe style composes a REAL QQuickRectangle
// (batchable scene-graph node) and NO Shape shell; a reapply that demands Shape features
// (gradient) swaps the composition — and back.
void QmlCssTests::rectanglePolicyFastPathAndSwap()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .card { background-color: #2244aa; border: 2px solid #ffcc00; border-radius: 6px 6px 0 0; }
        .card.fancy { background: linear-gradient(180deg, #2244aa, #112255); }
    )"));
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { objectName: "card"; cssClass: ["card"]; cssPrimitive: "div"; width: 100; height: 40 }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    auto findClass = [&](const char *cls) -> QQuickItem * {
        for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
            if (QString::fromLatin1(k->metaObject()->className()).contains(QLatin1String(cls)))
                return k;
        }
        return nullptr;
    };

    // Fast path: a Rectangle, no Shape.
    QQuickItem *rect = findClass("QQuickRectangle");
    QVERIFY2(rect, "rectangle-safe style did not take the fast Rectangle path");
    QVERIFY(!findClass("QQuickShape"));
    QCOMPARE(rect->property("color").value<QColor>(), QColor(QStringLiteral("#2244aa")));
    QCOMPARE(rect->property("topLeftRadius").toReal(), 6.0);
    QCOMPARE(rect->property("bottomRightRadius").toReal(), 0.0);

    // Reapply with a gradient: the policy swaps to the Shape shell.
    root->setProperty("cssClass", QVariant(QStringList{QStringLiteral("card"), QStringLiteral("fancy")}));
    QTRY_VERIFY_WITH_TIMEOUT(findClass("QQuickShape") != nullptr, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(findClass("QQuickRectangle") == nullptr, 2000);

    // And back.
    root->setProperty("cssClass", QVariant(QStringList{QStringLiteral("card")}));
    QTRY_VERIFY_WITH_TIMEOUT(findClass("QQuickRectangle") != nullptr, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(findClass("QQuickShape") == nullptr, 2000);
}

QTEST_MAIN(QmlCssTests)

void QmlCssTests::layoutReplacedForeignBoxKeepsIntrinsicSize()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);

    // Default block container (align-items stretch): "wrap" holds only a foreign Item with a
    // 140×140 intrinsic — it must keep 140×140, NOT stretch to the 500px cross axis (the
    // escape-hatch / qml-module wrap: a distorted gauge was the reported bug). "wide" holds an
    // 800×400 foreign — capped to the container width with the ratio preserved → 500×250.
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss

        CssRect {
            cssPrimitive: ""
            width: 500; height: 600

            CssRect {
                objectName: "wrap"; cssPrimitive: ""
                Item { implicitWidth: 140; implicitHeight: 140 }
            }
            CssRect {
                objectName: "wide"; cssPrimitive: ""
                Item { implicitWidth: 800; implicitHeight: 400 }
            }
            CssRect { objectName: "plain"; cssPrimitive: ""; style: ({ "height": "20px" }) }
        }
    )", QUrl());

    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);

    auto *wrap = root->findChild<QQuickItem *>(QStringLiteral("wrap"));
    auto *wide = root->findChild<QQuickItem *>(QStringLiteral("wide"));
    auto *plain = root->findChild<QQuickItem *>(QStringLiteral("plain"));
    QVERIFY(wrap && wide && plain);

    layoutEngine.layout(root, contentHolderOf(wrap));

    // Intrinsic, unstretched.
    QVERIFY2(std::abs(wrap->width() - 140.0) < 0.5, qPrintable(QString::number(wrap->width())));
    QVERIFY2(std::abs(wrap->height() - 140.0) < 0.5, qPrintable(QString::number(wrap->height())));
    // Capped at the container, ratio kept.
    QVERIFY2(std::abs(wide->width() - 500.0) < 0.5, qPrintable(QString::number(wide->width())));
    QVERIFY2(std::abs(wide->height() - 250.0) < 0.5, qPrintable(QString::number(wide->height())));
    // An ORDINARY box (no foreign content) still stretches to the container width.
    QVERIFY2(std::abs(plain->width() - 500.0) < 0.5, qPrintable(QString::number(plain->width())));
}

void QmlCssTests::cssRepeaterKeysObjectsById()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssPrimitive: ""
            width: 200; height: 200
            property string json: '[{"id":1,"t":"a"},{"id":2,"t":"b"},{"id":3,"t":"c"}]'
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRepeater {
                model: JSON.parse(box.json)
                delegate: Component {
                    CssRect { objectName: "row-" + modelData.id; property string label: modelData.t
                              cssClass: ["row"]; cssPrimitive: "div" }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    auto rows = [&]() {
        QHash<QString, QQuickItem *> out;
        for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
            if (k->objectName().startsWith(QLatin1String("row-")) && k->parentItem())
                out.insert(k->objectName(), k);
        }
        return out;
    };
    const auto before = rows();
    QCOMPARE(before.size(), 3);
    QPointer<QQuickItem> one = before.value(QStringLiteral("row-1"));
    QPointer<QQuickItem> two = before.value(QStringLiteral("row-2"));
    QVERIFY(one && two);

    // Fresh objects (a new JSON.parse): 2 moves first, 1's text changed, 3 gone, 4 new.
    root->setProperty("json", QStringLiteral(R"([{"id":2,"t":"b"},{"id":1,"t":"A"},{"id":4,"t":"d"}])"));
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const auto after = rows();
    QCOMPARE(after.value(QStringLiteral("row-1")), one.data());   // the same instances
    QCOMPARE(after.value(QStringLiteral("row-2")), two.data());
    QCOMPARE(one->property("label").toString(), QStringLiteral("A"));   // updated in place
    QVERIFY(after.contains(QStringLiteral("row-4")));
    QVERIFY(!after.contains(QStringLiteral("row-3")));
    // Document order (the layout's walk) follows the model: 2 before 1.
    const QList<QQuickItem *> order = one->parentItem()->childItems();
    QVERIFY(order.indexOf(two.data()) < order.indexOf(one.data()));
}

void QmlCssTests::layoutCountsChildVisits()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssPrimitive: ""
            width: 200; height: 200
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    // One relayout of a settled two-child box visits exactly its two children. Exactly one pass: the
    // rows' heights are fixed and their stretched width is resolved in that same pass, so place()
    // moves nothing past its 0.5px threshold and nothing is re-queued. If a future engine change makes
    // this fixture need a second pass, the delta becomes 4 — that is the change to investigate.
    const quint64 before = layoutEngine.childVisits();
    QVERIFY(QMetaObject::invokeMethod(root.get(), "requestRelayout"));
    QCOMPARE(layoutEngine.childVisits() - before, quint64(2));
}

void QmlCssTests::cssRepeaterAppendAndClearAreLinear()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssPrimitive: ""
            width: 200; height: 200
            property int count: 0
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRepeater {
                model: box.count
                delegate: Component {
                    CssRect { cssClass: ["row"]; cssPrimitive: "div" }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    // Post-mount mutations: the path real apps hit when data arrives after load (mount itself is
    // batched by the loader). Unbatched, each inserted/removed row re-laid the whole container:
    // appending n to n cost ~n*1.5n visits (~135000 for n = 300).
    constexpr int n = 300;
    root->setProperty("count", n);
    quint64 before = layoutEngine.childVisits();
    root->setProperty("count", 2 * n);
    const quint64 appendVisits = layoutEngine.childVisits() - before;
    before = layoutEngine.childVisits();
    root->setProperty("count", 0);
    const quint64 clearVisits = layoutEngine.childVisits() - before;

    QVERIFY2(appendVisits < quint64(8 * 2 * n), qPrintable(QStringLiteral("append visits %1").arg(appendVisits)));
    QVERIFY2(clearVisits < quint64(8 * 2 * n), qPrintable(QStringLiteral("clear visits %1").arg(clearVisits)));
}

// Tearing a list down — app exit, a <Show> dropping its page, any ancestor deleted — must cost
// O(N) layout work. Each destroyed row used to leave its container's content holder, which
// re-laid the remaining rows synchronously: 2000 rows cost ~8M child visits on exit.
void QmlCssTests::listTeardownIsLinear()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQuickWindow window;

    // Returns the child visits spent tearing down a list of n rows: the whole tree when
    // `repeaterOnly` is false, else just the CssRepeater (the container survives and must end up
    // laid out for zero rows).
    const auto teardownVisits = [&](int n, bool repeaterOnly) -> quint64 {
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import qmlcss
            CssRect {
                cssPrimitive: ""
                width: 200; height: 900
                style: ({ "display": "flex", "flex-direction": "column" })
                property alias list: list
                CssRect {
                    id: list
                    cssPrimitive: "div"
                    property int count: 0
                    style: ({ "display": "flex", "flex-direction": "column" })
                    CssRepeater {
                        model: list.count
                        delegate: Component {
                            CssRect { cssClass: ["row"]; cssPrimitive: "div"
                                CssText { text: "a" }
                                CssText { text: "b" }
                            }
                        }
                    }
                }
            }
        )", QUrl());
        QScopedPointer<QObject> root(component.create());
        if (!root) {
            qWarning() << component.errorString();
            return ~quint64(0);
        }
        qobject_cast<QQuickItem *>(root.data())->setParentItem(window.contentItem());
        auto *list = root->property("list").value<QQuickItem *>();
        list->setProperty("count", n);
        if (!qFuzzyCompare(list->implicitHeight(), qreal(10 * n))) {
            qWarning() << "mounted list height" << list->implicitHeight();
            return ~quint64(0);
        }
        const quint64 before = layoutEngine.childVisits();
        if (repeaterOnly) {
            delete list->findChild<CssRepeater *>();
            const quint64 visits = layoutEngine.childVisits() - before;
            if (!qFuzzyIsNull(list->implicitHeight())) {
                qWarning() << "emptied list height" << list->implicitHeight();
                return ~quint64(0);
            }
            return visits;
        }
        root.reset();
        return layoutEngine.childVisits() - before;
    };

    for (const bool repeaterOnly : { false, true }) {
        const quint64 small = teardownVisits(200, repeaterOnly);
        const quint64 large = teardownVisits(400, repeaterOnly);
        const QString msg = QStringLiteral("%1: visits(200) %2, visits(400) %3")
            .arg(repeaterOnly ? QStringLiteral("repeater only") : QStringLiteral("whole tree"))
            .arg(small).arg(large);
        QVERIFY2(large <= 3 * small + 50, qPrintable(msg));
        QVERIFY2(large < quint64(400 * 10), qPrintable(msg));
    }
}

// Showing a hidden list again — a <Show> page coming back, `visible: false -> true` on any
// ancestor — must cost O(N) layout work and leave the same geometry. Qt flips the whole subtree's
// effective visibility first, then emits visibleChanged deepest first: every row used to re-lay
// its container synchronously, O(N^2) per re-shown list. Rows added while hidden must appear.
void QmlCssTests::listShowIsLinear()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQuickWindow window;

    // The y of every visible row, in document order.
    const auto rowYs = [](QQuickItem *list) {
        QList<qreal> ys;
        const auto kids = qobject_cast<CssRect *>(list)->layoutContentHolder()->childItems();
        for (QQuickItem *k : kids) {
            if (qobject_cast<CssRect *>(k) && k->isVisible())
                ys.push_back(k->y());
        }
        return ys;
    };

    // Returns the child visits spent showing a list of n rows again after hiding `toggled`: the
    // list itself, or a plain Item wrapping it. `added` rows join while the list is hidden.
    const auto showVisits = [&](int n, bool wrapper, int added) -> quint64 {
        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import qmlcss
            CssRect {
                cssPrimitive: ""
                width: 200; height: 900
                style: ({ "display": "flex", "flex-direction": "column" })
                property alias list: list
                property alias wrapper: wrapper
                CssRect {
                    cssPrimitive: "div"
                    style: ({ "display": "flex", "flex-direction": "column" })
                    Item {
                        id: wrapper
                        width: 200; height: 900
                        CssRect {
                            id: list
                            width: 200
                            cssPrimitive: "div"
                            property int count: 0
                            style: ({ "display": "flex", "flex-direction": "column" })
                            CssRepeater {
                                model: list.count
                                delegate: Component {
                                    CssRect { cssClass: ["row"]; cssPrimitive: "div"
                                        CssText { text: "a" }
                                        CssText { text: "b" }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        )", QUrl());
        QScopedPointer<QObject> root(component.create());
        if (!root) {
            qWarning() << component.errorString();
            return ~quint64(0);
        }
        qobject_cast<QQuickItem *>(root.data())->setParentItem(window.contentItem());
        auto *list = root->property("list").value<QQuickItem *>();
        auto *toggled = wrapper ? root->property("wrapper").value<QQuickItem *>() : list;
        list->setProperty("count", n);
        if (!qFuzzyCompare(list->implicitHeight(), qreal(10 * n))) {
            qWarning() << "mounted list height" << list->implicitHeight();
            return ~quint64(0);
        }
        toggled->setVisible(false);
        if (added)
            list->setProperty("count", n + added);
        const quint64 before = layoutEngine.childVisits();
        toggled->setVisible(true);
        const quint64 visits = layoutEngine.childVisits() - before;
        // Settled synchronously: geometry is read right after setVisible returns.
        const int total = n + added;
        if (!qFuzzyCompare(list->implicitHeight(), qreal(10 * total))) {
            qWarning() << "re-shown list height" << list->implicitHeight() << "expected" << 10 * total;
            return ~quint64(0);
        }
        const QList<qreal> ys = rowYs(list);
        if (ys.size() != total) {
            qWarning() << "re-shown rows" << ys.size() << "expected" << total;
            return ~quint64(0);
        }
        for (int i = 0; i < total; ++i) {
            if (!qFuzzyCompare(ys[i] + 1, qreal(10 * i) + 1)) {
                qWarning() << "row" << i << "y" << ys[i];
                return ~quint64(0);
            }
        }
        return visits;
    };

    for (const bool wrapper : { false, true }) {
        for (const int added : { 0, 50 }) {
            const quint64 small = showVisits(200, wrapper, added);
            const quint64 large = showVisits(400, wrapper, added);
            const QString msg = QStringLiteral("%1, +%2 while hidden: visits(200) %3, visits(400) %4")
                .arg(wrapper ? QStringLiteral("wrapper") : QStringLiteral("list"))
                .arg(added).arg(small).arg(large);
            qInfo().noquote() << msg;
            QVERIFY2(large <= 3 * small + 50, qPrintable(msg));
            QVERIFY2(large < quint64(400 * 10), qPrintable(msg));
        }
    }
}

void QmlCssTests::contentHolderReportsLateForeignChild()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssPrimitive: ""
            width: 200
            style: ({ "display": "flex", "flex-direction": "column" })
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *box = qobject_cast<CssRect *>(root.get());
    QVERIFY(box);

    // The holder reports children one at a time (no rescan of every sibling per change).
    auto *holder = qobject_cast<CssContentHolder *>(box->layoutContentHolder());
    QVERIFY2(holder, "content holder is not a CssContentHolder");
    QSignalSpy added(holder, &CssContentHolder::childAdded);

    // A foreign (non-Css) child added AFTER mount still drives the box's size when it grows.
    auto *foreign = new QQuickItem;
    foreign->setParent(holder);
    foreign->setParentItem(holder);
    QCOMPARE(added.count(), 1);
    foreign->setImplicitWidth(40);
    foreign->setImplicitHeight(50);
    QTRY_COMPARE_WITH_TIMEOUT(box->implicitHeight(), 50.0, 1000);

    QSignalSpy removed(holder, &CssContentHolder::childRemoved);
    foreign->setParentItem(nullptr);
    QCOMPARE(removed.count(), 1);
}

void QmlCssTests::scrollExtentFollowsLayoutOnClear()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .list { overflow-y: auto; display: flex; flex-direction: column; }
        .row { height: 20px; }
    )"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssClass: ["list"]
            cssPrimitive: "div"
            width: 200; height: 150
            property int count: 50
            CssRepeater {
                model: box.count
                delegate: Component {
                    CssRect { cssClass: ["row"]; cssPrimitive: "div" }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QQuickItem *flick = nullptr;
    for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
        if (QString::fromLatin1(k->metaObject()->className()).contains(QLatin1String("Flickable"))) {
            flick = k;
            break;
        }
    }
    QVERIFY2(flick, "overflow-y: auto did not compose a Flickable");
    // 50 rows x 20px.
    QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentHeight").toReal() >= 995.0, 2000);

    // Clearing shrinks the extent back to the viewport — computed by the layout pass.
    root->setProperty("count", 0);
    QTRY_COMPARE_WITH_TIMEOUT(flick->property("contentHeight").toReal(), 150.0, 2000);

    // Growing again follows too.
    root->setProperty("count", 10);
    QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentHeight").toReal() >= 195.0, 2000);
}

// The bug a436d14 fixed: a CSS child that grows AFTER mount must extend the scroll content (the last
// card was cut off on a real GPU when the extent followed childrenRectChanged). The extent now comes
// from the layout pass: the child's growth notifies the parent, whose relayout republishes it.
void QmlCssTests::scrollExtentFollowsLateCssChildGrowth()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .list { overflow-y: auto; display: flex; flex-direction: column; }
        .row { height: 20px; }
        .row.tall { height: 400px; }
    )"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssClass: ["list"]
            cssPrimitive: "div"
            width: 200; height: 150
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { objectName: "last"; cssClass: ["row"]; cssPrimitive: "div" }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QQuickItem *flick = nullptr;
    for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
        if (QString::fromLatin1(k->metaObject()->className()).contains(QLatin1String("Flickable"))) {
            flick = k;
            break;
        }
    }
    QVERIFY2(flick, "overflow-y: auto did not compose a Flickable");
    QTRY_COMPARE_WITH_TIMEOUT(flick->property("contentHeight").toReal(), 150.0, 2000); // 40px fits

    auto *last = root->findChild<QQuickItem *>(QStringLiteral("last"));
    QVERIFY(last);
    last->setProperty("cssClass", QVariant(QStringList{QStringLiteral("row"), QStringLiteral("tall")}));
    // 20 + 400 = 420px of content: the last row must be reachable.
    QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentHeight").toReal() >= 419.0, 2000);
}

// Task 4 review finding #1: a flow child mid `transition: height` must publish its TARGET
// extent, not the pre-transition `k->height()` — place() sets x/y synchronously but animates
// width/height via CssRect::animateGeometry, and nothing re-publishes the extent as the
// animation ticks (Css children have no parent-relayout hook on geometry ticks). The restyle's
// own relayout must therefore publish the TARGET (420px) extent synchronously.
void QmlCssTests::scrollExtentUsesTransitionTarget()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .list { overflow-y: auto; display: flex; flex-direction: column; }
        .row { height: 20px; transition: height 400ms linear; }
        .row.tall { height: 400px; }
    )"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssClass: ["list"]
            cssPrimitive: "div"
            width: 200; height: 150
            CssRect { cssClass: ["row"]; cssPrimitive: "div" }
            CssRect { objectName: "last"; cssClass: ["row"]; cssPrimitive: "div" }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QQuickItem *flick = nullptr;
    for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
        if (QString::fromLatin1(k->metaObject()->className()).contains(QLatin1String("Flickable"))) {
            flick = k;
            break;
        }
    }
    QVERIFY2(flick, "overflow-y: auto did not compose a Flickable");
    // Two 20px rows fit the 150px viewport.
    QTRY_COMPARE_WITH_TIMEOUT(flick->property("contentHeight").toReal(), 150.0, 2000);

    auto *last = root->findChild<QQuickItem *>(QStringLiteral("last"));
    QVERIFY(last);
    last->setProperty("cssClass", QVariant(QStringList{QStringLiteral("row"), QStringLiteral("tall")}));
    // Assert IMMEDIATELY (no QTRY, no event processing): the restyle's own synchronous relayout
    // must already publish the TARGET extent (20 + 400 = 420px), before the height animation
    // has ticked even once.
    QVERIFY2(flick->property("contentHeight").toReal() >= 419.0,
             qPrintable(QStringLiteral("contentHeight published pre-transition height: %1")
                             .arg(flick->property("contentHeight").toReal())));
}

// Task 4 review finding #2: the old QQuickItem::childrenRect()-based extent gave bottom
// breathing equal to padding-top when padding-top > padding-bottom (r.y() == padding-top).
// The layout-pass extent must reproduce that: extentTop is the MINIMUM child y (== padding-top
// here, since the flex column starts children at originY), not an initial-0.0 that can only
// shrink via std::min and so can never go positive.
//
// Derivation (matches git show b161c08:src/qmlcss/cssscroll.h, the old implementation): content
// holder is the PADDING box (cssrect.cpp layoutChildren insets it by the BORDER only), so
// children start at y == originY == padding-top == 30. r = contentHolder->childrenRect() then has
// r.y() == 30 (first row's y) and r.height() == 200 (10 rows x 20px). pad = max(0, r.y()=30,
// bottomPad=0) = 30. contentH = max(ownerHeight=150, r.y()+r.height()+pad = 30+200+30 = 260) = 260.
void QmlCssTests::scrollExtentKeepsPaddingTopBreathing()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .list { overflow-y: auto; display: flex; flex-direction: column; padding-top: 30px; padding-bottom: 0px; }
        .row { height: 20px; }
    )"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssClass: ["list"]
            cssPrimitive: "div"
            width: 200; height: 150
            CssRepeater {
                model: 10
                delegate: Component {
                    CssRect { cssClass: ["row"]; cssPrimitive: "div" }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QQuickItem *flick = nullptr;
    for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
        if (QString::fromLatin1(k->metaObject()->className()).contains(QLatin1String("Flickable"))) {
            flick = k;
            break;
        }
    }
    QVERIFY2(flick, "overflow-y: auto did not compose a Flickable");
    // 30 (origin/padding-top) + 200 (10 rows x 20px) + 30 (bottom breathing == padding-top, the
    // old childrenRect() behaviour) == 260.
    QTRY_COMPARE_WITH_TIMEOUT(flick->property("contentHeight").toReal(), 260.0, 2000);
}

// Task 4 review finding (foreign children): a scroll box with NO Css children at all must still
// pick up a foreign (non-Css) child's EXPLICIT height — abs/foreign children keep using their
// current geometry (y()+height()), never the flow-target path (there is no flow).
void QmlCssTests::scrollExtentFollowsForeignChildSetHeight()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .list { overflow-y: auto; display: flex; flex-direction: column; }
    )"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssClass: ["list"]
            cssPrimitive: "div"
            width: 200; height: 150
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *box = qobject_cast<CssRect *>(root.get());
    QVERIFY(box);
    QQuickItem *flick = nullptr;
    for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
        if (QString::fromLatin1(k->metaObject()->className()).contains(QLatin1String("Flickable"))) {
            flick = k;
            break;
        }
    }
    QVERIFY2(flick, "overflow-y: auto did not compose a Flickable");

    auto *foreign = new QQuickItem;
    foreign->setParent(box->layoutContentHolder());
    foreign->setParentItem(box->layoutContentHolder());
    foreign->setHeight(500); // explicit height, NOT implicit
    QTRY_VERIFY_WITH_TIMEOUT(flick->property("contentHeight").toReal() >= 499.0, 2000);
}

void QmlCssTests::themeRegistryForgetsDestroyedItems()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssPrimitive: ""
            width: 200; height: 200
            property int count: 300
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRepeater {
                model: box.count
                delegate: Component { CssRect { cssClass: ["row"]; cssPrimitive: "div" } }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    // `box` itself has no cssId/cssClass/cssPrimitive (hasCssIdentity() is false), so the component
    // tree never auto-registers it — register it explicitly so it OUTLIVES the mass destruction of
    // rows below and the "same object twice" check further down is a genuine re-registration, not
    // box's first one.
    theme.loadCss(root.get());
    const int withRows = theme.registeredCount();
    QVERIFY2(withRows >= 301, qPrintable(QString::number(withRows)));

    root->setProperty("count", 0);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); // rows are deleteLater'd
    QTRY_VERIFY_WITH_TIMEOUT(theme.registeredCount() <= withRows - 300, 2000);

    // Registering the same object (box) twice keeps one entry.
    const int before = theme.registeredCount();
    theme.loadCss(root.get());
    QCOMPARE(theme.registeredCount(), before);
    // A full reapply after mass destruction still works (no dangling entries). reapplyAll() is
    // private; reload through the PUBLIC path — loadFromString() unconditionally calls
    // rebuildRules() then reapplyAll() (cssthemeloader.cpp) — rather than reach around it.
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
}

void QmlCssTests::stateChangeRestylesOnlyTheSubtree()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(R"(
        .row { height: 10px; }
        .row.hot .cell { background-color: #ff0000; }
    )"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssPrimitive: ""
            width: 200; height: 400
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRepeater {
                model: 300
                delegate: Component {
                    CssRect {
                        objectName: "row" + index
                        cssClass: ["row"]; cssPrimitive: "div"
                        CssRect { objectName: "cell" + index; cssClass: ["cell"]; cssPrimitive: "div" }
                    }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *row = root->findChild<QQuickItem *>(QStringLiteral("row7"));
    auto *cell = root->findChild<QQuickItem *>(QStringLiteral("cell7"));
    QVERIFY(row && cell);

    // Behaviour: the ancestor-scoped rule reaches the descendant.
    row->setProperty("cssClass", QVariant(QStringList{QStringLiteral("row"), QStringLiteral("hot")}));
    QTRY_COMPARE_WITH_TIMEOUT(cell->property("style").toMap().value(QStringLiteral("background-color")).toString(),
                              QStringLiteral("#ff0000"), 1000);

    // Cost: one state change re-applies the row and its ONE descendant — not the 600 registered boxes.
    Detail::g_applyStats.enabled = true;
    const auto before = Detail::g_applyStats.fromDescendant;
    row->setProperty("cssClass", QVariant(QStringList{QStringLiteral("row")}));
    const auto applied = Detail::g_applyStats.fromDescendant - before;
    Detail::g_applyStats.enabled = false;
    QCOMPARE(applied, quint64(1)); // exactly the one registered descendant (cell7)
}

void QmlCssTests::cssRepeaterSwapKeepsDocumentOrder()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssPrimitive: ""
            width: 200; height: 400
            property var order: []
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRect { objectName: "header"; cssClass: ["row"]; cssPrimitive: "div" }
            CssRepeater {
                model: box.order
                delegate: Component { CssRect { objectName: "r" + modelData; cssClass: ["row"]; cssPrimitive: "div" } }
            }
            CssRect { objectName: "footer"; cssClass: ["row"]; cssPrimitive: "div" }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    auto *box = qobject_cast<CssRect *>(root.get());
    QVERIFY(box);

    auto docOrder = [&]() {
        QStringList names;
        for (QQuickItem *k : box->layoutContentHolder()->childItems())
            if (!k->objectName().isEmpty())
                names << k->objectName();
        return names;
    };
    auto expect = [](const QStringList &rows) {
        return QStringList{QStringLiteral("header")} + rows + QStringList{QStringLiteral("footer")};
    };
    QStringList ids;
    for (int i = 0; i < 50; ++i)
        ids << QString::number(i);
    QVariantList model;
    for (const QString &s : ids) model << s;
    root->setProperty("order", model);                       // create (rows land after the footer!)
    QStringList rows;
    for (const QString &s : ids) rows << (QStringLiteral("r") + s);
    QCOMPARE(docOrder(), expect(rows));

    std::swap(model[1], model[48]);                          // swap
    std::swap(rows[1], rows[48]);
    root->setProperty("order", model);
    QCOMPARE(docOrder(), expect(rows));

    model.removeAt(20); rows.removeAt(20);                   // remove
    root->setProperty("order", model);
    QCOMPARE(docOrder(), expect(rows));

    model.append(QStringLiteral("x")); rows << QStringLiteral("rx"); // append
    root->setProperty("order", model);
    QCOMPARE(docOrder(), expect(rows));
}

void QmlCssTests::cssRepeaterWritesIndexOnlyWhenChanged()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssPrimitive: ""
            width: 200; height: 200
            property var order: ["a", "b", "c", "d"]
            // A plain counter PROPERTY read-and-incremented from inside the probe (`evals++`) would
            // itself become a dependency of every probe binding, so one row's genuine re-evaluation
            // cascades into re-evaluating every OTHER row's probe too (confirmed with a standalone
            // repro: QML's property-capture guard treats the read half of `x++` as a real dependency,
            // and in-place `push()` on a `var` array does NOT emit a change notification — so logging
            // into an array counts evaluations without wiring the probes to each other).
            property var evalLog: []
            CssRepeater {
                model: box.order
                delegate: Component {
                    CssRect {
                        cssPrimitive: "div"
                        // Re-evaluates whenever `index` is notified, even with an unchanged value.
                        property int probe: { box.evalLog.push(index); return index }
                    }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    const int base = root->property("evalLog").toList().size();

    // Append: the 4 surviving rows keep their index — none of their probes may re-run.
    root->setProperty("order", QVariant(QStringList{QStringLiteral("a"), QStringLiteral("b"),
                                                    QStringLiteral("c"), QStringLiteral("d"), QStringLiteral("e")}));
    QCOMPARE(root->property("evalLog").toList().size() - base, 1); // only the new row's probe
}

// Companion to cssRepeaterWritesIndexOnlyWhenChanged: that test proves a SURVIVOR that did
// NOT move keeps its index untouched. This proves the other half of `found != i` — a row
// that DID move (swap the two ends) gets its `index` context property updated to the new
// position, with instances reused (not re-created).
void QmlCssTests::cssRepeaterMovedRowGetsNewIndex()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssPrimitive: ""
            width: 200; height: 200
            property var order: ["a", "b", "c", "d"]
            CssRepeater {
                model: box.order
                delegate: Component {
                    CssRect {
                        cssPrimitive: "div"
                        objectName: "r" + modelData
                        property int idx: index
                    }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    QObject *a = root->findChild<QObject *>(QStringLiteral("ra"));
    QObject *b = root->findChild<QObject *>(QStringLiteral("rb"));
    QObject *c = root->findChild<QObject *>(QStringLiteral("rc"));
    QObject *d = root->findChild<QObject *>(QStringLiteral("rd"));
    QVERIFY(a && b && c && d);

    // Swap the two ends.
    root->setProperty("order", QVariant(QStringList{QStringLiteral("d"), QStringLiteral("b"),
                                                    QStringLiteral("c"), QStringLiteral("a")}));

    // Same instances found again by objectName (reused, not recreated) — and re-indexed.
    QObject *a2 = root->findChild<QObject *>(QStringLiteral("ra"));
    QObject *d2 = root->findChild<QObject *>(QStringLiteral("rd"));
    QCOMPARE(a2, a);
    QCOMPARE(d2, d);

    QCOMPARE(d->property("idx").toInt(), 0);
    QCOMPARE(b->property("idx").toInt(), 1);
    QCOMPARE(c->property("idx").toInt(), 2);
    QCOMPARE(a->property("idx").toInt(), 3);
}

void QmlCssTests::cssRepeaterRowsResolveOnceInsideTheScene()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".list .row { background-color: #123456; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssPrimitive: "div"; cssClass: ["list"]
            width: 200; height: 200
            property var rows: []
            CssRepeater {
                model: box.rows
                delegate: Component {
                    CssRect {
                        objectName: "row"
                        cssPrimitive: "div"; cssClass: ["row"]
                        CssText { cssPrimitive: "span"; text: "" + modelData }
                    }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QQuickWindow window;
    qobject_cast<QQuickItem *>(root.data())->setParentItem(window.contentItem());

    const bool statsWereOn = Detail::g_applyStats.enabled;
    Detail::g_applyStats.enabled = true;
    const int before = Detail::g_applyStats.fromLoad;
    root->setProperty("rows", QVariantList{1, 2, 3});
    const int applies = Detail::g_applyStats.fromLoad - before;
    Detail::g_applyStats.enabled = statsWereOn;

    // 3 rows x (row CssRect + CssText) = 6 styled elements -> 6 resolves (12 = every one healed again).
    QCOMPARE(applies, 6);
    const QList<QQuickItem *> rows = root->findChildren<QQuickItem *>(QStringLiteral("row"));
    QCOMPARE(rows.size(), 3);
    for (QQuickItem *row : rows) // the single resolve already saw the .list ancestor
        QCOMPARE(row->property("style").toMap().value(QStringLiteral("background-color")).toString(),
                 QStringLiteral("#123456"));
}

void QmlCssTests::cssIncubatorPageResolvesOnceInsideTheScene()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".list .page { background-color: #123456; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine; // no incubation controller: incubation runs synchronously (qqmlincubator.cpp)
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            cssPrimitive: "div"; cssClass: ["list"]
            width: 200; height: 200
            CssIncubator {
                objectName: "inc"
                active: false
                sourceComponent: Component {
                    CssRect {
                        objectName: "page"; cssPrimitive: "div"; cssClass: ["page"]
                        // Logs opacity on every evaluation; the FIRST runs in finalize's binding phase,
                        // before any componentComplete — i.e. while the page streams in.
                        property var opacityLog: []
                        property int probe: { opacityLog.push(opacity); return 0 }
                    }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QQuickWindow window;
    qobject_cast<QQuickItem *>(root.data())->setParentItem(window.contentItem());
    QObject *inc = root->findChild<QObject *>(QStringLiteral("inc"));
    QVERIFY(inc);

    const bool statsWereOn = Detail::g_applyStats.enabled;
    Detail::g_applyStats.enabled = true;
    const int before = Detail::g_applyStats.fromLoad;
    inc->setProperty("active", true);
    QTRY_VERIFY(root->findChild<QQuickItem *>(QStringLiteral("page")));
    const int applies = Detail::g_applyStats.fromLoad - before;
    Detail::g_applyStats.enabled = statsWereOn;

    QCOMPARE(applies, 1); // 2 = resolved parentless, healed on scene join
    auto *page = root->findChild<QQuickItem *>(QStringLiteral("page"));
    QCOMPARE(page->property("style").toMap().value(QStringLiteral("background-color")).toString(),
             QStringLiteral("#123456"));
    QCOMPARE(page->opacity(), 0.0); // the atomic-reveal hold is in place once mounted...
    // ...and already while finalize() streamed the page in (in the scene since setInitialState).
    QCOMPARE(page->property("opacityLog").toList().value(0).toDouble(), 0.0);
}

// --- Step 3 Task 1 (C21): composed parts must die with their owner --------------------------
//
// `~QQuickItem` only calls setParentItem(nullptr) on its visual children (qquickitem.cpp
// ~2406 in Qt 6.11.1) — it never deletes them. A part composed via QQmlComponent::create or
// beginCreate from C++ has C++ ownership and, unless given a QObject parent too
// (setParent(this)), nothing ever deletes it: the visual-only unparenting leaves it alive and
// unreachable. This instantiates one of every engine type that composes children this way,
// collects every visual descendant BEFORE destroying the owning root, destroys the root, and
// requires every descendant to actually be gone afterwards.
void QmlCssTests::composedItemsDieWithTheirOwner()
{
    QQmlEngine engine;

    // A real (if tiny) PNG so CssFill's url() background resolves to an actual image source —
    // not required for composition (ensureImageLayers triggers on the url() syntax alone), but
    // keeps the scenario realistic.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString png = dir.filePath(QStringLiteral("bg.png"));
    QImage bgImg(4, 4, QImage::Format_ARGB32);
    bgImg.fill(Qt::red);
    QVERIFY(bgImg.save(png));
    const QString pngUrl = QUrl::fromLocalFile(png).toString();

    struct Scenario {
        const char *name;
        QString qml;
    };
    const QList<Scenario> scenarios = {
        // csstext.cpp composeLabel — the composed Text label (measured leak); styled text keeps it composed.
        { "CssText", QStringLiteral(R"(
            import QtQuick
            import qmlcss
            CssText { text: "hi"; styledText: true; width: 80; height: 20 }
        )") },
        // cssimage.cpp:238/268/282 — Image + Shape mask + MultiEffect, always composed.
        { "CssImage", QStringLiteral(R"(
            import QtQuick
            import qmlcss
            CssImage { width: 40; height: 40 }
        )") },
        // cssicon.cpp:243/259 — Image + MultiEffect, always composed.
        { "CssIcon", QStringLiteral(R"(
            import QtQuick
            import qmlcss
            CssIcon { width: 32; height: 32 }
        )") },
        // csshr.cpp:227 — the composed Shape rule.
        { "CssHr", QStringLiteral(R"(
            import QtQuick
            import qmlcss
            CssHr { width: 100 }
        )") },
        // cssfilllayer.cpp:216 — the composed Shape (used standalone here; CssRect's render
        // shell also declares CssFillLayer instances, but those are ordinary QML-declared
        // children, not the comp->create() site under test).
        { "CssFillLayer", QStringLiteral(R"(
            import QtQuick
            import qmlcss
            CssFillLayer {
                width: 50; height: 50
                spec: ({ type: "color", color: Qt.rgba(1, 0, 0, 0.5) })
                radii: [0, 0, 0, 0]
            }
        )") },
        // cssdropshadow.cpp:104 — the composed MultiEffect, always composed.
        { "CssDropShadow", QStringLiteral(R"(
            import QtQuick
            import qmlcss
            CssDropShadow { width: 100; height: 40 }
        )") },
        // cssfill.cpp:545 (CssFill::composeLayer, shared by all 3 call sites) — m_rect composes
        // unconditionally; the url() background additionally composes the image + solid layers.
        { "CssFill", QStringLiteral(R"CSSFILL(
            import QtQuick
            import qmlcss
            CssFill { width: 80; height: 40; style: ({ "background": "url('%1')" }) }
        )CSSFILL").arg(pngUrl) },
    };

    QStringList survivors;
    for (const Scenario &s : scenarios) {
        QQmlComponent component(&engine);
        component.setData(s.qml.toUtf8(), QUrl());
        QScopedPointer<QObject> root(component.create());
        QVERIFY2(root, qPrintable(QStringLiteral("%1: %2").arg(QString::fromLatin1(s.name), component.errorString())));
        auto *item = qobject_cast<QQuickItem *>(root.data());
        QVERIFY2(item, s.name);

        QList<QPair<QByteArray, QPointer<QObject>>> descendants;
        collectComposedDescendants(item, descendants);
        QVERIFY2(!descendants.isEmpty(),
                 qPrintable(QStringLiteral("%1: composed nothing — scenario does not exercise the composing path")
                                .arg(QString::fromLatin1(s.name))));

        root.reset(); // synchronously destroys the owner (and anything it truly owns)

        // Flush any deleteLater() fallbacks (failed-cast cleanup paths) before judging survival.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();

        // Collect survivors per type instead of aborting on the first one: a regression in one type
        // must not hide another's.
        for (const auto &d : descendants) {
            if (!d.second.isNull())
                survivors << QStringLiteral("%1: composed %2 survived its owner's destruction")
                                 .arg(QString::fromLatin1(s.name), QString::fromLatin1(d.first));
        }
    }
    QVERIFY2(survivors.isEmpty(), qPrintable(survivors.join(QLatin1Char('\n'))));
}

void QmlCssTests::everyIdentityTypeIsACssBox()
{
    CssTheme theme;
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    const QStringList types{QStringLiteral("CssRect"), QStringLiteral("CssText"), QStringLiteral("CssFill"),
                            QStringLiteral("CssHr"), QStringLiteral("CssImage")};
    for (const QString &type : types) {
        QQmlComponent c(&engine);
        c.setData(QStringLiteral("import QtQuick\nimport qmlcss\n%1 { cssId: \"i\"; cssClass: [\"a\", \"b\"]; "
                                 "cssState: [\"s\"]; cssPrimitive: \"p\"; cssPart: \"q\"; cssAlternateId: [\"x\"] }")
                      .arg(type).toUtf8(), QUrl());
        QScopedPointer<QObject> o(c.create());
        QVERIFY2(o, qPrintable(type + QLatin1String(": ") + c.errorString()));
        auto *box = qobject_cast<CssBox *>(o.data());
        QVERIFY2(box, qPrintable(type));
        QCOMPARE(box->cssId(), QStringLiteral("i"));
        QCOMPARE(box->classList(), (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
        QCOMPARE(box->stateList(), QStringList{QStringLiteral("s")});
        QCOMPARE(box->cssPrimitive(), QStringLiteral("p"));
        QCOMPARE(box->cssPart(), QStringLiteral("q"));
        QCOMPARE(box->alternateIdList(), QStringList{QStringLiteral("x")});
        QVERIFY(box->hasCssIdentity());
    }
    // CssItem: identity without cssState in QML today — it still is a box.
    QQmlComponent item(&engine);
    item.setData("import QtQuick\nimport qmlcss\nItem { CssItem { objectName: \"ci\"; cssClass: [\"c\"] } }", QUrl());
    QScopedPointer<QObject> io(item.create());
    QVERIFY2(io, qPrintable(item.errorString()));
    auto *ci = qobject_cast<CssBox *>(io->findChild<QObject *>(QStringLiteral("ci")));
    QVERIFY(ci);
    QCOMPARE(ci->classList(), QStringList{QStringLiteral("c")});
    // CssIcon: a style sink without identity — deliberately NOT a box.
    QQmlComponent icon(&engine);
    icon.setData("import QtQuick\nimport qmlcss\nCssIcon {}", QUrl());
    QScopedPointer<QObject> ico(icon.create());
    QVERIFY2(ico, qPrintable(icon.errorString()));
    QVERIFY(!qobject_cast<CssBox *>(ico.data()));
}

// 4b-1: resolving a CssBox reads its identity and its ancestors' identities by member, not by name.
// Proven with a box whose Q_PROPERTY getters are bypassed: the theme must use classList(), so a value
// placed only in the typed view is what matches.
void QmlCssTests::identityReadsAreTyped()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".outer .inner { background-color: #123456; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent c(&engine);
    c.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { cssPrimitive: "div"; cssClass: ["outer"]; width: 50; height: 50
            CssRect { objectName: "inner"; cssPrimitive: "div"; cssClass: ["inner"] } }
    )", QUrl());
    QScopedPointer<QObject> root(c.create());
    QVERIFY2(root, qPrintable(c.errorString()));
    QQuickWindow window;
    qobject_cast<QQuickItem *>(root.data())->setParentItem(window.contentItem());
    auto *inner = root->findChild<QQuickItem *>(QStringLiteral("inner"));
    QVERIFY(inner);
    QCOMPARE(inner->property("style").toMap().value(QStringLiteral("background-color")).toString(),
             QStringLiteral("#123456"));
    const int before = Detail::g_namedIdentityReads;
    theme.loadCss(inner);
    QCOMPARE(Detail::g_namedIdentityReads - before, 0); // the box AND its CssBox ancestor: typed
}

// 4b-1 Task 3: a flex container's children are laid out by reading each CssBox child's resolved
// style through the typed accessor, never by QObject::property("style") — proven by forcing a
// relayout and asserting the by-name probe counter did not move, while the geometry is still
// correct (gap:4px between three 10px-wide items: x = 0, 14, 28).
void QmlCssTests::layoutReadsStyleTyped()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".flexbox { display: flex; gap: 4px; } .item { width: 10px; height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent comp(&engine);
    comp.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { cssClass: ["flexbox"]; width: 100; height: 20
            CssRect { objectName: "a"; cssClass: ["item"] }
            CssRect { objectName: "b"; cssClass: ["item"] }
            CssRect { objectName: "c"; cssClass: ["item"] } }
    )", QUrl());
    QScopedPointer<QObject> object(comp.create());
    QVERIFY2(object, qPrintable(comp.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);
    QQuickWindow window;
    root->setParentItem(window.contentItem());

    auto *a = root->findChild<QQuickItem *>(QStringLiteral("a"));
    auto *b = root->findChild<QQuickItem *>(QStringLiteral("b"));
    auto *c = root->findChild<QQuickItem *>(QStringLiteral("c"));
    QVERIFY(a && b && c);

    layoutEngine.layout(root, contentHolderOf(a));

    const int before = Detail::g_namedStyleReads;
    // Force a second relayout pass (the public API the other layout tests use): the by-name
    // probe must not move on a CssBox child.
    layoutEngine.layout(root, contentHolderOf(a));
    QCOMPARE(Detail::g_namedStyleReads - before, 0);

    QVERIFY2(std::abs(a->x() - 0.0) < 0.5, qPrintable(QString::number(a->x())));
    QVERIFY2(std::abs(b->x() - 14.0) < 0.5, qPrintable(QString::number(b->x())));
    QVERIFY2(std::abs(c->x() - 28.0) < 0.5, qPrintable(QString::number(c->x())));
}

// 4b-3 Task 2: the flex path reads a pre-parsed typed style per box. The first pass parses every
// distinct style once; a second full relayout (the container AND every row) must not touch a single
// style string again — the counter probe counts each length/box/`flex`-shorthand string parse.
void QmlCssTests::layoutParsesOncePerStyle()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        ".list { display: flex; flex-direction: column; gap: 2px; padding: 4px; } "
        ".row { display: flex; gap: 4px; padding: 2px 4px; margin: 1px; height: 20px; "
        "border: 1px solid #000; align-items: center; } "
        ".cell { width: 30%; min-width: 10px; max-width: 50vw; flex: 1 1; margin-left: 2px; }"));
    CssLayoutEngine layoutEngine(&theme);
    theme.setViewport(800, 600);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent comp(&engine);
    comp.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { cssClass: ["list"]; width: 400; height: 3000
            CssRepeater {
                model: 100
                delegate: Component {
                    CssRect { objectName: "row"; cssClass: ["row"]
                        CssRect { objectName: "cell"; cssClass: ["cell"] }
                        CssRect { cssClass: ["cell"] } }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> object(comp.create());
    QVERIFY2(object, qPrintable(comp.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);
    QQuickWindow window;
    root->setParentItem(window.contentItem());

    const QList<QQuickItem *> rows = root->findChildren<QQuickItem *>(QStringLiteral("row"));
    QCOMPARE(rows.size(), 100);
    const auto fullPass = [&]() {
        layoutEngine.layout(root, contentHolderOf(rows.first()));
        for (QQuickItem *row : rows)
            layoutEngine.layout(row, contentHolderOf(row->findChild<QQuickItem *>(QStringLiteral("cell"))));
    };
    fullPass();
    const int before = Detail::g_layoutStringParses;
    fullPass();
    QCOMPARE(Detail::g_layoutStringParses - before, 0);

    // The rows were really laid out: stacked, each at its own y.
    QVERIFY2(std::abs(rows[0]->y() - rows[1]->y()) > 20.0,
             qPrintable(QStringLiteral("%1 %2").arg(rows[0]->y()).arg(rows[1]->y())));
}

// 4b-1 Task 4 shared fixture: one window-hosted element built from `qml`, under `css`.
namespace {
struct IdentityFixture {
    CssTheme theme;
    CssLayoutEngine layoutEngine{&theme};
    QQmlEngine engine;
    QQuickWindow window;
    QScopedPointer<QObject> object;
    QString error;
    IdentityFixture(const QString &css, const QByteArray &qml)
    {
        theme.loadFromString(css);
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
        QQmlComponent comp(&engine);
        comp.setData("import QtQuick\nimport qmlcss\n" + qml, QUrl());
        object.reset(comp.create());
        error = comp.errorString();
        if (auto *item = qobject_cast<QQuickItem *>(object.data()))
            item->setParentItem(window.contentItem());
    }
    QString style(const char *key) const
    {
        return object->property("style").toMap().value(QLatin1String(key)).toString();
    }
};
const QString kIdentityCss = QStringLiteral(
    ".a { background-color: #111111; } .b { background-color: #222222; } "
    "#alt { background-color: #333333; } #base.p { background-color: #444444; }");
// Applies (load + sender) caused by `change`, with the stats probe restored afterwards.
template <typename F> int appliesDuring(F change)
{
    const bool wasOn = Detail::g_applyStats.enabled;
    Detail::g_applyStats.enabled = true;
    const int before = Detail::g_applyStats.fromLoad + Detail::g_applyStats.fromSender;
    change();
    const int after = Detail::g_applyStats.fromLoad + Detail::g_applyStats.fromSender;
    Detail::g_applyStats.enabled = wasOn;
    return after - before;
}
} // namespace

void QmlCssTests::identityChangeRestylesOnce()
{
    IdentityFixture f(kIdentityCss, "CssRect { width: 10; height: 10; cssClass: [\"a\"] }");
    QVERIFY2(f.object, qPrintable(f.error));
    QCOMPARE(f.style("background-color"), QStringLiteral("#111111"));
    const int applies = appliesDuring([&] { f.object->setProperty("cssClass", QStringList{QStringLiteral("b")}); });
    QCOMPARE(applies, 1);
    QCOMPARE(f.style("background-color"), QStringLiteral("#222222"));
}

void QmlCssTests::engineHoverRestylesOnce()
{
    IdentityFixture f(QStringLiteral("#h { background-color: #111111; } #h:hover { background-color: #555555; }"),
                      "CssRect { width: 10; height: 10; cssId: \"h\" }");
    QVERIFY2(f.object, qPrintable(f.error));
    QCOMPARE(f.style("background-color"), QStringLiteral("#111111"));
    QVERIFY(f.object->property("cssHoverStyled").toBool());
    const int applies = appliesDuring([&] { f.object->setProperty("cssEngineHover", true); });
    QCOMPARE(applies, 1);
    QCOMPARE(f.style("background-color"), QStringLiteral("#555555"));
}

void QmlCssTests::alternateIdChangeRestyles()
{
    IdentityFixture f(kIdentityCss, "CssRect { width: 10; height: 10; cssId: \"none\" }");
    QVERIFY2(f.object, qPrintable(f.error));
    QVERIFY(f.style("background-color").isEmpty());
    const int applies =
        appliesDuring([&] { f.object->setProperty("cssAlternateId", QStringList{QStringLiteral("alt")}); });
    QCOMPARE(f.style("background-color"), QStringLiteral("#333333"));
    QCOMPARE(applies, 1);
}

void QmlCssTests::cssPartChangeRestyles()
{
    IdentityFixture f(kIdentityCss, "CssRect { width: 10; height: 10; cssId: \"base\" }");
    QVERIFY2(f.object, qPrintable(f.error));
    QVERIFY(f.style("background-color").isEmpty());
    const int applies = appliesDuring([&] { f.object->setProperty("cssPart", QStringLiteral("p")); });
    QCOMPARE(f.style("background-color"), QStringLiteral("#444444"));
    QCOMPARE(applies, 1);
}

void QmlCssTests::imageStateAndPrimitiveRestyle()
{
    // CssImage::setStyle writes the item's opacity from the resolved `opacity` key.
    IdentityFixture f(QStringLiteral(".on { opacity: 0.5; } picture { opacity: 0.25; }"),
                      "CssImage { width: 10; height: 10 }");
    QVERIFY2(f.object, qPrintable(f.error));
    auto *img = qobject_cast<QQuickItem *>(f.object.data());
    QCOMPARE(img->opacity(), 1.0);
    const int stateApplies = appliesDuring([&] { f.object->setProperty("cssState", QStringList{QStringLiteral("on")}); });
    QCOMPARE(img->opacity(), 0.5);
    QCOMPARE(stateApplies, 1);
    f.object->setProperty("cssState", QStringList{});
    QCOMPARE(img->opacity(), 1.0);
    const int primitiveApplies = appliesDuring([&] { f.object->setProperty("cssPrimitive", QStringLiteral("picture")); });
    QCOMPARE(img->opacity(), 0.25);
    QCOMPARE(primitiveApplies, 1);
}

void QmlCssTests::hrPrimitiveRestyles()
{
    IdentityFixture f(QStringLiteral("div { border-top: 3px solid #ff8800; }"), "CssHr { width: 120 }");
    QVERIFY2(f.object, qPrintable(f.error));
    QVERIFY(f.object->property("line").toMap().value(QStringLiteral("color")).value<QColor>()
            != QColor(QStringLiteral("#ff8800")));
    const int applies = appliesDuring([&] { f.object->setProperty("cssPrimitive", QStringLiteral("div")); });
    const QVariantMap line = f.object->property("line").toMap();
    QCOMPARE(line.value(QStringLiteral("width")).toReal(), 3.0);
    QCOMPARE(line.value(QStringLiteral("color")).value<QColor>(), QColor(QStringLiteral("#ff8800")));
    QCOMPARE(applies, 1);
}

void QmlCssTests::contextNodesAreSharedByIdenticalSiblings()
{
    IdentityFixture f(QStringLiteral(".list .row { background-color: #111111; }"), R"(
        CssRect { cssClass: ["list"]; width: 50; height: 50
            CssRect { objectName: "r0"; cssClass: ["row"] }
            CssRect { objectName: "r1"; cssClass: ["row"] }
            CssRect { objectName: "r2"; cssClass: ["row"] } }
    )");
    QVERIFY2(f.object, qPrintable(f.error));
    auto *list = qobject_cast<CssBox *>(f.object.data());
    QVERIFY(list);
    QList<const CssContextNode *> nodes;
    for (const char *name : {"r0", "r1", "r2"}) {
        auto *row = qobject_cast<CssBox *>(f.object->findChild<QQuickItem *>(QLatin1String(name)));
        QVERIFY(row);
        QCOMPARE(row->styleMap().value(QStringLiteral("background-color")).toString(), QStringLiteral("#111111"));
        nodes << row->contextNodeForTest();
    }
    QVERIFY(nodes.at(0));
    QCOMPARE(nodes.at(1), nodes.at(0));
    QCOMPARE(nodes.at(2), nodes.at(0));
    QVERIFY(list->contextNodeForTest());
    QVERIFY(nodes.at(0) != list->contextNodeForTest());
    QCOMPARE(nodes.at(0)->parent.data(), list->contextNodeForTest());
}

void QmlCssTests::contextNodeFollowsAncestorIdentityAndReparenting()
{
    const QString css = QStringLiteral(".a .row { background-color: #111111; } .b .row { background-color: #222222; }");
    IdentityFixture f(css, R"(
        CssRect { width: 50; height: 50
            CssRect { objectName: "p1"; cssClass: ["a"]
                CssRect { objectName: "row"; cssClass: ["row"] } }
            CssRect { objectName: "p2"; cssClass: ["a"] } }
    )");
    QVERIFY2(f.object, qPrintable(f.error));
    auto *p1 = f.object->findChild<QQuickItem *>(QStringLiteral("p1"));
    auto *p2 = f.object->findChild<QQuickItem *>(QStringLiteral("p2"));
    auto *rowItem = f.object->findChild<QQuickItem *>(QStringLiteral("row"));
    auto *row = qobject_cast<CssBox *>(rowItem);
    QVERIFY(p1 && p2 && row);
    auto bg = [&] { return row->styleMap().value(QStringLiteral("background-color")).toString(); };
    // Strong refs keep the old nodes alive, so a freed node's address cannot be reused by a new one and
    // fake a "same pointer" (or a "changed pointer") result.
    auto ref = [](const CssContextNode *n) { return CssContextRef(const_cast<CssContextNode *>(n)); };
    QCOMPARE(bg(), QStringLiteral("#111111"));
    const CssContextRef n0 = ref(row->contextNodeForTest());
    QVERIFY(n0);

    p1->setProperty("cssClass", QStringList{QStringLiteral("b")}); // ancestor identity: live read
    const CssContextRef n1 = ref(row->contextNodeForTest());
    QVERIFY(n1 != n0);
    QCOMPARE(bg(), QStringLiteral("#222222"));

    rowItem->setParentItem(p2); // reparent under another `.a`
    f.theme.loadCss(rowItem);
    const CssContextRef n2 = ref(row->contextNodeForTest());
    QVERIFY(n2 != n1);
    QCOMPARE(n2->parent.data(), qobject_cast<CssBox *>(p2)->contextNodeForTest());
    QCOMPARE(bg(), QStringLiteral("#111111"));

    const quint64 generation = n2->generation;
    f.theme.loadFromString(css); // rule rebuild -> reapplyAll re-validates every box
    QVERIFY(row->contextNodeForTest()->generation > generation);
    QCOMPARE(bg(), QStringLiteral("#111111"));
}

void QmlCssTests::customPropertiesCascadeThroughContextNodes()
{
    const QString css = QStringLiteral(
        ":root { --tint: #101010; } .warm { --tint: #ff8800; } .cool { --tint: #0088ff; } "
        ".row { background-color: var(--tint); }");
    IdentityFixture f(css, R"(
        CssRect { objectName: "list"; cssClass: ["warm"]; width: 50; height: 50
            CssRect { objectName: "r0"; cssClass: ["row"] }
            CssRect { objectName: "r1"; cssClass: ["row"] } }
    )");
    QVERIFY2(f.object, qPrintable(f.error));
    auto *list = f.object.data();
    auto *r0 = qobject_cast<CssBox *>(f.object->findChild<QQuickItem *>(QStringLiteral("r0")));
    auto *r1 = qobject_cast<CssBox *>(f.object->findChild<QQuickItem *>(QStringLiteral("r1")));
    QVERIFY(r0 && r1);
    auto bg = [](CssBox *b) { return b->styleMap().value(QStringLiteral("background-color")).toString(); };
    // (a) the ancestor's --tint reaches the rows
    QCOMPARE(bg(r0), QStringLiteral("#ff8800"));
    QVERIFY(!r0->styleMap().contains(QStringLiteral("--tint")));
    // (c) identical rows still share one node, vars present
    QCOMPARE(r1->contextNodeForTest(), r0->contextNodeForTest());
    QCOMPARE(bg(r1), QStringLiteral("#ff8800"));
    // (b) a change of the ancestor's identity restyles the rows with its new --tint
    list->setProperty("cssClass", QStringList{QStringLiteral("cool")});
    QCOMPARE(bg(r0), QStringLiteral("#0088ff"));
    QCOMPARE(bg(r1), QStringLiteral("#0088ff"));
    list->setProperty("cssClass", QStringList{});
    QCOMPARE(bg(r0), QStringLiteral("#101010"));   // back to :root
    // a theme reload (new generation) recomputes the environment
    f.theme.loadFromString(QString(css).replace(QStringLiteral("#101010"), QStringLiteral("#202020")));
    QCOMPARE(bg(r0), QStringLiteral("#202020"));
}

void QmlCssTests::contextNodesDieWithTheirBoxes()
{
    const int before = Detail::g_liveContextNodes;
    {
        IdentityFixture f(QStringLiteral(".list .row { background-color: #111111; }"), R"(
            CssRect { cssClass: ["list"]; width: 50; height: 50
                Repeater { model: 50; CssRect { cssClass: ["row"] } } }
        )");
        QVERIFY2(f.object, qPrintable(f.error));
        // Repeater delegates are visual descendants only (not QObject children): walk the item tree.
        int rows = 0;
        QList<QQuickItem *> stack = qobject_cast<QQuickItem *>(f.object.data())->childItems();
        while (!stack.isEmpty()) {
            QQuickItem *k = stack.takeLast();
            auto *box = qobject_cast<CssBox *>(k);
            rows += box && box->classList() == QStringList{QStringLiteral("row")} && box->contextNodeForTest();
            stack += k->childItems();
        }
        QCOMPARE(rows, 50);
        const int grown = Detail::g_liveContextNodes - before;
        QVERIFY2(grown >= 2 && grown <= 3, qPrintable(QString::number(grown))); // list + row (+ maybe one)
        f.object.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(Detail::g_liveContextNodes, before);
    }
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(Detail::g_liveContextNodes, before);
}

void QmlCssTests::identicalRowsResolveOnce()
{
    // Reset before the fixture creates anything: every own resolve of the mount is counted.
    Detail::g_ownResolveMisses = 0;
    IdentityFixture f(QStringLiteral(".list .row { background-color: #111111; } .row:hover { background-color: #222222; } "
                                     ".row text { color: #333333; }"), R"(
        CssRect { cssClass: ["list"]; width: 50; height: 50
            Repeater { model: 100; CssRect { cssClass: ["row"]; CssText { text: "x" } } } }
    )");
    QVERIFY2(f.object, qPrintable(f.error));
    int rows = 0;
    QList<QQuickItem *> stack = qobject_cast<QQuickItem *>(f.object.data())->childItems();
    while (!stack.isEmpty()) {
        QQuickItem *k = stack.takeLast();
        auto *box = qobject_cast<CssBox *>(k);
        if (box && box->classList() == QStringList{QStringLiteral("row")}) {
            ++rows;
            QCOMPARE(k->property("style").toMap().value(QStringLiteral("background-color")).toString(),
                     QStringLiteral("#111111"));
            QVERIFY(k->property("cssHoverStyled").toBool());
        }
        stack += k->childItems();
    }
    QCOMPARE(rows, 100);
    // list, row, text, plus the hover on/off probes of the boxes that compute cssHoverStyled.
    QVERIFY2(Detail::g_ownResolveMisses <= 8, qPrintable(QString::number(Detail::g_ownResolveMisses)));
}

// 4b-4 Task 1: rect(.outer) > rect(.mid, no text props) > text. The label gets the outer's color, font
// size and alignment, and re-resolving the text reads no inherited value by name (all ancestors are CssBoxes).
void QmlCssTests::inheritanceWalkIsTyped()
{
    ComposedTextScope composed;
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        ".outer { color: #123456; font-size: 20px; text-align: center; } .mid { width: 10px; }"
        ".outer.hot { color: #abcdef; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent c(&engine);
    c.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { cssPrimitive: "div"; cssClass: ["outer"]; width: 100; height: 50
            CssRect { cssPrimitive: "div"; cssClass: ["mid"]
                CssText { objectName: "t"; text: "x" } } }
    )", QUrl());
    QScopedPointer<QObject> root(c.create());
    QVERIFY2(root, qPrintable(c.errorString()));
    QQuickWindow window;
    qobject_cast<QQuickItem *>(root.data())->setParentItem(window.contentItem());
    auto *text = root->findChild<QQuickItem *>(QStringLiteral("t"));
    QVERIFY(text);
    QQuickItem *label = nullptr;
    for (QQuickItem *k : text->findChildren<QQuickItem *>())
        if (QByteArray(k->metaObject()->className()).contains("QQuickText")) {
            label = k;
            break;
        }
    QVERIFY(label);
    QCOMPARE(label->property("color").value<QColor>(), QColor(QStringLiteral("#123456")));
    QCOMPARE(label->property("font").value<QFont>().pointSizeF(), theme.parseFontSize(QStringLiteral("20px"), 11));
    QCOMPARE(label->property("horizontalAlignment").toInt(), int(Qt::AlignHCenter));

    // A re-resolve with an unchanged style is a no-op, so also force a full applyToText (a default changes):
    // all eight inherited values are re-read through the ancestors.
    const int before = Detail::g_namedInheritReads;
    theme.loadCss(text);
    text->setProperty("defaultFontSize", 12.0);
    QCOMPARE(Detail::g_namedInheritReads - before, 0);
    QCOMPARE(label->property("color").value<QColor>(), QColor(QStringLiteral("#123456")));
    QCOMPARE(label->property("font").value<QFont>().pointSizeF(), theme.parseFontSize(QStringLiteral("20px"), 12));

    // Live propagation still works: the outer's color change reaches the label through the inheritedChanged
    // chain (outer -> mid -> text, each connected once at componentComplete).
    root->setProperty("cssClass", QStringList{QStringLiteral("outer"), QStringLiteral("hot")});
    QCOMPARE(label->property("color").value<QColor>(), QColor(QStringLiteral("#abcdef")));
    QCOMPARE(text->property("inheritedColor").toString(), QStringLiteral("#abcdef"));
}

// 4b-4 Task 1: the eight inherited* getters on every node of a nested tree, against literals captured on the
// by-name implementation (the typed walk must answer the same). Covers own-wins, a level that sets nothing,
// CssFill in the chain, a CssText whose direct parent is a CssText WITH a color (direct parent wins) and one
// whose direct parent has an EMPTY inheritedColor (grandparent rule: it skips its parent's own values).
void QmlCssTests::inheritanceGettersUnchanged()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        ".a { color: #111111; font-family: Foo; font-size: 20px; text-align: center; }"
        ".b { color: #222222; font-weight: bold; }"
        ".d { line-height: 2; letter-spacing: 1px; }"
        ".t1 { text-transform: uppercase; }"
        ".t2 { color: #333333; font-size: 9px; }"
        ".p { font-size: 30px; }"
        ".m { font-weight: 300; text-align: right; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent c(&engine);
    c.setData(R"(
        import QtQuick
        import qmlcss
        Item { width: 400; height: 400
          CssRect { objectName: "a"; cssPrimitive: "div"; cssClass: ["a"]; width: 200; height: 200
            CssFill { objectName: "b"; cssPrimitive: "div"; cssClass: ["b"]
              CssRect { objectName: "c"; cssPrimitive: "div"
                CssRect { objectName: "d"; cssPrimitive: "div"; cssClass: ["d"]
                  CssText { objectName: "t1"; cssClass: ["t1"]; text: "1"
                    CssText { objectName: "t1c"; text: "1c" } }
                  CssText { objectName: "t2"; cssClass: ["t2"]; text: "2" } } } } }
          CssRect { objectName: "p"; cssPrimitive: "div"; cssClass: ["p"]; y: 200; width: 200; height: 200
            CssText { objectName: "o"; text: "o"
              CssText { objectName: "m"; cssClass: ["m"]; text: "m"
                CssText { objectName: "i"; text: "i" } } } } }
    )", QUrl());
    QScopedPointer<QObject> root(c.create());
    QVERIFY2(root, qPrintable(c.errorString()));
    QQuickWindow window;
    qobject_cast<QQuickItem *>(root.data())->setParentItem(window.contentItem());

    static const char *getters[] = {"inheritedColor", "inheritedFontFamily", "inheritedFontSize",
                                    "inheritedFontWeight", "inheritedLineHeight", "inheritedLetterSpacing",
                                    "inheritedTextTransform", "inheritedTextAlign"};
    struct Row { const char *name; QStringList values; };
    const QList<Row> expected = {
        {"a", {"#111111", "Foo", "20px", "", "", "", "", "center"}},
        {"b", {"#222222", "Foo", "20px", "bold", "", "", "", "center"}},
        {"c", {"#222222", "Foo", "20px", "bold", "", "", "", "center"}},
        {"d", {"#222222", "Foo", "20px", "bold", "2", "1px", "", "center"}},
        {"t1", {"#222222", "Foo", "20px", "bold", "2", "1px", "uppercase", "center"}},
        // direct parent t1 has a non-empty inheritedColor -> inherits from t1
        {"t1c", {"#222222", "Foo", "20px", "bold", "2", "1px", "uppercase", "center"}},
        {"t2", {"#333333", "Foo", "9px", "bold", "2", "1px", "", "center"}},
        {"p", {"", "", "30px", "", "", "", "", ""}},
        {"o", {"", "", "30px", "", "", "", "", ""}},
        // o's direct parent is a content holder (no inheritedColor), grandparent p declares it -> p
        // m: direct parent o has an EMPTY inheritedColor; grandparent is p's content holder -> nothing
        {"m", {"", "", "", "300", "", "", "", "right"}},
        // i: direct parent m has an EMPTY inheritedColor; grandparent o declares it -> o (skips m's own)
        {"i", {"", "", "30px", "", "", "", "", ""}},
    };
    for (const Row &row : expected) {
        auto *node = root->findChild<QQuickItem *>(QLatin1String(row.name));
        QVERIFY2(node, row.name);
        for (int g = 0; g < 8; ++g)
            QVERIFY2(node->property(getters[g]).toString() == row.values.at(g),
                     qPrintable(QStringLiteral("%1.%2 = '%3', expected '%4'")
                                    .arg(QLatin1String(row.name), QLatin1String(getters[g]),
                                         node->property(getters[g]).toString(), row.values.at(g))));
    }
}

// 4b-4 Task 2: rect(.row) > text. Count the properties written to the composed label: the mount writes the
// whole payload, an unchanged re-apply writes none, and each single change writes exactly its own property.
void QmlCssTests::textPayloadWritesOnlyChanges()
{
    ComposedTextScope composed;
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { color: #111111; } .row2 { color: #222222; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent c(&engine);
    c.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { cssPrimitive: "div"; cssClass: ["row"]; width: 100; height: 50
            CssText { objectName: "t"; text: "a" } }
    )", QUrl());
    int mark = Detail::g_textLabelWrites;
    QScopedPointer<QObject> row(c.create());
    QVERIFY2(row, qPrintable(c.errorString()));
    QQuickWindow window;
    qobject_cast<QQuickItem *>(row.data())->setParentItem(window.contentItem());
    auto *text = row->findChild<QQuickItem *>(QStringLiteral("t"));
    QVERIFY(text);
    QQuickItem *label = composedText(text);
    QVERIFY(label);
    auto delta = [&mark]() {
        const int d = Detail::g_textLabelWrites - mark;
        mark = Detail::g_textLabelWrites;
        return d;
    };
    // 1. the mount writes the whole payload
    QVERIFY(delta() > 0);
    QCOMPARE(label->property("color").value<QColor>(), QColor(QStringLiteral("#111111")));

    // 2. an unchanged re-resolve, plus a forced full re-apply through the inheritance chain, writes nothing
    theme.loadCss(qobject_cast<QQuickItem *>(row.data()));
    QVERIFY(QMetaObject::invokeMethod(row.data(), "inheritedChanged"));
    QCOMPARE(delta(), 0);

    // 3. the inherited color changes: one write (color)
    row->setProperty("cssClass", QStringList{QStringLiteral("row2")});
    QCOMPARE(delta(), 1);
    QCOMPARE(label->property("color").value<QColor>(), QColor(QStringLiteral("#222222")));

    // 4. a new default font size with no inherited font-size: one write (font; line-height stays "normal")
    text->setProperty("defaultFontSize", 15.0);
    QCOMPARE(delta(), 1);
    QCOMPARE(label->property("font").value<QFont>().pointSizeF(), 15.0);

    // 5. the text changes: one write
    text->setProperty("text", QStringLiteral("b"));
    QCOMPARE(delta(), 1);
    QCOMPARE(label->property("text").toString(), QStringLiteral("b"));

    // 6. styledText goes through the same payload: one write, and a forced re-apply after it writes nothing
    text->setProperty("styledText", true);
    QCOMPARE(delta(), 1);
    QCOMPARE(label->property("textFormat").toInt(), 4); // Text.StyledText
    QVERIFY(QMetaObject::invokeMethod(row.data(), "inheritedChanged"));
    QCOMPARE(delta(), 0);
}

// Step 5a Task 1: SQ_PHASE_STATS opt-in phase timers must cost nothing when disabled (no site ever
// touches the buckets map) and must count exactly what a 100-row mount creates when enabled.
void QmlCssTests::phaseStatsAreFreeWhenDisabled()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(".row { height: 10px; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect {
            id: box
            cssPrimitive: ""
            width: 200; height: 2000
            property int count: 0
            style: ({ "display": "flex", "flex-direction": "column" })
            CssRepeater {
                model: box.count
                delegate: Component {
                    CssRect { cssClass: ["row"]
                        CssText { text: "a" }
                        CssText { text: "b" }
                    }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));
    QQuickWindow window;
    qobject_cast<QQuickItem *>(root.data())->setParentItem(window.contentItem());

    const bool wasOn = Detail::g_phaseStats.enabled;

    // Stats OFF: a 100-row mount (200 CssText, 100 delegates) leaves every bucket's count at 0 — in
    // practice no bucket is even created, since the disabled branch never calls time()/bump().
    Detail::g_phaseStats.enabled = false;
    Detail::g_phaseStats.buckets.clear();
    root->setProperty("count", 100);
    for (auto it = Detail::g_phaseStats.buckets.constBegin(); it != Detail::g_phaseStats.buckets.constEnd(); ++it)
        QCOMPARE(it->count, qint64(0));

    // Stats ON: tear down and remount fresh so the counts below are exactly this mount's.
    root->setProperty("count", 0);
    Detail::g_phaseStats.enabled = true;
    Detail::g_phaseStats.buckets.clear();
    root->setProperty("count", 100);
    Detail::g_phaseStats.enabled = wasOn;

    QCOMPARE(Detail::g_phaseStats.buckets.value("repeater.beginCreate").count, qint64(100));
    QVERIFY(Detail::g_phaseStats.buckets.value("text.metrics").count >= 200);
    Detail::g_phaseStats.buckets.clear();
}

// Step 5b Task 2: csslength.cpp's cssSharedLayoutStyle used to be a ONE-entry memo — only two
// CONSECUTIVE boxes of the same resolved style shared a parsed CssLayoutStyle. 100 rows of
// `.row > .id, .label` create in alternating order (row, id, label, row, id, label, …): every
// class's underlying style map is the SAME shared QVariantMap (the context node's own-resolve memo,
// 4b-2 — identical siblings share one node, whose ownResolves caches by the own-input key), but the
// one-entry memo was defeated by the alternation and reparsed almost every box.
void QmlCssTests::typedStylesSharedAcrossAlternatingClasses()
{
    CssTheme theme;
    theme.loadFromString(QStringLiteral(
        ".list { display: flex; flex-direction: column; } "
        ".row { display: flex; height: 20px; } "
        ".id { width: 10px; } "
        ".label { flex: 1; }"));
    CssLayoutEngine layoutEngine(&theme);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
    engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layoutEngine);
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { cssClass: ["list"]; width: 400; height: 3000
            CssRepeater {
                model: 100
                delegate: Component {
                    CssRect { objectName: "row"; cssClass: ["row"]
                        CssRect { objectName: "id"; cssClass: ["id"] }
                        CssRect { objectName: "label"; cssClass: ["label"] }
                    }
                }
            }
        }
    )", QUrl());
    QScopedPointer<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.data());
    QVERIFY(root);
    QQuickWindow window;
    root->setParentItem(window.contentItem());

    const QList<QQuickItem *> rows = root->findChildren<QQuickItem *>(QStringLiteral("row"));
    QCOMPARE(rows.size(), 100);

    // Access each box's cached layout style exactly as the layout engine's child scan does
    // (CssBox::layoutStyleRef), one full pass over the container and every row's content.
    layoutEngine.layout(root, contentHolderOf(rows.first()));
    for (QQuickItem *row : rows)
        layoutEngine.layout(row, contentHolderOf(row->findChild<QQuickItem *>(QStringLiteral("id"))));

    QSet<const void *> distinct;
    for (QQuickItem *row : rows) {
        distinct.insert(qobject_cast<CssBox *>(row)->layoutStyleRef().data());
        for (const QLatin1String &name : { QLatin1String("id"), QLatin1String("label") }) {
            auto *child = row->findChild<QQuickItem *>(name);
            QVERIFY(child);
            distinct.insert(qobject_cast<CssBox *>(child)->layoutStyleRef().data());
        }
    }
    // Today (the one-entry memo): ~300, one per box. After the bounded cache: 3 — one per class.
    QCOMPARE(distinct.size(), 3);
}

// ---- Step 5v-1: virtualized CssRepeater (fixed pitch, no pool) ----------------------------------

namespace {

// A 400px scroll column of fixed 20px rows, 2px apart, 8px padding on every side: row i sits at
// y = 8 + i * 22 and the logical extent of N rows is 8 + N * 20 + (N - 1) * 2 + 8.
const char *const kVirtualCss = R"(
    .list { display: flex; flex-direction: column; overflow-y: auto; padding: 8px; gap: 2px; }
    .row { display: flex; flex-direction: row; height: 20px; gap: 4px; background-color: #dde6f0; }
    .id { width: 48px; color: #203040; font-size: 12px; }
    .label { flex: 1; color: #102030; font-size: 12px; }
)";
constexpr qreal kPad = 8, kRowH = 20, kGap = 2, kPitch = kRowH + kGap, kViewH = 400;

QVariantList makeVirtualRows(int n, int idBase = 0)
{
    QVariantList rows;
    rows.reserve(n);
    for (int i = 0; i < n; ++i)
        rows.append(QVariantMap{ { QStringLiteral("id"), idBase + i + 1 },
                                 { QStringLiteral("label"), QStringLiteral("row %1").arg(idBase + i) } });
    return rows;
}

struct VirtualScene {
    CssTheme theme;
    CssLayoutEngine layout{ &theme };
    QQmlEngine engine;
    QQuickWindow window;
    QScopedPointer<QObject> root;
    QQuickItem *list = nullptr;
    QQuickItem *flick = nullptr;
    QQuickItem *holder = nullptr;
    CssRepeater *rep = nullptr;
    QByteArray rootClass = "list";

    explicit VirtualScene(const QString &css)
    {
        theme.loadFromString(css);
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);
        window.resize(300, int(kViewH));
        window.setColor(Qt::white);
    }

    QString mount(bool virtualize, const QVariantList &rows, const char *delegate = nullptr)
    {
        static const char *const kDefaultDelegate = R"(
                        CssRect { cssClass: ["row"]; cssPrimitive: "div"
                            CssText { cssClass: ["id"]; text: "" + modelData.id }
                            CssText { objectName: "label"; cssClass: ["label"]; text: "" + modelData.label }
                        })";
        QQmlComponent c(&engine);
        c.setData(QByteArray(R"(
            import QtQuick
            import qmlcss
            CssRect {
                id: list
                objectName: "list"
                cssClass: [")" + rootClass + R"("]
                cssPrimitive: "div"
                width: 300; height: 400
                property var rows: []
                // Identity-preserving edits (5c): a NEW array holding the SAME element objects, as a
                // Solid store/signal update produces — unlike setRows(), which converts fresh objects.
                // setRows() stores a C++ list (a sequence, re-converted on every read): own it as a JS array.
                function adopt() { rows = rows.slice() }
                function truncate(n) { rows = rows.slice(0, n) }
                function swapRows(a, b) { const r = rows.slice(); const t = r[a]; r[a] = r[b]; r[b] = t; rows = r }
                function removeRow(i) { const r = rows.slice(); r.splice(i, 1); rows = r }
                function setLabel(i, t) { const r = rows.slice(); r[i] = { id: r[i].id, label: t }; rows = r }
                function updateEvery(k) {
                    rows = rows.map((e, i) => i % k ? e : ({ id: e.id, label: e.label + " !" }))
                }
                function appendRows(n, idBase) {
                    const r = rows.slice()
                    for (let i = 0; i < n; ++i)
                        r.push({ id: idBase + i + 1, label: "row " + (idBase + i) })
                    rows = r
                }
                CssRepeater {
                    model: list.rows
                    virtualize: )") + (virtualize ? "true" : "false") + R"(
                    delegate: Component {)" + (delegate ? delegate : kDefaultDelegate) + R"(
                    }
                }
            }
        )", QUrl());
        root.reset(c.create());
        if (!root)
            return c.errorString();
        list = qobject_cast<QQuickItem *>(root.data());
        list->setParentItem(window.contentItem());
        rep = root->findChild<CssRepeater *>();
        for (QQuickItem *k : root->findChildren<QQuickItem *>()) {
            if (QByteArray(k->metaObject()->className()).contains("Flickable")) {
                flick = k;
                break;
            }
        }
        if (!rep || !flick)
            return QStringLiteral("no repeater / no Flickable");
        holder = rep->parentItem();
        setRows(rows);
        return QString();
    }
    void setRows(const QVariantList &rows) { root->setProperty("rows", QVariant::fromValue(rows)); }
    // The model as the QML side holds it now (after a JS-side edit).
    QVariantList currentRows() const
    {
        QVariant v = root->property("rows");
        if (v.metaType() == QMetaType::fromType<QJSValue>())
            v = v.value<QJSValue>().toVariant();
        return v.toList();
    }
    bool call(const char *fn, const QVariant &a, const QVariant &b = QVariant())
    {
        return b.isValid() ? QMetaObject::invokeMethod(root.data(), fn, Q_ARG(QVariant, a), Q_ARG(QVariant, b))
                           : QMetaObject::invokeMethod(root.data(), fn, Q_ARG(QVariant, a));
    }
    void setContentY(qreal y) { flick->setProperty("contentY", y); }
    qreal contentHeight() const { return flick->property("contentHeight").toReal(); }
    qreal maxContentY() const { return contentHeight() - flick->height(); }

    // The materialized rows (the holder's row children), in document order.
    QList<QQuickItem *> rows() const
    {
        QList<QQuickItem *> out;
        for (QQuickItem *k : holder->childItems())
            if (qobject_cast<CssRect *>(k))
                out.append(k);
        return out;
    }
    static int indexOf(QQuickItem *row) { return qmlContext(row)->contextProperty(QStringLiteral("index")).toInt(); }
    static QString labelOf(QQuickItem *row)
    {
        QObject *l = row->findChild<QObject *>(QStringLiteral("label"));
        return l ? l->property("text").toString() : QString();
    }
    QImage grab()
    {
        QCoreApplication::processEvents();
        return window.grabWindow().convertToFormat(QImage::Format_ARGB32);
    }
};

// Every materialized row shows its model entry at its logical position; returns "" or the problem.
QString checkVirtualRows(const VirtualScene &s, const QVariantList &model)
{
    const QList<QQuickItem *> rows = s.rows();
    for (QQuickItem *row : rows) {
        const int i = VirtualScene::indexOf(row);
        if (i < 0 || i >= model.size())
            return QStringLiteral("row with index %1 of %2").arg(i).arg(model.size());
        const QString want = model.at(i).toMap().value(QStringLiteral("label")).toString();
        if (VirtualScene::labelOf(row) != want)
            return QStringLiteral("row %1 shows '%2', model says '%3'").arg(i).arg(VirtualScene::labelOf(row), want);
        if (!qFuzzyCompare(row->y() + 1, kPad + i * kPitch + 1) || !qFuzzyCompare(row->height(), kRowH))
            return QStringLiteral("row %1 at y %2 h %3, want y %4").arg(i).arg(row->y()).arg(row->height()).arg(kPad + i * kPitch);
    }
    return QString();
}

struct CreateDestroyCounts {
    qint64 created = 0, destroyed = 0;
};
// Rows that ENTERED / LEFT the window: since 5v-2 an entering row is created or rebound from the
// recycling pool, a leaving one destroyed or parked in it.
CreateDestroyCounts takeCounts()
{
    CreateDestroyCounts c;
    c.created = Detail::g_phaseStats.buckets.value("repeater.beginCreate").count
        + Detail::g_phaseStats.buckets.value("repeater.rebind").count;
    c.destroyed = Detail::g_phaseStats.buckets.value("repeater.destroyRow").count
        + Detail::g_phaseStats.buckets.value("repeater.park").count;
    Detail::g_phaseStats.buckets.clear();
    return c;
}

} // namespace

void QmlCssTests::virtualListMaterializesOnlyTheWindow()
{
    VirtualScene s(QString::fromLatin1(kVirtualCss));
    const QString err = s.mount(true, makeVirtualRows(10000));
    QVERIFY2(err.isEmpty(), qPrintable(err));
    const int bound = 3 * int(kViewH / kPitch) + 4;
    for (const qreal y : { 0.0, 110000.0 }) {
        s.setContentY(y);
        const int alive = s.rows().size();
        QVERIFY2(alive > 0 && alive <= bound, qPrintable(QStringLiteral("contentY %1: %2 rows alive (bound %3)").arg(y).arg(alive).arg(bound)));
        QCOMPARE(s.rep->materializedCount(), alive);
    }
    QCOMPARE(s.contentHeight(), kPad + 10000 * kRowH + 9999 * kGap + kPad);
}

void QmlCssTests::virtualListScrollsExactly()
{
    VirtualScene s(QString::fromLatin1(kVirtualCss));
    const QVariantList model = makeVirtualRows(10000);
    QVERIFY2(s.mount(true, model).isEmpty(), "mount");
    for (const qreal cy : { 110000.0, s.maxContentY() }) {
        s.setContentY(cy);
        QCOMPARE(s.flick->property("contentY").toReal(), cy);
        const QString bad = checkVirtualRows(s, model);
        QVERIFY2(bad.isEmpty(), qPrintable(bad));
        QSet<int> alive;
        for (QQuickItem *row : s.rows()) {
            const int i = VirtualScene::indexOf(row);
            alive.insert(i);
            const qreal top = kPad + i * kPitch;
            // Nothing outside the window [contentY - viewport, contentY + 2 viewport] exists.
            QVERIFY2(top + kRowH > cy - kViewH && top < cy + 2 * kViewH,
                     qPrintable(QStringLiteral("row %1 outside the window at contentY %2").arg(i).arg(cy)));
            QCOMPARE(row->x(), kPad);
            QCOMPARE(row->width(), 300 - 2 * kPad);
        }
        // Every row intersecting the viewport is materialized.
        for (int i = 0; i < model.size(); ++i) {
            const qreal top = kPad + i * kPitch;
            if (top + kRowH > cy && top < cy + kViewH)
                QVERIFY2(alive.contains(i), qPrintable(QStringLiteral("visible row %1 missing at contentY %2").arg(i).arg(cy)));
        }
    }
}

void QmlCssTests::virtualListMatchesEagerPixels()
{
    const QVariantList model = makeVirtualRows(1000);
    VirtualScene eager(QString::fromLatin1(kVirtualCss));
    VirtualScene virt(QString::fromLatin1(kVirtualCss));
    QVERIFY2(eager.mount(false, model).isEmpty(), "eager mount");
    QVERIFY2(virt.mount(true, model).isEmpty(), "virtual mount");
    QCOMPARE(eager.rows().size(), 1000);
    QVERIFY(virt.rows().size() < 1000);
    QCOMPARE(virt.contentHeight(), eager.contentHeight());
    eager.window.show();
    virt.window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&eager.window));
    QVERIFY(QTest::qWaitForWindowExposed(&virt.window));
    for (const qreal cy : { 0.0, 1234.0, eager.maxContentY() }) {
        eager.setContentY(cy);
        virt.setContentY(cy);
        const QImage a = eager.grab();
        const QImage b = virt.grab();
        QCOMPARE(a.size(), b.size());
        int diff = 0, painted = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                diff += a.pixel(x, y) != b.pixel(x, y);
                painted += a.pixel(x, y) != QColor(Qt::white).rgba();
            }
        }
        if (!qEnvironmentVariableIsEmpty("SQ_VIRTUAL_GRAB_DIR")) {
            const QString dir = qEnvironmentVariable("SQ_VIRTUAL_GRAB_DIR");
            a.save(QStringLiteral("%1/eager-%2.png").arg(dir).arg(int(cy)));
            b.save(QStringLiteral("%1/virtual-%2.png").arg(dir).arg(int(cy)));
        }
        QVERIFY2(diff == 0, qPrintable(QStringLiteral("contentY %1: %2 pixels differ").arg(cy).arg(diff)));
        // ...and the frame is not blank: rows actually painted.
        QVERIFY2(painted > a.width() * a.height() / 2, qPrintable(QString::number(painted)));
    }
}

void QmlCssTests::virtualListKeyedUpdate()
{
    VirtualScene s(QString::fromLatin1(kVirtualCss));
    QVariantList model = makeVirtualRows(1000);
    QVERIFY2(s.mount(true, model).isEmpty(), "mount");
    const int window = s.rows().size();
    QVERIFY(window > 0 && window < 100);

    const bool wasOn = Detail::g_phaseStats.enabled;
    Detail::g_phaseStats.enabled = true;
    Detail::g_phaseStats.buckets.clear();
    const auto verify = [&](const char *what) {
        const QString bad = checkVirtualRows(s, model);
        QVERIFY2(bad.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(what), bad)));
        QCOMPARE(s.contentHeight(), kPad + model.size() * kRowH + (model.size() - 1) * kGap + kPad);
    };

    // swap 1 <-> 998 at the top: row 1 leaves the window (destroyed), entry 998 enters it (created).
    model.swapItemsAt(1, 998);
    s.setRows(model);
    CreateDestroyCounts c = takeCounts();
    QCOMPARE(c.created, qint64(1));
    QCOMPARE(c.destroyed, qint64(1));
    verify("swap");
    s.setContentY(s.maxContentY());
    verify("swap, at the end");
    s.setContentY(0);
    verify("swap, back at the top");
    takeCounts();

    // remove 500 (outside the window): nothing is created or destroyed.
    model.removeAt(500);
    s.setRows(model);
    c = takeCounts();
    QCOMPARE(c.created, qint64(0));
    QCOMPARE(c.destroyed, qint64(0));
    verify("remove");
    s.setContentY(kPad + 500 * kPitch - 100);
    verify("remove, around 500");
    s.setContentY(0);
    takeCounts();

    // update every 10th label (fresh maps, same ids): window rows rebind in place, nothing created.
    for (int i = 0; i < model.size(); i += 10) {
        QVariantMap m = model.at(i).toMap();
        m.insert(QStringLiteral("label"), QStringLiteral("updated %1").arg(i));
        model[i] = m;
    }
    s.setRows(model);
    c = takeCounts();
    QCOMPARE(c.created, qint64(0));
    QCOMPARE(c.destroyed, qint64(0));
    verify("update");

    // append below the window: nothing is created until scrolled to.
    model.append(makeVirtualRows(25, 5000));
    s.setRows(model);
    c = takeCounts();
    QCOMPARE(c.created, qint64(0));
    QCOMPARE(c.destroyed, qint64(0));
    verify("append");
    s.setContentY(s.maxContentY());
    verify("append, at the end");
    c = takeCounts();
    QVERIFY(c.created <= window + 2 && c.destroyed <= window + 2);

    Detail::g_phaseStats.enabled = wasOn;
    Detail::g_phaseStats.buckets.clear();
}

void QmlCssTests::virtualListFallsBackEager()
{
    const QString fixed = QString::fromLatin1(kVirtualCss);
    // (Auto height is windowed since 5v-3; a percentage height still depends on the container.)
    const QString autoHeight = fixed + QStringLiteral(" .row { height: 5%; }");
    const QString wrapped = fixed + QStringLiteral(" .list { flex-wrap: wrap; }");
    const QVariantList model = makeVirtualRows(300);

    VirtualScene s(fixed);
    QVERIFY2(s.mount(true, model).isEmpty(), "mount");
    QVERIFY(s.rows().size() < 300);

    // The row's y for every logical index, from a scene where all rows exist.
    const auto ys = [](const VirtualScene &v) {
        QHash<int, qreal> out;
        for (QQuickItem *row : v.rows())
            out.insert(VirtualScene::indexOf(row), row->y());
        return out;
    };

    for (const QString &broken : { autoHeight, wrapped }) {
        s.theme.loadFromString(broken);
        QCOMPARE(s.rows().size(), 300);
        QCOMPARE(s.rep->materializedCount(), 300);
        VirtualScene eager(broken);
        QVERIFY2(eager.mount(false, model).isEmpty(), "eager mount");
        QCOMPARE(ys(s), ys(eager));
        QCOMPARE(s.contentHeight(), eager.contentHeight());

        s.theme.loadFromString(fixed);
        QVERIFY2(s.rows().size() < 300, qPrintable(QString::number(s.rows().size())));
        const QString bad = checkVirtualRows(s, model);
        QVERIFY2(bad.isEmpty(), qPrintable(bad));
    }

    // Accessibility active -> eager (the AT sees every row); inactive again -> windowed.
    const int previous = Detail::g_virtualA11yOverride;
    Detail::g_virtualA11yOverride = 1;
    QVERIFY(QMetaObject::invokeMethod(s.list, "requestRelayout"));
    QCOMPARE(s.rows().size(), 300);
    Detail::g_virtualA11yOverride = 0;
    QVERIFY(QMetaObject::invokeMethod(s.list, "requestRelayout"));
    QVERIFY(s.rows().size() < 300);
    Detail::g_virtualA11yOverride = previous;
    const QString bad = checkVirtualRows(s, model);
    QVERIFY2(bad.isEmpty(), qPrintable(bad));
}

// ---- Step 5c: identity-first incremental reconciliation ------------------------------------------

namespace {

// A keyed list driven from JS the way a Solid update drives it: every edit is a NEW array, and elements
// that did not change are the SAME objects. Each row logs its modelData evaluations ("m<id>") and its
// index evaluations ("i<id>") into evalLog (a push into a var array does not notify, so the probes do
// not depend on each other — see cssRepeaterWritesIndexOnlyWhenChanged).
struct IdentityScene {
    CssTheme theme;
    CssLayoutEngine layout{ &theme };
    QQmlEngine engine;
    QScopedPointer<QObject> root;

    QString mount(const QString &json)
    {
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);
        engine.rootContext()->setContextProperty(QStringLiteral("cssLayout"), &layout);
        QQmlComponent c(&engine);
        c.setData(R"(
            import QtQuick
            import qmlcss
            CssRect {
                id: box
                cssPrimitive: ""
                width: 200; height: 400
                style: ({ "display": "flex", "flex-direction": "column" })
                property var rows: []
                property var evalLog: []
                function setTo(json) { rows = JSON.parse(json) }
                function same() { rows = rows.slice() }
                function swap(a, b) { const r = rows.slice(); const t = r[a]; r[a] = r[b]; r[b] = t; rows = r }
                function removeAt(i) { const r = rows.slice(); r.splice(i, 1); rows = r }
                function replaceAt(i, json) { const r = rows.slice(); r[i] = JSON.parse(json); rows = r }
                function append(json) { rows = rows.concat(JSON.parse(json)) }
                function mutate(i, t) { rows[i].t = t; rows = rows.slice() }
                function dupFirst() { rows = [rows[0], rows[0]] }
                CssRepeater {
                    id: rep
                    model: box.rows
                    delegate: Component {
                        CssRect {
                            cssPrimitive: "div"
                            height: 10
                            objectName: "row-" + modelData.id
                            property string label: { box.evalLog.push("m" + modelData.id); return modelData.t }
                            // Re-entrancy probe: a row reacting to its new modelData by assigning the model
                            // (imperatively: through the `model:` binding QML reports a binding loop instead).
                            onLabelChanged: if (label === "go") rep.model = box.rows.concat([{ id: 99, t: "late" }])
                            property int idx: { box.evalLog.push("i" + modelData.id); return index }
                        }
                    }
                }
            }
        )", QUrl());
        root.reset(c.create());
        if (!root)
            return c.errorString();
        return run("setTo", json) ? QString() : QStringLiteral("setTo failed");
    }
    bool run(const char *fn, const QVariant &a = QVariant(), const QVariant &b = QVariant())
    {
        Detail::g_repeaterConversions = 0;
        const bool ok = !a.isValid() ? QMetaObject::invokeMethod(root.data(), fn)
            : !b.isValid()           ? QMetaObject::invokeMethod(root.data(), fn, Q_ARG(QVariant, a))
                                     : QMetaObject::invokeMethod(root.data(), fn, Q_ARG(QVariant, a), Q_ARG(QVariant, b));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        return ok;
    }
    // Live rows, in document order.
    QList<QQuickItem *> rows() const
    {
        QList<QQuickItem *> out;
        auto *rep = root->findChild<CssRepeater *>();
        if (!rep || !rep->parentItem())
            return out;
        for (QQuickItem *k : rep->parentItem()->childItems())
            if (k->objectName().startsWith(QLatin1String("row-")))
                out.append(k);
        return out;
    }
    QStringList labels() const
    {
        QStringList out;
        for (QQuickItem *r : rows())
            out.append(r->property("label").toString());
        return out;
    }
    QQuickItem *row(int i) const { return rows().value(i); }
    int logSize() const { return int(root->property("evalLog").toList().size()); }
    QStringList logSince(int from) const
    {
        QStringList out;
        const QVariantList l = root->property("evalLog").toList();
        for (int i = from; i < l.size(); ++i)
            out.append(l.at(i).toString());
        return out;
    }
};

const char *const kFourRows = R"([{"id":1,"t":"a"},{"id":2,"t":"b"},{"id":3,"t":"c"},{"id":4,"t":"d"}])";

} // namespace

void QmlCssTests::repeaterIdentityFastPath()
{
    IdentityScene s;
    QVERIFY2(s.mount(QString::fromLatin1(kFourRows)).isEmpty(), "mount");
    const QList<QQuickItem *> before = s.rows();
    QCOMPARE(before.size(), 4);

    // A new array of the SAME objects: nothing converted, no context written, every instance kept.
    int base = s.logSize();
    QVERIFY(s.run("same"));
    QCOMPARE(Detail::g_repeaterConversions, 0);
    QCOMPARE(s.logSince(base), QStringList());
    QCOMPARE(s.rows(), before);

    // Swap the ends: the two rows move (index only), nothing converted.
    base = s.logSize();
    QVERIFY(s.run("swap", 0, 3));
    QCOMPARE(Detail::g_repeaterConversions, 0);
    QStringList log = s.logSince(base);
    log.sort();
    QCOMPARE(log, (QStringList{ QStringLiteral("i1"), QStringLiteral("i4") }));
    QCOMPARE(s.rows(), (QList<QQuickItem *>{ before[3], before[1], before[2], before[0] }));
    QCOMPARE(before[0]->property("idx").toInt(), 3);
    QCOMPARE(before[3]->property("idx").toInt(), 0);

    // Remove the second: it is destroyed, the rows after it shift (index only), nothing converted.
    QPointer<QQuickItem> gone = before[1];
    base = s.logSize();
    QVERIFY(s.run("removeAt", 1));
    QCOMPARE(Detail::g_repeaterConversions, 0);
    QVERIFY(!gone || !gone->parentItem());
    log = s.logSince(base);
    log.sort();
    QCOMPARE(log, (QStringList{ QStringLiteral("i1"), QStringLiteral("i3") }));
    QCOMPARE(s.rows(), (QList<QQuickItem *>{ before[3], before[2], before[0] }));
    QCOMPARE(s.labels(), (QStringList{ QStringLiteral("d"), QStringLiteral("c"), QStringLiteral("a") }));

    // Append one: exactly the new element is converted; no survivor is touched.
    base = s.logSize();
    QVERIFY(s.run("append", QStringLiteral(R"([{"id":9,"t":"z"}])")));
    QCOMPARE(Detail::g_repeaterConversions, 1);
    for (const QString &e : s.logSince(base))
        QVERIFY2(e.endsWith(QLatin1Char('9')), qPrintable(e));
    QCOMPARE(s.labels(), (QStringList{ QStringLiteral("d"), QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("z") }));
    QCOMPARE(s.row(3)->property("idx").toInt(), 3);
}

void QmlCssTests::repeaterNewObjectSameIdUpdates()
{
    IdentityScene s;
    QVERIFY2(s.mount(QString::fromLatin1(kFourRows)).isEmpty(), "mount");
    const QList<QQuickItem *> before = s.rows();

    // C23 unchanged: a NEW object with the same id reuses the row and updates modelData in place —
    // and it is the only element converted.
    int base = s.logSize();
    QVERIFY(s.run("replaceAt", 1, QStringLiteral(R"({"id":2,"t":"B"})")));
    QCOMPARE(Detail::g_repeaterConversions, 1);
    QCOMPARE(s.rows(), before);
    QCOMPARE(s.labels(), (QStringList{ QStringLiteral("a"), QStringLiteral("B"), QStringLiteral("c"), QStringLiteral("d") }));
    QStringList log = s.logSince(base);
    log.removeAll(QStringLiteral("i2")); // the idx probe also reads modelData.id
    QCOMPARE(log, QStringList{ QStringLiteral("m2") });

    // A new object EQUAL to the old one: converted, but modelData is not re-set (today's rule).
    base = s.logSize();
    QVERIFY(s.run("replaceAt", 2, QStringLiteral(R"({"id":3,"t":"c"})")));
    QCOMPARE(Detail::g_repeaterConversions, 1);
    QCOMPARE(s.logSince(base), QStringList());
    QCOMPARE(s.rows(), before);
}

void QmlCssTests::repeaterMutatedSameObjectIsNotReread()
{
    // Owner decision 2026-10-10 (5c), Solid web <For> semantics: an element is compared by IDENTITY. An
    // object mutated in place and re-sent in a new array is the same element — its row is not re-read.
    IdentityScene s;
    QVERIFY2(s.mount(QString::fromLatin1(kFourRows)).isEmpty(), "mount");
    const QList<QQuickItem *> before = s.rows();
    QVERIFY(s.run("mutate", 1, QStringLiteral("X")));
    QCOMPARE(Detail::g_repeaterConversions, 0);
    QCOMPARE(s.rows(), before);
    QCOMPARE(s.labels(), (QStringList{ QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d") }));
    // A fresh object (the immutable-update idiom) is re-read.
    QVERIFY(s.run("replaceAt", 1, QStringLiteral(R"({"id":2,"t":"X"})")));
    QCOMPARE(s.labels(), (QStringList{ QStringLiteral("a"), QStringLiteral("X"), QStringLiteral("c"), QStringLiteral("d") }));
    QCOMPARE(s.rows(), before);
}

void QmlCssTests::repeaterDuplicateIdsFallBackToUnkeyed()
{
    IdentityScene s;
    QVERIFY2(s.mount(QStringLiteral(R"([{"id":1,"t":"a"},{"id":2,"t":"b"},{"id":3,"t":"c"}])")).isEmpty(), "mount");
    const QList<QQuickItem *> before = s.rows();
    QPointer<QQuickItem> one = before[0], two = before[1], three = before[2];

    // A duplicate id AFTER a prefix that would match by key with an edit (1 -> "A"): the whole model is
    // unkeyed, matched by VALUE exactly as before 5c — row 1 is NOT updated in place but recreated, {2,b}
    // keeps its delegate, the second id 2 is new.
    QVERIFY(s.run("setTo", QStringLiteral(R"([{"id":1,"t":"A"},{"id":2,"t":"b"},{"id":2,"t":"c"}])")));
    QCOMPARE(s.labels(), (QStringList{ QStringLiteral("A"), QStringLiteral("b"), QStringLiteral("c") }));
    QVERIFY(!one || !one->parentItem());
    QVERIFY(!three || !three->parentItem());
    QCOMPARE(s.row(1), two.data());
    QCOMPARE(two->property("idx").toInt(), 1);

    // Back to keyed from the unkeyed state (fresh objects): first row per id wins, as before.
    QPointer<QQuickItem> rowA = s.row(0), dupC = s.row(2);
    QVERIFY(s.run("setTo", QStringLiteral(R"([{"id":2,"t":"b2"},{"id":1,"t":"A"},{"id":3,"t":"c"}])")));
    QCOMPARE(s.labels(), (QStringList{ QStringLiteral("b2"), QStringLiteral("A"), QStringLiteral("c") }));
    QCOMPARE(s.row(0), two.data());
    QCOMPARE(s.row(1), rowA.data());
    QVERIFY(!dupC || !dupC->parentItem());

    // ...and identity works again from there.
    const QList<QQuickItem *> keyed = s.rows();
    QVERIFY(s.run("same"));
    QCOMPARE(Detail::g_repeaterConversions, 0);
    QCOMPARE(s.rows(), keyed);

    // The SAME object twice is a duplicate id too: unkeyed, value matching.
    QPointer<QQuickItem> first = keyed[0];
    QVERIFY(s.run("dupFirst"));
    QCOMPARE(s.labels(), (QStringList{ QStringLiteral("b2"), QStringLiteral("b2") }));
    QCOMPARE(s.row(0), first.data());
    QVERIFY(s.row(1) != first.data());
}

void QmlCssTests::repeaterReentrantModelChangeIsDeferred()
{
    IdentityScene s;
    QVERIFY2(s.mount(QString::fromLatin1(kFourRows)).isEmpty(), "mount");
    const QList<QQuickItem *> before = s.rows();
    // Row 2's modelData write (pass 2 of the reconciliation) makes it set a new model from inside the
    // reconciliation: that one runs right after, never on top of the half-done one.
    QVERIFY(s.run("replaceAt", 1, QStringLiteral(R"({"id":2,"t":"go"})")));
    QCOMPARE(s.labels(), (QStringList{ QStringLiteral("a"), QStringLiteral("go"), QStringLiteral("c"), QStringLiteral("d"),
                                       QStringLiteral("late") }));
    QCOMPARE(s.rows().mid(0, 4), before);
    QCOMPARE(s.row(4)->property("idx").toInt(), 4);
}

void QmlCssTests::virtualListIdentityOps()
{
    VirtualScene s(QString::fromLatin1(kVirtualCss));
    QVERIFY2(s.mount(true, makeVirtualRows(1000)).isEmpty(), "mount");
    const bool wasOn = Detail::g_phaseStats.enabled;
    Detail::g_phaseStats.enabled = true;
    Detail::g_phaseStats.buckets.clear();
    const auto verify = [&](const char *what) {
        const QVariantList model = s.currentRows();
        const QString bad = checkVirtualRows(s, model);
        QVERIFY2(bad.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(QLatin1String(what), bad)));
        QCOMPARE(s.contentHeight(), kPad + model.size() * kRowH + (model.size() - 1) * kGap + kPad);
    };

    QVERIFY(QMetaObject::invokeMethod(s.root.data(), "adopt"));
    takeCounts();
    Detail::g_repeaterConversions = 0;
    QVERIFY(s.call("swapRows", 1, 998));
    QCOMPARE(Detail::g_repeaterConversions, 0);
    CreateDestroyCounts c = takeCounts();
    QCOMPARE(c.created, qint64(1));
    QCOMPARE(c.destroyed, qint64(1));
    verify("swap");
    s.setContentY(s.maxContentY());
    verify("swap, at the end");
    s.setContentY(0);
    takeCounts();

    Detail::g_repeaterConversions = 0;
    QVERIFY(s.call("removeRow", 500));
    QCOMPARE(Detail::g_repeaterConversions, 0);
    c = takeCounts();
    QCOMPARE(c.created, qint64(0));
    QCOMPARE(c.destroyed, qint64(0));
    verify("remove");

    Detail::g_repeaterConversions = 0;
    QVERIFY(s.call("updateEvery", 10));
    QCOMPARE(Detail::g_repeaterConversions, 100); // 999 rows: indices 0, 10, ..., 990
    c = takeCounts();
    QCOMPARE(c.created, qint64(0));
    QCOMPARE(c.destroyed, qint64(0));
    verify("update");

    Detail::g_repeaterConversions = 0;
    QVERIFY(s.call("appendRows", 25, 5000));
    QCOMPARE(Detail::g_repeaterConversions, 25);
    c = takeCounts();
    QCOMPARE(c.created, qint64(0));
    verify("append");
    s.setContentY(s.maxContentY());
    verify("append, at the end");

    Detail::g_phaseStats.enabled = wasOn;
    Detail::g_phaseStats.buckets.clear();
}

// Measurement, not a gate: SQ_REPEATER_BENCH=1 prints repeater.toList / repeater.match per keyed op on a
// 10000-row virtual list driven from JS (identity-preserving edits).
void QmlCssTests::repeaterKeyedOpsBench()
{
    if (!qEnvironmentVariableIntValue("SQ_REPEATER_BENCH"))
        QSKIP("set SQ_REPEATER_BENCH=1 to run");
    VirtualScene s(QString::fromLatin1(kVirtualCss));
    QVERIFY2(s.mount(true, makeVirtualRows(10000)).isEmpty(), "mount");
    QVERIFY(QMetaObject::invokeMethod(s.root.data(), "adopt"));
    const bool wasOn = Detail::g_phaseStats.enabled;
    Detail::g_phaseStats.enabled = true;
    const auto op = [&](const char *name, const char *fn, const QVariant &a, const QVariant &b) {
        Detail::g_phaseStats.buckets.clear();
        Detail::g_repeaterConversions = 0;
        QElapsedTimer t;
        t.start();
        QVERIFY(s.call(fn, a, b));
        const double wall = t.nsecsElapsed() / 1e6;
        const auto ms = [](const char *k) { return Detail::g_phaseStats.buckets.value(k).ns / 1e6; };
        if (Detail::g_phaseStats.enabled)
            fprintf(stderr, "[5c-bench] %-8s toList %7.3f ms  match %7.3f ms  sum %7.3f ms  conversions %6d  wall %7.3f ms\n",
                    name, ms("repeater.toList"), ms("repeater.match"), ms("repeater.toList") + ms("repeater.match"),
                    Detail::g_repeaterConversions, wall);
        else
            fprintf(stderr, "[5c-bench] %-8s (phase stats off) wall %7.3f ms\n", name, wall);
    };
    // Rounds with the phase timers (where the time goes), then without (wall clock not inflated by the
    // per-sample timer cost).
    for (int round = 0; round < 6; ++round) {
        Detail::g_phaseStats.enabled = round < 3;
        op("update", "updateEvery", 10, QVariant());
        op("swap", "swapRows", 1, 998);
        op("swap", "swapRows", 1, 998);
        op("remove", "removeRow", 500, QVariant());
        op("append", "appendRows", 1000, 100000 + round * 1000);
        QVERIFY(s.call("truncate", 10000)); // every round starts from N = 10000
    }
    Detail::g_phaseStats.enabled = wasOn;
    Detail::g_phaseStats.buckets.clear();
}

// ---- Step 5v-2: bounded recycling pool -------------------------------------------------------------

namespace {

// kVirtualCss's geometry; the row's colour comes from an ANCESTOR-SCOPED rule on its record's status
// (a row resolved without its `.list` ancestor — detached — would lose it), plus an engine-hover rule.
const char *const kPoolCss = R"(
    .list { display: flex; flex-direction: column; overflow-y: auto; padding: 8px; gap: 2px; }
    .row { display: flex; flex-direction: row; height: 20px; gap: 4px; }
    .list .a { background-color: #dd2222; }
    .list .b { background-color: #2222dd; }
    .row:hover { background-color: #22dd22; }
    .id { width: 48px; color: #203040; font-size: 12px; }
    .label { flex: 1; color: #102030; font-size: 12px; }
)";
const char *const kPoolDelegate = R"(
                        CssRect { cssClass: ["row", modelData.status]; cssPrimitive: "div"
                            CssText { cssClass: ["id"]; text: "" + modelData.id }
                            CssText { objectName: "label"; cssClass: ["label"]; text: "" + modelData.label }
                        })";

QVariantList makePoolRows(int n)
{
    QVariantList rows;
    rows.reserve(n);
    for (int i = 0; i < n; ++i)
        rows.append(QVariantMap{ { QStringLiteral("id"), i + 1 },
                                 { QStringLiteral("label"), QStringLiteral("row %1").arg(i) },
                                 { QStringLiteral("status"), i % 2 ? QStringLiteral("b") : QStringLiteral("a") } });
    return rows;
}

// The largest window: rows intersecting [contentY - viewport, contentY + 2 viewport], plus rounding.
constexpr int kMaxWindow = int(3 * kViewH / kPitch) + 2;

QString bgOf(QQuickItem *row)
{
    return row->property("style").toMap().value(QStringLiteral("background-color")).toString().toLower();
}

// Every materialized row's background is its record's status colour (`a`/`b` -> the given colours).
QString checkPoolColors(const VirtualScene &s, const QString &a, const QString &b)
{
    for (QQuickItem *row : s.rows()) {
        const int i = VirtualScene::indexOf(row);
        const QString want = i % 2 ? b : a;
        if (bgOf(row) != want)
            return QStringLiteral("row %1 background '%2', want '%3'").arg(i).arg(bgOf(row), want);
    }
    return QString();
}

// The repeater's detached rows (parked in the pool): QObject children of the repeater, no parent item.
QList<QPointer<QQuickItem>> parkedRows(CssRepeater *rep)
{
    QList<QPointer<QQuickItem>> out;
    for (QQuickItem *k : rep->findChildren<QQuickItem *>(Qt::FindDirectChildrenOnly))
        if (!k->parentItem())
            out.append(k);
    return out;
}

void flushDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

struct PhaseStatsOn {
    bool was = Detail::g_phaseStats.enabled;
    PhaseStatsOn() { Detail::g_phaseStats.enabled = true; Detail::g_phaseStats.buckets.clear(); }
    ~PhaseStatsOn() { Detail::g_phaseStats.enabled = was; Detail::g_phaseStats.buckets.clear(); }
};
qint64 bucketCount(const char *k) { return Detail::g_phaseStats.buckets.value(k).count; }

} // namespace

void QmlCssTests::virtualPoolBoundsCreations()
{
    const PhaseStatsOn stats;
    VirtualScene s(QString::fromLatin1(kVirtualCss));
    const QVariantList model = makeVirtualRows(10000);
    QVERIFY2(s.mount(true, model).isEmpty(), "mount");
    // Top to bottom in viewport steps, the way a user scrolls.
    int steps = 0;
    for (qreal cy = 0; cy < s.maxContentY() + kViewH; cy += kViewH, ++steps) {
        s.setContentY(std::min(cy, s.maxContentY()));
        QVERIFY2(s.rep->pooledCount() <= s.rep->poolCapacity(),
                 qPrintable(QStringLiteral("pool %1 > capacity %2").arg(s.rep->pooledCount()).arg(s.rep->poolCapacity())));
        QVERIFY(s.rep->poolCapacity() <= kMaxWindow);
        if (steps % 97 == 0) {
            const QString bad = checkVirtualRows(s, model);
            QVERIFY2(bad.isEmpty(), qPrintable(bad));
        }
    }
    const QString bad = checkVirtualRows(s, model);
    QVERIFY2(bad.isEmpty(), qPrintable(bad));
    const qint64 created = bucketCount("repeater.beginCreate");
    fprintf(stderr, "[5v-2] traversal of 10000 rows in %d steps: created %lld, rebound %lld, parked %lld, destroyed %lld\n",
            steps, static_cast<long long>(created), static_cast<long long>(bucketCount("repeater.rebind")),
            static_cast<long long>(bucketCount("repeater.park")), static_cast<long long>(bucketCount("repeater.destroyRow")));
    QVERIFY2(created <= 2 * kMaxWindow, qPrintable(QStringLiteral("%1 rows created (bound %2)").arg(created).arg(2 * kMaxWindow)));
    // Memory stays flat: what is alive after the traversal is the window plus the bounded pool.
    flushDeletes();
    const int alive = int(s.rep->findChildren<QQuickItem *>(Qt::FindDirectChildrenOnly).size());
    QVERIFY2(alive <= 2 * kMaxWindow, qPrintable(QString::number(alive)));
}

void QmlCssTests::virtualPoolRecycledRowsAreCorrect()
{
    const PhaseStatsOn stats;
    const QVariantList model = makePoolRows(2000);
    VirtualScene eager(QString::fromLatin1(kPoolCss));
    VirtualScene virt(QString::fromLatin1(kPoolCss));
    QVERIFY2(eager.mount(false, model, kPoolDelegate).isEmpty(), "eager mount");
    QVERIFY2(virt.mount(true, model, kPoolDelegate).isEmpty(), "virtual mount");
    const QString red = QStringLiteral("#dd2222"), blue = QStringLiteral("#2222dd");
    // Far and back, in viewport steps and in jumps.
    for (qreal cy = 0; cy < virt.maxContentY(); cy += kViewH * 3.7)
        virt.setContentY(cy);
    virt.setContentY(virt.maxContentY());
    virt.setContentY(0);
    virt.setContentY(20000);
    QVERIFY2(bucketCount("repeater.rebind") > 0, "no row was recycled");
    for (const qreal cy : { 0.0, 1234.0, 20001.0, virt.maxContentY() }) {
        virt.setContentY(cy);
        const QString bad = checkVirtualRows(virt, model);
        QVERIFY2(bad.isEmpty(), qPrintable(bad));
        const QString badColor = checkPoolColors(virt, red, blue);
        QVERIFY2(badColor.isEmpty(), qPrintable(badColor));
    }
    eager.window.show();
    virt.window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&eager.window));
    QVERIFY(QTest::qWaitForWindowExposed(&virt.window));
    for (const qreal cy : { 0.0, 1234.0, eager.maxContentY() }) {
        eager.setContentY(cy);
        virt.setContentY(cy);
        const QImage a = eager.grab();
        const QImage b = virt.grab();
        QCOMPARE(a.size(), b.size());
        int diff = 0;
        for (int y = 0; y < a.height(); ++y)
            for (int x = 0; x < a.width(); ++x)
                diff += a.pixel(x, y) != b.pixel(x, y);
        QVERIFY2(diff == 0, qPrintable(QStringLiteral("contentY %1: %2 pixels differ").arg(cy).arg(diff)));
    }
}

void QmlCssTests::virtualPoolResetsState()
{
    const QString red = QStringLiteral("#dd2222"), blue = QStringLiteral("#2222dd");
    const qreal far = 5000 * kPitch;
    {
        // Engine hover on a row that is then recycled: the record it lands on is not hovered.
        VirtualScene s(QString::fromLatin1(kPoolCss));
        QVERIFY2(s.mount(true, makePoolRows(10000), kPoolDelegate).isEmpty(), "mount");
        QPointer<QQuickItem> hovered;
        for (QQuickItem *row : s.rows())
            if (VirtualScene::indexOf(row) == 3)
                hovered = row;
        QVERIFY(hovered);
        QVERIFY(hovered->setProperty("cssEngineHover", true));
        QCOMPARE(bgOf(hovered), QStringLiteral("#22dd22"));
        s.setContentY(far);
        QVERIFY2(hovered && s.rows().contains(hovered.data()), "the hovered row was not recycled into the window");
        QCOMPARE(hovered->property("cssEngineHover").toBool(), false);
        const QString bad = checkPoolColors(s, red, blue);
        QVERIFY2(bad.isEmpty(), qPrintable(bad));
    }
    {
        // A long background transition: a recycled row whose new record has the OTHER status shows the
        // new colour at once instead of fading from the previous record's.
        VirtualScene s(QString::fromLatin1(kPoolCss) + QStringLiteral(" .row { transition: background-color 5000ms linear; }"));
        QVERIFY2(s.mount(true, makePoolRows(10000), kPoolDelegate).isEmpty(), "mount");
        QHash<QQuickItem *, int> before;
        for (QQuickItem *row : s.rows())
            before.insert(row, VirtualScene::indexOf(row) % 2);
        s.setContentY(far + kPitch); // shifts parity for some rows, keeps it for others
        int flipped = 0;
        for (QQuickItem *row : s.rows()) {
            if (!before.contains(row) || before.value(row) == VirtualScene::indexOf(row) % 2)
                continue;
            ++flipped;
            QQuickItem *fill = nullptr;
            for (QQuickItem *k : row->childItems())
                if (QByteArray(k->metaObject()->className()).startsWith("QQuickRectangle"))
                    fill = k;
            QVERIFY2(fill, "no fast-path Rectangle on the row");
            const QColor want(VirtualScene::indexOf(row) % 2 ? blue : red);
            QCOMPARE(fill->property("color").value<QColor>(), want);
        }
        QVERIFY2(flipped > 0, "no recycled row changed status");
    }
}

void QmlCssTests::virtualPoolDrains()
{
    const QString fixed = QString::fromLatin1(kVirtualCss);
    VirtualScene s(fixed);
    const int baseline = s.theme.registeredCount();
    const QVariantList model = makeVirtualRows(1000);
    QVERIFY2(s.mount(true, model).isEmpty(), "mount");
    // One viewport down the window spans [0, 3 viewports); back at the top it spans [0, 2 viewports):
    // the rows of the third viewport leave and nothing enters — they stay parked.
    const auto park = [&]() -> QList<QPointer<QQuickItem>> {
        s.setContentY(kViewH);
        s.setContentY(0);
        flushDeletes(); // only parked rows remain detached (destroyed ones are gone)
        return parkedRows(s.rep);
    };
    const auto allDead = [](const QList<QPointer<QQuickItem>> &ps) {
        for (const auto &p : ps)
            if (p)
                return false;
        return true;
    };
    const auto noDetached = [&]() { return parkedRows(s.rep).isEmpty(); };

    // clear
    QList<QPointer<QQuickItem>> parked = park();
    QVERIFY2(!parked.isEmpty() && s.rep->pooledCount() == parked.size(),
             qPrintable(QStringLiteral("parked %1, pool %2").arg(parked.size()).arg(s.rep->pooledCount())));
    s.setRows({});
    QCOMPARE(s.rep->pooledCount(), 0);
    flushDeletes();
    QVERIFY(allDead(parked));
    QVERIFY(noDetached());

    // eligibility flips to eager (container-relative row heights): the pool drains, every row is in
    // the holder
    s.setRows(model);
    parked = park();
    QVERIFY(!parked.isEmpty());
    s.theme.loadFromString(fixed + QStringLiteral(" .row { height: 5%; }"));
    QCOMPARE(s.rep->materializedCount(), 1000);
    QCOMPARE(s.rep->pooledCount(), 0);
    flushDeletes();
    QVERIFY(noDetached());
    s.theme.loadFromString(fixed);
    QVERIFY(s.rows().size() < 1000);

    // virtualize turned off
    parked = park();
    QVERIFY(!parked.isEmpty());
    s.rep->setVirtualize(false);
    QCOMPARE(s.rep->pooledCount(), 0);
    flushDeletes();
    QVERIFY(noDetached());
    s.rep->setVirtualize(true);
    QVERIFY(QMetaObject::invokeMethod(s.list, "requestRelayout"));
    QVERIFY(s.rows().size() < 1000);

    // delegate change: parked rows of the old delegate never come back
    parked = park();
    QVERIFY(!parked.isEmpty());
    QQmlComponent other(&s.engine);
    other.setData(R"(
        import QtQuick
        import qmlcss
        CssRect { cssClass: ["row"]; cssPrimitive: "div" }
    )", QUrl());
    QVERIFY2(other.isReady(), qPrintable(other.errorString()));
    QQmlComponent *const original = s.rep->delegate();
    s.rep->setDelegate(&other);
    QCOMPARE(s.rep->pooledCount(), 0);
    flushDeletes();
    QVERIFY(allDead(parked));

    // destruction: nothing parked survives the repeater; the census returns to its baseline
    s.rep->setDelegate(original);
    parked = park();
    QVERIFY(!parked.isEmpty());
    // The row contexts the repeater created (its QObject children), parked rows' included.
    QList<QPointer<QQmlContext>> contexts;
    for (QQmlContext *c : s.rep->findChildren<QQmlContext *>(Qt::FindDirectChildrenOnly))
        contexts.append(c);
    QVERIFY(contexts.size() >= parked.size());
    s.root.reset();
    flushDeletes();
    QVERIFY(allDead(parked));
    for (const auto &c : contexts)
        QVERIFY(!c);
    QCOMPARE(s.theme.registeredCount(), baseline);
}

void QmlCssTests::virtualPoolThemeReload()
{
    VirtualScene s(QString::fromLatin1(kPoolCss));
    QVERIFY2(s.mount(true, makePoolRows(2000), kPoolDelegate).isEmpty(), "mount");
    s.setContentY(kViewH); // see virtualPoolDrains: back at the top, the third viewport's rows stay parked
    s.setContentY(0);
    const int parked = s.rep->pooledCount();
    QVERIFY2(parked > 0, "nothing parked");
    QString css = QString::fromLatin1(kPoolCss);
    css.replace(QStringLiteral("#dd2222"), QStringLiteral("#aa8800")).replace(QStringLiteral("#2222dd"), QStringLiteral("#0088aa"));
    const QString a = QStringLiteral("#aa8800"), b = QStringLiteral("#0088aa");
    {
        const PhaseStatsOn stats;
        const int registered = s.theme.registeredCount();
        s.theme.loadFromString(css);
        // Bounded: every registered element (visible or parked) is applied at most once.
        const qint64 applies = bucketCount("css.apply");
        QVERIFY2(applies <= registered, qPrintable(QStringLiteral("%1 applies for %2 registered").arg(applies).arg(registered)));
    }
    QString bad = checkPoolColors(s, a, b);
    QVERIFY2(bad.isEmpty(), qPrintable(bad));
    // The parked rows come back: styled by the NEW sheet, under their real ancestors.
    {
        const PhaseStatsOn stats;
        s.setContentY(kViewH);
        QVERIFY2(bucketCount("repeater.rebind") >= parked, "parked rows were not reused");
    }
    bad = checkPoolColors(s, a, b);
    QVERIFY2(bad.isEmpty(), qPrintable(bad));
}

// ---- Step 5v-3: auto-height rows — measured/estimated extent, key anchoring ----------------------

namespace {

// The 5v-1 column without a row height: each row is as tall as its label, which wraps in the column
// width — one line for a short label, two for a long one.
const char *const kAutoCss = R"(
    .list { display: flex; flex-direction: column; overflow-y: auto; padding: 8px; gap: 2px; }
    .row { display: flex; flex-direction: row; gap: 4px; background-color: #dde6f0; }
    .id { width: 48px; color: #203040; font-size: 12px; }
    .label { flex: 1; color: #102030; font-size: 12px; }
)";

QString shortLabel(int i) { return QStringLiteral("row %1").arg(i); }
QString longLabel(int i) { return QStringLiteral("row %1 has a longer label that wraps to two lines").arg(i); }

template <typename IsLong>
QVariantList makeAutoRows(int n, IsLong isLong)
{
    QVariantList rows;
    rows.reserve(n);
    for (int i = 0; i < n; ++i)
        rows.append(QVariantMap{ { QStringLiteral("id"), i + 1 },
                                 { QStringLiteral("label"), isLong(i) ? longLabel(i) : shortLabel(i) } });
    return rows;
}

qreal screenY(const VirtualScene &s, QQuickItem *row) { return row->mapToItem(s.list, QPointF(0, 0)).y(); }
qreal contentYOf(const VirtualScene &s) { return s.flick->property("contentY").toReal(); }

// The row at the viewport top: the first materialized row whose bottom is below it.
QQuickItem *topRow(const VirtualScene &s)
{
    for (QQuickItem *row : s.rows())
        if (screenY(s, row) + row->height() > 0)
            return row;
    return nullptr;
}

QQuickItem *rowLabelled(const VirtualScene &s, const QString &label)
{
    for (QQuickItem *row : s.rows())
        if (VirtualScene::labelOf(row) == label)
            return row;
    return nullptr;
}

// Every materialized row shows its model entry, in index order, stacked without overlap.
QString checkAutoRows(const VirtualScene &s, const QVariantList &model)
{
    qreal prevBottom = -1e9;
    for (QQuickItem *row : s.rows()) {
        const int i = VirtualScene::indexOf(row);
        if (i < 0 || i >= model.size())
            return QStringLiteral("row with index %1 of %2").arg(i).arg(model.size());
        const QString want = model.at(i).toMap().value(QStringLiteral("label")).toString();
        if (VirtualScene::labelOf(row) != want)
            return QStringLiteral("row %1 shows '%2', model says '%3'").arg(i).arg(VirtualScene::labelOf(row), want);
        if (row->y() < prevBottom + kGap - 0.5)
            return QStringLiteral("row %1 at y %2 overlaps the previous row (bottom %3)").arg(i).arg(row->y()).arg(prevBottom);
        prevBottom = row->y() + row->height();
    }
    return QString();
}

// Top to bottom in viewport steps (the window keeps one viewport on each side: every row is seen).
// Returns the number of steps, or -1 when it did not reach the end.
int traverse(VirtualScene &s, int maxAlive = 0)
{
    qreal cy = 0;
    for (int steps = 1; steps < 100000; ++steps) {
        s.setContentY(cy);
        if (maxAlive && s.rows().size() > maxAlive)
            return -1;
        const qreal at = contentYOf(s);
        if (at >= s.maxContentY() - 0.5)
            return steps;
        cy = std::min(at + kViewH, s.maxContentY());
    }
    return -1;
}

const char *const kAutoTallDelegate = R"(
                        CssRect { cssClass: modelData.id % 10 == 0 ? ["row", "tall"] : ["row"]; cssPrimitive: "div"
                            CssText { cssClass: ["id"]; text: "" + modelData.id }
                            CssText { objectName: "label"; cssClass: ["label"]; text: "" + modelData.label }
                        })";

} // namespace

void QmlCssTests::virtualAutoHeightWindows()
{
    const int n = 5000;
    const QVariantList model = makeAutoRows(n, [](int i) { return i % 2 == 1; });
    VirtualScene eager(QString::fromLatin1(kAutoCss));
    QVERIFY2(eager.mount(false, model).isEmpty(), "eager mount");
    const qreal shortH = eager.rows().at(0)->height(), longH = eager.rows().at(1)->height();
    QVERIFY2(longH > shortH + 4, qPrintable(QStringLiteral("short %1 long %2").arg(shortH).arg(longH)));

    VirtualScene s(QString::fromLatin1(kAutoCss));
    QVERIFY2(s.mount(true, model).isEmpty(), "virtual mount");
    const int bound = int(3 * kViewH / (shortH + kGap)) + 4;
    const int alive = s.rows().size();
    QVERIFY2(alive > 0 && alive <= bound, qPrintable(QStringLiteral("%1 rows alive (bound %2)").arg(alive).arg(bound)));
    QCOMPARE(s.rep->materializedCount(), alive);
    // Before every row was seen the extent is an estimate between all-short and all-long.
    const qreal lo = 2 * kPad + n * shortH + (n - 1) * kGap, hi = 2 * kPad + n * longH + (n - 1) * kGap;
    QVERIFY2(s.contentHeight() >= lo && s.contentHeight() <= hi,
             qPrintable(QStringLiteral("extent %1 outside [%2, %3]").arg(s.contentHeight()).arg(lo).arg(hi)));
    // Seen once, every row is measured: the extent is exactly the eager one.
    QVERIFY2(traverse(s, bound) > 0, "traversal did not reach the end, or the window grew past its bound");
    QVERIFY2(qAbs(s.contentHeight() - eager.contentHeight()) < 0.01,
             qPrintable(QStringLiteral("extent %1, eager %2").arg(s.contentHeight()).arg(eager.contentHeight())));
    const QString bad = checkAutoRows(s, model);
    QVERIFY2(bad.isEmpty(), qPrintable(bad));

    // The ORIGINAL rows bench (examples/bench-rows.css: auto-height rows) is eligible now.
    VirtualScene bench(QStringLiteral(R"(
        .bench { display: flex; flex-direction: column; overflow-y: auto; height: 100vh; }
        .bench-row { display: flex; flex-direction: row; gap: 8px; padding: 2px 8px; }
        .bench-id { width: 60px; }
        .bench-label { flex-grow: 1; }
    )"));
    bench.rootClass = "bench";
    QVERIFY2(bench.mount(true, makeVirtualRows(10000), R"(
                        CssRect { cssClass: ["bench-row"]; cssPrimitive: "div"
                            CssText { cssClass: ["bench-id"]; text: "" + modelData.id }
                            CssText { objectName: "label"; cssClass: ["bench-label"]; text: "" + modelData.label }
                        })").isEmpty(), "bench mount");
    QVERIFY2(bench.rep->materializedCount() > 0 && bench.rep->materializedCount() < 200,
             qPrintable(QString::number(bench.rep->materializedCount())));
}

void QmlCssTests::virtualAnchorNoJump()
{
    const int n = 5000;
    // Short rows up to 2000, long after: the rows measured at the top and in the middle disagree, so
    // every newly measured row moves the estimate — and every estimated row above the viewport.
    const QVariantList model = makeAutoRows(n, [](int i) { return i >= 2000; });
    VirtualScene s(QString::fromLatin1(kAutoCss));
    QVERIFY2(s.mount(true, model).isEmpty(), "mount");
    s.setContentY(s.contentHeight() / 2);
    QVERIFY(s.rows().size() < 200);
    const qreal cy = contentYOf(s);
    QVERIFY(cy > 10 * kViewH);
    // R: the row that will be at the viewport top one viewport up (already in the window, measured).
    QQuickItem *r = nullptr;
    for (QQuickItem *row : s.rows()) {
        if (screenY(s, row) + row->height() > -kViewH) {
            r = row;
            break;
        }
    }
    QVERIFY(r);
    const QString label = VirtualScene::labelOf(r);
    const qreal expected = screenY(s, r) + kViewH;
    const qreal extentBefore = s.contentHeight();
    s.setContentY(cy - kViewH);
    // Rows entered the window and were measured: the estimate (and the extent) changed.
    QVERIFY2(qAbs(s.contentHeight() - extentBefore) > 1, qPrintable(QString::number(s.contentHeight() - extentBefore)));
    QQuickItem *r2 = rowLabelled(s, label);
    QVERIFY(r2);
    const qreal jump = screenY(s, r2) - expected;
    fprintf(stderr, "[5v-3] estimate correction: extent %+.1f px, row at the viewport top moved %+.4f px\n",
            s.contentHeight() - extentBefore, jump);
    QVERIFY2(qAbs(jump) <= 0.5, qPrintable(QStringLiteral("the top row jumped %1 px").arg(jump)));
    const QString bad = checkAutoRows(s, model);
    QVERIFY2(bad.isEmpty(), qPrintable(bad));
}

void QmlCssTests::virtualAutoHeightPixelParity()
{
    const QVariantList model = makeAutoRows(1000, [](int i) { return i % 3 == 0; });
    VirtualScene eager(QString::fromLatin1(kAutoCss));
    VirtualScene virt(QString::fromLatin1(kAutoCss));
    QVERIFY2(eager.mount(false, model).isEmpty(), "eager mount");
    QVERIFY2(virt.mount(true, model).isEmpty(), "virtual mount");
    QVERIFY(virt.rows().size() < 200);
    QVERIFY(traverse(virt) > 0);
    QVERIFY2(qAbs(virt.contentHeight() - eager.contentHeight()) < 0.01,
             qPrintable(QStringLiteral("%1 vs %2").arg(virt.contentHeight()).arg(eager.contentHeight())));
    eager.window.show();
    virt.window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&eager.window));
    QVERIFY(QTest::qWaitForWindowExposed(&virt.window));
    for (const qreal cy : { 0.0, 1234.0, eager.maxContentY() }) {
        eager.setContentY(cy);
        virt.setContentY(cy);
        QCOMPARE(contentYOf(virt), contentYOf(eager));
        const QImage a = eager.grab();
        const QImage b = virt.grab();
        QCOMPARE(a.size(), b.size());
        int diff = 0, painted = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                diff += a.pixel(x, y) != b.pixel(x, y);
                painted += a.pixel(x, y) != QColor(Qt::white).rgba();
            }
        }
        QVERIFY2(diff == 0, qPrintable(QStringLiteral("contentY %1: %2 pixels differ").arg(cy).arg(diff)));
        QVERIFY2(painted > a.width() * a.height() / 2, qPrintable(QString::number(painted)));
    }
}

void QmlCssTests::virtualMeasurementInvalidation()
{
    const PhaseStatsOn stats;
    const int n = 600;
    VirtualScene s(QString::fromLatin1(kAutoCss));
    QVERIFY2(s.mount(true, makeAutoRows(n, [](int) { return false; })).isEmpty(), "mount");
    QVERIFY(traverse(s) > 0);
    s.setContentY(s.contentHeight() / 2);
    QQuickItem *top = topRow(s);
    QVERIFY(top);
    const QString topLabel = VirtualScene::labelOf(top);
    const qreal topY = screenY(s, top);
    const qreal shortH = top->height();
    // A row above the viewport top, inside the window: it grows from one line to two.
    const int u = VirtualScene::indexOf(top) - 3;
    QVERIFY(rowLabelled(s, shortLabel(u)));
    const qreal extentBefore = s.contentHeight();
    Detail::g_phaseStats.buckets.clear();
    QVERIFY(s.call("setLabel", u, longLabel(u)));
    QQuickItem *grown = rowLabelled(s, longLabel(u));
    QVERIFY(grown);
    const qreal delta = grown->height() - shortH;
    QVERIFY2(delta > 4, qPrintable(QString::number(delta)));
    QCOMPARE(bucketCount("repeater.measure"), qint64(1)); // only that row re-measured
    QVERIFY2(qAbs(s.contentHeight() - (extentBefore + delta)) < 0.01,
             qPrintable(QStringLiteral("extent %1, want %2").arg(s.contentHeight()).arg(extentBefore + delta)));
    QVERIFY(rowLabelled(s, topLabel));
    QVERIFY2(qAbs(screenY(s, rowLabelled(s, topLabel)) - topY) <= 0.5,
             qPrintable(QStringLiteral("top row moved %1 px").arg(screenY(s, rowLabelled(s, topLabel)) - topY)));

    // The container width changes: every measurement is invalid, re-measured as rows are seen.
    const QVariantList model = s.currentRows();
    Detail::g_phaseStats.buckets.clear();
    s.list->setWidth(220);
    QVERIFY2(bucketCount("repeater.measure") >= s.rows().size(),
             qPrintable(QStringLiteral("%1 measured, %2 rows").arg(bucketCount("repeater.measure")).arg(s.rows().size())));
    QVERIFY(traverse(s) > 0);
    {
        VirtualScene eager(QString::fromLatin1(kAutoCss));
        QVERIFY2(eager.mount(false, model).isEmpty(), "eager mount");
        eager.list->setWidth(220);
        QVERIFY2(qAbs(s.contentHeight() - eager.contentHeight()) < 0.01,
                 qPrintable(QStringLiteral("width: %1 vs eager %2").arg(s.contentHeight()).arg(eager.contentHeight())));
    }
    // A theme reload (a new font size): likewise.
    const QString bigger = QString::fromLatin1(kAutoCss) + QStringLiteral(" .label { font-size: 16px; }");
    Detail::g_phaseStats.buckets.clear();
    s.theme.loadFromString(bigger);
    QVERIFY(bucketCount("repeater.measure") >= s.rows().size());
    QVERIFY(traverse(s) > 0);
    {
        VirtualScene eager(bigger);
        QVERIFY2(eager.mount(false, model).isEmpty(), "eager mount");
        eager.list->setWidth(220);
        QVERIFY2(qAbs(s.contentHeight() - eager.contentHeight()) < 0.01,
                 qPrintable(QStringLiteral("theme: %1 vs eager %2").arg(s.contentHeight()).arg(eager.contentHeight())));
    }
    const QString bad = checkAutoRows(s, model);
    QVERIFY2(bad.isEmpty(), qPrintable(bad));
}

void QmlCssTests::virtualAnchorRemoved()
{
    const int n = 2000;
    VirtualScene s(QString::fromLatin1(kAutoCss));
    QVERIFY2(s.mount(true, makeAutoRows(n, [](int i) { return i % 3 == 0; })).isEmpty(), "mount");
    s.setContentY(s.contentHeight() / 2);
    QQuickItem *top = topRow(s);
    QVERIFY(top);
    const int idx = VirtualScene::indexOf(top);
    const QString succLabel = VirtualScene::labelOf(s.rows().at(s.rows().indexOf(top) + 1));
    const qreal succY = screenY(s, rowLabelled(s, succLabel));
    QVERIFY(s.call("removeRow", idx));
    QQuickItem *succ = rowLabelled(s, succLabel);
    QVERIFY(succ);
    QCOMPARE(VirtualScene::indexOf(succ), idx);
    QVERIFY2(qAbs(screenY(s, succ) - succY) <= 0.5,
             qPrintable(QStringLiteral("successor moved %1 px").arg(screenY(s, succ) - succY)));
    QString bad = checkAutoRows(s, s.currentRows());
    QVERIFY2(bad.isEmpty(), qPrintable(bad));

    // At the end, the viewport stays at the end (bottom-stick) when a visible row goes.
    s.setContentY(s.maxContentY());
    QVERIFY(s.call("removeRow", VirtualScene::indexOf(topRow(s))));
    QVERIFY2(qAbs(contentYOf(s) - s.maxContentY()) <= 0.5,
             qPrintable(QStringLiteral("contentY %1, end %2").arg(contentYOf(s)).arg(s.maxContentY())));
    bad = checkAutoRows(s, s.currentRows());
    QVERIFY2(bad.isEmpty(), qPrintable(bad));
}

void QmlCssTests::virtualFixedPitchStaysExact()
{
    const PhaseStatsOn stats;
    {
        // Equal fixed-height rows: the exact 5v-1 path, no measurement and no prefix sums at all.
        VirtualScene s(QString::fromLatin1(kVirtualCss));
        const QVariantList model = makeVirtualRows(2000);
        QVERIFY2(s.mount(true, model).isEmpty(), "mount");
        for (const qreal cy : { 0.0, 20000.0, s.maxContentY(), 777.0 }) {
            s.setContentY(cy);
            QCOMPARE(contentYOf(s), cy);
            const QString bad = checkVirtualRows(s, model);
            QVERIFY2(bad.isEmpty(), qPrintable(bad));
        }
        QCOMPARE(bucketCount("repeater.measure"), qint64(0));
        QCOMPARE(bucketCount("repeater.extentRebuild"), qint64(0));
    }
    // Fixed heights that DISAGREE (every 10th row is taller) are no longer an eager fallback: windowed
    // over measured extents, exact once seen.
    const QString css = QString::fromLatin1(kVirtualCss) + QStringLiteral(" .row.tall { height: 40px; }");
    const QVariantList model = makeVirtualRows(2000);
    VirtualScene eager(css);
    QVERIFY2(eager.mount(false, model, kAutoTallDelegate).isEmpty(), "eager mount");
    VirtualScene s(css);
    QVERIFY2(s.mount(true, model, kAutoTallDelegate).isEmpty(), "mount");
    QVERIFY2(s.rows().size() < 200, qPrintable(QString::number(s.rows().size())));
    QVERIFY(traverse(s) > 0);
    QVERIFY2(qAbs(s.contentHeight() - eager.contentHeight()) < 0.01,
             qPrintable(QStringLiteral("%1 vs %2").arg(s.contentHeight()).arg(eager.contentHeight())));
}
