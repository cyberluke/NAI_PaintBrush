/*****************************************************************************
 * PaintbrushCanvasView.qml : NAI Paintbrush — canvas + selection overlay
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Dialogs

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style
import VLC.Paintbrush

/* The canvas area: checkered transparency background, the tile canvas item
 * and the contextual selection/object toolbar that appears when something
 * is selected. The canvas item itself is owned by the workspace root and
 * is reparented here so it can be shared with the tool rail and palette. */
Item {
    id: root

    required property PaintbrushCanvas canvas

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    Component.onCompleted: {
        canvas.parent = root
        canvas.anchors.fill = root
        canvas.visible = true
    }

    /* checkerboard behind transparent content */
    Grid {
        id: checkerGrid

        anchors.fill: parent
        columns: Math.max(1, Math.ceil(width / 16))
        rows: Math.max(1, Math.ceil(height / 16))

        Repeater {
            model: checkerGrid.columns * checkerGrid.rows
            Rectangle {
                width: 16
                height: 16
                color: (Math.floor(index / checkerGrid.columns) + index % checkerGrid.columns) % 2 === 0
                       ? theme.bg.primary
                       : Qt.darker(theme.bg.primary, 1.06)
            }
        }
    }

    /* contextual toolbar: selection / object actions */
    Rectangle {
        id: selectionBar

        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.margins: VLCStyle.margin_small

        height: selectionBarRow.height + VLCStyle.margin_small * 2
        width: selectionBarRow.width + VLCStyle.margin_normal * 2
        radius: VLCStyle.margin_small
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
        visible: (canvas.hasSelection || PaintbrushCtx.objectSelected) && !PaintbrushCtx.welcomeVisible

        RowLayout {
            id: selectionBarRow
            anchors.centerIn: parent
            spacing: VLCStyle.margin_xsmall

            Widgets.LabelExt {
                text: canvas.hasSelection ? qsTr("Selection")
                                          : qsTr("Object %1").arg(PaintbrushCtx.objectMaskBoundsText)
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_small
                visible: text.length > 0
            }

            Widgets.IconToolButton {
                description: qsTr("Cut out")
                text: VLCIcons.del
                visible: PaintbrushCtx.objectSelected
                onClicked: PaintbrushCtx.cutOutObject()
            }
            Widgets.IconToolButton {
                description: qsTr("Add to canvas")
                text: VLCIcons.add
                visible: PaintbrushCtx.objectSelected
                onClicked: PaintbrushCtx.addObjectToCanvas()
            }
            Widgets.IconToolButton {
                description: qsTr("Ask about this…")
                text: VLCIcons.info
                visible: PaintbrushCtx.objectSelected || canvas.hasSelection
                onClicked: {
                    if (PaintbrushCtx.objectSelected)
                        PaintbrushCtx.askAboutObject(qsTr("What is this object?"))
                    else
                        PaintbrushCtx.askAboutSelection(qsTr("What is shown in this selection?"))
                }
            }
            Widgets.IconToolButton {
                description: qsTr("Track in video")
                text: VLCIcons.play
                visible: PaintbrushCtx.objectSelected && PaintbrushCtx.playerHasVideo
                onClicked: PaintbrushCtx.trackObject(0, 24)
            }
            Widgets.IconToolButton {
                description: qsTr("Make sticker")
                text: VLCIcons.add
                visible: PaintbrushCtx.objectSelected
                onClicked: PaintbrushCtx.makeSticker()
            }
            Widgets.IconToolButton {
                description: qsTr("Remove object")
                text: VLCIcons.remove
                visible: PaintbrushCtx.objectSelected
                onClicked: PaintbrushCtx.removeObject("")
            }
            Widgets.IconToolButton {
                description: qsTr("Export object PNG")
                text: VLCIcons.ic_fluent_arrow_download_24_regular
                visible: PaintbrushCtx.objectSelected
                onClicked: exportObjectDialog.open()
            }

            Widgets.IconToolButton {
                description: qsTr("Copy")
                text: VLCIcons.ic_fluent_document_copy_24_regular
                visible: canvas.hasSelection
                onClicked: canvas.copySelection()
            }
            Widgets.IconToolButton {
                description: qsTr("Cut")
                text: VLCIcons.del
                visible: canvas.hasSelection
                onClicked: canvas.cutSelection()
            }
            Widgets.IconToolButton {
                description: qsTr("Clear selection")
                text: VLCIcons.clear
                visible: canvas.hasSelection
                onClicked: canvas.selectionClear()
            }
        }
    }

    /* status overlay: coordinates / zoom */
    Rectangle {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: VLCStyle.margin_small

        height: VLCStyle.dp(24, VLCStyle.scale)
        width: statusLabel.implicitWidth + VLCStyle.margin_small * 2
        radius: VLCStyle.margin_xsmall
        color: Qt.rgba(0, 0, 0, 0.55)

        Widgets.LabelExt {
            id: statusLabel
            anchors.centerIn: parent
            text: canvas.statusText
            color: "white"
            font.pixelSize: VLCStyle.fontSize_xsmall
        }
    }

    FileDialog {
        id: exportObjectDialog
        title: qsTr("Export selected object")
        fileMode: FileDialog.SaveFile
        nameFilters: ["PNG image (*.png)", "All files (*)"]
        defaultSuffix: "png"
        onAccepted: {
            if (selectedFile)
                PaintbrushCtx.exportObjectPng(selectedFile)
        }
    }
}