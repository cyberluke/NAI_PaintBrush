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
 * per-tool option bar. SOLID / single responsibility: it only lays out a strip
 * of *items* along an orientation. Every item is a plain JS object, one of:
 *
 *   { kind: "tool",  iconSource (TDesign base name) | iconGlyph (QChar),
 *     tip, checkable, checked, onTriggered }
 *   { kind: "icon",  same as "tool" but a plain (non-restored) action }
 *   { kind: "slider", label, value, from, to, stepSize, onChange }
 *
 * Vertical toolbars lay out in `columns` columns (row-major Grid gives the
 * classic two-column rail look); horizontal toolbars lay left-to-right (wrapping
 * when narrow so option sliders are never clipped). The SAME delegate draws
 * every cell in both orientations — only the Flow direction differs — so
 * hover / active / icon state logic is identical (open for extension).
 *
 * The two regions are two instances of this component:
 *   - PaintbrushToolRail         -> orientation Qt.Vertical, columns 2
 *   - PaintbrushToolOptionsBar   -> orientation Qt.Horizontal
 * The USING file rebuilds `items` when an item's `checked` / `value` must
 * change with the canvas state — the Repeater recreates the cells from the
 * fresh list (plain JS cells cannot bind reactively, so the owner refreshes).
 *
 * Cells are perfect squares (1:1); TDesign vector icons are pre-coloured
 * white vs orange and swapped by state, with the orange wash on active/hover.
 * Icon-less cells fall back to a QChar glyph (VLCIcons) tinted orange. */
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

    /* horizontal wrappable Flow: how many rows are needed given the available
     * `width` (wraps sliders instead of clipping them) */
    function _wrappedRows(width) {
        if (root.orientation !== Qt.Horizontal) return _rows
        let rows = 1, w = 0
        for (const d of root.items) {
            if (!d || d.visible === false) continue
            const iw = _itemW(d)
            if (w + iw + (w > 0 ? root.spacing : 0) > Math.max(1, width)) { ++rows; w = iw }
            else w += iw + (w > 0 ? root.spacing : 0)
        }
        return rows
    }

    /* vertical content height = rows * (cell + spacing) so the Flow wraps
     * into `columns` columns */
    readonly property real _rows: Math.ceil(items.length / Math.max(1, columns))
    readonly property bool _isHoriz: orientation === Qt.Horizontal
    readonly property real _layoutRows: _isHoriz && width > 0
                                        ? _wrappedRows(width) : _rows
    readonly property real _fluidHeight: _layoutRows * (cell + spacing)
    readonly property real _allWidth: {
        /* sum of every item's own width for the ideal horizontal strip */
        let w = 0
        for (const d of items) if (d) w += _itemW(d) + (w > 0 ? spacing : 0)
        return Math.max(cell, w)
    }

    /* Exposed for a floating host bar to size itself to the ACTUAL wrapped
     * content (including sliders) instead of an assumption — no clipping */
    readonly property real contentHeight: _fluidHeight

    implicitWidth: _isHoriz ? _allWidth
                            : columns * cell + (columns - 1) * spacing
    implicitHeight: _isHoriz ? _fluidHeight : _fluidHeight

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    readonly property string tdesignPath: "qrc:///icons/tdesign/"

    Flow {
        id: strip
        x: 0
        y: 0
        /* vertical = bounded height so flow wraps into columns (row-major); a
         * Grid would give the same result but Flow keeps ONE code path */
        width: root.orientation === Qt.Horizontal
               ? root.width : root.columns * root.cell + (root.columns - 1) * root.spacing
        height: root._fluidHeight
        flow: root.orientation === Qt.Horizontal ? Flow.LeftToRight : Flow.TopToBottom
        spacing: root.spacing
        clip: false

        Repeater {
            model: root.items

            delegate: Item {
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
                          ? (root.orientation === Qt.Horizontal
                             ? (root.cell * 5) : root.cell)
                          : root.cell)
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
        }
    }
}