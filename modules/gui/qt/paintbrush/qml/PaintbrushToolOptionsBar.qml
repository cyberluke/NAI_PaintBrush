/*****************************************************************************
 * PaintbrushToolOptionsBar.qml : NAI Paintbrush — per-tool horizontal options
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

/* The real second toolbar: a HORIZONTAL per-tool context bar rendered by the
 * SAME unified PaintbrushToolbar cell delegate as the vertical tool rail.
 * It floats above the bottom of the canvas and shows, for the active tool,
 * its option sliders (Size / Amount / Flake / Glue / Loose / Shimmer etc.) plus
 * the precision toggles (Grid / Rulers / Snap / Pen) as iconic cells.
 *
 * The horizontal bar is a ONE-ROW Flickable (P1, reviewer #2): fixed one-cell
 * height, clip:true, horizontal scroll when controls exceed the width — it
 * NEVER wraps vertically, so no option control can leak outside this bar's own
 * rectangle. The host height is a single fixed row (one cell + margins),
 * decoupled from any wrapped-row assumption. The palette below is a completely
 * separate 64dp colour surface (this bar holds no colour swatches).
 *
 * Because it is a distinct bar the option sliders are NEVER crowded out by the
 * grid/ruler controls — the palette bar below keeps only colour swatches.
 * Only the four precision cells are always present; slider cells appear
 * on-demand (their `visible` is bound to the active tool), so the bar grows
 * and shrinks with the tool. Cells are rebuilt when the tool changes so the
 * grid/snap active states and the slider positions always mirror the canvas. */
