/*****************************************************************************
 * PaintbrushToolRail.qml : NAI Paintbrush — classic tool rail
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

/* The classic Paintbrush tool rail. Tools are text-labelled like the
 * original Paintbrush toolbar; Object Select shows a small sparkle when
 * the AI segmentation backend is available. */
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

        Component {
            id: toolButton

            Basic.Button {
                id: btn

                property int toolId: 0
                property bool sparkle: false

                Layout.fillWidth: true
                Layout.preferredHeight: VLCStyle.dp(34, VLCStyle.scale)
                checkable: true
                autoExclusive: true
                checked: canvas.tool === toolId
                onClicked: canvas.tool = toolId

                contentItem: Text {
                    text: btn.text
                    font.pixelSize: VLCStyle.fontSize_xsmall
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: btn.checked ? theme.accent : theme.fg.primary
                }

                background: Rectangle {
                    color: btn.checked ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.12)
                                       : "transparent"
                    radius: VLCStyle.margin_xsmall
                    border.width: btn.hovered && !btn.checked ? 1 : 0
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
            }
        }

        // selection tools
        Loader {
            sourceComponent: toolButton
            active: true
            onLoaded: {
                item.text = qsTr("Sel")
                item.toolId = 1
                item.ToolTip.text = qsTr("Rect Select")
            }
        }
        Loader {
            sourceComponent: toolButton
            active: true
            onLoaded: {
                item.text = qsTr("Lasso")
                item.toolId = 2
                item.ToolTip.text = qsTr("Freeform Select")
            }
        }

        // AI object select
        Loader {
            sourceComponent: toolButton
            active: true
            onLoaded: {
                item.text = qsTr("Object")
                item.toolId = 3
                item.sparkle = true
                item.ToolTip.text = qsTr("Object Select (AI)")
            }
        }

        Item { Layout.fillHeight: true; height: 2 }

        // paint tools
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Pencil"); item.toolId = 4; item.ToolTip.text = qsTr("Pencil") } }
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Brush"); item.toolId = 5; item.ToolTip.text = qsTr("Brush") } }
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Eraser"); item.toolId = 6; item.ToolTip.text = qsTr("Eraser") } }
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Fill"); item.toolId = 7; item.ToolTip.text = qsTr("Fill") } }
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Pick"); item.toolId = 8; item.ToolTip.text = qsTr("Eyedropper") } }
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Spray"); item.toolId = 9; item.ToolTip.text = qsTr("Spray") } }

        Item { Layout.fillHeight: true; height: 2 }

        // shape tools
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Line"); item.toolId = 10; item.ToolTip.text = qsTr("Line") } }
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Rect"); item.toolId = 11; item.ToolTip.text = qsTr("Rectangle") } }
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Rnd"); item.toolId = 12; item.ToolTip.text = qsTr("Rounded Rectangle") } }
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Oval"); item.toolId = 13; item.ToolTip.text = qsTr("Ellipse") } }
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Text"); item.toolId = 14; item.ToolTip.text = qsTr("Text") } }
        Loader { sourceComponent: toolButton; active: true; onLoaded: { item.text = qsTr("Pan"); item.toolId = 15; item.ToolTip.text = qsTr("Pan / Zoom") } }

        Item { Layout.fillHeight: true; height: 2 }

        // zoom controls
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
            text: VLCIcons.ic_fluent_full_screen_maximize
            description: qsTr("Fit to view")
            onClicked: canvas.fitToView()
        }
    }
}