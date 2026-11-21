/*****************************************************************************
 * PaintbrushStrip.qml : NAI Paintbrush — the adaptive bottom strip
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * Groups, left to right:
 *   [ parameters : flexible ] [ colour : intrinsic ] [ primary action ]
 *
 * - Fixed height; tool changes never reflow the canvas.
 * - Colour is a COMPACT control (active chip, background chip, recent swatches,
 *   palette popover with all 27 persisted palettes + picker), not a second bar.
 * - Colour participates in layout only for colour-capable tools, so Object
 *   Select leaves no spacer.
 * - The primary action reuses real controller verbs (Keep / Boost).
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import QtQuick.Dialogs

import VLC.Style
import VLC.Widgets as Widgets
import VLC.Paintbrush

Item {
    id: root

    property var canvas: null

    /* Sized to the tallest real control set (options row 32 + margins 16,
     * colour chips 28), not an arbitrary round number. */
    implicitHeight: VLCStyle.dp(64, VLCStyle.scale)

    /* Semantic capability: expand this table instead of testing tool numbers
     * at call sites. Object Select (3) is the only colour-free tool today. */
    readonly property bool colourCapable: PaintbrushCtx.toolUsesColor
    readonly property bool selecting: PaintbrushCtx.toolHasPrimaryAction

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    Rectangle {
        anchors.fill: parent
        radius: VLCStyle.margin_small
        color: theme.bg.primary
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: VLCStyle.margin_small
        spacing: VLCStyle.margin_small

        /* ---- parameters: the only flexible group ------------------------ */
        PaintbrushToolOptionsBar {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            canvas: root.canvas
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            visible: root.colourCapable
            color: theme.border
            opacity: 0.6
        }

        /* ---- colour: compact, intrinsic --------------------------------- */
        RowLayout {
            visible: root.colourCapable
            Layout.alignment: Qt.AlignVCenter
            spacing: VLCStyle.margin_xsmall

            /* active colour */
            Rectangle {
                width: VLCStyle.dp(28, VLCStyle.scale)
                height: width
                radius: 4
                color: root.canvas ? root.canvas.fgColor : "white"
                border.width: 1
                border.color: theme.border
                MouseArea {
                    anchors.fill: parent
                    onClicked: colorDialog.open()
                }
            }
            /* background colour */
            Rectangle {
                width: VLCStyle.dp(22, VLCStyle.scale)
                height: width
                radius: 4
                color: root.canvas ? root.canvas.bgColor : "black"
                border.width: 1
                border.color: theme.border
                MouseArea {
                    anchors.fill: parent
                    onClicked: bgDialog.open()
                }
            }

            /* recent swatches from the active palette */
            Repeater {
                model: PaintbrushCtx.recentSelectedColors
                Rectangle {
                    width: VLCStyle.dp(18, VLCStyle.scale)
                    height: width
                    radius: 3
                    color: modelData
                    border.width: 1
                    border.color: theme.border
                    MouseArea {
                        anchors.fill: parent
                        onClicked: if (root.canvas) root.canvas.fgColor = modelData
                    }
                }
            }

            /* full palette library */
            Button {
                text: qsTr("Palettes")
                onClicked: palettePopup.open()
            }
        }

        /* ---- primary action -------------------------------------------- */
        /* Primary action is a TOOL CAPABILITY, not a global fallback. Only a
         * finished selection has an apply verb; painting and transforming
         * tools are continuous and have none, so they show no action at all
         * (and the layout gives the space back to the parameters). */
        Widgets.ActionButtonPrimary {
            Layout.alignment: Qt.AlignVCenter
            visible: root.selecting
            text: qsTr("Keep")
            enabled: PaintbrushCtx.canKeep
            onClicked: PaintbrushCtx.keepSelection()
        }
    }

    ColorDialog {
        id: colorDialog
        title: qsTr("Foreground colour")
        onAccepted: if (root.canvas) root.canvas.fgColor = selectedColor
    }
    ColorDialog {
        id: bgDialog
        title: qsTr("Background colour")
        onAccepted: if (root.canvas) root.canvas.bgColor = selectedColor
    }

    Popup {
        id: palettePopup
        width: VLCStyle.dp(420, VLCStyle.scale)
        height: VLCStyle.dp(320, VLCStyle.scale)
        modal: true
        focus: true

        background: Rectangle {
            color: theme.bg.primary
            radius: VLCStyle.margin_small
            border.width: 1
            border.color: theme.border
        }

        contentItem: ListView {
            clip: true
            model: PaintbrushCtx.paletteList()
            spacing: VLCStyle.margin_xsmall

            delegate: ColumnLayout {
                width: ListView.view.width
                spacing: 2

                Widgets.LabelExt {
                    text: modelData.name
                    color: theme.fg.primary
                    font.pixelSize: VLCStyle.fontSize_xsmall
                }
                RowLayout {
                    spacing: 2
                    Repeater {
                        model: modelData.colors
                        Rectangle {
                            width: VLCStyle.dp(20, VLCStyle.scale)
                            height: width
                            radius: 3
                            color: modelData
                            border.width: 1
                            border.color: theme.border
                            MouseArea {
                                anchors.fill: parent
                                onClicked: if (root.canvas) {
                                    root.canvas.fgColor = modelData
                                    palettePopup.close()
                                }
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
}
