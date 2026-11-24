/*****************************************************************************
 * pb_creative_poses.cpp : Paintbrush creative tracked-pose selection
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_creative_poses.hpp"

#include "pb_document.hpp"

#include <cmath>

namespace paintbrush::creative {

QVector<QImage> collectTrackedPoses(const ObjectTrack &track, int want,
                                    QVector<qint64> *pts,
                                    const PoseFrameDecoder &decode,
                                    const PoseCutout &cutout)
{
    want = qBound(1, want, 8);
    if (pts)
        pts->clear();

    QVector<QImage> poses;
    const int n = track.keyframes.size();
    if (n <= 0 || !decode || !cutout)
        return poses;

    QVector<int> indices;
    if (n <= want)
    {
        for (int i = 0; i < n; ++i)
            indices.append(i);
    }
    else if (want == 1)
    {
        indices.append(0);
    }
    else
    {
        for (int k = 0; k < want; ++k)
        {
            const int idx = qBound(
                0,
                int(std::lround(double(k) * (n - 1) / double(want - 1))),
                n - 1);
            if (!indices.contains(idx))
                indices.append(idx);
        }
    }

    for (int idx : indices)
    {
        const TrackKeyframe &kf = track.keyframes.at(idx);
        const QImage frame = decode(track, kf);
        if (frame.isNull())
            continue;
        const QImage sticker = cutout(frame, kf.mask);
        if (sticker.isNull())
            continue;
        poses.append(sticker);
        if (pts)
            pts->append(kf.timeMs);
    }
    return poses;
}

} // namespace paintbrush::creative
