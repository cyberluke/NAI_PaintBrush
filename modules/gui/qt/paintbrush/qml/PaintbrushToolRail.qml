/*****************************************************************************
 * PaintbrushToolRail.qml : NAI Paintbrush — iconic tool rail
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* The Paintbrush tool rail (A36): a single-column strip of SQUARE iconic
 * targets rendered by the unified PaintbrushToolbar component (orientation =
 * Vertical), so the vertical rail and the horizontal per-tool option bar share
 * the exact same cell / hover / active logic (SOLID: one toolbar component,
 * two orientations). Tooltips are secondary; the three zoom controls stay
 * pinned at the bottom.
 *
 * Icon style: TDesign (Tencent) MIT SVG glyphs, pre-coloured into two
 * variants (idle = white #fff, active/hover = VLC orange #FF8800) and swapped
 * by state. Items are plain JS cells, rebuilt when canvas.tool changes so the
 * active cell always reflects the real tool. */
Item {
    id: root

    required property PaintbrushCanvas canvas

    ColorContext {
        palette: VLCStyle.palette
        id: theme
        colorSet: ColorContext.View
    }

    readonly property var toolCells: [
        { toolId: 1,  iconSource: "rect_select",     tip: qsTr("Rect Select") },
        { toolId: 2,  iconSource: "freeform_select", tip: qsTr("Freeform Select") },
        { toolId: 3,  iconSource: "scan",            tip: qsTr("Object Select (AI)") },
        { toolId: 4,  iconSource: "pencil",          tip: qsTr("Pencil") },
        { toolId: 5,  iconSource: "brush",           tip: qsTr("Brush") },
        { toolId: 6,  iconSource: "eraser",          tip: qsTr("Eraser") },
        { toolId: 7,  iconSource: "fill",            tip: qsTr("Fill") },
        { toolId: 8,  iconSource: "eyedropper",      tip: qsTr("Eyedropper") },
        { toolId: 9,  iconSource: "spray",           tip: qsTr("Spray") },
        { toolId: 16, iconSource: "bristle",         tip: qsTr("Bristle Paint") },
        { toolId: 17, iconSource: "glitter",         tip: qsTr("Glitter") },
        { toolId: 18, iconSource: "glitter",         tip: qsTr("Glitter Powder") },
        { toolId: 10, iconSource: "line",            tip: qsTr("Line") },
        { toolId: 11, iconSource: "rect",            tip: qsTr("Rectangle") },
        { toolId: 12, iconSource: "rounded_rect",    tip: qsTr("Rounded Rectangle") },
        { toolId: 13, iconSource: "ellipse",         tip: qsTr("Ellipse") },
        { toolId: 14, iconSource: "text",            tip: qsTr("Text") },
        { toolId: 15, iconSource: "move",            tip: qsTr("Move layer (drag object) / pan (drag empty)") },
        { toolId: 20, iconSource: "image_search",    tip: qsTr("Ask") }
    ]

    function rebuildItems()
    {
        const out = []
        /* NOTE: avoid Object.assign / object spread — Qt's QML JS engine
         * (Qv4) does not implement them, and a thrown ReferenceError here
         * would silently leave the tool rail empty */
        for (let i = 0; i < toolCells.length; ++i)
        {
            const src = toolCells[i]
            const cell = {
                kind: "tool",
                checkable: true,
                toolId: src.toolId,
                iconSource: src.iconSource,
                tip: src.tip,
                checked: (canvas.tool === src.toolId),
                onTriggered: () => { canvas.tool = src.toolId }
            }
            out.push(cell)
        }
        toolBar.items = out
    }

    /* Keep the active cell visible: the AI can select a tool that lives below
     * the fold, and a highlight the user cannot see is indistinguishable from
     * "the assistant did nothing". */
    function revealActiveTool()
    {
        if (!toolBar || !toolBar.items || toolBar.items.length === 0)
            return
        let index = -1
        for (let i = 0; i < toolBar.items.length; ++i) {
            if (toolBar.items[i].toolId === canvas.tool) {
                index = i
                break
            }
        }
        if (index < 0)
            return
        const cols = toolBar.columns > 0 ? toolBar.columns : 2
        const row = Math.floor(index / cols)
        const cell = toolBar.cell
        const y = row * (cell + toolBar.spacing)
        const content = scroll.contentItem
        if (!content)
            return
        const maxY = Math.max(0, content.contentHeight - scroll.height)
        content.contentY = Math.max(0, Math.min(maxY,
                                                 y - (scroll.height - cell) / 2))
    }
    Component.onCompleted: Qt.callLater(revealActiveTool)

    Rectangle {
        anchors.fill: parent
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: VLCStyle.margin_xxxsmall
        spacing: VLCStyle.margin_xxxsmall

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ScrollView {
                id: scroll
                anchors.fill: parent
                clip: true
                ScrollBar.vertical.policy: ScrollBar.AsNeeded
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                PaintbrushToolbar {
                    id: toolBar
                    orientation: Qt.Vertical
                    /* the ScrollView has no `availableWidth`; size content to
                     * the viewport so the two-column Grid lays out (the rail
                     * would otherwise collapse to 0 width and hide every cell) */
                    width: scroll.width
                    cell: Math.min(VLCStyle.dp(40, VLCStyle.scale),
                                   (root.width - 2 * VLCStyle.margin_xxxsmall) * 0.55)
                    /* explicit height for the ScrollView content so the
                     * two-column Grid lays out its rows reliably */
                    height: (Math.ceil(items.length / columns)) * cell
                            + (Math.ceil(items.length / columns) - 1) * spacing
                    Component.onCompleted: rebuildItems()
                }
            }
        }

        /* zoom controls, pinned at the bottom */
        Widgets.IconToolButton {
            Layout.fillWidth: true
            Layout.preferredHeight: VLCStyle.dp(30, VLCStyle.scale)
            text: VLCIcons.add
            description: qsTr("Zoom in")
            onClicked: canvas.zoomIn()
        }
        Widgets.IconToolButton {
            Layout.fillWidth: true
            Layout.preferredHeight: VLCStyle.dp(30, VLCStyle.scale)
            text: VLCIcons.remove
            description: qsTr("Zoom out")
            onClicked: canvas.zoomOut()
        }
        Widgets.IconToolButton {
            Layout.fillWidth: true
            Layout.preferredHeight: VLCStyle.dp(30, VLCStyle.scale)
            text: VLCIcons.fullscreen
            description: qsTr("Fit to view")
            onClicked: canvas.fitToView()
        }
    }

    Connections {
        target: canvas
        function onToolChanged(t) {
            rebuildItems()
            Qt.callLater(revealActiveTool)
        }
    }
}