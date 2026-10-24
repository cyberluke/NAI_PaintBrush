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

/* The Paintbrush tool rail (A36): a 2-column grid of large (~40dp) iconic
 * targets, tooltips secondary. Replaces the 15 text-only 34dp buttons.
 * The tool -> canvas.tool integer mapping is unchanged (pb_common.hpp
 * ToolId). The three zoom controls stay at the bottom. */
Item {
    id: root

    required property PaintbrushCanvas canvas

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

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

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 2
            columnSpacing: VLCStyle.margin_xxxsmall
            rowSpacing: VLCStyle.margin_xxxsmall

            Repeater {
                model: [
                    /* selection tools */
                    { toolId: 1, icon: VLCIcons.grid, tip: qsTr("Rect Select"), sparkle: false },
                    { toolId: 2, icon: VLCIcons.history, tip: qsTr("Freeform Select"), sparkle: false },
                    { toolId: 3, icon: VLCIcons.topbar_discover, tip: qsTr("Object Select (AI)"), sparkle: true },
                    { toolId: 4, icon: VLCIcons.record, tip: qsTr("Pencil"), sparkle: false },
                    /* paint tools */
                    { toolId: 5, icon: VLCIcons.effect_filter, tip: qsTr("Brush"), sparkle: false },
                    { toolId: 6, icon: VLCIcons.clear, tip: qsTr("Eraser"), sparkle: false },
                    { toolId: 7, icon: VLCIcons.transparency, tip: qsTr("Fill"), sparkle: false },
                    { toolId: 8, icon: VLCIcons.search, tip: qsTr("Eyedropper"), sparkle: false },
                    { toolId: 9, icon: VLCIcons.visualization, tip: qsTr("Spray"), sparkle: false },
                    /* Paintbrush 2.0 art materials (pb_common.hpp ToolId 16/17) */
                    { toolId: 16, icon: VLCIcons.renderer, tip: qsTr("Bristle Paint"), sparkle: false },
                    { toolId: 17, icon: VLCIcons.shuffle, tip: qsTr("Glitter"), sparkle: false },
                    /* shape tools */
                    { toolId: 10, icon: VLCIcons.atob, tip: qsTr("Line"), sparkle: false },
                    { toolId: 11, icon: VLCIcons.space, tip: qsTr("Rectangle"), sparkle: false },
                    { toolId: 12, icon: VLCIcons.aspect_ratio, tip: qsTr("Rounded Rectangle"), sparkle: false },
                    { toolId: 13, icon: VLCIcons.circle, tip: qsTr("Ellipse"), sparkle: false },
                    { toolId: 14, icon: VLCIcons.ic_fluent_text_change_case_24_regular, tip: qsTr("Text"), sparkle: false },
                    /* navigation */
                    { toolId: 15, icon: VLCIcons.ic_fluent_arrow_move, tip: qsTr("Move canvas"), sparkle: false }
                ]

                delegate: Basic.Button {
                    id: toolBtn

                    required property int toolId
                    required property string icon
                    required property string tip
                    required property bool sparkle

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: VLCStyle.dp(40, VLCStyle.scale)
                    checkable: true
                    autoExclusive: true
                    checked: canvas.tool === toolId
                    onClicked: canvas.tool = toolId

                    /* large icon glyph; the tooltip carries the name */
                    contentItem: Text {
                        text: toolBtn.icon
                        font.family: VLCIcons.fontFamily
                        font.pixelSize: VLCStyle.fontSize_large
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        color: toolBtn.checked ? theme.accent : theme.fg.primary
                    }

                    background: Rectangle {
                        color: toolBtn.checked
                               ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.14)
                               : (toolBtn.hovered
                                  ? Qt.rgba(0.5, 0.5, 0.5, 0.10)
                                  : "transparent")
                        radius: VLCStyle.margin_xsmall
                        border.width: toolBtn.hovered && !toolBtn.checked ? 1 : 0
                        border.color: theme.border
                    }

                    /* sparkle indicator when the AI backend is available */
                    Rectangle {
                        visible: sparkle && PaintbrushCtx.segmentationAvailable
                        width: 6
                        height: 6
                        radius: 3
                        color: theme.accent
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

        /* zoom controls */
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