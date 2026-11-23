/*****************************************************************************
 * PaintbrushMaskPanel.qml : NAI Paintbrush — object-mask cleanup panel
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

import VLC.MainInterface
import VLC.Widgets as Widgets
import VLC.Style

/* A compact overlay for cleaning up an object mask with a brush.
 *
 * Two modes share the same surface:
 *   - selection mode: a SAM mask already exists (PaintbrushCtx.objectSelected);
 *     the CTA commits the cleaned mask to a new cutout layer;
 *   - layer mode: a committed object cutout is being re-edited
 *     (PaintbrushCtx.layerMaskEditActive); the CTA closes the edit session
 *     and keeps the native layer mask, Revert restores it.
 *
 * The actual painting happens on the canvas (Object Select brush): this panel
 * only exposes the mode, the brush size and the actions. */
Rectangle {
    id: root

    property var canvas: null
    property bool layerMode: PaintbrushCtx.layerMaskEditActive

    readonly property bool wanted: canvas !== null
                                   && (PaintbrushCtx.objectSelected
                                       || layerMode)

    implicitWidth: content.implicitWidth + VLCStyle.margin_normal * 2
    implicitHeight: content.implicitHeight + VLCStyle.margin_small * 2
    radius: VLCStyle.margin_small
    color: theme.bg.primary
    border.width: 1
    border.color: theme.border
    visible: wanted

    ColorContext {
        id: theme
        palette: VLCStyle.palette
        colorSet: ColorContext.View
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: VLCStyle.margin_small
        spacing: VLCStyle.margin_xsmall

        RowLayout {
            spacing: VLCStyle.margin_xsmall

            Widgets.LabelExt {
                text: root.layerMode ? qsTr("Layer mask")
                                     : qsTr("Object mask")
                color: theme.fg.primary
                font.bold: true
            }
            Widgets.LabelExt {
                Layout.fillWidth: true
                text: root.layerMode
                      ? qsTr("Paint to keep or remove parts, then press Done.")
                      : qsTr("Clean up the mask, then commit it to a layer.")
                color: theme.fg.secondary
                font.pixelSize: VLCStyle.fontSize_small
                wrapMode: Text.WordWrap
            }
        }

        RowLayout {
            spacing: VLCStyle.margin_xsmall

            Widgets.ButtonExt {
                text: qsTr("Add")
                checkable: true
                checked: root.canvas !== null && root.canvas.maskPaintMode === 0
                onClicked: if (root.canvas) root.canvas.maskPaintMode = 0
            }
            Widgets.ButtonExt {
                text: qsTr("Erase")
                checkable: true
                checked: root.canvas !== null && root.canvas.maskPaintMode === 1
                onClicked: if (root.canvas) root.canvas.maskPaintMode = 1
            }

            Widgets.LabelExt {
                text: qsTr("Brush")
                color: theme.fg.secondary
            }
            Slider {
                id: brushSlider
                Layout.preferredWidth: VLCStyle.dp(150, VLCStyle.scale)
                from: 1
                to: 64
                stepSize: 1
                value: root.canvas ? root.canvas.brushSize : 16
                onMoved: if (root.canvas) root.canvas.brushSize = value
            }
            Widgets.LabelExt {
                Layout.preferredWidth: VLCStyle.dp(30, VLCStyle.scale)
                text: root.canvas ? Math.round(root.canvas.brushSize) : ""
                color: theme.fg.secondary
            }

            Widgets.ButtonExt {
                visible: !root.layerMode
                text: qsTr("Soft edge")
                checkable: true
                checked: PaintbrushCtx.objectMaskSoftEdge
                onClicked: {
                    if (checked)
                        PaintbrushCtx.setObjectMaskEdgeSoft()
                    else
                        PaintbrushCtx.setObjectMaskEdgeHard()
                }
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: VLCStyle.margin_xsmall

            Widgets.ButtonExt {
                visible: root.layerMode
                text: qsTr("Revert")
                onClicked: PaintbrushCtx.cancelLayerMaskEdit()
            }
            Widgets.ButtonExt {
                visible: root.layerMode
                text: qsTr("Reset")
                onClicked: PaintbrushCtx.resetLayerMask()
            }
            Widgets.ButtonExt {
                text: root.layerMode ? qsTr("Done") : qsTr("Commit to layer")
                highlighted: true
                onClicked: {
                    if (root.layerMode)
                        PaintbrushCtx.endLayerMaskEdit()
                    else
                        PaintbrushCtx.addObjectToCanvas()
                }
            }
        }
    }
}
