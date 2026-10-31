/*****************************************************************************
 * PaintbrushToolbar.qml : NAI Paintbrush — unified iconic toolbar component
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* ONE toolbar component shared by the vertical tool rail and the horizontal
 * per-tool option bar. SOLID / single responsibility: it SHARES THE CELL
 * DELEGATE, never the layout container — the two orientations are radically
 * different and must not couple.
 *
 * Every item is a plain JS object, one of:
 *   { kind: "tool",  iconSource (TDesign base name) | iconGlyph (QChar),
 *     tip, checkable, checked, onTriggered }
 *   { kind: "icon",  same as "tool" but a plain (non-restored) action }
 *   { kind: "slider", label, value, from, to, stepSize, onChange }
 *
 * Layout per orientation (P1, reviewer #2):
 *   - Qt.Vertical (PaintbrushToolRail)   -> a Flow that wraps items into
 *     `columns` columns (row-major two-column rail). Height grows with the
 *     wrapped rows (fixed width = columns * cell).
 *   - Qt.Horizontal (PaintbrushToolOptionsBar) -> a ONE-ROW Flickable/Row
 *     that does NOT wrap vertically. Fixed one-cell height, `clip:true`, and
 *     horizontal scrolling when controls exceed the available width. No content
 *     ever draws outside its host rectangle, and no slider is clipped — it can
 *     be scrolled into view instead. Palette stays a completely separate 64dp
 *     colour surface (this component never renders colour swatches).
 *
 * The owner rebuilds `items` when `checked`/`value` must change (plain JS
 * cells cannot bind reactively), then triggers `triggered(action,value)` for
 * declarative dispatch.
 *
 * Cells: icon cells are perfect squares (1:1); slider cells are a wider
 * labelled + value track. TDesign vector icons are pre-coloured white vs
 * orange and swapped by state, with the orange wash on active/hover. Icon-less
 * cells fall back to a QChar glyph (VLCIcons) tinted orange. */
