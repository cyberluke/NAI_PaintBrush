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
 *   used colours, palette popover), not a second bar. It participates in
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

    /* Saved palettes are REAL buttons in this strip, not only a popover:
     * the active palette renders as direct swatch buttons, and the library
     * popover switches which palette is active. */
    property int activePaletteIndex: 0
    readonly property var palettes: PaintbrushCtx.paletteList()
    readonly property var activePalette:
        palettes && palettes.length > 0
            ? palettes[Math.max(0, Math.min(activePaletteIndex,
                                            palettes.length - 1))]
            : null

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

            /* recently USED colours — recorded by the controller when a
             * stroke/fill commits, never by selecting a colour (so clicking
             * a palette swatch cannot duplicate the palette into this row) */
            Repeater {
                model: PaintbrushCtx.recentUsedColors
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

            /* active palette: real palette buttons in the toolbar. Click a
             * swatch to paint with it; the Palettes popover switches the
             * active palette. */
            Rectangle {
                Layout.preferredWidth: 1
                Layout.preferredHeight: VLCStyle.dp(18, VLCStyle.scale)
                Layout.alignment: Qt.AlignVCenter
                visible: root.activePalette !== null
                color: theme.border
                opacity: 0.6
            }
            Repeater {
                model: root.activePalette
                       ? root.activePalette.colors.slice(0, 10) : []
                delegate: Button {
                    id: paletteSwatch
                    Layout.preferredWidth: VLCStyle.dp(18, VLCStyle.scale)
                    Layout.preferredHeight: VLCStyle.dp(18, VLCStyle.scale)
                    padding: 0
                    focusPolicy: Qt.StrongFocus
                    /* the swatch that IS the active foreground colour; a plain
                     * click selects it and nothing else changes */
                    readonly property bool isActiveColor:
                        root.canvas
                        ? Qt.colorEqual(root.canvas.fgColor, modelData) : false
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("%1 palette colour %2")
                                        .arg(root.activePalette
                                             ? root.activePalette.name : "")
                                        .arg(modelData)
                    onClicked: if (root.canvas) root.canvas.fgColor = modelData
                    background: Rectangle {
                        radius: 3
                        color: modelData
                        border.width: paletteSwatch.isActiveColor
                                      || paletteSwatch.activeFocus ? 2 : 1
                        border.color: paletteSwatch.isActiveColor
                                      || paletteSwatch.activeFocus
                                      ? theme.accent : theme.border
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("%1 — click to paint").arg(modelData)
                }
            }

            Widgets.ButtonExt {
                id: paletteButton
                text: qsTr("Palettes")
                Layout.preferredHeight: root._control
                font.pixelSize: VLCStyle.fontSize_xsmall
                Accessible.name: qsTr("Open saved palettes")
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: root.activePalette
                              ? qsTr("Saved palettes — active: %1")
                                .arg(root.activePalette.name)
                              : qsTr("Saved palettes")
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
                id: paletteRow
                width: ListView.view.width
                spacing: 2
                required property int index
                required property var modelData

                /* selecting a palette here makes it the active palette whose
                 * swatches are directly available in the bottom strip */
                Button {
                    id: paletteHeader
                    Layout.fillWidth: true
                    Layout.preferredHeight: VLCStyle.dp(24, VLCStyle.scale)
                    padding: 0
                    focusPolicy: Qt.StrongFocus
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Use palette %1")
                                        .arg(paletteRow.modelData.name)
                    onClicked: {
                        root.activePaletteIndex = paletteRow.index
                        palettePopup.close()
                    }
                    background: Rectangle {
                        radius: 4
                        color: paletteHeader.hovered ? theme.bg.secondary
                                                     : "transparent"
                        border.width: paletteRow.index === root.activePaletteIndex
                                      ? 1 : 0
                        border.color: theme.accent
                    }
                    contentItem: Widgets.LabelExt {
                        leftPadding: VLCStyle.margin_xxsmall
                        text: paletteRow.modelData.name
                        color: paletteRow.index === root.activePaletteIndex
                               ? theme.accent : theme.fg.primary
                        font.pixelSize: VLCStyle.fontSize_xsmall
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                RowLayout {
                    spacing: 2
                    Repeater {
                        model: paletteRow.modelData.colors
                        delegate: Button {
                            id: popupSwatch
                            Layout.preferredWidth: VLCStyle.dp(20, VLCStyle.scale)
                            Layout.preferredHeight: VLCStyle.dp(20, VLCStyle.scale)
                            padding: 0
                            focusPolicy: Qt.StrongFocus
                            readonly property bool isActiveColor:
                                root.canvas
                                ? Qt.colorEqual(root.canvas.fgColor, modelData)
                                : false
                            Accessible.role: Accessible.Button
                            Accessible.name: qsTr("Use colour %1").arg(modelData)
                            onClicked: {
                                if (root.canvas) {
                                    root.activePaletteIndex = paletteRow.index
                                    root.canvas.fgColor = modelData
                                    palettePopup.close()
                                }
                            }
                            background: Rectangle {
                                radius: 3
                                color: modelData
                                border.width: popupSwatch.isActiveColor
                                              || popupSwatch.activeFocus ? 2 : 1
                                border.color: popupSwatch.isActiveColor
                                              || popupSwatch.activeFocus
                                              ? theme.accent : theme.border
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
}
