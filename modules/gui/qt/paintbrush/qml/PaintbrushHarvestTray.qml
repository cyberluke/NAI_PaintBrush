/*****************************************************************************
 * PaintbrushHarvestTray.qml : NAI Paintbrush — the Harvest Tray (spec §5.4)
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

/* A compact strip of harvested moments: thumbnail + timestamp only. Never AI
 * diagnostics, mask morphology or transform controls. Items are dragged (or
 * clicked) onto the canvas; the tray itself never clutters the canvas. The
 * strip collapses to nothing while the tray is empty. */
Item {
    id: root

    required property PaintbrushCanvas canvas
    /* the canvas view item the drops land on (for canvas-space mapping) */
    property Item dropTarget: null

    readonly property bool hasItems: PaintbrushCtx.harvestCount > 0
    implicitHeight: hasItems ? VLCStyle.dp(96, VLCStyle.scale) : 0
    visible: hasItems

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

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: VLCStyle.margin_normal
        anchors.rightMargin: VLCStyle.margin_normal
        spacing: VLCStyle.margin_small

        ColumnLayout {
            Layout.alignment: Qt.AlignVCenter
            spacing: 2
            Widgets.LabelExt {
                text: qsTr("Tray")
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_normal
                font.bold: true
            }
            Widgets.LabelExt {
                text: PaintbrushCtx.harvestCount === 1
                      ? qsTr("1 moment")
                      : qsTr("%1 moments").arg(PaintbrushCtx.harvestCount)
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_xsmall
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: ListView.Horizontal
            spacing: VLCStyle.margin_small
            clip: true
            model: PaintbrushCtx.harvestTray
            boundsBehavior: Flickable.StopAtBounds

            delegate: Item {
                id: cell
                required property int harvestId
                required property string thumbnail
                required property string timeText
                required property bool favorite
                required property string label
                required property int index

                width: VLCStyle.dp(84, VLCStyle.scale)
                height: list.height

                Rectangle {
                    id: card
                    anchors.fill: parent
                    anchors.topMargin: VLCStyle.margin_xsmall
                    anchors.bottomMargin: VLCStyle.margin_xsmall
                    radius: VLCStyle.margin_xsmall
                    color: dragArea.containsMouse
                           ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.12)
                           : Qt.rgba(0.5, 0.5, 0.5, 0.08)
                    border.width: 1
                    border.color: dragArea.containsMouse ? theme.accent : theme.border

                    Image {
                        id: thumb
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: timeLabel.top
                        anchors.margins: VLCStyle.margin_xxsmall
                        source: cell.thumbnail
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        asynchronous: false
                    }
                    Widgets.LabelExt {
                        id: timeLabel
                        anchors.bottom: parent.bottom
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottomMargin: 3
                        text: cell.timeText
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_xsmall
                    }
                    /* favorite dot: a quiet marker, not a star button */
                    Rectangle {
                        visible: cell.favorite
                        width: 6; height: 6; radius: 3
                        color: theme.accent
                        anchors.top: parent.top
                        anchors.right: parent.right
                        anchors.margins: 4
                    }
                }

                /* drag a ghost of the thumbnail onto the canvas; release
                 * places the pose at the drop point. A plain click places it
                 * at the canvas center. */
                MouseArea {
                    id: dragArea
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    property bool dragging: false
                    property point pressPos

                    onPressed: (mouse) => {
                        if (mouse.button === Qt.LeftButton) {
                            pressPos = Qt.point(mouse.x, mouse.y)
                            dragging = false
                        }
                    }
                    onPositionChanged: (mouse) => {
                        if (!(mouse.buttons & Qt.LeftButton))
                            return
                        if (!dragging && Math.abs(mouse.x - pressPos.x) + Math.abs(mouse.y - pressPos.y) > 6) {
                            dragging = true
                            ghost.source = cell.thumbnail
                            ghost.visible = true
                        }
                        if (dragging) {
                            const p = dragArea.mapToItem(root.parent, mouse.x, mouse.y)
                            ghost.x = p.x - ghost.width / 2
                            ghost.y = p.y - ghost.height / 2
                        }
                    }
                    onReleased: (mouse) => {
                        if (mouse.button === Qt.RightButton) {
                            trayMenu.harvestId = cell.harvestId
                            trayMenu.favorite = cell.favorite
                            trayMenu.popup()
                            return
                        }
                        ghost.visible = false
                        if (dragging && root.dropTarget) {
                            const inTarget = dragArea.mapToItem(root.dropTarget, mouse.x, mouse.y)
                            if (inTarget.x >= 0 && inTarget.y >= 0 &&
                                inTarget.x <= root.dropTarget.width && inTarget.y <= root.dropTarget.height) {
                                const cp = root.canvas.canvasFromItem
                                           ? root.canvas.canvasFromItem(root.dropTarget.mapToItem(root.canvas, inTarget.x, inTarget.y))
                                           : Qt.point(0, 0)
                                PaintbrushCtx.placeHarvest(cell.harvestId, cp)
                            }
                        } else if (!dragging) {
                            PaintbrushCtx.placeHarvest(cell.harvestId, Qt.point(0, 0))
                        }
                        dragging = false
                    }
                    onDoubleClicked: PaintbrushCtx.seekToHarvest(cell.harvestId)

                    ToolTip.visible: containsMouse && !dragging
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("Drag onto the canvas · double-click to jump to this moment")
                }
            }
        }
    }

    /* the drag ghost lives in the parent so it can travel over the canvas */
    Image {
        id: ghost
        parent: root.parent
        visible: false
        z: 100
        width: VLCStyle.dp(96, VLCStyle.scale)
        height: VLCStyle.dp(96, VLCStyle.scale)
        fillMode: Image.PreserveAspectFit
        opacity: 0.85
    }

    Menu {
        id: trayMenu
        property int harvestId: 0
        property bool favorite: false
        MenuItem {
            text: trayMenu.favorite ? qsTr("Unmark favorite") : qsTr("Mark favorite")
            onTriggered: PaintbrushCtx.toggleHarvestFavorite(trayMenu.harvestId)
        }
        MenuItem {
            text: qsTr("Jump to this moment")
            onTriggered: PaintbrushCtx.seekToHarvest(trayMenu.harvestId)
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Remove from tray")
            onTriggered: PaintbrushCtx.removeHarvest(trayMenu.harvestId)
        }
    }
}
