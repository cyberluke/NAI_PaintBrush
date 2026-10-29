/*****************************************************************************
 * pb_export.cpp : NAI Paintbrush — export helpers
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_export.hpp"
#include "pb_document.hpp"

#include <QImageWriter>
#include <QFileInfo>
#include <QDir>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>

namespace paintbrush {

static QString writeImage(const QImage &image, const QString &path,
                          const QString &format)
{
    QImageWriter writer(path);
    if (!format.isEmpty())
        writer.setFormat(format.toLower().toLatin1());
    writer.setQuality(92);
    if (!writer.write(image))
        return QStringLiteral("Export failed: %1").arg(writer.errorString());
    return {};
}

QString ExportService::exportImage(PaintbrushDocument *doc, const QString &path,
                                   const QString &format)
{
    return exportImageOptions(doc, path, format, 1.0, false);
}

QString ExportService::exportImageOptions(PaintbrushDocument *doc,
                                          const QString &path,
                                          const QString &format,
                                          double scale,
                                          bool cropToContent,
                                          const QColor &background)
{
    if (!doc)
        return QStringLiteral("No document to export.");
    /* the canonical renderer: export shows exactly what the screen shows.
     * At scale != 1 the paper, material state and finish chains are
     * re-evaluated in world units at export resolution (spec §55) instead of
     * bitmap-upscaling the 1x flatten. */
    QImage flat = (scale > 0.0 && !qFuzzyCompare(scale, 1.0))
        ? DocumentRenderer::renderScaled(*doc, float(scale))
        : DocumentRenderer::render(*doc);
    if (flat.isNull())
        return QStringLiteral("The canvas is empty.");

    if (cropToContent)
    {
        /* trim fully-transparent margins so exports are tight to the art */
        int minX = flat.width(), minY = flat.height();
        int maxX = -1, maxY = -1;
        for (int y = 0; y < flat.height(); ++y)
        {
            const QRgb *line = reinterpret_cast<const QRgb *>(flat.constScanLine(y));
            for (int x = 0; x < flat.width(); ++x)
            {
                if (qAlpha(line[x]) != 0)
                {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }
        if (maxX >= 0)
            flat = flat.copy(QRect(minX, minY, maxX - minX + 1, maxY - minY + 1));
    }

    const QFileInfo info(path);
    QString fmt = format;
    if (fmt.isEmpty())
        fmt = info.suffix();

    /* opaque formats get a background fill so alpha doesn't render black */
    const QString lower = fmt.toLower();
    if (background.alpha() > 0 &&
        (lower == QStringLiteral("jpg") || lower == QStringLiteral("jpeg")))
    {
        QImage opaque(flat.size(), QImage::Format_RGB32);
        opaque.fill(background);
        QPainter p(&opaque);
        p.drawImage(0, 0, flat);
        p.end();
        flat = opaque;
    }

    return writeImage(flat, path, fmt);
}

QString ExportService::exportImageFile(const QImage &image, const QString &path,
                                       const QString &format)
{
    if (image.isNull())
        return QStringLiteral("Nothing to export.");

    const QFileInfo info(path);
    QString fmt = format;
    if (fmt.isEmpty())
        fmt = info.suffix();
    return writeImage(image, path, fmt);
}

QString ExportService::exportMask(const QImage &mask, const QString &path)
{
    if (mask.isNull())
        return QStringLiteral("No mask to export.");
    const QImage gray = maskToImage(mask);
    return writeImage(gray, path, QStringLiteral("png"));
}

QString ExportService::exportCutout(const QImage &cutout, const QString &path)
{
    if (cutout.isNull())
        return QStringLiteral("No cutout to export.");

    /* trim transparent bounds */
    int minX = cutout.width(), minY = cutout.height();
    int maxX = -1, maxY = -1;
    for (int y = 0; y < cutout.height(); ++y)
    {
        const QRgb *line = reinterpret_cast<const QRgb *>(cutout.constScanLine(y));
        for (int x = 0; x < cutout.width(); ++x)
        {
            if (qAlpha(line[x]) != 0)
            {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < 0)
        return QStringLiteral("The cutout is empty.");

    const QImage trimmed = cutout.copy(QRect(minX, minY,
                                             maxX - minX + 1, maxY - minY + 1));
    return writeImage(trimmed, path, QStringLiteral("png"));
}

QString ExportService::exportLivingPoster(PaintbrushDocument *doc, const QString &dir,
                                          const QString &stem,
                                          qint64 durationMs, int fps,
                                          bool paper, double scale)
{
    if (!doc)
        return QStringLiteral("No document to export.");
    if (durationMs <= 0)
        return QStringLiteral("Duration must be positive.");
    if (fps <= 0 || fps > 120)
        return QStringLiteral("FPS must be between 1 and 120.");

    QDir outDir(dir);
    if (!outDir.exists() && !QDir().mkpath(dir))
        return QStringLiteral("Cannot create %1.").arg(dir);

    /* canonical time grid: the loop closes exactly at durationMs, and the
     * first frame (t=0) equals the static poster for a phase-aligned loop */
    const int frames = qMax(1, int(std::lround(double(durationMs) * fps / 1000.0)));
    for (int i = 0; i < frames; ++i)
    {
        const qint64 t = qMin<qint64>(durationMs, qint64(double(i) * durationMs / frames));
        QImage frame = paper
            ? DocumentRenderer::renderScaled(*doc, float(scale), t)
            : DocumentRenderer::renderTransparentAt(*doc, float(scale), t);
        if (frame.isNull())
            return QStringLiteral("Render failed at frame %1.").arg(i);
        const QString path = outDir.filePath(
            QStringLiteral("%1_%2.png").arg(stem,
                QString::number(i).rightJustified(4, QLatin1Char('0'))));
        const QString err = writeImage(frame, path, QStringLiteral("png"));
        if (!err.isEmpty())
            return err;
    }
    return {};
}

/* ------------------------------------------------- Living Showroom video */

namespace {

/* Deterministic probe order: PATH first, then the environments this
 * project actually builds in (msys2, VLC contribs). */
QString findFfmpeg()
{
    QString found = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!found.isEmpty())
        return found;
    const QStringList candidates = {
        QStringLiteral("C:/msys64/usr/bin/ffmpeg.exe"),
        QStringLiteral("C:/msys64/mingw64/bin/ffmpeg.exe"),
        QStringLiteral("C:/msys64/ucrt64/bin/ffmpeg.exe"),
    };
    for (const QString &c : candidates)
        if (QFile::exists(c))
            return c;
    return {};
}

QString ffmpegEncoderProbe(const QString &ffmpeg, const QString &encoder)
{
    static QHash<QString, QString> s_cache;   /* encoder -> ffmpeg path or "" */
    const auto it = s_cache.constFind(encoder);
    if (it != s_cache.constEnd())
        return it.value();
    QString result;
    QProcess probe;
    probe.start(ffmpeg, { QStringLiteral("-hide_banner"), QStringLiteral("-encoders") });
    if (probe.waitForStarted(3000) && probe.waitForFinished(8000))
    {
        const QByteArray out = probe.readAllStandardOutput() + probe.readAllStandardError();
        if (out.contains(encoder.toUtf8()))
            result = ffmpeg;
    }
    s_cache.insert(encoder, result);
    return result;
}

} // namespace

bool ExportService::canExportLivingVideo(const QString &format)
{
    const QString ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty())
        return false;
    const QString encoder = format == QLatin1String("webm")
        ? QStringLiteral("libvpx-vp9") : QStringLiteral("libx264");
    return !ffmpegEncoderProbe(ffmpeg, encoder).isEmpty();
}

QString ExportService::exportLivingVideo(PaintbrushDocument *doc, const QString &dir,
                                         const QString &stem, qint64 durationMs, int fps,
                                         const QString &format, const QString &outPath)
{
    const QString ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty())
        return QStringLiteral("ffmpeg is not available on this system — install it or add it to PATH.");
    const bool webm = format == QLatin1String("webm");
    const QString encoder = webm ? QStringLiteral("libvpx-vp9") : QStringLiteral("libx264");
    if (ffmpegEncoderProbe(ffmpeg, encoder).isEmpty())
        return QStringLiteral("ffmpeg has no %1 encoder — the %2 export cannot be muxed.")
            .arg(encoder, webm ? QStringLiteral("WebM/VP9 alpha") : QStringLiteral("MP4/H.264"));

    /* canonical intermediate: the deterministic PNG sequence (paper for
     * MP4, transparent for WebM alpha) */
    const QString err = exportLivingPoster(doc, dir, stem, durationMs, fps,
                                           !webm, 1.0);
    if (!err.isEmpty())
        return err;

    QStringList args;
    args << QStringLiteral("-y") << QStringLiteral("-hide_banner")
         << QStringLiteral("-framerate") << QString::number(fps)
         << QStringLiteral("-i")
         << QDir(dir).filePath(QStringLiteral("%1_%2.png").arg(stem, QStringLiteral("%04d")))
         << QStringLiteral("-c:v") << encoder;
    if (webm)
        args << QStringLiteral("-pix_fmt") << QStringLiteral("yuva420p")
             << QStringLiteral("-auto-alt-ref") << QStringLiteral("0");
    else
        args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p")
             << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    args << outPath;

    QProcess mux;
    mux.start(ffmpeg, args);
    if (!mux.waitForStarted(5000))
        return QStringLiteral("Cannot start ffmpeg.");
    if (!mux.waitForFinished(600000))
    {
        mux.kill();
        return QStringLiteral("ffmpeg timed out while muxing the video.");
    }
    if (mux.exitCode() != 0 || !QFile::exists(outPath))
        return QStringLiteral("ffmpeg failed to mux the video (%1).")
            .arg(QString::fromUtf8(mux.readAllStandardError()).trimmed());
    if (QFileInfo(outPath).size() <= 0)
        return QStringLiteral("The muxed video is empty.");
    return {};
}

} // namespace paintbrush