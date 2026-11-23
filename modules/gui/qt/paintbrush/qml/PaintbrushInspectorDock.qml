/*****************************************************************************
 * PaintbrushInspectorDock.qml : NAI Paintbrush — inspector drawer
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

/* Inspector drawer — rewritten as a real tool panel.
 *
 * The old layout let the layer list consume all remaining height and stacked
 * every AI verb as a full-width primary button below it, so the bottom of the
 * panel fell off the window with no way to scroll. This version:
 *   - keeps the header and the Layers/AI switch sticky;
 *   - puts each tab's body in a ScrollView with an ALWAYS-VISIBLE scrollbar;
 *   - gives the layer list its own bounded height + scrollbar (many layers
 *     never inflate the panel);
 *   - groups controls into collapsible sections (Transform, Motion, Matte,
 *     Subject library, Advanced matte);
 *   - restyles actions as compact neutral rows; orange is reserved for the
 *     one primary action instead of a wall of orange buttons.
 *
 * Open/close API is unchanged (pinned / manualClosed / expanded / setOpen /
 * drawerWidth) so the workspace topbar toggle keeps working. */

Item {
    id: root

    required property PaintbrushCanvas canvas

    ColorContext {
        palette: VLCStyle.palette
        id: theme
        colorSet: ColorContext.View
    }

    /* open state: pinned (user-opened) by default so the layer stack is
     * visible at launch; a selection/object also auto-opens it */
    property bool pinned: true
    property bool manualClosed: false
    /* 0 = Layers, 1 = AI. Selecting an object switches to AI automatically. */
    property int tabIndex: 0
    /* collapsed state of the disclosure sections */
    property bool transformOpen: true
    property bool motionOpen: true
    property bool matteAdvancedOpen: false
    property bool subjectLibraryOpen: false

    readonly property bool expanded: (canvas.hasSelection
                                     || PaintbrushCtx.objectSelected
                                     || pinned)
                                    && !manualClosed
    readonly property real drawerWidth: Math.min((parent ? parent.width : 360) * 0.28,
                                                 VLCStyle.dp(360, VLCStyle.scale))

    function setOpen(open) {
        manualClosed = !open
        if (open)
            pinned = true
        else
            pinned = false
    }

    /* ask the workspace to open NAI Settings (its diagnostics section) */
    signal requestSettings()

    Connections {
        target: canvas
        function onSelectionChanged(hasSel) {
            if (hasSel) {
                manualClosed = false
                root.tabIndex = 1
            }
        }
    }
    Connections {
        target: PaintbrushCtx
        function onObjectSelectionChanged() {
            if (PaintbrushCtx.objectSelected) {
                manualClosed = false
                root.tabIndex = 1
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.expanded
        color: theme.bg.primary
        border.width: 1
        border.color: theme.border
    }

    /* The canvas is behind the drawer; a wheel over non-scrolling chrome must
     * not zoom the canvas. ScrollViews/controls above handle their own wheel. */
    MouseArea {
        anchors.fill: parent
        visible: root.expanded
        onWheel: function(wheel) { wheel.accepted = true }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: VLCStyle.margin_small
        spacing: VLCStyle.margin_xsmall
        visible: root.expanded

        /* ------------------------------------------------------- header */
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
                Layout.fillWidth: true
            }
            Widgets.IconToolButton {
                text: VLCIcons.settings
                description: qsTr("NAI Settings (diagnostics)")
                onClicked: root.requestSettings()
            }
            Widgets.IconToolButton {
                text: root.pinned ? VLCIcons.close : VLCIcons.ic_fluent_chevron_right_24
                description: root.pinned ? qsTr("Close inspector")
                                         : qsTr("Dismiss inspector")
                onClicked: {
                    root.pinned = false
                    root.manualClosed = true
                }
            }
        }

        /* --------------------------------------------------------- tabs */
        RowLayout {
            Layout.fillWidth: true
            spacing: VLCStyle.margin_xxxsmall

            DockTab {
                label: qsTr("Layers")
                selected: root.tabIndex === 0
                onClicked: root.tabIndex = 0
            }
            DockTab {
                label: qsTr("AI")
                selected: root.tabIndex === 1
                onClicked: root.tabIndex = 1
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: theme.border
        }

        /* -------------------------------------------------------- body */
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.tabIndex

            /* ============================== LAYERS ==================== */
            ScrollView {
                id: layersScroll

                clip: true
                contentWidth: availableWidth
                ScrollBar.vertical.policy: ScrollBar.AlwaysOn
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: layersScroll.availableWidth
                    spacing: VLCStyle.margin_xsmall

                    /* bounded layer list: its own scrollbar prevents layer
                     * growth from pushing the controls out of the panel */
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Math.min(
                            VLCStyle.dp(184, VLCStyle.scale),
                            VLCStyle.dp(40, VLCStyle.scale)
                                * Math.max(1, layerList.count)
                            + 2 * VLCStyle.margin_xxxsmall
                            + VLCStyle.dp(8, VLCStyle.scale))
                        radius: VLCStyle.margin_xsmall
                        color: theme.bg.secondary
                        border.width: 1
                        border.color: theme.border

                        ListView {
                            id: layerList

                            anchors.fill: parent
                            anchors.margins: VLCStyle.margin_xxxsmall
                            clip: true
                            spacing: VLCStyle.margin_xxxsmall
                            model: PaintbrushCtx.layerModel
                            currentIndex: PaintbrushCtx.document.currentLayerIndex
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }

                            delegate: Rectangle {
                                id: layerRow

                                required property int index
                                required property string name
                                required property bool layerVisible
                                required property string thumb

                                width: layerList.width - VLCStyle.dp(12, VLCStyle.scale)
                                height: VLCStyle.dp(40, VLCStyle.scale)
                                radius: VLCStyle.margin_xsmall
                                color: PaintbrushCtx.document.currentLayerIndex === index
                                       ? Qt.rgba(theme.accent.r, theme.accent.g,
                                                 theme.accent.b, 0.16)
                                       : (layerHover.containsMouse
                                          ? theme.bg.primary : "transparent")

                                MouseArea {
                                    id: layerHover
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: PaintbrushCtx.setCurrentLayer(index)
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: VLCStyle.margin_xsmall
                                    anchors.rightMargin: VLCStyle.margin_xsmall
                                    spacing: VLCStyle.margin_xsmall

                                    Image {
                                        Layout.preferredWidth: VLCStyle.dp(28, VLCStyle.scale)
                                        Layout.preferredHeight: VLCStyle.dp(22, VLCStyle.scale)
                                        source: thumb
                                        fillMode: Image.PreserveAspectFit
                                        asynchronous: true
                                    }

                                    Widgets.CheckBoxExt {
                                        Layout.preferredWidth: VLCStyle.dp(22, VLCStyle.scale)
                                        Layout.preferredHeight: VLCStyle.dp(22, VLCStyle.scale)
                                        text: ""
                                        tristate: false
                                        checked: layerVisible
                                        onClicked: PaintbrushCtx.setLayerVisible(index,
                                                                                !layerVisible)
                                        ToolTip.visible: hovered
                                        ToolTip.text: layerVisible ? qsTr("Hide layer")
                                                                   : qsTr("Show layer")
                                    }

                                    Widgets.LabelExt {
                                        Layout.fillWidth: true
                                        text: name
                                        color: theme.fg.primary
                                        font.pixelSize: VLCStyle.fontSize_normal
                                        elide: Text.ElideRight
                                    }

                                    Widgets.IconLabel {
                                        Layout.preferredWidth: VLCStyle.dp(20, VLCStyle.scale)
                                        Layout.alignment: Qt.AlignVCenter
                                        text: VLCIcons.ic_fluent_arrow_move
                                        color: theme.fg.secondary
                                        font.pixelSize: VLCStyle.fontSize_small

                                        DragHandler {
                                            target: null
                                            xAxis.enabled: false
                                            onActiveChanged: if (!active) {
                                                const dy = Math.round(activeTranslation.y
                                                                       / Math.max(1, layerRow.height))
                                                const to = Math.max(0, Math.min(layerList.count - 1,
                                                                               index + dy))
                                                if (to !== index)
                                                    PaintbrushCtx.moveLayerTo(index, to)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: VLCStyle.margin_xxxsmall

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

                    /* opacity */
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: VLCStyle.margin_xsmall

                        Widgets.LabelExt {
                            text: qsTr("Opacity")
                            color: theme.fg.secondary
                            font.pixelSize: VLCStyle.fontSize_small
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
                            font.pixelSize: VLCStyle.fontSize_small
                            Layout.preferredWidth: VLCStyle.dp(34, VLCStyle.scale)
                        }
                    }

                    /* Object-cutout mask tools: only that layer role keeps an
                     * editable native mask (committed SAM artifacts can be
                     * cleaned with the same brush panel). */
                    DockSection {
                        title: qsTr("Object mask")
                        open: true
                        visible: PaintbrushCtx.selectedLayerMaskEditable

                        InspectorAction {
                            label: qsTr("Edit mask…")
                            iconText: VLCIcons.fullscreen
                            onClicked: PaintbrushCtx.beginLayerMaskEdit()
                        }
                        InspectorAction {
                            label: qsTr("Reset mask")
                            iconText: VLCIcons.ic_fluent_arrow_sync_24_regular
                            onClicked: PaintbrushCtx.resetLayerMask()
                        }
                    }

                    /* transform + position, collapsed as one section */
                    DockSection {
                        title: qsTr("Transform")
                        open: root.transformOpen

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: VLCStyle.margin_xsmall

                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: VLCStyle.margin_xsmall
                                rowSpacing: VLCStyle.margin_xxxsmall

                                Widgets.LabelExt {
                                    text: qsTr("Scale")
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
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
                                    text: qsTr("Rotate")
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
                                }
                                Widgets.SpinBoxExt {
                                    id: layerRotSpin
                                    Layout.fillWidth: true
                                    from: -180
                                    to: 180
                                    value: Math.round(PaintbrushCtx.layerModel.rotationOf(
                                                          PaintbrushCtx.document.currentLayerIndex))
                                    onValueModified: PaintbrushCtx.transformLayer(
                                                         PaintbrushCtx.document.currentLayerIndex,
                                                         layerScaleSpin.value / 100.0,
                                                         value)
                                }

                                Widgets.LabelExt {
                                    text: qsTr("X")
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
                                }
                                Widgets.SpinBoxExt {
                                    id: layerXSpin
                                    Layout.fillWidth: true
                                    from: -100000
                                    to: 100000
                                    value: Math.round(PaintbrushCtx.layerPositionOf(
                                                          PaintbrushCtx.document.currentLayerIndex).x)
                                    onValueModified: PaintbrushCtx.setLayerPosition(
                                                         PaintbrushCtx.document.currentLayerIndex,
                                                         value, layerYSpin.value)
                                }

                                Widgets.LabelExt {
                                    text: qsTr("Y")
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
                                }
                                Widgets.SpinBoxExt {
                                    id: layerYSpin
                                    Layout.fillWidth: true
                                    from: -100000
                                    to: 100000
                                    value: Math.round(PaintbrushCtx.layerPositionOf(
                                                          PaintbrushCtx.document.currentLayerIndex).y)
                                    onValueModified: PaintbrushCtx.setLayerPosition(
                                                         PaintbrushCtx.document.currentLayerIndex,
                                                         layerXSpin.value, value)
                                }
                            }
                        }
                    }

                    /* living motion (only when a rigged asset exists) */
                    DockSection {
                        title: qsTr("Motion")
                        open: root.motionOpen
                        visible: PaintbrushCtx.riggedAssetCount() > 0
                                 && PaintbrushCtx.document.currentLayerIndex >= 0

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: VLCStyle.margin_xsmall

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: VLCStyle.margin_xsmall

                                Widgets.LabelExt {
                                    text: qsTr("Amount")
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
                                }
                                ComboBox {
                                    id: motionCombo
                                    Layout.fillWidth: true
                                    model: [qsTr("Off"), qsTr("Subtle"),
                                            qsTr("Normal"), qsTr("Strong")]
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

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: VLCStyle.margin_xsmall

                                Widgets.LabelExt {
                                    text: qsTr("Compare")
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
                                }
                                Switch {
                                    id: compareSwitch
                                    Layout.fillWidth: true
                                    text: compareSwitch.checked ? qsTr("Static poster")
                                                                : qsTr("Living loop")
                                    checked: PaintbrushCtx.livingPreviewPaused()
                                    onToggled: PaintbrushCtx.setLivingPreviewPaused(checked)
                                }
                            }
                        }
                    }

                    /* selection refinement (only when a selection exists) */
                    DockSection {
                        title: qsTr("Selection")
                        open: true
                        visible: canvas.hasSelection

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: VLCStyle.margin_xsmall

                            GridLayout {
                                Layout.fillWidth: true
                                columns: 4
                                rowSpacing: VLCStyle.margin_xxxsmall
                                columnSpacing: VLCStyle.margin_xxxsmall

                                Widgets.IconToolButton {
                                    text: VLCIcons.ic_fluent_arrow_sync_24_regular
                                    description: qsTr("Invert selection")
                                    onClicked: canvas.selectionInvert()
                                }
                                Widgets.IconToolButton {
                                    text: VLCIcons.add
                                    description: qsTr("Grow selection")
                                    onClicked: canvas.selectionGrow(4)
                                }
                                Widgets.IconToolButton {
                                    text: VLCIcons.remove
                                    description: qsTr("Shrink selection")
                                    onClicked: canvas.selectionShrink(4)
                                }
                                Widgets.IconToolButton {
                                    text: VLCIcons.ic_fluent_arrow_sync_24_regular
                                    description: qsTr("Feather selection")
                                    onClicked: canvas.selectionFeather(4)
                                }
                                Widgets.IconToolButton {
                                    text: VLCIcons.ok
                                    description: qsTr("Fill holes")
                                    onClicked: canvas.selectionFillHoles()
                                }
                                Widgets.IconToolButton {
                                    text: VLCIcons.fullscreen
                                    description: qsTr("Smooth selection")
                                    onClicked: canvas.selectionSmooth()
                                }
                                Widgets.IconToolButton {
                                    text: VLCIcons.remove
                                    description: qsTr("Clear selection")
                                    onClicked: canvas.selectionClear()
                                }
                                Widgets.IconToolButton {
                                    text: VLCIcons.ic_fluent_arrow_move
                                    description: qsTr("Crop to selection")
                                    onClicked: canvas.cropToSelection()
                                }
                            }
                        }
                    }
                }
            }

            /* ================================ AI ====================== */
            ScrollView {
                id: aiScroll

                clip: true
                contentWidth: availableWidth
                ScrollBar.vertical.policy: ScrollBar.AlwaysOn
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: aiScroll.availableWidth
                    spacing: VLCStyle.margin_xsmall

                    /* first step: nothing selected yet */
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: emptyBody.implicitHeight
                                         + 2 * VLCStyle.margin_small
                        radius: VLCStyle.margin_xsmall
                        color: theme.bg.secondary
                        border.width: 1
                        border.color: theme.border
                        visible: !PaintbrushCtx.objectSelected && !canvas.hasSelection

                        ColumnLayout {
                            id: emptyBody
                            x: VLCStyle.margin_small
                            y: VLCStyle.margin_small
                            width: parent.width - 2 * VLCStyle.margin_small
                            spacing: VLCStyle.margin_xsmall

                            Widgets.LabelExt {
                                Layout.fillWidth: true
                                text: qsTr("Nothing selected")
                                color: theme.fg.primary
                                font.pixelSize: VLCStyle.fontSize_large
                            }
                            Widgets.LabelExt {
                                Layout.fillWidth: true
                                text: qsTr("Pick Object Select and paint over the subject you want to work with. NAI will find its edges.")
                                color: theme.fg.secondary
                                font.pixelSize: VLCStyle.fontSize_normal
                                wrapMode: Text.WordWrap
                            }
                            InspectorAction {
                                label: qsTr("Object Select")
                                iconText: VLCIcons.fullscreen
                                primary: true
                                actionEnabled: PaintbrushCtx.segmentationAvailable
                                onClicked: canvas.tool = 3
                            }
                        }
                    }

                    /* subject library: compact, collapsed until wanted */
                    DockSection {
                        title: qsTr("Subject library")
                        open: root.subjectLibraryOpen
                        visible: !PaintbrushCtx.objectSelected && !canvas.hasSelection

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: VLCStyle.margin_xxxsmall

                            InspectorAction {
                                label: qsTr("Place subject on canvas")
                                iconText: VLCIcons.add
                                onClicked: {
                                    const list = PaintbrushCtx.subjectList()
                                    if (list.length === 0) {
                                        console.log("PB_QML place: no subjects in the library")
                                        return
                                    }
                                    const err = PaintbrushCtx.placeSubject(
                                                    list[list.length - 1].id, "main")
                                    if (err.length > 0)
                                        console.log("PB_QML place failed: " + err)
                                }
                            }
                            InspectorAction {
                                label: qsTr("Style last subject — Ink")
                                iconText: VLCIcons.ok
                                onClicked: {
                                    const list = PaintbrushCtx.instanceList("main")
                                    if (list.length === 0) {
                                        console.log("PB_QML style: no instances")
                                        return
                                    }
                                    const err = PaintbrushCtx.applySubjectStyle(
                                                    list[list.length - 1].id, "ink")
                                    if (err.length > 0)
                                        console.log("PB_QML style failed: " + err)
                                }
                            }
                            InspectorAction {
                                label: qsTr("Derive artwork — Ink")
                                iconText: VLCIcons.fullscreen
                                onClicked: {
                                    const list = PaintbrushCtx.subjectList()
                                    if (list.length === 0) {
                                        console.log("PB_QML derive: no subjects")
                                        return
                                    }
                                    const err = PaintbrushCtx.deriveArtwork(
                                                    list[list.length - 1].id, "ink")
                                    if (err.length > 0)
                                        console.log("PB_QML derive failed: " + err)
                                }
                            }
                            InspectorAction {
                                label: qsTr("Add contour layer (vector)")
                                iconText: VLCIcons.ic_fluent_arrow_move
                                onClicked: {
                                    const err = PaintbrushCtx.addVectorLayerFromSelection()
                                    if (err.length > 0)
                                        console.log("PB_QML vector layer failed: " + err)
                                }
                            }
                            InspectorAction {
                                label: qsTr("Derive contour — vector")
                                iconText: VLCIcons.ic_fluent_arrow_sync_24_regular
                                onClicked: {
                                    const list = PaintbrushCtx.subjectList()
                                    if (list.length === 0) {
                                        console.log("PB_QML contour: no subjects")
                                        return
                                    }
                                    const err = PaintbrushCtx.deriveContour(
                                                    list[list.length - 1].id)
                                    if (err.length > 0)
                                        console.log("PB_QML contour failed: " + err)
                                }
                            }
                        }
                    }

                    /* object + selection actions: compact neutral rows, one
                     * accent primary, instead of a stack of orange bars */
                    DockSection {
                        title: qsTr("Actions")
                        open: true
                        visible: PaintbrushCtx.objectSelected || canvas.hasSelection

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: VLCStyle.margin_xxxsmall

                            InspectorAction {
                                label: qsTr("Find by words…")
                                iconText: VLCIcons.search
                                actionEnabled: PaintbrushCtx.segmentationAvailable
                                onClicked: textPromptDialog.open()
                            }
                            InspectorAction {
                                label: qsTr("Remove object")
                                iconText: VLCIcons.remove
                                actionEnabled: PaintbrushCtx.objectSelected
                                              && PaintbrushCtx.generationConfigured
                                onClicked: PaintbrushCtx.removeObject("")
                            }
                            InspectorAction {
                                label: qsTr("Extract object")
                                iconText: VLCIcons.fullscreen
                                primary: true
                                actionEnabled: PaintbrushCtx.objectSelected
                                onClicked: PaintbrushCtx.extractObject()
                            }
                            InspectorAction {
                                label: qsTr("Replace background…")
                                iconText: VLCIcons.ic_fluent_arrow_sync_24_regular
                                actionEnabled: PaintbrushCtx.objectSelected
                                              && PaintbrushCtx.generationConfigured
                                onClicked: bgPromptDialog.open()
                            }
                            InspectorAction {
                                label: qsTr("Generative fill…")
                                iconText: VLCIcons.add
                                actionEnabled: canvas.hasSelection
                                              && PaintbrushCtx.generationConfigured
                                onClicked: fillPromptDialog.open()
                            }
                            InspectorAction {
                                label: qsTr("Find product…")
                                iconText: VLCIcons.search
                                actionEnabled: PaintbrushCtx.lookupAvailable
                                              && PaintbrushCtx.objectSelected
                                onClicked: PaintbrushCtx.findProduct()
                            }
                            InspectorAction {
                                label: qsTr("Auto collage…")
                                iconText: VLCIcons.list
                                onClicked: collageDialog.open()
                            }

                            Widgets.LabelExt {
                                Layout.fillWidth: true
                                text: qsTr("Object Select: drag on the mask to paint it in; Alt-drag or right-drag to erase.")
                                color: theme.fg.secondary
                                font.pixelSize: VLCStyle.fontSize_small
                                wrapMode: Text.WordWrap
                                visible: PaintbrushCtx.objectSelected
                            }
                        }
                    }

                    /* mask edge + matte, grouped and collapsible */
                    DockSection {
                        title: qsTr("Matte")
                        open: true
                        visible: PaintbrushCtx.objectSelected

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: VLCStyle.margin_xsmall

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: VLCStyle.margin_xsmall

                                Widgets.LabelExt {
                                    text: qsTr("Edge")
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
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

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: VLCStyle.margin_xsmall

                                BusyIndicator {
                                    running: PaintbrushCtx.matteRefining
                                    visible: running
                                    implicitWidth: VLCStyle.dp(18, VLCStyle.scale)
                                    implicitHeight: implicitWidth
                                }
                                Widgets.LabelExt {
                                    Layout.fillWidth: true
                                    text: PaintbrushCtx.matteRefining
                                          ? qsTr("Refining the matte…")
                                          : qsTr("Matte mode")
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
                                }
                                Widgets.IconToolButton {
                                    text: VLCIcons.ic_fluent_arrow_sync_24_regular
                                    description: qsTr("Recompute the matte and load diagnostics")
                                    onClicked: PaintbrushCtx.refreshMatte()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: VLCStyle.margin_xxxsmall

                                Repeater {
                                    model: [qsTr("Raw"), qsTr("Refined"), qsTr("Neural")]
                                    delegate: ModeChip {
                                        required property int index
                                        required property string modelData
                                        label: modelData
                                        selected: PaintbrushCtx.matteMode === index
                                        onClicked: PaintbrushCtx.matteMode = index
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: VLCStyle.margin_xsmall

                                Widgets.LabelExt {
                                    text: qsTr("Quality")
                                    color: theme.fg.secondary
                                    font.pixelSize: VLCStyle.fontSize_small
                                    Layout.fillWidth: true
                                }
                                ComboBox {
                                    model: [qsTr("Preview"), qsTr("Balanced"),
                                            qsTr("Maximum")]
                                    currentIndex: PaintbrushCtx.matteQuality
                                    onActivated: PaintbrushCtx.matteQuality = currentIndex
                                    Layout.preferredWidth: VLCStyle.dp(140, VLCStyle.scale)
                                    font.pixelSize: VLCStyle.fontSize_normal
                                }
                            }

                            Widgets.LabelExt {
                                Layout.fillWidth: true
                                text: PaintbrushCtx.matteInfoText
                                visible: text.length > 0
                                color: PaintbrushCtx.matteAvailable
                                       ? theme.fg.secondary : theme.accent
                                font.pixelSize: VLCStyle.fontSize_small
                                wrapMode: Text.WordWrap
                            }

                            Widgets.LabelExt {
                                Layout.fillWidth: true
                                text: PaintbrushCtx.mattingStatusText
                                visible: !PaintbrushCtx.matteAvailable && text.length > 0
                                color: theme.fg.secondary
                                font.pixelSize: VLCStyle.fontSize_small
                                wrapMode: Text.WordWrap
                            }

                            InspectorAction {
                                label: root.matteAdvancedOpen
                                       ? qsTr("Hide boundary and artefact controls")
                                       : qsTr("Boundary and artefact controls")
                                iconText: root.matteAdvancedOpen
                                          ? VLCIcons.chevron_down
                                          : VLCIcons.ic_fluent_chevron_right_24
                                onClicked: root.matteAdvancedOpen = !root.matteAdvancedOpen
                            }

                            ColumnLayout {
                                id: matteAdvanced
                                Layout.fillWidth: true
                                spacing: VLCStyle.margin_xsmall
                                visible: root.matteAdvancedOpen

                                function applyAdvanced(key, value) {
                                    var a = PaintbrushCtx.matteAdvanced()
                                    a[key] = value
                                    PaintbrushCtx.setMatteAdvanced(a.boundary, a.detail,
                                                                   a.artifacts, a.holes,
                                                                   a.decontamination, a.temporal)
                                }

                                Repeater {
                                    model: [
                                        { key: "boundary", label: qsTr("Boundary band (px)"),
                                          from: 1, to: 24 },
                                        { key: "detail", label: qsTr("Fine-detail recovery"),
                                          from: 0, to: 100 },
                                        { key: "artifacts", label: qsTr("Artefact suppression"),
                                          from: 0, to: 100 },
                                        { key: "holes", label: qsTr("Enclosed-hole recovery"),
                                          from: 0, to: 100 },
                                        { key: "decontamination", label: qsTr("Colour decontamination"),
                                          from: 0, to: 100 },
                                        { key: "temporal", label: qsTr("Temporal stability"),
                                          from: 0, to: 100 }
                                    ]
                                    delegate: ColumnLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: 0

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Widgets.LabelExt {
                                                text: modelData.label
                                                color: theme.fg.primary
                                                font.pixelSize: VLCStyle.fontSize_small
                                                Layout.fillWidth: true
                                            }
                                            Widgets.LabelExt {
                                                text: Math.round(control.value)
                                                color: theme.fg.secondary
                                                font.pixelSize: VLCStyle.fontSize_small
                                            }
                                        }
                                        Slider {
                                            id: control
                                            Layout.fillWidth: true
                                            from: modelData.from
                                            to: modelData.to
                                            stepSize: 1
                                            value: PaintbrushCtx.matteAdvanced()[modelData.key]
                                            onMoved: matteAdvanced.applyAdvanced(
                                                         modelData.key, Math.round(value))
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: VLCStyle.margin_xsmall

                                    Widgets.LabelExt {
                                        text: qsTr("Diagnostic view")
                                        color: theme.fg.secondary
                                        font.pixelSize: VLCStyle.fontSize_small
                                        Layout.fillWidth: true
                                    }
                                    ComboBox {
                                        id: matteDiagView
                                        model: [qsTr("Original"), qsTr("Semantic"),
                                                qsTr("Topology"), qsTr("Trimap"),
                                                qsTr("Alpha"), qsTr("Foreground"),
                                                qsTr("Background"), qsTr("Premultiplied")]
                                        Layout.preferredWidth: VLCStyle.dp(140, VLCStyle.scale)
                                        font.pixelSize: VLCStyle.fontSize_normal
                                    }
                                }

                                Image {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: VLCStyle.dp(150, VLCStyle.scale)
                                    visible: source.length > 0
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true
                                    cache: false
                                    source: PaintbrushCtx.matteGeneration >= 0
                                            ? PaintbrushCtx.matteDiagnosticDataUrl(
                                                  matteDiagView.currentIndex)
                                            : ""
                                }

                                InspectorAction {
                                    label: qsTr("Export diagnostics…")
                                    iconText: VLCIcons.fullscreen
                                    onClicked: PaintbrushCtx.exportMatteDiagnostics()
                                }
                            }
                        }
                    }

                    /* "Ask about this" result */
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: visionBody.implicitHeight
                                         + 2 * VLCStyle.margin_small
                        radius: VLCStyle.margin_xsmall
                        color: Qt.rgba(theme.accent.r, theme.accent.g,
                                       theme.accent.b, 0.08)
                        border.width: 1
                        border.color: Qt.rgba(theme.accent.r, theme.accent.g,
                                              theme.accent.b, 0.25)
                        visible: PaintbrushCtx.visionHasResult

                        ColumnLayout {
                            id: visionBody
                            x: VLCStyle.margin_small
                            y: VLCStyle.margin_small
                            width: parent.width - 2 * VLCStyle.margin_small
                            spacing: VLCStyle.margin_xsmall

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: VLCStyle.margin_xsmall

                                Image {
                                    Layout.preferredWidth: VLCStyle.dp(48, VLCStyle.scale)
                                    Layout.preferredHeight: VLCStyle.dp(48, VLCStyle.scale)
                                    source: PaintbrushCtx.objectSelected
                                            ? PaintbrushCtx.objectThumbnailDataUrl : ""
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
                                        font.pixelSize: VLCStyle.fontSize_small
                                    }
                                    Widgets.LabelExt {
                                        Layout.fillWidth: true
                                        text: PaintbrushCtx.visionAnswer
                                        color: theme.fg.primary
                                        font.pixelSize: VLCStyle.fontSize_normal
                                        wrapMode: Text.WordWrap
                                    }
                                    Widgets.LabelExt {
                                        Layout.fillWidth: true
                                        text: PaintbrushCtx.visionEvidence
                                        visible: PaintbrushCtx.visionEvidence.length > 0
                                        color: theme.fg.secondary
                                        font.pixelSize: VLCStyle.fontSize_small
                                        wrapMode: Text.WordWrap
                                    }
                                    Widgets.LabelExt {
                                        Layout.fillWidth: true
                                        text: PaintbrushCtx.visionUncertainty
                                        visible: PaintbrushCtx.visionUncertainty.length > 0
                                        color: theme.fg.secondary
                                        font.pixelSize: VLCStyle.fontSize_small
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: VLCStyle.margin_xxxsmall

                                Button {
                                    text: qsTr("Copy")
                                    font.pixelSize: VLCStyle.fontSize_small
                                    onClicked: PaintbrushCtx.copyVisionAnswer()
                                }
                                Button {
                                    text: qsTr("Add as text")
                                    font.pixelSize: VLCStyle.fontSize_small
                                    onClicked: PaintbrushCtx.addVisionAnswerAsText()
                                }
                                Button {
                                    text: qsTr("Clear")
                                    font.pixelSize: VLCStyle.fontSize_small
                                    onClicked: PaintbrushCtx.clearVisionResult()
                                }
                            }
                        }
                    }

                    /* "Find product" result */
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: lookupBody.implicitHeight
                                         + 2 * VLCStyle.margin_small
                        radius: VLCStyle.margin_xsmall
                        color: Qt.rgba(theme.accent.r, theme.accent.g,
                                       theme.accent.b, 0.08)
                        border.width: 1
                        border.color: Qt.rgba(theme.accent.r, theme.accent.g,
                                              theme.accent.b, 0.25)
                        visible: PaintbrushCtx.lookupMatches.length > 0

                        ColumnLayout {
                            id: lookupBody
                            x: VLCStyle.margin_small
                            y: VLCStyle.margin_small
                            width: parent.width - 2 * VLCStyle.margin_small
                            spacing: VLCStyle.margin_xsmall

                            Repeater {
                                model: PaintbrushCtx.lookupMatches
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Widgets.LabelExt {
                                        Layout.fillWidth: true
                                        text: modelData.title
                                        color: theme.fg.primary
                                        font.pixelSize: VLCStyle.fontSize_normal
                                        font.bold: true
                                        wrapMode: Text.WordWrap
                                    }
                                    Widgets.LabelExt {
                                        Layout.fillWidth: true
                                        text: modelData.description
                                        color: theme.fg.secondary
                                        font.pixelSize: VLCStyle.fontSize_small
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
                                        font.pixelSize: VLCStyle.fontSize_small
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

    }

    /* --------------------------------------------------------- components */
    component DockTab: Rectangle {
        id: tab

        property string label
        property bool selected: false
        signal clicked()

        Layout.fillWidth: true
        implicitHeight: VLCStyle.dp(32, VLCStyle.scale)
        radius: VLCStyle.margin_xsmall
        color: tab.selected
               ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.16)
               : theme.bg.secondary
        border.width: 1
        border.color: tab.selected ? theme.accent : theme.border

        Widgets.LabelExt {
            anchors.centerIn: parent
            text: tab.label
            color: tab.selected ? theme.accent : theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_normal
        }
        MouseArea {
            anchors.fill: parent
            onClicked: tab.clicked()
        }
        Accessible.role: Accessible.Button
        Accessible.name: tab.label
    }

    component DockSection: ColumnLayout {
        id: section

        default property alias content: sectionBody.data
        property string title
        property bool open: true

        Layout.fillWidth: true
        spacing: VLCStyle.margin_xxxsmall

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: VLCStyle.dp(34, VLCStyle.scale)
            radius: VLCStyle.margin_xsmall
            color: theme.bg.secondary
            border.width: 1
            border.color: theme.border

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: VLCStyle.margin_small
                anchors.rightMargin: VLCStyle.margin_xsmall
                spacing: VLCStyle.margin_xsmall

                Widgets.LabelExt {
                    Layout.fillWidth: true
                    text: section.title
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_small
                    font.bold: true
                }
                Widgets.IconLabel {
                    text: section.open ? VLCIcons.chevron_down
                                       : VLCIcons.ic_fluent_chevron_right_24
                    color: theme.fg.secondary
                    font.pixelSize: VLCStyle.fontSize_small
                }
            }
            MouseArea {
                anchors.fill: parent
                onClicked: section.open = !section.open
            }
            Accessible.role: Accessible.Button
            Accessible.name: section.title
        }

        ColumnLayout {
            id: sectionBody
            Layout.fillWidth: true
            visible: section.open
            spacing: VLCStyle.margin_xsmall
        }
    }

    component ModeChip: Rectangle {
        id: chip

        property string label
        property bool selected: false
        signal clicked()

        Layout.fillWidth: true
        implicitHeight: VLCStyle.dp(30, VLCStyle.scale)
        radius: VLCStyle.margin_xsmall
        color: chip.selected
               ? Qt.rgba(theme.accent.r, theme.accent.g, theme.accent.b, 0.18)
               : theme.bg.secondary
        border.width: 1
        border.color: chip.selected ? theme.accent : theme.border

        Widgets.LabelExt {
            anchors.centerIn: parent
            text: chip.label
            color: chip.selected ? theme.accent : theme.fg.secondary
            font.pixelSize: VLCStyle.fontSize_small
        }
        MouseArea {
            anchors.fill: parent
            onClicked: chip.clicked()
        }
        Accessible.role: Accessible.Button
        Accessible.name: chip.label
    }

    component InspectorAction: Rectangle {
        id: action

        property string label
        property string iconText: ""
        property bool primary: false
        property bool actionEnabled: true
        signal clicked()

        Layout.fillWidth: true
        implicitHeight: VLCStyle.dp(36, VLCStyle.scale)
        radius: VLCStyle.margin_xsmall
        color: !action.actionEnabled ? theme.bg.secondary
               : action.primary ? theme.accent
               : (actionHover.containsMouse
                  ? Qt.rgba(theme.accent.r, theme.accent.g,
                            theme.accent.b, 0.12)
                  : theme.bg.secondary)
        border.width: 1
        border.color: action.primary ? theme.accent
                     : (actionHover.containsMouse ? theme.accent : theme.border)
        opacity: action.actionEnabled ? 1.0 : 0.45

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: VLCStyle.margin_small
            anchors.rightMargin: VLCStyle.margin_small
            spacing: VLCStyle.margin_xsmall

            Widgets.IconLabel {
                visible: action.iconText !== ""
                text: action.iconText
                color: action.primary ? theme.bg.primary : theme.accent
                font.pixelSize: VLCStyle.fontSize_normal
            }
            Widgets.LabelExt {
                Layout.fillWidth: true
                text: action.label
                color: action.primary ? theme.bg.primary : theme.fg.primary
                font.pixelSize: VLCStyle.fontSize_normal
                elide: Text.ElideRight
            }
        }
        MouseArea {
            id: actionHover
            anchors.fill: parent
            hoverEnabled: true
            enabled: action.actionEnabled
            onClicked: action.clicked()
        }
        Accessible.role: Accessible.Button
        Accessible.name: action.label
    }

    /* ------------------------------------------------------------ dialogs */
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
        /* Declared inside the Inspector drawer: without an explicit overlay
         * parent the x/y below are interpreted in the drawer's coordinate
         * space and the dialog lands off-screen (the window dims, nothing
         * else appears). */
        parent: Overlay.overlay
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
