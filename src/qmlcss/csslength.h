#pragma once

#include <QSharedData>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace QmlCss {

// JS parseFloat: the leading numeric prefix (sign, digits, '.', exponent) of `s`; NaN if none.
double cssParseFloat(const QString &s);
// `aspect-ratio` value ("2", "16/9") as a positive ratio; NaN when absent/invalid.
double cssAspectRatio(const QString &value);

// One CSS length, parsed ONCE from its (trimmed) style string and evaluated at layout time: a
// percentage needs the container's axis, a viewport unit the CURRENT viewport, so neither is
// resolved here. Absent (no value) and Invalid (auto, fit-content, … — anything the string path
// evaluated to NaN for every input) both evaluate to NaN but stay distinct. Expr (calc / min / max /
// clamp) keeps its source string — `value` indexes CssLayoutStyle::exprs — and is evaluated by the
// engine's expression evaluator, exactly as before.
struct CssLength {
    enum Kind : quint8 { Absent, Number, Percent, Vw, Vh, Vmin, Vmax, Expr, Invalid };
    double value = 0;
    Kind kind = Absent;
};

// Everything the flex path, place()/clampSize()/placeAbsolute() and layout()'s child scan read from a
// box's resolved style, parsed once per resolved style map (CssBox::layoutStyle). Each field is the
// literal translation of a string read the layout did on every pass. Immutable after parsing and a
// pure function of the map: per-instance geometry/animation never lives here (two boxes sharing one
// map share one parsed style; `50%` still resolves against each box's own container at layout).
struct CssLayoutStyle : QSharedData {
    // justify-content (default flex-start); every unknown value falls into Start: no distribution.
    enum class Justify : quint8 { Start, Center, End, SpaceBetween, SpaceAround, SpaceEvenly };
    // align-items / align-self: stretch, center, flex-end|end, anything else (flex-start, bogus).
    // Inherit = no align-self (take the container's align-items).
    enum class Align : quint8 { Stretch, Center, End, Other, Inherit };
    // align-content (default stretch); unknown values pack at the start.
    enum class AlignContent : quint8 { Stretch, Center, End, SpaceBetween, SpaceAround, Start };

    // display / position (as the child scan and the container read them)
    bool displayNone = false;   // display == "none"
    bool displayGrid = false;   // display (default "block") == "grid"
    bool displayFlex = false;   // display (default "block") contains "flex"
    bool positionAbsolute = false;
    // container flex properties
    bool directionRow = true;   // flex-direction (default "row") starts with "row"
    bool wrap = false;          // flex-wrap starts with "wrap"
    Justify justify = Justify::Start;
    Align alignItems = Align::Stretch;
    AlignContent alignContent = AlignContent::Stretch;
    CssLength gap;              // gap -> row-gap -> column-gap; Absent evaluates to 0
    // item properties
    Align alignSelf = Align::Inherit;
    bool borderBox = false;     // box-sizing == "border-box"
    bool insetBox = false;      // `inset` present: offset[] are its box sides (avail 0, NaN -> 0)
    int order = 0;
    double grow = 0;            // flex-grow, else the `flex` shorthand's leading number; NaN -> 0
    double shrink = 0;          // flex-shrink / `flex` 2nd token, else (grow > 0 ? 1 : 0); NaN -> 0
    double aspect = 0;          // cssAspectRatio (NaN when absent)
    CssLength width, height, minWidth, maxWidth, minHeight, maxHeight, flexBasis;
    // Box sides [top, right, bottom, left]. *Box = the shorthand's token for that side (NaN -> 0),
    // *Side = the longhand override (applied when it evaluates to a number). Border: borderWidthBox =
    // `border-width`, borderAll = the `border` shorthand's leading token, borderSide = the
    // `border-<side>-width` longhand, else `border-<side>`'s leading token.
    CssLength marginBox[4], marginSide[4];
    CssLength paddingBox[4], paddingSide[4];
    CssLength borderWidthBox[4], borderAll, borderSide[4];
    CssLength offset[4];        // top, right, bottom, left — or the inset box when insetBox
    QStringList exprs;          // source strings of the Expr lengths

    static CssLayoutStyle parse(const QVariantMap &style);
};

// The parsed style for `style`, shared with the previous call when that call saw the SAME shared map
// data (identical rows resolve to one implicitly shared map: they get one parsed style, not N).
QExplicitlySharedDataPointer<CssLayoutStyle> cssSharedLayoutStyle(const QVariantMap &style);

} // namespace QmlCss
