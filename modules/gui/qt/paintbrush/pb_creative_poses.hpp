/*****************************************************************************
 * pb_creative_poses.hpp : Paintbrush creative tracked-pose selection
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#ifndef PB_CREATIVE_POSES_HPP
#define PB_CREATIVE_POSES_HPP

#include <QImage>
#include <QVector>
#include <QtGlobal>

#include <functional>

namespace paintbrush {

struct ObjectTrack;
struct TrackKeyframe;

namespace creative {

using PoseFrameDecoder =
    std::function<QImage(const ObjectTrack &, const TrackKeyframe &)>;
using PoseCutout = std::function<QImage(const QImage &, const QImage &)>;

/* Pick up to `want` PTS-ordered keyframes from `track`, decode each through
 * `decode`, cut it through `cutout`, and return the successful poses plus
 * their real keyframe times. This is the single selection rule shared by the
 * controller and the live acceptance harness; it never invents a pose. */
QVector<QImage> collectTrackedPoses(const ObjectTrack &track, int want,
                                    QVector<qint64> *pts,
                                    const PoseFrameDecoder &decode,
                                    const PoseCutout &cutout);

} // namespace creative
} // namespace paintbrush

#endif // PB_CREATIVE_POSES_HPP
