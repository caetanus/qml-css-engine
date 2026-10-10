#pragma once

#include "qmlcss/csstheme.h" // CssAncestorInfo

#include <QExplicitlySharedDataPointer>
#include <QHash>
#include <QPointer>
#include <QSharedData>
#include <QVariantMap>

namespace QmlCss {

// The selector context of one element: what it contributes as an ANCESTOR (id, classes+cssState, primitive)
// plus its nearest CSS ancestor's node. Immutable once interned; shared by every element with the same
// identity under the same ancestry (2000 identical rows -> one node). Also memoises the element's OWN resolves
// (keyed by the own-only inputs: own classes incl. hover/focus, ordered alternate ids, part mode).
//
// Lifetime: live boxes own their nodes (and a node owns its parent); the theme's intern table is weak. The
// parent's ADDRESS is part of the intern key — safe only because of that strong parent ref.
struct CssContextNode : QSharedData {
    QExplicitlySharedDataPointer<CssContextNode> parent;
    CssAncestorInfo self;
    quint64 generation = 0;
    mutable QHash<QString, QVariantMap> ownResolves;
    // A QPointer: a node may outlive its theme (a box destroyed after it) and must not touch freed memory.
    QPointer<const CssTheme> owner;
    QString internKey;
    ~CssContextNode();
};

using CssContextRef = QExplicitlySharedDataPointer<CssContextNode>;

} // namespace QmlCss
