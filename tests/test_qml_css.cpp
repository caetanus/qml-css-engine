#include "test_qml_css.h"

#include "qmlcss/csstheme.h"

#include <QColor>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>

void QmlCssTests::cssRectLoadsAndRestyles()
    {
        CssTheme theme;
        theme.loadFromString(QStringLiteral("#box { background-color: #112233; border-radius: 7px; }"));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import "qrc:/qmlcss" as Css

            Css.CssRect {
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
        CssTheme theme;
        theme.loadFromString(QStringLiteral("#label { color: rgba(10, 20, 30, 0.5); font-size: 18px; }"));

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("cssTheme"), &theme);

        QQmlComponent component(&engine);
        component.setData(R"(
            import QtQuick
            import "qrc:/qmlcss" as Css

            Css.CssText {
                cssId: "label"
                text: "Label"
            }
        )", QUrl());

        QScopedPointer<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));

        QCOMPARE(object->property("style").toMap().value(QStringLiteral("font-size")).toString(),
                 QStringLiteral("18px"));
        const QColor color = object->property("color").value<QColor>();
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
        component.setData(R"(
            import QtQuick
            import "qrc:/qmlcss" as Css

            Rectangle {
                width: 24
                height: 24

                Css.CssItem {
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

QTEST_MAIN(QmlCssTests)
