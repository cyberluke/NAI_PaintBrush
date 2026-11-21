/*****************************************************************************
 * PaintbrushStrip.qml : NAI Paintbrush — the single adaptive bottom strip
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * ONE surface, left to right:
 *   [ tool identity ] [ per-tool parameters : flexible ] [ colour : intrinsic ]
 *   [ workspace verbs: Mood / Boost ] [ primary action: Keep ]
 *
 * - One background, one separator line. No nested card, no second border.
 * - Height comes from the tallest real control (32dp cell + 2 x 8dp padding)
 *   and stays constant across tool changes, so the canvas never reflows.
 * - Colour is a COMPACT control (foreground chip, background chip, recent
 *   selected colours, palette popover), not a second bar. It participates in
 *   layout only for colour-capable tools, so Object Select leaves no spacer.
 * - Keep is shown by tool CAPABILITY (Object Select) and enabled by selection
 *   state, so the verb never disappears while a mask is being refined.
 * - Mood / Boost / Undo Boost / Cancel are real controller verbs, moved here
 *   from the floating canvas pills; the canvas keeps no chrome of its own.
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

    /* 32dp control row + 8dp vertical breathing room on each side. Derived
     * from the controls, not an arbitrary 64/96dp band. */
    readonly property int _control: VLCStyle.dp(32, VLCStyle.scale)
    readonly property int _pad: VLCStyle.margin_xsmall
    implicitHeight: _control + 2 * _pad

    /* Semantic capabilities; QML never tests tool numbers here. */
    readonly property bool colourCapable: PaintbrushCtx.toolUsesColor
    readonly property bool hasPrimaryAction: PaintbrushCtx.toolHasPrimaryAction
    readonly property bool showIdentity: width >= VLCStyle.dp(640, VLCStyle.scale)

    /* The M shortcut routes here so the popover keeps one owner. */
    function toggleMoodPicker() {
        if (moodPicker.opened)
            moodPicker.close()
        else
            moodPicker.open()
    }

    ColorContext {
        palette: VLCStyle.palette
        id: theme
        colorSet: ColorContext.View
    }

    /* ---- the one and only surface ------------------------------------ */
    Rectangle {
        anchors.fill: parent
        color: theme.bg.primary

        /* a single hairline separates the chrome from the canvas; there is no
         * second frame, radius or inset card inside this surface */
        Rectangle {
            anchors.top: parent.top
            width: parent.width
            height: 1
            color: theme.border
            opacity: 0.55
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: VLCStyle.margin_small
        anchors.rightMargin: VLCStyle.margin_small
        anchors.topMargin: root._pad
        anchors.bottomMargin: root._pad
        spacing: VLCStyle.margin_small

        /* ---- tool identity ------------------------------------------- */
        Widgets.LabelExt {
            visible: root.showIdentity
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: VLCStyle.dp(72, VLCStyle.scale)
            text: root.canvas ? root.canvas.toolName : ""
            color: theme.fg.primary
            font.pixelSize: VLCStyle.fontSize_small
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        Rectangle {
            visible: root.showIdentity
            Layout.preferredWidth: 1
            Layout.preferredHeight: root._control * 0.6
            Layout.alignment: Qt.AlignVCenter
            color: theme.border
            opacity: 0.6
        }

        /* ---- parameters: the only flexible group ---------------------- */
        PaintbrushToolOptionsBar {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            canvas: root.canvas
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: root._control * 0.6
            Layout.alignment: Qt.AlignVCenter
            visible: root.colourCapable
            color: theme.border
            opacity: 0.6
        }

        /* ---- colour: compact, intrinsic ------------------------------- */
        RowLayout {
            id: colourGroup
            visible: root.colourCapable
            Layout.alignment: Qt.AlignVCenter
            spacing: VLCStyle.margin_xxsmall

            /* foreground / background. Buttons, not bare Rectangles, so the
             * chips are keyboard-operable and announce themselves. */
            Button {
                id: fgChip
                Layout.preferredWidth: VLCStyle.dp(26, VLCStyle.scale)
                Layout.preferredHeight: VLCStyle.dp(26, VLCStyle.scale)
                padding: 0
                focusPolicy: Qt.StrongFocus
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Foreground colour")
                onClicked: colorDialog.open()
                background: Rectangle {
                    radius: 4
                    color: root.canvas ? root.canvas.fgColor : "white"
                    border.width: fgChip.activeFocus ? 2 : 1
                    border.color: fgChip.activeFocus ? theme.accent : theme.border
                }
            }
            Button {
                id: bgChip
                Layout.preferredWidth: VLCStyle.dp(20, VLCStyle.scale)
                Layout.preferredHeight: VLCStyle.dp(20, VLCStyle.scale)
                padding: 0
                focusPolicy: Qt.StrongFocus
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Background colour")
                onClicked: bgDialog.open()
                background: Rectangle {
                    radius: 4
                    color: root.canvas ? root.canvas.bgColor : "black"
                    border.width: bgChip.activeFocus ? 2 : 1
                    border.color: bgChip.activeFocus ? theme.accent : theme.border
                }
            }

            /* recent SELECTED colours (history, not palette content) */
            Repeater {
                model: PaintbrushCtx.recentSelectedColors
                delegate: Button {
                    Layout.preferredWidth: VLCStyle.dp(18, VLCStyle.scale)
                    Layout.preferredHeight: VLCStyle.dp(18, VLCStyle.scale)
                    padding: 0
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Use colour %1").arg(modelData)
                    onClicked: if (root.canvas) root.canvas.fgColor = modelData
                    background: Rectangle {
                        radius: 3
                        color: modelData
                        border.width: parent.activeFocus ? 2 : 1
                        border.color: parent.activeFocus ? theme.accent
                                                         : theme.border
                    }
                }
            }

            Widgets.ButtonExt {
                id: paletteButton
                text: qsTr("Palettes")
                Layout.preferredHeight: root._control
                font.pixelSize: VLCStyle.fontSize_xsmall
                Accessible.name: qsTr("Open saved palettes")
                onClicked: palettePopup.open()
            }
        }

        /* ---- workspace verbs (previously floating over the canvas) ---- */
        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: root._control * 0.6
            Layout.alignment: Qt.AlignVCenter
            color: theme.border
            opacity: 0.6
        }

        Widgets.ButtonExt {
            id: moodButton
            text: qsTr("Mood")
            Layout.preferredHeight: root._control
            font.pixelSize: VLCStyle.fontSize_xsmall
            enabled: root.canvas !== null
            Accessible.name: qsTr("Art direction: mood, paper, type, badges")
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: qsTr("Paper, light, palette, type and badges in one look")
            onClicked: moodPicker.open()
        }

        Widgets.ButtonExt {
            id: boostButton
            text: PaintbrushCtx.boostBusy ? qsTr("Boosting…") : qsTr("Boost")
            Layout.preferredHeight: root._control
            font.pixelSize: VLCStyle.fontSize_xsmall
            enabled: PaintbrushCtx.canBoost && !PaintbrushCtx.boostBusy
            Accessible.name: qsTr("Boost the composition")
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: PaintbrushCtx.boostBusy
                          ? qsTr("Applying the art direction…")
                          : (PaintbrushCtx.canBoost
                             ? qsTr("Art-direct the composition: hierarchy, depth, motion. One undo.")
                             : qsTr("Drop a sticker or image on the canvas first — Boost needs source art."))
            onClicked: PaintbrushCtx.boost()
        }

        Widgets.ButtonExt {
            text: qsTr("Undo Boost")
            Layout.preferredHeight: root._control
            font.pixelSize: VLCStyle.fontSize_xsmall
            visible: PaintbrushCtx.canUndo
                     && PaintbrushCtx.undoText.indexOf("Boost") >= 0
                     && !PaintbrushCtx.boostBusy
            Accessible.name: qsTr("Undo the last Boost")
            onClicked: PaintbrushCtx.undo()
        }

        Widgets.ButtonExt {
            text: qsTr("Cancel")
            Layout.preferredHeight: root._control
            font.pixelSize: VLCStyle.fontSize_xsmall
            visible: PaintbrushCtx.boostBusy
            Accessible.name: qsTr("Cancel Boost")
            onClicked: PaintbrushCtx.cancelBoost()
        }

        /* ---- primary action: a TOOL CAPABILITY ------------------------ *
         * Object Select owns this verb; it stays visible (disabled until a
         * cut-out exists) so refining a mask never removes the control. */
        Widgets.ActionButtonPrimary {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredHeight: root._control
            visible: root.hasPrimaryAction
            text: qsTr("Keep")
            enabled: PaintbrushCtx.canKeep
            Accessible.name: qsTr("Keep this cut-out")
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: !PaintbrushCtx.segmentationAvailable
                          ? qsTr("Object Select is unavailable — configure the SAM model in NAI Settings.")
                          : (PaintbrushCtx.objectSelected
                             ? qsTr("Keep this cut-out in your tray")
                             : qsTr("Click the person or object you want to cut out."))
            onClicked: PaintbrushCtx.keepSelection()
        }
    }

    /* ---- colour pickers ---------------------------------------------- */
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

    /* ---- art direction popover (anchored to its button, above the bar) -- */
    PaintbrushMoodPicker {
        id: moodPicker
        canvas: root.canvas
        parent: moodButton
        x: moodButton.width - width
        y: -height - VLCStyle.margin_small
    }

    /* ---- palette library -------------------------------------------- */
    Popup {
        id: palettePopup
        parent: paletteButton
        x: paletteButton.width - width
        y: -height - VLCStyle.margin_small
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
                        delegate: Button {
                            Layout.preferredWidth: VLCStyle.dp(20, VLCStyle.scale)
                            Layout.preferredHeight: VLCStyle.dp(20, VLCStyle.scale)
                            padding: 0
                            focusPolicy: Qt.StrongFocus
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Use colour %1").arg(modelData)
                            onClicked: {
                                if (root.canvas) {
                                    root.canvas.fgColor = modelData
                                    palettePopup.close()
                                }
                            }
                            background: Rectangle {
                                radius: 3
                                color: modelData
                                border.width: parent.activeFocus ? 2 : 1
                                border.color: parent.activeFocus ? theme.accent
                                                                 : theme.border
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
}
