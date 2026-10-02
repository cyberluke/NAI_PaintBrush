/*****************************************************************************
 * PaintbrushInspectorDock.qml : NAI Paintbrush — layers & AI actions
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

/* Right-hand dock: layers list, object actions, AI availability and
 * diagnostics. Collapsible via the header. */
Item {
    id: root

    required property PaintbrushCanvas canvas

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    property bool expanded: true

    Rectangle {
        anchors.fill: parent
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: VLCStyle.margin_small
        spacing: VLCStyle.margin_xsmall

        /* header */
        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.IconLabel {
                text: VLCIcons.list
                color: theme.accent
                font.pixelSize: VLCStyle.fontSize_large
            }
            Widgets.LabelExt {
                text: qsTr("Inspector")
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_large
            }
            Item { Layout.fillWidth: true }
            Widgets.IconToolButton {
                text: root.expanded ? VLCIcons.ic_fluent_chevron_down_24 : VLCIcons.ic_fluent_chevron_up_24
                description: qsTr("Collapse inspector")
                onClicked: root.expanded = !root.expanded
            }
        }

        /* layers */
        Widgets.LabelExt {
            text: qsTr("Layers")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }

        ListView {
            id: layerList

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: VLCStyle.margin_xxxsmall

            property int currentIndex: PaintbrushCtx.document.currentLayerIndex

            model: PaintbrushCtx.document.layerCount

            delegate: Rectangle {
                required property int index

                width: layerList.width
                height: VLCStyle.dp(30, VLCStyle.scale)
                radius: VLCStyle.margin_xsmall
                color: PaintbrushCtx.document.currentLayerIndex === index
                       ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.15)
                       : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: VLCStyle.margin_xsmall
                    anchors.rightMargin: VLCStyle.margin_xsmall
                    spacing: VLCStyle.margin_xsmall

                    Widgets.CheckButton {
                        Layout.preferredWidth: VLCStyle.dp(22, VLCStyle.scale)
                        Layout.preferredHeight: VLCStyle.dp(22, VLCStyle.scale)
                        checked: true
                        onClicked: PaintbrushCtx.setLayerVisible(index, checked)
                    }

                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Layer %1").arg(index + 1)
                        color: theme.fg.primary
                        font.pixelSize: VLCStyle.fontSize_small
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: PaintbrushCtx.setCurrentLayer(index)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.IconToolButton {
                text: VLCIcons.add
                description: qsTr("New layer")
                onClicked: PaintbrushCtx.newLayer()
            }
            Widgets.IconToolButton {
                text: VLCIcons.del
                description: qsTr("Delete layer")
                enabled: PaintbrushCtx.document.layerCount > 1
                onClicked: PaintbrushCtx.deleteLayer(layerList.currentIndex)
            }
            Widgets.IconToolButton {
                text: VLCIcons.ic_fluent_arrow_sync_24_regular
                description: qsTr("Duplicate layer")
                onClicked: PaintbrushCtx.duplicateLayer(layerList.currentIndex)
            }
            Widgets.IconToolButton {
                text: VLCIcons.ic_fluent_chevron_up_24
                description: qsTr("Move layer up")
                onClicked: PaintbrushCtx.moveLayer(layerList.currentIndex, -1)
            }
            Widgets.IconToolButton {
                text: VLCIcons.ic_fluent_chevron_down_24
                description: qsTr("Move layer down")
                onClicked: PaintbrushCtx.moveLayer(layerList.currentIndex, 1)
            }
        }

        Widgets.LabelExt {
            text: qsTr("Selection")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.IconToolButton {
                text: VLCIcons.ic_fluent_arrow_sync_24_regular
                description: qsTr("Invert selection")
                enabled: canvas.hasSelection
                onClicked: canvas.selectionInvert()
            }
            Widgets.IconToolButton {
                text: VLCIcons.add
                description: qsTr("Grow selection")
                enabled: canvas.hasSelection
                onClicked: canvas.selectionGrow(4)
            }
            Widgets.IconToolButton {
                text: VLCIcons.remove
                description: qsTr("Shrink selection")
                enabled: canvas.hasSelection
                onClicked: canvas.selectionShrink(4)
            }
            Widgets.IconToolButton {
                text: VLCIcons.ic_fluent_arrow_sync_24_regular
                description: qsTr("Feather selection")
                enabled: canvas.hasSelection
                onClicked: canvas.selectionFeather(4)
            }
            Widgets.IconToolButton {
                text: VLCIcons.ok
                description: qsTr("Fill holes")
                enabled: canvas.hasSelection
                onClicked: canvas.selectionFillHoles()
            }
            Widgets.IconToolButton {
                text: VLCIcons.ic_fluent_full_screen_maximize
                description: qsTr("Smooth selection")
                enabled: canvas.hasSelection
                onClicked: canvas.selectionSmooth()
            }
            Widgets.IconToolButton {
                text: VLCIcons.remove
                description: qsTr("Clear selection")
                enabled: canvas.hasSelection
                onClicked: canvas.selectionClear()
            }
            Widgets.IconToolButton {
                text: VLCIcons.ic_fluent_full_screen_maximize
                description: qsTr("Crop to selection")
                enabled: canvas.hasSelection
                onClicked: canvas.cropToSelection()
            }
        }

        /* AI actions */
        Widgets.LabelExt {
            text: qsTr("AI actions")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xxxsmall

            Widgets.ActionButtonPrimary {
                Layout.fillWidth: true
                text: qsTr("Find by words…")
                enabled: PaintbrushCtx.segmentationAvailable
                onClicked: textPromptDialog.open()
            }
            Widgets.ActionButtonPrimary {
                Layout.fillWidth: true
                text: qsTr("Remove object")
                enabled: PaintbrushCtx.objectSelected && PaintbrushCtx.generationConfigured
                onClicked: PaintbrushCtx.removeObject("")
            }
            Widgets.ActionButtonPrimary {
                Layout.fillWidth: true
                text: qsTr("Replace background…")
                enabled: PaintbrushCtx.objectSelected && PaintbrushCtx.generationConfigured
                onClicked: bgPromptDialog.open()
            }
            Widgets.ActionButtonPrimary {
                Layout.fillWidth: true
                text: qsTr("Generative fill…")
                enabled: canvas.hasSelection && PaintbrushCtx.generationConfigured
                onClicked: fillPromptDialog.open()
            }
            Widgets.ActionButtonPrimary {
                Layout.fillWidth: true
                text: qsTr("Find product…")
                enabled: PaintbrushCtx.lookupAvailable && PaintbrushCtx.objectSelected
                onClicked: PaintbrushCtx.companionAsk(
                               qsTr("What brand or product is the selected object, if the visual evidence supports it?"))
            }
            Widgets.ActionButtonPrimary {
                Layout.fillWidth: true
                text: qsTr("Auto collage…")
                onClicked: collageDialog.open()
            }
        }

        /* "Ask about this" result */
        Rectangle {
            Layout.fillWidth: true
            radius: VLCStyle.margin_xsmall
            color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.08)
            visible: PaintbrushCtx.visionHasResult

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: VLCStyle.margin_small
                spacing: VLCStyle.margin_xsmall

                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall

                    /* object thumbnail */
                    Image {
                        Layout.preferredWidth: VLCStyle.dp(44, VLCStyle.scale)
                        Layout.preferredHeight: VLCStyle.dp(44, VLCStyle.scale)
                        source: PaintbrushCtx.objectSelected
                                ? PaintbrushCtx.objectThumbnailDataUrl
                                : ""
                        fillMode: Image.PreserveAspectFit
                        visible: PaintbrushCtx.objectSelected
                        asynchronous: true
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Widgets.LabelExt {
                            text: qsTr("Likely identification")
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_xxxsmall
                        }
                        Widgets.LabelExt {
                            Layout.fillWidth: true
                            text: PaintbrushCtx.visionAnswer
                            color: theme.fg.primary
                            font.pixelSize: VLCStyle.fontSize_small
                            wrapMode: Text.WordWrap
                        }
                        Widgets.LabelExt {
                            Layout.fillWidth: true
                            text: PaintbrushCtx.visionEvidence
                            visible: PaintbrushCtx.visionEvidence.length > 0
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_xsmall
                            wrapMode: Text.WordWrap
                        }
                        Widgets.LabelExt {
                            Layout.fillWidth: true
                            text: PaintbrushCtx.visionUncertainty
                            visible: PaintbrushCtx.visionUncertainty.length > 0
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_xsmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xxxsmall

                    Button {
                        text: qsTr("Copy")
                        onClicked: PaintbrushCtx.copyVisionAnswer()
                    }
                    Button {
                        text: qsTr("Add as text")
                        onClicked: PaintbrushCtx.addVisionAnswerAsText()
                    }
                    Button {
                        text: qsTr("Clear")
                        onClicked: PaintbrushCtx.clearVisionResult()
                    }
                }
            }
        }

        /* AI availability + diagnostics */
        Widgets.LabelExt {
            text: qsTr("AI")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xxxsmall

            RowLayout {
                Layout.fillWidth: true
                Rectangle {
                    width: 8; height: 8; radius: 4
                    color: PaintbrushCtx.segmentationAvailable ? "limegreen" : "#c33"
                }
                Widgets.LabelExt {
                    Layout.fillWidth: true
                    text: PaintbrushCtx.segmentationAvailable
                          ? qsTr("Object Select ready")
                          : qsTr("Object Select unavailable")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_xsmall
                    wrapMode: Text.WordWrap
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Rectangle {
                    width: 8; height: 8; radius: 4
                    color: PaintbrushCtx.visionConfigured ? "limegreen" : "#c33"
                }
                Widgets.LabelExt {
                    Layout.fillWidth: true
                    text: PaintbrushCtx.visionConfigured
                          ? qsTr("Ask about this ready")
                          : PaintbrushCtx.visionUnavailableReason
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_xsmall
                    wrapMode: Text.WordWrap
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Rectangle {
                    width: 8; height: 8; radius: 4
                    color: PaintbrushCtx.generationConfigured ? "limegreen" : "#c33"
                }
                Widgets.LabelExt {
                    Layout.fillWidth: true
                    text: PaintbrushCtx.generationConfigured
                          ? qsTr("Image generation ready")
                          : PaintbrushCtx.generationUnavailableReason
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_xsmall
                    wrapMode: Text.WordWrap
                }
            }
        }

        Widgets.IconToolButton {
            Layout.alignment: Qt.AlignRight
            text: VLCIcons.info
            description: qsTr("Diagnostics")
            onClicked: diagnosticsDialog.open()
        }
    }

    Dialog {
        id: diagnosticsDialog
        title: qsTr("NAI Paintbrush diagnostics")
        modal: true
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        width: Math.min(parent.width - VLCStyle.margin_large * 2, VLCStyle.dp(520, VLCStyle.scale))

        contentItem: ColumnLayout {
            width: parent.width

            ScrollView {
                Layout.fillWidth: true
                Layout.preferredHeight: VLCStyle.dp(260, VLCStyle.scale)
                TextEdit {
                    text: PaintbrushCtx.diagnosticsText
                    readOnly: true
                    font.family: "monospace"
                    font.pixelSize: VLCStyle.fontSize_xsmall
                    color: theme.fg.primary
                    wrapMode: Text.WrapAnywhere
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall

                Button {
                    text: qsTr("Copy")
                    onClicked: PaintbrushCtx.copyDiagnostics()
                }
                Button {
                    text: qsTr("Clear AI cache")
                    onClicked: PaintbrushCtx.clearAiCache()
                }
                Button {
                    text: qsTr("Unload AI models")
                    onClicked: PaintbrushCtx.unloadAiModels()
                }
                Button {
                    text: qsTr("Reveal autosave")
                    onClicked: Qt.openUrlExternally(
                                   "file:///" + encodeURIComponent(PaintbrushCtx.autosavePath()))
                }
            }
        }
    }

    Dialog {
        id: textPromptDialog
        title: qsTr("Find by words")
        modal: true
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        standardButtons: Dialog.Ok | Dialog.Cancel

        contentItem: ColumnLayout {
            spacing: VLCStyle.margin_small
            Widgets.LabelExt {
                text: qsTr("Describe the object, e.g. “handbag”, “red lipstick”, “microphone”")
                color: theme.fg.secondary
                wrapMode: Text.WordWrap
            }
            TextField {
                id: textPromptField
                Layout.fillWidth: true
                placeholderText: qsTr("e.g. person wearing the blue jacket")
            }
        }

        onAccepted: PaintbrushCtx.refineObjectByText(textPromptField.text)
    }

    Dialog {
        id: bgPromptDialog
        title: qsTr("Replace background")
        modal: true
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        standardButtons: Dialog.Ok | Dialog.Cancel

        contentItem: ColumnLayout {
            spacing: VLCStyle.margin_small
            Widgets.LabelExt {
                text: qsTr("Describe the new background (empty = gentle gradient)")
                color: theme.fg.secondary
                wrapMode: Text.WordWrap
            }
            TextField {
                id: bgPromptField
                Layout.fillWidth: true
                placeholderText: qsTr("e.g. sunset beach")
            }
        }

        onAccepted: PaintbrushCtx.replaceBackground(bgPromptField.text)
    }

    Dialog {
        id: fillPromptDialog
        title: qsTr("Generative fill")
        modal: true
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        standardButtons: Dialog.Ok | Dialog.Cancel

        contentItem: ColumnLayout {
            spacing: VLCStyle.margin_small
            Widgets.LabelExt {
                text: qsTr("What should fill the selection?")
                color: theme.fg.secondary
                wrapMode: Text.WordWrap
            }
            TextField {
                id: fillPromptField
                Layout.fillWidth: true
                placeholderText: qsTr("e.g. a field of flowers")
            }
        }

        onAccepted: PaintbrushCtx.generativeFill(fillPromptField.text)
    }

    Dialog {
        id: collageDialog
        title: qsTr("Auto collage")
        modal: true
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        standardButtons: Dialog.Close

        contentItem: ColumnLayout {
            spacing: VLCStyle.margin_small

            Widgets.LabelExt {
                text: qsTr("Arrange the content layers into a layout. Every piece stays an editable layer.")
                color: theme.fg.secondary
                wrapMode: Text.WordWrap
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                rowSpacing: VLCStyle.margin_xsmall
                columnSpacing: VLCStyle.margin_xsmall

                Repeater {
                    model: [
                        { label: qsTr("Free scatter"), preset: 0 },
                        { label: qsTr("Balanced grid"), preset: 1 },
                        { label: qsTr("Editorial"), preset: 2 },
                        { label: qsTr("Scrapbook"), preset: 3 },
                        { label: qsTr("Vertical poster"), preset: 4 },
                        { label: qsTr("Horizontal banner"), preset: 5 }
                    ]
                    Button {
                        Layout.fillWidth: true
                        text: modelData.label
                        onClicked: {
                            PaintbrushCtx.autoCollage(modelData.preset)
                            collageDialog.close()
                        }
                    }
                }
            }
        }
    }
}