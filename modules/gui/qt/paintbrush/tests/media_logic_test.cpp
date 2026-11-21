/*****************************************************************************
 * media_logic_test.cpp : standalone harness for pb_media_logic.hpp
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

/* pb_media_logic.hpp is deliberately VLC-free: this test compiles with Qt
 * Core/Gui only and verifies the exact-frame decision rules that everything
 * else (thumbnails, tracking, Object Select, Add frame) depends on. */

#include "pb_media_logic.hpp"

#include <QCoreApplication>
#include <QImage>
#include <QString>

#include <cstdio>

using namespace paintbrush;

namespace {

int failures = 0;

void check(const char *name, bool ok)
{
    std::printf("ASSERT.%s|%s\n", name, ok ? "PASS" : "FAIL");
    if (!ok)
        ++failures;
}

QImage solidImage(int w, int h)
{
    QImage image(w, h, QImage::Format_RGB32);
    image.fill(Qt::black);
    return image;
}

void testFrameRef()
{
    FrameRef ref;
    check("frameref.empty-invalid", !ref.isValid());

    ref.mrl = QStringLiteral("file:///video.mp4");
    ref.ptsMs = 0;
    check("frameref.mrl-zero-pts-valid", ref.isValid());

    ref.ptsMs = -1;
    check("frameref.negative-pts-invalid", !ref.isValid());

    ref.ptsMs = 6000;
    ref.width = 1920;
    ref.height = 1080;
    check("frameref.full-valid", ref.isValid());
}

void testDecodedFrame()
{
    DecodedFrame frame;
    check("decoded.empty-not-ok", !frame.ok());

    frame.image = solidImage(2, 2);
    check("decoded.image-unverified-not-ok", !frame.ok());

    frame.ptsVerified = true;
    check("decoded.image-verified-ok", frame.ok());

    frame.image = QImage();
    check("decoded.null-image-not-ok", !frame.ok());
}

void testPtsTolerance()
{
    check("pts.exact-zero", ptsWithinTolerance(1000, 1000, 0));
    check("pts.one-ms-outside-zero", !ptsWithinTolerance(1000, 1001, 0));
    check("pts.inside-250", ptsWithinTolerance(1000, 1250, 250));
    check("pts.outside-250", !ptsWithinTolerance(1000, 1251, 250));
    check("pts.negative-actual", !ptsWithinTolerance(1000, -1, 250));
    check("pts.negative-request-invalid",
          !ptsWithinTolerance(-500, -500, 250));
    check("pts.negative-tolerance-clamped",
          ptsWithinTolerance(1000, 1000, -5));
}

void testLruCache()
{
    /* Each 10x10 RGB32 image is 400 bytes. Budget 1000 keeps two entries;
     * the third insertion evicts the least-recently-used one. */
    LruCache<QImage> cache(1000);
    cache.insert(QStringLiteral("a"), solidImage(10, 10));
    cache.insert(QStringLiteral("b"), solidImage(10, 10));
    check("lru.two-fit", cache.size() == 2);
    check("lru.bytes", cache.bytes() == 800);

    /* Touch a so b becomes the LRU entry. */
    (void)cache.get(QStringLiteral("a"));
    cache.insert(QStringLiteral("c"), solidImage(10, 10));
    check("lru.budget-eviction", cache.size() == 2);
    check("lru.touched-survives", cache.contains(QStringLiteral("a")));
    check("lru.untouched-evicted", !cache.contains(QStringLiteral("b")));
    check("lru.new-entry-present", cache.contains(QStringLiteral("c")));

    const QList<QString> order = cache.keysLruOrder();
    check("lru.order-mru-first",
          order.size() == 2 && order[0] == QStringLiteral("c")
              && order[1] == QStringLiteral("a"));

    cache.clear();
    check("lru.clear", cache.size() == 0 && cache.bytes() == 0);
}

void testThumbnailPriority()
{
    ThumbnailPriorityQueue queue({1000, 2000, 3000}, 1800);
    queue.push(3000);
    queue.push(1000);
    queue.push(2000);
    check("priority.anchor-first", queue.popHighest() == 1000);
    check("priority.second", queue.popHighest() == 2000);
    check("priority.third", queue.popHighest() == 3000);
    check("priority.empty-zero", queue.popHighest() == 0);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testFrameRef();
    testDecodedFrame();
    testPtsTolerance();
    testLruCache();
    testThumbnailPriority();

    std::printf("VERDICT|%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
