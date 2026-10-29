/*****************************************************************************
 * PaintbrushInspectorDock.qml : NAI Paintbrush — contextual inspector drawer
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

/* Contextual inspector drawer (A37). Closed by default; opens only when the
 * active tool/selection needs it:
 *   - a selection exists  -> layer controls + selection actions
 *   - an object is selected -> AI actions (find/remove/extract/replace/...)
 *   - pinned by the user  -> stays open
 * Layers bind to the real PaintbrushLayerModel (A38): the visibility
 * checkbox reads the layer's `visible` role, the label its `name`, the
 * opacity slider and transform spinboxes initialize from the layer's
 * opacity/transform. AI diagnostics live behind the Diagnostics action and
 * are fed by CapabilityReport-derived status strings (no status dots in the
 * creative space, S5). */
Item {
    id: root

    required property PaintbrushCanvas canvas

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    /* contextual open state: default closed, auto-opens for selections */
    property bool pinned: false
    /* Explicit user dismissal: when true the inspector stays collapsed even
     * while a selection/object is active (the fold button can always close
     * it). Cleared by the topbar toggle and when a NEW selection is made. */
    property bool manualClosed: false
    /* collapsed-by-default disclosure for legacy voice/generation actions */
    property bool legacyActionsOpen: false
    readonly property bool expanded: (canvas.hasSelection
                                     || PaintbrushCtx.objectSelected
                                     || pinned)
                                    && !manualClosed
    readonly property real drawerWidth: Math.min((parent ? parent.width : 320) * 0.26,
                                                 VLCStyle.dp(320, VLCStyle.scale))

    /* open/close requested from outside (topbar toggle) */
    function setOpen(open) {
        manualClosed = !open
        if (!open)
            pinned = false
    }

    function openDiagnostics() {
        /* the dialog is a top-level Overlay child, so it lays out against
         * the Overlay rather than this (possibly collapsed, 0-width) dock */
        diagnosticsDialog.open()
    }

    /* a NEW selection re-allows the contextual auto-open (an explicit
     * dismissal only lasts for the current selection/object) */
    Connections {
        target: canvas
        function onSelectionChanged(hasSel) {
            if (hasSel)
                manualClosed = false
        }
    }
    Connections {
        target: PaintbrushCtx
        function onObjectSelectionChanged() {
            if (PaintbrushCtx.objectSelected)
                manualClosed = false
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.expanded
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: VLCStyle.margin_small
        spacing: VLCStyle.margin_xsmall
        visible: root.expanded

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
                text: root.pinned ? qsTr("Inspector") : qsTr("Inspector (selection)")
                color: theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_large
            }
            Item { Layout.fillWidth: true }
            Widgets.IconToolButton {
                text: root.pinned ? VLCIcons.close : VLCIcons.ic_fluent_chevron_right_24
                description: root.pinned ? qsTr("Close inspector")
                                         : qsTr("Dismiss inspector")
                onClicked: {
                    /* Dismiss always works, even mid-selection: the fold
                     * button must be able to collapse the dock while
                     * hasSelection/objectSelected would otherwise keep it
                     * open. */
                    root.pinned = false
                    root.manualClosed = true
                }
            }
        }

        /* ------------------------------------------------------- layers */
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

            model: PaintbrushCtx.layerModel
            currentIndex: PaintbrushCtx.document.currentLayerIndex

            delegate: Rectangle {
                required property int index
                required property string name
                required property bool layerVisible
                required property string thumb

                width: layerList.width
                height: VLCStyle.dp(34, VLCStyle.scale)
                radius: VLCStyle.margin_xsmall
                color: PaintbrushCtx.document.currentLayerIndex === index
                       ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.15)
                       : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: VLCStyle.margin_xsmall
                    anchors.rightMargin: VLCStyle.margin_xsmall
                    spacing: VLCStyle.margin_xsmall

                    /* layer thumbnail (real raster, "" when no content) */
                    Image {
                        Layout.preferredWidth: VLCStyle.dp(28, VLCStyle.scale)
                        Layout.preferredHeight: VLCStyle.dp(22, VLCStyle.scale)
                        source: thumb
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                    }

                    /* visibility checkbox bound to the real layer state */
                    Widgets.CheckButton {
                        Layout.preferredWidth: VLCStyle.dp(22, VLCStyle.scale)
                        Layout.preferredHeight: VLCStyle.dp(22, VLCStyle.scale)
                        checked: layerVisible
                        onClicked: PaintbrushCtx.setLayerVisible(index, checked)
                    }

                    /* real layer name */
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: name
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

        /* layer controls bound to the current layer's model state: opacity
         * (new, A38) plus image/text-layer transform (scale/rotation) */
        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.LabelExt {
                text: qsTr("Opacity")
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_xsmall
            }
            Slider {
                id: opacitySlider
                Layout.fillWidth: true
                from: 0
                to: 100
                stepSize: 1
                value: PaintbrushCtx.layerModel.opacityOf(
                           PaintbrushCtx.document.currentLayerIndex) * 100
                onMoved: PaintbrushCtx.setLayerOpacity(
                             PaintbrushCtx.document.currentLayerIndex,
                             value / 100.0)
            }
            Widgets.LabelExt {
                text: Math.round(opacitySlider.value) + "%"
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_xsmall
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall

            Widgets.LabelExt {
                text: qsTr("Scale")
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_xsmall
            }
            Widgets.SpinBoxExt {
                id: layerScaleSpin
                Layout.fillWidth: true
                from: 1
                to: 400
                value: Math.round(PaintbrushCtx.layerModel.scaleOf(
                                      PaintbrushCtx.document.currentLayerIndex) * 100)
                onValueModified: PaintbrushCtx.transformLayer(
                                     PaintbrushCtx.document.currentLayerIndex,
                                     value / 100.0,
                                     layerRotSpin.value)
            }
            Widgets.LabelExt {
                text: qsTr("Rot")
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_xsmall
            }
            Widgets.SpinBoxExt {
                id: layerRotSpin
                Layout.preferredWidth: VLCStyle.dp(64, VLCStyle.scale)
                from: -180
                to: 180
                value: Math.round(PaintbrushCtx.layerModel.rotationOf(
                                      PaintbrushCtx.document.currentLayerIndex))
                onValueModified: PaintbrushCtx.transformLayer(
                                     PaintbrushCtx.document.currentLayerIndex,
                                     layerScaleSpin.value / 100.0,
                                     value)
            }
        }

        /* ---------------------------------------- living harvest (stage 3)
         * Motion group: Off/Subtle/Normal/Strong maps to photo-safe
         * amplitudes (0 / 0.35 / 0.65 / 1.0). The layer needs a rigged
         * asset first (rig it in the harvest tray). */
        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall
            /* currentLayerIndex is a notified property: it re-evaluates this
             * binding on every layer/document change, so the group appears
             * as soon as an asset is rigged */
            visible: PaintbrushCtx.riggedAssetCount() > 0 &&
                     PaintbrushCtx.document.currentLayerIndex >= 0

            Widgets.LabelExt {
                text: qsTr("Motion")
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_xsmall
            }
            ComboBox {
                id: motionCombo
                Layout.fillWidth: true
                model: [qsTr("Off"), qsTr("Subtle"), qsTr("Normal"), qsTr("Strong")]
                currentIndex: {
                    const a = PaintbrushCtx.layerPuppetAmplitude(
                                  PaintbrushCtx.document.currentLayerIndex)
                    if (!PaintbrushCtx.layerHasPuppet(
                            PaintbrushCtx.document.currentLayerIndex))
                        return 0
                    if (a <= 0.5)
                        return 1
                    if (a <= 0.85)
                        return 2
                    return 3
                }
                onActivated: {
                    const idx = PaintbrushCtx.document.currentLayerIndex
                    let assetId = PaintbrushCtx.layerPuppetAssetId(idx)
                    if (assetId <= 0)
                        assetId = PaintbrushCtx.firstRiggedAssetId()
                    if (currentIndex === 0)
                        PaintbrushCtx.removeLayerPuppet(idx)
                    else if (assetId > 0)
                        PaintbrushCtx.setLayerPuppet(idx, assetId,
                            [0.35, 0.65, 1.0][currentIndex - 1])
                }
            }
        }

        /* Before/After moment: the static poster next to the living loop.
         * The canvas rests at exactly the exported frame 0 while compared. */
        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall
            visible: PaintbrushCtx.riggedAssetCount() > 0 &&
                     PaintbrushCtx.document.currentLayerIndex >= 0

            Widgets.LabelExt {
                text: qsTr("Compare")
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_xsmall
            }
            Switch {
                id: compareSwitch
                Layout.fillWidth: true
                text: compareSwitch.checked ? qsTr("Static poster") : qsTr("Living loop")
                checked: PaintbrushCtx.livingPreviewPaused()
                onToggled: PaintbrushCtx.setLivingPreviewPaused(checked)
            }
        }

        /* ------------------------------------------ selection (contextual) */
        Widgets.LabelExt {
            text: qsTr("Selection")
            color: theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
            visible: canvas.hasSelection
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xsmall
            visible: canvas.hasSelection

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
                text: VLCIcons.fullscreen
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
                text: VLCIcons.ic_fluent_arrow_move
                description: qsTr("Crop to selection")
                enabled: canvas.hasSelection
                onClicked: canvas.cropToSelection()
            }
        }

        /* ------------------------------ AI actions (object / selection) */
            /* Legacy voice/generation actions are collapsed by default so the
             * primary Inspector stays clean after a plain Rect Select; the
             * product verbs (Keep / Boost / Add to canvas / Export object / Cut)
             * live on the canvas bars, not here. */
            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall
                visible: PaintbrushCtx.objectSelected || canvas.hasSelection
                Widgets.IconToolButton {
                    text: root.legacyActionsOpen ? VLCIcons.chevron_down
                                                 : VLCIcons.ic_fluent_chevron_right_24
                    description: qsTr("AI actions")
                    checkable: true
                    checked: root.legacyActionsOpen
                    onClicked: root.legacyActionsOpen = !root.legacyActionsOpen
                }
                Widgets.LabelExt {
                    text: qsTr("More AI actions")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_small
                    Layout.fillWidth: true
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xxxsmall
                visible: (PaintbrushCtx.objectSelected || canvas.hasSelection)
                         && root.legacyActionsOpen

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
                text: qsTr("Extract object…")
                enabled: PaintbrushCtx.objectSelected
                onClicked: PaintbrushCtx.extractObject()
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
                onClicked: PaintbrushCtx.findProduct()
            }
            Widgets.ActionButtonPrimary {
                Layout.fillWidth: true
                text: qsTr("Auto collage…")
                onClicked: collageDialog.open()
            }

            /* freehand mask refinement hint (Object Select tool) */
            Widgets.LabelExt {
                Layout.fillWidth: true
                text: qsTr("Object Select: drag on the mask to paint it in, Alt-drag or right-drag to erase it. Brush size is used.")
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_xsmall
                wrapMode: Text.WordWrap
                visible: PaintbrushCtx.objectSelected
            }

            /* object mask edge mode (Hard = crisp boundary, Soft = feathered alpha) */
            RowLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xsmall
                visible: PaintbrushCtx.objectSelected

                Widgets.LabelExt {
                    text: qsTr("Edge")
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_xsmall
                    Layout.fillWidth: true
                }
                Widgets.IconToolButton {
                    text: PaintbrushCtx.objectMaskSoftEdge
                          ? VLCIcons.ic_fluent_arrow_sync_24_regular
                          : VLCIcons.fullscreen
                    description: qsTr("Soft edge (feathered alpha)")
                    checkable: true
                    checked: PaintbrushCtx.objectMaskSoftEdge
                    onClicked: PaintbrushCtx.objectMaskSoftEdge
                               ? PaintbrushCtx.setObjectMaskEdgeHard()
                               : PaintbrushCtx.setObjectMaskEdgeSoft()
                }
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
                            font.pixelSize: VLCStyle.fontSize_xsmall
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

        /* "Find product" result */
        Rectangle {
            Layout.fillWidth: true
            radius: VLCStyle.margin_xsmall
            color: Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.08)
            visible: PaintbrushCtx.lookupMatches.length > 0

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: VLCStyle.margin_small
                spacing: VLCStyle.margin_xxxsmall

                Repeater {
                    model: PaintbrushCtx.lookupMatches
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Widgets.LabelExt {
                            Layout.fillWidth: true
                            text: modelData.title
                            color: theme.fg.primary
                            font.pixelSize: VLCStyle.fontSize_small
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }
                        Widgets.LabelExt {
                            Layout.fillWidth: true
                            text: modelData.description
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_xsmall
                            wrapMode: Text.WordWrap
                        }
                        Widgets.LabelExt {
                            Layout.fillWidth: true
                            text: {
                                const md = modelData.metadata
                                let parts = []
                                if (md && md.category) parts.push(md.category)
                                if (md && md.brand) parts.push(md.brand)
                                if (md && md.model) parts.push(md.model)
                                if (modelData.confidence > 0)
                                    parts.push(qsTr("%1% confident")
                                               .arg(Math.round(modelData.confidence * 100)))
                                return parts.join(" · ")
                            }
                            visible: text.length > 0
                            color: theme.accent
                            font.pixelSize: VLCStyle.fontSize_xsmall
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }

        /* diagnostics action (A37): status dots are not part of the
         * creative space — they live in the diagnostics dialog, fed by
         * CapabilityReport-derived strings (S5) */
        Widgets.IconToolButton {
            Layout.alignment: Qt.AlignRight
            text: VLCIcons.settings
            description: qsTr("AI diagnostics")
            onClicked: diagnosticsDialog.open()
        }
    }

    Dialog {
        id: diagnosticsDialog
        title: qsTr("NAI Paintbrush diagnostics")
        modal: true
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        /* layout against the top-level Overlay, never this (possibly collapsed
         * to 0-width, i.e. when there is no selection) inspector dock */
        width: Overlay.overlay
               ? Math.min(Overlay.overlay.width - VLCStyle.margin_large * 2,
                          VLCStyle.dp(520, VLCStyle.scale))
               : VLCStyle.dp(520, VLCStyle.scale)

        contentItem: ColumnLayout {
            width: parent.width

            /* capability truth (S5): one row per AI feature, status text
             * derived from CapabilityReport — the dot color is green only
             * when the capability is genuinely configured */
            ColumnLayout {
                Layout.fillWidth: true
                spacing: VLCStyle.margin_xxxsmall

                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: PaintbrushCtx.segmentationAvailable ? "limegreen" : "#c33"
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Object Select — %1").arg(PaintbrushCtx.segmentationStatusText)
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_xsmall
                        wrapMode: Text.WordWrap
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: PaintbrushCtx.visionConfigured ? "limegreen" : "#c33"
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Ask about this — %1").arg(PaintbrushCtx.visionStatusText)
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_xsmall
                        wrapMode: Text.WordWrap
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: VLCStyle.margin_xsmall
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: PaintbrushCtx.generationConfigured ? "limegreen" : "#c33"
                    }
                    Widgets.LabelExt {
                        Layout.fillWidth: true
                        text: qsTr("Image generation — %1").arg(PaintbrushCtx.generationStatusText)
                        color: theme.fg.secondary
                        font.pixelSize: VLCStyle.fontSize_xsmall
                        wrapMode: Text.WordWrap
                    }
                }
            }

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
        width: Math.min(Overlay.overlay ? Overlay.overlay.width : 640, VLCStyle.dp(480, VLCStyle.scale))
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        standardButtons: Dialog.Ok | Dialog.Cancel

        contentItem: ColumnLayout {
            spacing: VLCStyle.margin_small
            Widgets.LabelExt {
                text: qsTr("Describe the object, e.g. “handbag”, “red lipstick”, “microphone”")
                color: theme.fg.secondary
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
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
        width: Math.min(Overlay.overlay ? Overlay.overlay.width : 640, VLCStyle.dp(480, VLCStyle.scale))
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        standardButtons: Dialog.Ok | Dialog.Cancel

        contentItem: ColumnLayout {
            spacing: VLCStyle.margin_small
            Widgets.LabelExt {
                text: qsTr("Describe the new background (empty = gentle gradient)")
                color: theme.fg.secondary
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
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
        width: Math.min(Overlay.overlay ? Overlay.overlay.width : 640, VLCStyle.dp(480, VLCStyle.scale))
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        standardButtons: Dialog.Ok | Dialog.Cancel

        contentItem: ColumnLayout {
            spacing: VLCStyle.margin_small
            Widgets.LabelExt {
                text: qsTr("What should fill the selection?")
                color: theme.fg.secondary
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
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
        width: Math.min(Overlay.overlay ? Overlay.overlay.width : 640, VLCStyle.dp(480, VLCStyle.scale))
        x: Overlay.overlay ? Math.round((Overlay.overlay.width - width) / 2) : 0
        y: Overlay.overlay ? Math.round((Overlay.overlay.height - height) / 2) : 0
        standardButtons: Dialog.Close

        contentItem: ColumnLayout {
            spacing: VLCStyle.margin_small

            Widgets.LabelExt {
                text: qsTr("Arrange the content layers into a layout. Every piece stays an editable layer.")
                color: theme.fg.secondary
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
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