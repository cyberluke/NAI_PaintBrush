/*****************************************************************************
 * PaintbrushToolOptionsBar.qml : NAI Paintbrush — per-tool horizontal options
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

import VLC.MainInterface
import VLC.Style
import VLC.Paintbrush

/* The per-tool horizontal options rendered by the SAME unified
 * PaintbrushToolbar cell delegate as the vertical tool rail. It is the middle
 * section of the single bottom strip and owns NO background, border or radius
 * of its own: the strip is one surface, so nesting a second bordered card here
 * produced the "empty field inside a giant frame" the user saw.
 *
 * Shows, for the active tool, its option sliders (Size / Amount / Flake /
 * Glue / Loose / Shimmer etc.) plus precision toggles (Grid / Rulers / Snap /
 * Pen). Object Select exposes its own mask controls (size, hard/soft edge)
 * through the semantic `toolHasPrimaryAction` capability instead of a tool-id
 * table. Only the four precision cells are always present; slider cells appear
 * on-demand, and the whole section flexes inside the strip. Cells are rebuilt
 * when the relevant canvas state changes so active states and slider positions
 * always mirror the canvas. */
Item {
    id: root

    required property PaintbrushCanvas canvas

    Component.onCompleted: rebuild()

    /* one control row: the strip supplies the surrounding padding */
    implicitHeight: VLCStyle.dp(32, VLCStyle.scale)
    /* Overflow clips inside the strip instead of drawing over the canvas. */
    clip: true

    PaintbrushToolbar {
        id: optionBar
        orientation: Qt.Horizontal
        anchors.fill: parent
        cell: VLCStyle.dp(32, VLCStyle.scale)
        spacing: VLCStyle.margin_xsmall
    }


    function rebuild()
    {
        const out = []

        /* -------- precision toggles (always available) ----------------- */
        out.push({ kind: "icon", iconGlyph: VLCIcons.grid, tip: qsTr("Grid"),
                   checkable: true, checked: canvas.gridVisible,
                   onTriggered: () => canvas.gridVisible = !canvas.gridVisible })
        out.push({ kind: "icon", iconGlyph: VLCIcons.aspect_ratio, tip: qsTr("Rulers"),
                   checkable: true, checked: canvas.rulersVisible,
                   onTriggered: () => canvas.rulersVisible = !canvas.rulersVisible })
        out.push({ kind: "icon", iconGlyph: VLCIcons.grid, tip: qsTr("Snap to grid"),
                   checkable: true, checked: canvas.snapToGrid,
                   onTriggered: () => canvas.snapToGrid = !canvas.snapToGrid })
        out.push({ kind: "icon", iconGlyph: VLCIcons.active_indicator, tip: qsTr("Pen pressure"),
                   checkable: true, checked: canvas.pressureEnabled,
                   onTriggered: () => canvas.pressureEnabled = !canvas.pressureEnabled })

        /* -------- Object Select: mask brush size + edge mode ----------- *
         * Driven by the semantic tool capability, not a numeric tool id:
         * the mask is painted with the normal brush size, and the edge mode
         * decides whether the cut-out is crisp or feathered. */
        if (PaintbrushCtx.toolHasPrimaryAction)
        {
            out.push({ kind: "slider", label: qsTr("Mask"), from: 1, to: 64, stepSize: 1,
                       value: canvas.brushSize,
                       onChange: (v) => canvas.brushSize = v })
            out.push({ kind: "icon",
                       iconGlyph: PaintbrushCtx.objectMaskSoftEdge
                                  ? VLCIcons.ic_fluent_arrow_sync_24_regular
                                  : VLCIcons.fullscreen,
                       tip: PaintbrushCtx.objectMaskSoftEdge
                            ? qsTr("Soft edge — click for a crisp boundary")
                            : qsTr("Hard edge — click for a feathered boundary"),
                       checkable: true,
                       checked: PaintbrushCtx.objectMaskSoftEdge,
                       action: "objectMaskEdgeToggle" })
        }

        /* -------- shape/draw tools: shared Size ------------------------ */
        if (PaintbrushCtx.toolHasSizeParameter)
            out.push({ kind: "slider", label: qsTr("Size"), from: 1, to: 64, stepSize: 1,
                       value: canvas.brushSize,
                       onChange: (v) => canvas.brushSize = v })

        /* -------- Spray (9): flow -------------------------------------- */
        if (PaintbrushCtx.toolHasFlowParameter)
            out.push({ kind: "slider", label: qsTr("Flow"), from: 0.1, to: 4, stepSize: 0.1,
                       value: canvas.sprayFlow,
                       onChange: (v) => canvas.sprayFlow = v })

        /* -------- Bristle (16): size + flow + pigment ------------------- */
        if (PaintbrushCtx.toolHasBristleParameters)
        {
            out.push({ kind: "slider", label: qsTr("Size"), from: 1, to: 160, stepSize: 1,
                       value: canvas.brushSize, onChange: (v) => canvas.brushSize = v })
            out.push({ kind: "slider", label: qsTr("Flow"), from: 0.05, to: 1, stepSize: 0.05,
                       value: canvas.brushFlow, onChange: (v) => canvas.brushFlow = v })
            out.push({ kind: "slider", label: qsTr("Pigment"), from: 0.0, to: 1, stepSize: 0.05,
                       value: canvas.brushColorVariation,
                       onChange: (v) => canvas.brushColorVariation = v })
        }

        /* -------- Glitter (17): size amount flake shimmer glue ----------- */
        if (PaintbrushCtx.toolHasGlitterParameters)
        {
            out.push({ kind: "slider", label: qsTr("Size"), from: 1, to: 160, stepSize: 1,
                       value: canvas.brushSize, onChange: (v) => canvas.brushSize = v })
            out.push({ kind: "slider", label: qsTr("Amount"), from: 0.1, to: 2, stepSize: 0.05,
                       value: canvas.glitterAmount, onChange: (v) => canvas.glitterAmount = v })
            out.push({ kind: "slider", label: qsTr("Flake"), from: 0.2, to: 2.5, stepSize: 0.05,
                       value: canvas.glitterFlakeSize, onChange: (v) => canvas.glitterFlakeSize = v })
            out.push({ kind: "slider", label: qsTr("Shimmer"), from: 0.0, to: 1, stepSize: 0.05,
                       value: canvas.glitterShimmer, onChange: (v) => canvas.glitterShimmer = v })
            out.push({ kind: "slider", label: qsTr("Glue"), from: 0.0, to: 1, stepSize: 0.05,
                       value: canvas.glitterAdhesion, onChange: (v) => canvas.glitterAdhesion = v })
        }

        /* -------- Glitter Powder (18): workbench ------------------------ */
        if (PaintbrushCtx.toolHasPowderParameters)
        {
            /* leading subtool group: Glue / Glitter / Push / Eraser are
             * mutually-exclusive checkable modes; Pour / Blow are momentary
             * actions. A non-interactive spacer separates the two groups.
             * Declarative action data (never JS callbacks): the toolbar emits
             * triggered(action, value) and rebuild() below dispatches (P0.3). */
            out.push({ kind: "tool", iconSource: "dropzone", tip: qsTr("Glue"),
                       checkable: true, checked: PaintbrushCtx.powderToolIsGlue,
                       action: "powderTool", value: 0 })
            out.push({ kind: "tool", iconSource: "glitter", tip: qsTr("Glitter"),
                       checkable: true, checked: PaintbrushCtx.powderToolIsGlitter,
                       action: "powderTool", value: 1 })
            out.push({ kind: "tool", iconSource: "move", tip: qsTr("Push"),
                       checkable: true, checked: PaintbrushCtx.powderToolIsPush,
                       action: "powderTool", value: 2 })
            out.push({ kind: "tool", iconSource: "eraser", tip: qsTr("Eraser"),
                       checkable: true, checked: PaintbrushCtx.powderToolIsEraser,
                       action: "powderTool", value: 3 })
            out.push({ kind: "spacer" })
            out.push({ kind: "icon", iconSource: "download", tip: qsTr("Pour"),
                       action: "powderPour" })
            out.push({ kind: "icon", iconSource: "windy", tip: qsTr("Blow"),
                       action: "powderBlow" })

            /* contextual options, dependent on the active subtool (P0.5: each
             * sub-tool keeps its OWN reference size range/default; Glitter adds
             * Amount; Shimmer is a single semantic control on Glitter). */
            if (PaintbrushCtx.powderToolIsGlue)            /* Glue: width */
                out.push({ kind: "slider", label: qsTr("Size"), from: 4, to: 60, stepSize: 1,
                           value: canvas.powderGlueWidth, onChange: (v) => canvas.powderGlueWidth = v })
            else if (PaintbrushCtx.powderToolIsGlitter)       /* Glitter: radius + amount + shimmer */
            {
                out.push({ kind: "slider", label: qsTr("Size"), from: 6, to: 80, stepSize: 1,
                           value: canvas.powderGlitterSize, onChange: (v) => canvas.powderGlitterSize = v })
                out.push({ kind: "slider", label: qsTr("Amount"), from: 0.1, to: 2, stepSize: 0.05,
                           value: canvas.glitterAmount, onChange: (v) => canvas.glitterAmount = v })
                out.push({ kind: "slider", label: qsTr("Shimmer"), from: 0.0, to: 1, stepSize: 0.05,
                           value: canvas.glitterShimmer, onChange: (v) => canvas.glitterShimmer = v })
            }
            else if (PaintbrushCtx.powderToolIsPush)       /* Push: radius */
                out.push({ kind: "slider", label: qsTr("Size"), from: 8, to: 60, stepSize: 1,
                           value: canvas.powderPushSize, onChange: (v) => canvas.powderPushSize = v })
            else if (PaintbrushCtx.powderToolIsEraser)       /* Eraser: width */
                out.push({ kind: "slider", label: qsTr("Size"), from: 8, to: 120, stepSize: 1,
                           value: canvas.powderEraserWidth, onChange: (v) => canvas.powderEraserWidth = v })
        }

        /* the old standalone Smear (19) option has been removed from the main
         * rail (P0.10): Smear is now the Push sub-tool of the Glitter Powder
         * material workbench, exposed via the powder tool bar instead. */

        optionBar.items = out
    }

    /* Declarative action dispatch (P0.3): the toolbar emits triggered(action,
     * value) — one observable path, no JS callbacks in the item model. */
    function dispatchAction(action, value) {
        switch (action) {
        case "powderTool":
            canvas.powderTool = value
            break
        case "powderPour":
            canvas.pourPowderNow()
            break
        case "powderBlow":
            canvas.blowPowderNow()
            break
        case "objectMaskEdgeToggle":
            if (PaintbrushCtx.objectMaskSoftEdge)
                PaintbrushCtx.setObjectMaskEdgeHard()
            else
                PaintbrushCtx.setObjectMaskEdgeSoft()
            break
        }
    }

    Connections {
        target: optionBar
        function onTriggered(action, value) { dispatchAction(action, value) }
    }

    /* precision toggles + sliders mirror the canvas, so any of these state
     * changes repaints the cells (the unified toolbar renders plain cells,
     * rebuilt here rather than bound, to keep one source of truth) */
    Connections {
        target: canvas
        function onToolChanged(t) { rebuild() }
        function onPowderToolChanged(t) { rebuild() }
        function onGridVisibleChanged() { rebuild() }
        function onRulersVisibleChanged() { rebuild() }
        function onSnapToGridChanged() { rebuild() }
        function onPressureEnabledChanged() { rebuild() }
    }

    Connections {
        target: PaintbrushCtx
        /* The controller emits this once a real canvas is attached and the
         * semantic capability properties are valid. Without it, Object Select
         * controls built at Component.onCompleted can miss the capability
         * change and never appear. */
        function onToolCapabilitiesChanged() { rebuild() }
        function onObjectMaskEdgeChanged() { rebuild() }
    }
}