/*****************************************************************************
 * PaintbrushToolRail.qml : NAI Paintbrush — iconic tool rail
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Controls.Basic as Basic

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* The Paintbrush tool rail (A36): a 2-column grid of large, SQUARE iconic
 * targets, tooltips secondary. Replaces the 15 text-only 34dp buttons.
 * The tool -> canvas.tool integer mapping is unchanged (pb_common.hpp
 * ToolId). The three zoom controls stay pinned at the bottom.
 *
 * Icon style: TDesign (Tencent) MIT SVG glyphs, pre-coloured into two
 * variants (idle = white #fff, active/hover = VLC orange #FF8800) and swapped
 * by state — no runtime tinting shader required. Cells are perfect squares so
 * the icons keep a 1:1 aspect ratio. */
Item {
    id: root

    required property PaintbrushCanvas canvas

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    readonly property string tdesignPath: "qrc:///icons/tdesign/"

    /* Square cell geometry: derive from the rail width so columns never
     * overflow and cells stay perfectly square (1:1 aspect). */
    readonly property real _spacing: VLCStyle.margin_xxxsmall
    readonly property real _margin: VLCStyle.margin_xxxsmall
    readonly property real cellSize: (width - 2 * _margin - _spacing) / 2

    Rectangle {
        anchors.fill: parent
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: _margin
        spacing: _spacing

        /* Tool grid: square cells, centered in the available space. */
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Grid {
                id: toolGrid
                anchors.centerIn: parent
                columns: 2
                columnSpacing: _spacing
                rowSpacing: _spacing

                Repeater {
                    model: [
                        /* selection tools */
                        { toolId: 1, iconBase: "rect_select", tip: qsTr("Rect Select"), sparkle: false },
                        { toolId: 2, iconBase: "freeform_select", tip: qsTr("Freeform Select"), sparkle: false },
                        { toolId: 3, iconBase: "object_select", tip: qsTr("Object Select (AI)"), sparkle: true },
                        { toolId: 4, iconBase: "pencil", tip: qsTr("Pencil"), sparkle: false },
                        /* paint tools */
                        { toolId: 5, iconBase: "brush", tip: qsTr("Brush"), sparkle: false },
                        { toolId: 6, iconBase: "eraser", tip: qsTr("Eraser"), sparkle: false },
                        { toolId: 7, iconBase: "fill", tip: qsTr("Fill"), sparkle: false },
                        { toolId: 8, iconBase: "eyedropper", tip: qsTr("Eyedropper"), sparkle: false },
                        { toolId: 9, iconBase: "spray", tip: qsTr("Spray"), sparkle: false },
                        /* Paintbrush 2.0 art materials (pb_common.hpp ToolId 16/17) */
                        { toolId: 16, iconBase: "bristle", tip: qsTr("Bristle Paint"), sparkle: false },
                        { toolId: 17, iconBase: "glitter", tip: qsTr("Glitter"), sparkle: false },
                        /* shape tools */
                        { toolId: 10, iconBase: "line", tip: qsTr("Line"), sparkle: false },
                        { toolId: 11, iconBase: "rect", tip: qsTr("Rectangle"), sparkle: false },
                        { toolId: 12, iconBase: "rounded_rect", tip: qsTr("Rounded Rectangle"), sparkle: false },
                        { toolId: 13, iconBase: "ellipse", tip: qsTr("Ellipse"), sparkle: false },
                        { toolId: 14, iconBase: "text", tip: qsTr("Text"), sparkle: false },
                        /* navigation */
                        { toolId: 15, iconBase: "move", tip: qsTr("Move canvas"), sparkle: false }
                    ]

                    delegate: Basic.Button {
                        id: toolBtn

                        required property int toolId
                        required property string iconBase
                        required property string tip
                        required property bool sparkle

                        /* Fixed square cell (1:1) — no vertical stretching. */
                        width: root.cellSize
                        height: root.cellSize

                        checkable: true
                        autoExclusive: true
                        checked: canvas.tool === toolId
                        onClicked: canvas.tool = toolId

                        /* TDesign vector icon: white idle, VLC orange active/hover.
                         * The two SVGs are pre-coloured (idle = #fff, active = #FF8800)
                         * so no runtime tinting shader is needed. */
                        contentItem: Image {
                            source: root.tdesignPath + (toolBtn.checked || toolBtn.hovered
                                                        ? toolBtn.iconBase + "_active.svg"
                                                        : toolBtn.iconBase + ".svg")
                            sourceSize: Qt.size(root.cellSize * 2, root.cellSize * 2)
                            fillMode: Image.PreserveAspectFit
                            smooth: true
                            anchors.centerIn: toolBtn
                            width: root.cellSize * 0.66
                            height: root.cellSize * 0.66
                        }

                        background: Rectangle {
                            color: toolBtn.checked
                                   ? Qt.rgba(1.0, 0.533, 0.0, 0.18)   /* orange wash */
                                   : (toolBtn.hovered
                                      ? Qt.rgba(1.0, 1.0, 1.0, 0.10)
                                      : "transparent")
                            radius: VLCStyle.margin_xsmall
                            border.width: toolBtn.hovered && !toolBtn.checked ? 1 : 0
                            border.color: Qt.rgba(1.0, 1.0, 1.0, 0.25)
                        }

                        /* sparkle indicator when the AI backend is available */
                        Rectangle {
                            visible: sparkle && PaintbrushCtx.segmentationAvailable
                            width: 6
                            height: 6
                            radius: 3
                            color: "#FF8800"
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: 2
                        }

                        ToolTip.visible: toolBtn.hovered
                        ToolTip.text: toolBtn.tip
                        ToolTip.delay: 600
                    }
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
}