Item {
    id: root

    /* Qt.Horizontal or Qt.Vertical */
    property int orientation: Qt.Horizontal

    /* list of item objects (see header doc) */
    property var items: []

    /* Declarative action dispatch: instead of storing JS closures in the item
     * objects (fragile QV4 callbacks), a cell may carry:
     *   action: "powderTool", value: 2     or     action: "powderPour"
     * The owning bar connects to `triggered` and performs the observable
     * dispatch. Kept alongside the legacy onTriggered path for compat. */
    signal triggered(string action, var value)

    /* square cell size used for icon buttons (sliders size their own width) */
    property real cell: VLCStyle.dp(40, VLCStyle.scale)

    /* number of columns a VERTICAL toolbar lays out in (row-major) */
    property int columns: 2

    /* spacing between cells */
    property real spacing: VLCStyle.margin_xxxsmall

    /* per-cell width: icons/actions are one cell, sliders are five cells,
     * spacers are a fraction (so no item is silently clipped — P1 layout) */
    function _itemW(d) {
        if (d) {
            if (d.kind === "spacer") return root.cell * 0.4
            if (d.kind === "slider" || d.kind === "option")
                return (root.orientation === Qt.Horizontal ? root.cell * 5 : root.cell)
        }
        return root.cell
    }

    /* horizontal one-row content: the sum of every item's own width */
    readonly property bool _isHoriz: orientation === Qt.Horizontal
    readonly property real _allWidth: {
        let w = 0
        for (const d of items) if (d && d.visible !== false)
            w += _itemW(d) + (w > 0 ? spacing : 0)
        return Math.max(cell, w)
    }

    /* vertical wrapped rows (two-column rail) */
    readonly property real _rows: Math.ceil(items.length / Math.max(1, columns))
    readonly property real _hCellW: columns * cell + (columns - 1) * spacing

    /* ONE-CELL content height. The horizontal bar NEVER wraps vertically; the
     * host bar is exactly one row tall and the Flickable scrolls horizontally.
     * contentHeight is therefore just the single cell row, decoupled from any
     * wrapped-row assumption (this decoupling is the P1 toolbar fix). */
    readonly property real contentHeight: _isHoriz ? (cell + spacing) : _rows * (cell + spacing)

    implicitWidth: _isHoriz ? _allWidth : _hCellW
    implicitHeight: contentHeight

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    readonly property string tdesignPath: "qrc:///icons/tdesign/"

    /* The REUSED cell delegate: identical visual/press/tooltip logic in both
     * orientations. Instantiated by the vertical Flow and the horizontal Row. */
    component ToolCellDelegate : Item {
        id: cell

        required property var modelData
        readonly property bool isSlider: modelData
            && (modelData.kind === "slider" || modelData.kind === "option")
        readonly property bool isSpacer: modelData
            && modelData.kind === "spacer"

        visible: modelData ? (modelData.visible !== false) : true
        width: isSpacer
               ? (root.cell * 0.4)
               : (isSlider
                  ? (root.cell * 5) : root.cell)
        height: root.cell

        /* ---------- iconic action cell ------------------------- */
        Rectangle {
            id: cellBg
            anchors.fill: parent
            radius: VLCStyle.margin_xsmall
            color: cellBtnChecked
                   ? Qt.rgba(1.0, 0.533, 0.0, 0.20)
                   : (cellHover ? Qt.rgba(1.0, 1.0, 1.0, 0.10) : "transparent")
            border.width: cellHover && !cellBtnChecked ? 1 : 0
            border.color: Qt.rgba(1.0, 1.0, 1.0, 0.25)
            visible: !cell.isSlider && !cell.isSpacer

            property bool cellBtnChecked: !!(cell.modelData && cell.modelData.checked)
            property bool cellHover: false

            Image {
                width: root.cell * 0.62
                height: root.cell * 0.62
                anchors.centerIn: parent
                fillMode: Image.PreserveAspectFit
                smooth: true
                visible: !!(cell.modelData && cell.modelData.iconSource)
                source: (cell.modelData && cell.modelData.iconSource)
                        ? root.tdesignPath
                          + (cellBg.cellBtnChecked || cellBg.cellHover
                             ? cell.modelData.iconSource + "_active.svg"
                             : cell.modelData.iconSource + ".svg")
                        : ""
            }
            Widgets.LabelExt {
                text: (modelData && modelData.iconGlyph) || ""
                anchors.centerIn: parent
                font.pixelSize: VLCStyle.fontSize_xxlarge
                color: cellBg.cellBtnChecked || cellBg.cellHover
                       ? "#FF8800" : theme.fg.primary
                visible: !(modelData && modelData.iconSource)
            }

            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                onEntered: cellBg.cellHover = true
                onExited: cellBg.cellHover = false
                onClicked: {
                const d = cell.modelData
                if (d && d.action)
                    root.triggered(d.action, d.value)
                else if (d && d.onTriggered)
                    d.onTriggered()
            }
            }

            ToolTip {
                visible: cellBg.cellHover
                text: cell.modelData ? (cell.modelData.tip || "") : ""
                delay: 600
            }
        }

        /* ---------- labelled slider cell (horizontal only) ----- */
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: VLCStyle.margin_xsmall
            anchors.rightMargin: VLCStyle.margin_xsmall
            spacing: VLCStyle.margin_xsmall
            visible: cell.isSlider
            enabled: visible

            Widgets.LabelExt {
                text: (cell.isSlider && cell.modelData) ? (cell.modelData.label ?? "") : ""
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_small
            }
            Slider {
                id: sld
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                from: (cell.isSlider && cell.modelData) ? (cell.modelData.from ?? 0) : 0
                to: (cell.isSlider && cell.modelData) ? (cell.modelData.to ?? 1) : 1
                stepSize: (cell.isSlider && cell.modelData) ? (cell.modelData.stepSize ?? 0.05) : 0.05
                value: (cell.isSlider && cell.modelData) ? (cell.modelData.value ?? 0) : 0
                onMoved: if (cell.isSlider && cell.modelData && cell.modelData.onChange)
                             cell.modelData.onChange(value)
            }
            Widgets.LabelExt {
                text: (cell.isSlider && cell.modelData) ? (Math.round(sld.value)) : ""
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_small
            }
        }
    }

    /* Vertical rail: bounded-width Flow wraps into `columns` columns. */
    Flow {
        id: strip
        x: 0
        y: 0
        visible: !root._isHoriz
        width: root._hCellW
        height: root.contentHeight
        flow: Flow.TopToBottom
        spacing: root.spacing
        clip: false

        Repeater {
            model: root.items
            delegate: ToolCellDelegate {}
        }
    }

    /* Horizontal options: ONE-ROW Flickable with a Row of cells. clip:true so
     * no control, tooltip body or child may draw outside this bar's own
     * rectangle; horizontal contentWidth scrolls when wider than the host. */
    Flickable {
        id: hstrip
        x: 0
        y: 0
        visible: root._isHoriz
        width: root.width
        height: root.cell
        clip: true
        contentWidth: hrow.width
        contentHeight: root.cell
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
        interactive: true

        Row {
            id: hrow
            y: 0
            height: root.cell
            spacing: root.spacing

            Repeater {
                model: root.items
                delegate: ToolCellDelegate {}
            }
        }
    }
}