/*****************************************************************************
 * PaintbrushStrip.qml : NAI Paintbrush — the adaptive bottom strip
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************
 * ONE surface for contextual editing. Groups, left to right:
 *
 *   [ parameters : flexible ]            [ colour : intrinsic ]
 *
 * - The strip owns a FIXED height, so switching tools never reflows the canvas.
 * - Parameters take Layout.fillWidth; colour keeps an intrinsic width instead
 *   of stretching across the window (the old failure mode).
 * - The colour group is removed from the layout (not merely invisible) for
 *   colour-independent contexts, so it leaves no spacer: Object Select and an
 *   active segmentation collapse it to zero width.
 * - Only real, existing controls are used; no decorative buttons.
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts

import VLC.Style
import VLC.Paintbrush

Item {
    id: root

    property var canvas: null

    /* Fixed height: the strip must never resize the canvas. */
    implicitHeight: VLCStyle.dp(96, VLCStyle.scale)

    /* Colour is meaningful for painting/materials only. Keep this a single
     * expression so adding a tool means editing one line. */
    readonly property bool colourRelevant:
        canvas !== null
        && canvas.tool !== 3            /* Object Select (AI) */
        && !PaintbrushCtx.segmentationBusy

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
            id: optionsBar
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 0
            canvas: root.canvas
        }

        /* separator: only when the colour group exists */
        Rectangle {
            Layout.preferredWidth: 1
            Layout.fillHeight: true
            visible: root.colourRelevant
            color: theme.border
            opacity: 0.6
        }

        /* ---- colour: intrinsic width, present only when relevant -------- */
        PaintbrushPaletteBar {
            id: paletteBar
            Layout.preferredWidth: visible ? VLCStyle.dp(430, VLCStyle.scale) : 0
            Layout.fillHeight: true
            visible: root.colourRelevant
            canvas: root.canvas
        }
    }
}
