#include "csstheme.h"
#include "csstheme_p.h"

#include "qmlcss/cssbox.h"
#include "qmlcss/cssfill.h"
#include "qmlcss/cssrect.h"
#include "qmlcss/cssrepeater.h"
#include "qmlcss/csstext.h"

#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>

#include <cstdio>

namespace QmlCss {

namespace {

// One pass's tallies (Task 1 of the 5a plan). A box's layout style pointers are grouped by its
// FIRST css class — a group with more than one distinct pointer is exactly what step 5b (Task 2)
// fixes (identical classes sharing one parsed CssLayoutStyle instead of one per box).
struct CensusTally {
    int cssRect = 0;
    int cssFill = 0;
    int cssTextLean = 0;
    int cssTextComposed = 0;
    int composedLabels = 0;
    int paintShells = 0;
    int hoverHandlers = 0;
    int repeaterRows = 0;
    QHash<QString, QSet<const void *>> stylesByClass;
};

void tallyBox(CssBox *box, CensusTally &c)
{
    const QStringList &classes = box->classList();
    const QString key = classes.isEmpty() ? QStringLiteral("(none)") : classes.first();
    c.stylesByClass[key].insert(box->layoutStyleRef().data());
}

// Walk the whole item tree once (outside any timed section): classify every item by its real
// type (CssRect/CssFill/CssText) or, for the parts those compose via the Qt type-system, by its
// metaobject class name — the paint shells and the HoverHandler never get a C++ type of their own.
void walk(QQuickItem *item, CensusTally &c)
{
    if (!item)
        return;
    if (auto *rect = qobject_cast<CssRect *>(item)) {
        ++c.cssRect;
        tallyBox(rect, c);
    } else if (auto *fill = qobject_cast<CssFill *>(item)) {
        ++c.cssFill;
        tallyBox(fill, c);
    } else if (auto *text = qobject_cast<CssText *>(item)) {
        // A lean CssText draws itself (ItemHasContents) and composes no QQuickText child; a
        // composed one forwards onto a REAL QtQuick Text (see CssText::composeLabel).
        bool composed = false;
        for (QQuickItem *k : item->childItems()) {
            if (QByteArray(k->metaObject()->className()).contains("QQuickText")) {
                composed = true;
                ++c.composedLabels;
                break;
            }
        }
        if (composed)
            ++c.cssTextComposed;
        else
            ++c.cssTextLean;
        tallyBox(text, c);
    } else if (auto *box = qobject_cast<CssBox *>(item)) {
        // CssHr/CssImage/CssItem/… — still a layout style holder, counted in the per-class
        // breakdown even though they are not one of the three types tallied above.
        tallyBox(box, c);
    } else if (auto *rep = qobject_cast<CssRepeater *>(item)) {
        c.repeaterRows += rep->rowCount();
    } else {
        const QByteArray cls(item->metaObject()->className());
        if (cls.contains("QQuickShape") || cls == "QQuickRectangle")
            ++c.paintShells; // CssRect/CssFill's composed fill/border Shape, or the fast-rect path
        else if (cls.contains("HoverHandler"))
            ++c.hoverHandlers;
    }
    const auto kids = item->childItems();
    for (QQuickItem *k : kids)
        walk(k, c);
}

} // namespace

void CssTheme::dumpCensus() const
{
    // Every registered box's window, deduplicated — the walk below covers the whole scene once per
    // window regardless of how many of its elements are registered.
    QSet<QQuickWindow *> windows;
    for (const QPointer<QObject> &b : m_bindings) {
        if (auto *item = qobject_cast<QQuickItem *>(b.data())) {
            if (QQuickWindow *w = item->window())
                windows.insert(w);
        }
    }

    CensusTally c;
    for (QQuickWindow *w : windows)
        walk(w->contentItem(), c);

    fprintf(stderr, "[census] CssRect=%d CssFill=%d CssText=%d (lean=%d composed=%d) composedLabels=%d\n",
            c.cssRect, c.cssFill, c.cssTextLean + c.cssTextComposed, c.cssTextLean, c.cssTextComposed,
            c.composedLabels);
    fprintf(stderr, "[census] paintShells=%d hoverHandlers=%d repeaterRows=%d contextNodes=%d\n",
            c.paintShells, c.hoverHandlers, c.repeaterRows, Detail::g_liveContextNodes);
    QStringList classKeys = c.stylesByClass.keys();
    classKeys.sort();
    for (const QString &key : classKeys) {
        fprintf(stderr, "[census-style] .%s -> %d distinct CssLayoutStyle\n", qPrintable(key),
                c.stylesByClass.value(key).size());
    }
}

} // namespace QmlCss
