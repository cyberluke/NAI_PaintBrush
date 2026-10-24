/*****************************************************************************
 * PaintbrushTextEditorBar.qml : NAI Paintbrush — inline text editing
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

/* Editable typography, no dialog hell (spec §57): when the current layer is
 * a text layer, this slim bar appears above the palette with the word, the
 * font role, size, tracking and color. Every committed change is one typed
 * undo step (EditText command); the canvas re-renders glyphs from the
 * treatment — never a bitmap stretch. Double-clicking a text layer on the
 * canvas selects it and opens this bar focused. */
Rectangle {
    id: root

    required property PaintbrushCanvas canvas

    visible: treatmentMap.size > 0
    height: visible ? VLCStyle.dp(56, VLCStyle.scale) : 0
    color: theme.bg.primary
    border.width: 1
    border.color: theme.border
    radius: VLCStyle.margin_xsmall
    z: 4

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    /* the treatment of the current layer, re-read on every document change */
    readonly property int layerIndex: PaintbrushCtx.document.currentLayerIndex
    property var treatmentMap: ({})

    function refreshTreatment() {
        treatmentMap = PaintbrushCtx.textTreatmentFor(layerIndex)
    }
    function openEditor() {
        refreshTreatment()
        if (treatmentMap.size > 0)
            wordField.forceActiveFocus()
    }
    Component.onCompleted: refreshTreatment()
    Connections {
        target: PaintbrushCtx.document
        function onCurrentLayerChanged() { root.refreshTreatment() }
        function onLayersChanged() { root.refreshTreatment() }
    }
    Connections {
        target: PaintbrushCtx
        function onDocumentChanged() { root.refreshTreatment() }
    }

    /* commit helpers — one EditText undo step per committed gesture */
    function commitText() {
        if (root.treatmentMap.size === 0)
            return
        PaintbrushCtx.editTextLayer(root.layerIndex,
                                    wordField.text,
                                    roleBox.currentValue,
                                    sizeSlider.value,
                                    swatchRow.currentColor,
                                    trackingSlider.value)
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: VLCStyle.margin_xsmall
        spacing: VLCStyle.margin_small

        Widgets.IconLabel {
            text: VLCIcons.ic_fluent_text_change_case_24_regular
            font.pixelSize: VLCStyle.fontSize_large
            color: theme.accent
        }

        /* the word itself */
        TextField {
            id: wordField
            Layout.preferredWidth: VLCStyle.dp(180, VLCStyle.scale)
            text: root.treatmentMap.text ?? ""
            placeholderText: qsTr("Word…")
            selectByMouse: true
            onEditingFinished: root.commitText()
            Keys.onReturnPressed: {
                root.commitText()
                focus = false
            }
            Keys.onEscapePressed: {
                root.refreshTreatment()
                focus = false
            }
        }

        /* font role */
        ComboBox {
            id: roleBox
            Layout.preferredWidth: VLCStyle.dp(140, VLCStyle.scale)
            model: PaintbrushCtx.typographyList
            textRole: "name"
            valueRole: "id"
            currentIndex: {
                const id = root.treatmentMap.style ?? ""
                const list = PaintbrushCtx.typographyList
                for (let i = 0; i < list.length; ++i)
                    if (list[i].id === id)
                        return i
                return 0
            }
            onActivated: root.commitText()
        }

        /* size, world units (spec §57: world-unit size, stable at export) */
        Widgets.LabelExt {
            text: qsTr("Size")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_xsmall
        }
        Slider {
            id: sizeSlider
            Layout.preferredWidth: VLCStyle.dp(110, VLCStyle.scale)
            from: 12
            to: 400
            stepSize: 1
            value: root.treatmentMap.size ?? 96
            /* one undo step per drag, not per tick */
            onPressedChanged: if (!pressed) root.commitText()
        }
        Widgets.LabelExt {
            text: Math.round(sizeSlider.value)
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_xsmall
            Layout.preferredWidth: VLCStyle.dp(24, VLCStyle.scale)
        }

        /* tracking (em fraction) */
        Widgets.LabelExt {
            text: qsTr("Track")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_xsmall
        }
        Slider {
            id: trackingSlider
            Layout.preferredWidth: VLCStyle.dp(90, VLCStyle.scale)
            from: -0.15
            to: 0.5
            stepSize: 0.01
            value: root.treatmentMap.tracking ?? 0
            onPressedChanged: if (!pressed) root.commitText()
        }

        /* color swatches from the current mood palette + the treatment color */
        Row {
            id: swatchRow
            Layout.alignment: Qt.AlignVCenter
            spacing: 4

            readonly property string currentColor: root.treatmentMap.color ?? "#ff1f1b18"

            Repeater {
                model: PaintbrushCtx.moodPalette
                delegate: Rectangle {
                    required property var modelData
                    width: VLCStyle.dp(22, VLCStyle.scale)
                    height: width
                    radius: 4
                    color: modelData
                    border.width: swatchRow.currentColor === modelData ? 2 : 1
                    border.color: swatchRow.currentColor === modelData ? theme.accent : theme.border
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            /* keep the treatment color but switch the swatch */
                            const m = root.treatmentMap
                            m.color = modelData
                            root.treatmentMap = m
                            root.commitText()
                        }
                    }
                }
            }
            Rectangle {
                width: VLCStyle.dp(22, VLCStyle.scale)
                height: width
                radius: 4
                color: swatchRow.currentColor
                border.width: 2
                border.color: theme.accent
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.commitText()
                }
            }
        }

        Item { Layout.fillWidth: true }

        Widgets.IconToolButton {
            text: VLCIcons.clear
            description: qsTr("Close editor")
            onClicked: root.treatmentMap = ({})
        }
    }
}