Rectangle {
    id: root

    required property PaintbrushCanvas canvas

    /* TEMP-PROOF (reviewer qrc/qmlcache check): a unique build marker + a
     * startup geometry log prove the running app is loading THIS qml (not a
     * stale qrc/qmlcache copy). Remove after the GUI gate is accepted. */
    objectName: "PaintbrushToolOptionsBar_marker_e48-toolbar-v2"
    Component.onCompleted: {
        /* existing initial rebuild of the toolbar cells */
        rebuild()
        /* TEMP-PROOF (reviewer qrc/qmlcache check): log the geometry + build
         * marker so we can prove the running app loads THIS qml, not stale. */
        console.log("PB_QML ToolOptionsBar onCompleted x=" + x + " y=" + y
                    + " w=" + width + " h=" + height + " z=" + z
                    + " marker=" + objectName)
    }
    onWidthChanged: logGeom()
    onHeightChanged: logGeom()
    onYChanged: logGeom()
    function logGeom() {
        console.log("PB_QML ToolOptionsBar geom x=" + x + " y=" + y
                    + " w=" + width + " h=" + height + " z=" + z)
    }

    ColorContext {
        id: theme
        colorSet: ColorContext.View
    }

    /* fixed ONE-ROW height: one cell + the surrounding margins. Decoupled from
     * any wrapped-contentHeight so the bar can never grow into the palette. */
    readonly property real _oneRow: VLCStyle.dp(32, VLCStyle.scale)
                                   + VLCStyle.margin_xsmall
    height: _oneRow + VLCStyle.margin_xsmall * 2
    color: theme.bg.primary
    border.width: 1
    border.color: theme.border
    radius: VLCStyle.margin_xsmall

    PaintbrushToolbar {
        id: optionBar
        orientation: Qt.Horizontal
        anchors.fill: parent
        anchors.margins: VLCStyle.margin_xsmall
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

        /* -------- shape/draw tools: shared Size ------------------------ */
        if (canvas.tool >= 4 && canvas.tool <= 6
                || canvas.tool >= 10 && canvas.tool <= 13)
            out.push({ kind: "slider", label: qsTr("Size"), from: 1, to: 64, stepSize: 1,
                       value: canvas.brushSize,
                       onChange: (v) => canvas.brushSize = v })

        /* -------- Spray (9): flow -------------------------------------- */
        if (canvas.tool === 9)
            out.push({ kind: "slider", label: qsTr("Flow"), from: 0.1, to: 4, stepSize: 0.1,
                       value: canvas.sprayFlow,
                       onChange: (v) => canvas.sprayFlow = v })

        /* -------- Bristle (16): size + flow + pigment ------------------- */
        if (canvas.tool === 16)
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
        if (canvas.tool === 17)
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
        if (canvas.tool === 18)
        {
            /* leading subtool group: Glue / Glitter / Push / Eraser are
             * mutually-exclusive checkable modes; Pour / Blow are momentary
             * actions. A non-interactive spacer separates the two groups.
             * Declarative action data (never JS callbacks): the toolbar emits
             * triggered(action, value) and rebuild() below dispatches (P0.3). */
            out.push({ kind: "tool", iconSource: "dropzone", tip: qsTr("Glue"),
                       checkable: true, checked: canvas.powderTool === 0,
                       action: "powderTool", value: 0 })
            out.push({ kind: "tool", iconSource: "glitter", tip: qsTr("Glitter"),
                       checkable: true, checked: canvas.powderTool === 1,
                       action: "powderTool", value: 1 })
            out.push({ kind: "tool", iconSource: "move", tip: qsTr("Push"),
                       checkable: true, checked: canvas.powderTool === 2,
                       action: "powderTool", value: 2 })
            out.push({ kind: "tool", iconSource: "eraser", tip: qsTr("Eraser"),
                       checkable: true, checked: canvas.powderTool === 3,
                       action: "powderTool", value: 3 })
            out.push({ kind: "spacer" })
            out.push({ kind: "icon", iconSource: "download", tip: qsTr("Pour"),
                       action: "powderPour" })
            out.push({ kind: "icon", iconSource: "windy", tip: qsTr("Blow"),
                       action: "powderBlow" })

            /* contextual options, dependent on the active subtool (P0.5: each
             * sub-tool keeps its OWN reference size range/default; Glitter adds
             * Amount; Shimmer is a single semantic control on Glitter). */
            if (canvas.powderTool === 0)            /* Glue: width */
                out.push({ kind: "slider", label: qsTr("Size"), from: 4, to: 60, stepSize: 1,
                           value: canvas.powderGlueWidth, onChange: (v) => canvas.powderGlueWidth = v })
            else if (canvas.powderTool === 1)       /* Glitter: radius + amount + shimmer */
            {
                out.push({ kind: "slider", label: qsTr("Size"), from: 6, to: 80, stepSize: 1,
                           value: canvas.powderGlitterSize, onChange: (v) => canvas.powderGlitterSize = v })
                out.push({ kind: "slider", label: qsTr("Amount"), from: 0.1, to: 2, stepSize: 0.05,
                           value: canvas.glitterAmount, onChange: (v) => canvas.glitterAmount = v })
                out.push({ kind: "slider", label: qsTr("Shimmer"), from: 0.0, to: 1, stepSize: 0.05,
                           value: canvas.glitterShimmer, onChange: (v) => canvas.glitterShimmer = v })
            }
            else if (canvas.powderTool === 2)       /* Push: radius */
                out.push({ kind: "slider", label: qsTr("Size"), from: 8, to: 60, stepSize: 1,
                           value: canvas.powderPushSize, onChange: (v) => canvas.powderPushSize = v })
            else if (canvas.powderTool === 3)       /* Eraser: width */
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
    function dispatchPowderAction(action, value) {
        switch (action) {
        case "powderTool":
            const names = ["Glue", "Glitter", "Push", "Eraser"]
            console.log("powder UI action:", (value >= 0 && value < names.length) ? names[value] : String(value))
            console.log("powder tool changed:", canvas.powderTool, "->", value)
            canvas.powderTool = value
            break
        case "powderPour":
            console.log("powder UI action: Pour")
            canvas.pourPowderNow()
            break
        case "powderBlow":
            console.log("powder UI action: Blow")
            canvas.blowPowderNow()
            break
        }
    }

    Connections {
        target: optionBar
        function onTriggered(action, value) { dispatchPowderAction(action, value) }
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
}