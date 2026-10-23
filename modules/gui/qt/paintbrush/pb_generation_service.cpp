/*****************************************************************************
 * pb_generation_service.cpp : NAI Paintbrush — generative object actions
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_generation_service.hpp"
#include "pb_common.hpp"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QBuffer>
#include <QUrl>
#include <QSslError>
#include <QPainter>
#include <QImage>
#include <QVector>
#include <QPointer>
#include <QtMath>
#include <QDateTime>

#include <thread>

namespace paintbrush {

GenerationService::GenerationService(QObject *parent)
    : QObject(parent)
{
}

GenerationService::~GenerationService()
{
    cancelAll();
}

void GenerationService::configure(vlc_object_t *obj)
{
    m_obj = obj;
    m_baseUrl = pbConfigString(obj, "paintbrush-image-base-url").trimmed();
    m_model = pbConfigString(obj, "paintbrush-image-model").trimmed();
    m_apiKey = pbApiKey(obj, "PAINTBRUSH_IMAGE_API_KEY", "paintbrush-image-api-key");
    m_timeoutMs = 120000;
    /* A7: TLS validation stays ON unless the operator explicitly opts into
     * self-signed local endpoints (per-service or shared dev flag). */
    m_insecureTls = pbConfigBool(obj, "paintbrush-image-insecure")
                 || pbConfigBool(obj, "paintbrush-dev-insecure");

    if (m_baseUrl.isEmpty())
    {
        /* No external image provider: the local CPU fallbacks (onion-peel
         * inpaint / box blur) keep the actions usable, but they are NOT
         * generative — capability() reports local-heal / local-blur and the
         * UI must present them as such (A3/A4). */
        m_unavailableReason =
            QStringLiteral("No image provider configured — the local heal / blur "
                           "fallback is active (pixel operations, not generative).");
        emit availabilityChanged(true, m_unavailableReason);
        return;
    }
    m_unavailableReason.clear();
    emit availabilityChanged(true, {});
}

QString GenerationService::defaultPrompt(Action action, const QString &userPrompt)
{
    switch (action)
    {
    case ACTION_REMOVE_OBJECT:
        return QStringLiteral("Remove the selected object and seamlessly continue the "
                              "background behind it. Do not add new objects.");
    case ACTION_GENERATIVE_FILL:
        return userPrompt.isEmpty()
                   ? QStringLiteral("Fill the selected area with content that blends "
                                    "naturally with the surrounding image.")
                   : userPrompt;
    case ACTION_REPLACE_BACKGROUND:
        return userPrompt.isEmpty()
                   ? QStringLiteral("Replace the background with a soft, pleasant gradient "
                                    "that keeps the subject unchanged.")
                   : userPrompt;
    case ACTION_MAKE_STICKER:
        return QStringLiteral("Simplify the subject into a clean cartoon sticker style. "
                              "Keep the subject recognizable, remove background noise.");
    }
    return userPrompt;
}

QJsonObject GenerationService::firstDataObject(const QByteArray &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return {};
    const QJsonArray data = doc.object().value(QStringLiteral("data")).toArray();
    if (data.isEmpty())
        return {};
    return data.first().toObject();
}

CapabilityReport GenerationService::capability(Action action) const
{
    /* A3: remote-generation is claimed ONLY when a provider is configured.
     * Without a provider the report names the actual local fallback — never
     * "generative fill" (A4). */
    if (!m_baseUrl.isEmpty())
        return remoteGenerationReport(m_baseUrl, m_model);

    if (action == ACTION_REPLACE_BACKGROUND)
    {
        return localGenerationReport(
            CapabilityReport::LocalBlur,
            QStringLiteral("No image provider configured — local box-blur "
                           "background replace is active. This is a pixel "
                           "filter, not a generative model."));
    }
    return localGenerationReport(
        CapabilityReport::LocalHeal,
        QStringLiteral("No image provider configured — local onion-peel "
                       "inpaint heal is active. This is pixel propagation, "
                       "not a generative model."));
}

/* ------------------------------------------------------------------ local */

namespace {

/* Fill the masked region (white in `mask`) by multi-source propagation:
 * boundary pixels are filled from their already-known neighbours, then the
 * newly filled pixels become the source for the next layer. This is the
 * classic onion-peel inpaint, but done with a FIFO queue so the total work
 * is O(mask area) instead of O(mask radius x image size). */
static QImage localInpaint(const QImage &src, const QImage &mask)
{
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    const int w = img.width(), h = img.height();
    if (w <= 0 || h <= 0)
        return img;

    const QImage m8 = mask.convertToFormat(QImage::Format_Grayscale8);
    if (m8.size() != img.size())
        return img;

    /* state: 0 = known, 1 = masked, 2 = queued/filled */
    QVector<quint8> state(w * h);
    QVector<int> queue;
    queue.reserve(w * h);

    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};

    for (int y = 0; y < h; ++y)
    {
        const quint8 *line = m8.constScanLine(y);
        for (int x = 0; x < w; ++x)
        {
            const int i = y * w + x;
            if (line[x] >= 128)
            {
                state[i] = 1;
                /* seed: any masked pixel adjacent to a known pixel */
                bool nearKnown = false;
                for (int k = 0; k < 4 && !nearKnown; ++k)
                {
                    const int nx = x + dx[k], ny = y + dy[k];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                        continue;
                    const quint8 nv = m8.constScanLine(ny)[nx];
                    if (nv < 128)
                        nearKnown = true;
                }
                if (nearKnown)
                {
                    state[i] = 2;
                    queue.append(i);
                }
            }
        }
    }

    int head = 0;
    while (head < queue.size())
    {
        const int i = queue[head++];
        const int x = i % w, y = i / w;
        if (state[i] != 2)
            continue;

        qint64 r = 0, g = 0, b = 0, a = 0;
        int n = 0;
        for (int k = 0; k < 4; ++k)
        {
            const int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                continue;
            /* only pixels that are already known/filled (state 0) are valid
             * colour sources; queued (2) and masked (1) are not */
            if (state[ny * w + nx] != 0)
                continue;
            const QRgb c = img.pixel(nx, ny);
            r += qRed(c); g += qGreen(c); b += qBlue(c); a += qAlpha(c);
            ++n;
        }
        if (n > 0)
        {
            img.setPixel(x, y, qRgba(int(r / n), int(g / n), int(b / n),
                                     int(a / n)));
            state[i] = 0; /* filled; becomes a source for the next layer */

            /* newly exposed masked neighbours join the queue */
            for (int k = 0; k < 4; ++k)
            {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h)
                    continue;
                const int ni = ny * w + nx;
                if (state[ni] == 1)
                {
                    state[ni] = 2;
                    queue.append(ni);
                }
            }
        }
    }
    return img;
}

/* Soften everything except the subject: the "replace background" local
 * fallback. Keeps the subject sharp and turns the rest into a gentle blur. */
static QImage replaceBackgroundLocal(const QImage &src, const QImage &objectMask)
{
    QImage img = src.convertToFormat(QImage::Format_ARGB32);
    const QImage m8 = objectMask.convertToFormat(QImage::Format_Grayscale8);
    if (m8.size() != img.size())
        return img;

    /* heavy box blur (two passes, radius 12 => cheap 3x3 repeated) */
    QImage blurred = img;
    for (int pass = 0; pass < 4; ++pass)
    {
        QImage next = blurred;
        const int w = blurred.width(), h = blurred.height();
        for (int y = 1; y < h - 1; ++y)
            for (int x = 1; x < w - 1; ++x)
            {
                qint64 r = 0, g = 0, b = 0, a = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const QRgb c = blurred.pixel(x + dx, y + dy);
                        r += qRed(c); g += qGreen(c); b += qBlue(c); a += qAlpha(c);
                    }
                next.setPixel(x, y, qRgba(int(r / 9), int(g / 9), int(b / 9),
                                          int(a / 9)));
            }
        blurred = next;
    }

    /* composite: subject from the original, background from the blur */
    QImage out = img;
    QPainter p(&out);
    p.drawImage(0, 0, blurred);
    p.end();

    /* blend: where mask is white keep original, else keep blurred */
    for (int y = 0; y < out.height(); ++y)
    {
        const quint8 *ml = m8.constScanLine(y);
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        const QRgb *orig = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        const QRgb *blur = reinterpret_cast<const QRgb *>(blurred.constScanLine(y));
        for (int x = 0; x < out.width(); ++x)
        {
            const int v = ml[x];
            if (v >= 128)
                line[x] = orig[x];
            else if (v > 0)
            {
                /* soft edge between subject and background */
                const int keep = v;
                const int mix = 255 - keep;
                const QRgb o = orig[x], b = blur[x];
                line[x] = qRgba(
                    (qRed(o) * keep + qRed(b) * mix) / 255,
                    (qGreen(o) * keep + qGreen(b) * mix) / 255,
                    (qBlue(o) * keep + qBlue(b) * mix) / 255,
                    255);
            }
            else
                line[x] = blur[x];
        }
    }
    return out;
}

} // namespace

void GenerationService::generate(Action action, const QImage &image,
                                 const QImage &mask, const QString &prompt,
                                 const QString &requestId)
{
    if (image.isNull())
    {
        emit errorOccurred(requestId,
                           QStringLiteral("Image generation needs an image."));
        return;
    }

    /* local fallback when no external provider is configured */
    if (m_baseUrl.isEmpty())
    {
        /* A5: the CPU inpaint / box blur runs on a worker thread so
         * generate() returns immediately and the GUI thread never blocks.
         * resultReady is delivered back on the GUI thread through a queued
         * invocation once the worker finishes. */
        setBusy(true);

        QImage imageCopy = image;
        QImage maskCopy = mask;
        imageCopy.detach(); /* deep copies: the GUI thread may keep using */
        maskCopy.detach();  /* the caller's images while we read these     */

        const Action actionCopy = action;
        const QString id = requestId;
        const quint64 epoch = m_cancelEpoch;
        QPointer<GenerationService> self(this);

        std::thread([self, id, actionCopy, imageCopy, maskCopy, epoch]() {
            QImage result;
            if (actionCopy == ACTION_REPLACE_BACKGROUND)
            {
                /* mask here is the inverse of the subject; pass the subject mask */
                QImage subjectMask = maskCopy.convertToFormat(QImage::Format_Grayscale8);
                subjectMask.invertPixels();
                result = replaceBackgroundLocal(imageCopy, subjectMask);
            }
            else
            {
                /* remove object / generative fill: inpaint the masked region */
                result = localInpaint(imageCopy, maskCopy);
            }

            QMetaObject::invokeMethod(self, [self, id, result, epoch]() {
                if (!self || epoch != self->m_cancelEpoch)
                    return; /* service destroyed or generation cancelled */
                self->m_busy = false;
                emit self->busyChanged(false);
                if (result.isNull())
                    emit self->errorOccurred(
                        id, QStringLiteral("Local inpainting produced no result."));
                else
                    emit self->resultReady(id, result);
            }, Qt::QueuedConnection);
        }).detach();
        return;
    }

    /* external provider path */
    QUrl url(m_baseUrl);
    QString path = url.path();
    if (!path.endsWith(QStringLiteral("/images/edits")))
    {
        if (!path.endsWith(QLatin1Char('/')))
            path += QLatin1Char('/');
        url.setPath(path + QStringLiteral("images/edits"));
    }

    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);

    auto addTextPart = [multi](const QString &name, const QString &value) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"%1\"").arg(name));
        part.setBody(value.toUtf8());
        multi->append(part);
    };
    auto addImagePart = [multi](const QString &name, const QImage &img) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        img.save(&buffer, "PNG");
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentTypeHeader,
                       QStringLiteral("image/png"));
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QStringLiteral("form-data; name=\"%1\"; filename=\"%1.png\"")
                           .arg(name));
        part.setBody(bytes);
        multi->append(part);
    };

    addImagePart(QStringLiteral("image"), image);
    if (!mask.isNull())
        addImagePart(QStringLiteral("mask"), mask);
    addTextPart(QStringLiteral("prompt"), defaultPrompt(action, prompt));
    if (!m_model.isEmpty())
        addTextPart(QStringLiteral("model"), m_model);
    addTextPart(QStringLiteral("n"), QStringLiteral("1"));

    QNetworkRequest request(url);
    request.setTransferTimeout(m_timeoutMs);
    if (!m_apiKey.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());

    QNetworkReply *reply = m_network.post(request, multi);
    multi->setParent(reply);
    reply->setProperty("pbRequestId", requestId);
    reply->setProperty("pbStartedAt", QDateTime::currentMSecsSinceEpoch());
    m_replies.append(reply);

    handleSslErrors(reply);

    setBusy(true);
    connect(reply, &QNetworkReply::finished, this, &GenerationService::onReplyFinished);
}

void GenerationService::onReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    m_replies.removeAll(reply);
    const QString requestId = reply->property("pbRequestId").toString();
    const qint64 startedAt = reply->property("pbStartedAt").toLongLong();
    const qint64 elapsed = startedAt > 0
        ? QDateTime::currentMSecsSinceEpoch() - startedAt : 0;
    const bool imageFetch = reply->property("pbIsImageFetch").toBool();
    const char *kind = imageFetch ? "generation-image" : "generation";

    const QByteArray data = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();

    if (error != QNetworkReply::NoError)
    {
        QString detail = QString::fromUtf8(data).left(300);
        if (detail.isEmpty())
            detail = reply->errorString();

        /* A7: TLS validation failures are surfaced truthfully instead of
         * being silently ignored. (Qt5/Qt6-safe: the failure detail is
         * recorded by the sslErrors handler, never read back from a
         * getter that only Qt5 had.) */
        const QString sslDetail = reply->property("pbSslDetail").toString();
        const bool tlsFailure = !sslDetail.isEmpty() ||
                                error == QNetworkReply::SslHandshakeFailedError;

        if (tlsFailure)
        {
            pbLogRequest(m_obj, kind, requestId, elapsed, false,
                         QStringLiteral("TLS: %1").arg(sslDetail));
            emit errorOccurred(requestId,
                QStringLiteral("Image generation failed: TLS certificate validation "
                               "failed. %1 Enable the paintbrush-dev-insecure option "
                               "only for trusted self-signed local endpoints.")
                    .arg(sslDetail.isEmpty() ? detail : sslDetail));
        }
        else
        {
            pbLogRequest(m_obj, kind, requestId, elapsed, false, detail);
            emit errorOccurred(requestId,
                               QStringLiteral("Image generation failed: provider did not respond (%1). %2")
                                   .arg(int(error)).arg(detail));
        }
    }
    else if (imageFetch)
    {
        /* A6: the provider returned an image URL; this reply carries the
         * downloaded bytes. */
        QImage result = QImage::fromData(data);
        if (result.isNull())
        {
            pbLogRequest(m_obj, kind, requestId, elapsed, false,
                         QStringLiteral("unreadable image bytes"));
            emit errorOccurred(requestId,
                               QStringLiteral("Image generation failed: the provider "
                                              "image URL returned unreadable data."));
        }
        else
        {
            pbLogRequest(m_obj, kind, requestId, elapsed, true);
            emit resultReady(requestId, result);
        }
    }
    else
    {
        const QJsonObject first = firstDataObject(data);
        if (first.isEmpty())
        {
            pbLogRequest(m_obj, kind, requestId, elapsed, false,
                         QStringLiteral("unreadable result"));
            emit errorOccurred(requestId,
                               QStringLiteral("Image generation failed: provider returned "
                                              "an unreadable result."));
        }
        else
        {
            const QString b64 = first.value(QStringLiteral("b64_json")).toString();
            if (!b64.isEmpty())
            {
                QImage result = QImage::fromData(QByteArray::fromBase64(b64.toLatin1()));
                if (result.isNull())
                {
                    pbLogRequest(m_obj, kind, requestId, elapsed, false,
                                 QStringLiteral("undecodable image"));
                    emit errorOccurred(requestId,
                                       QStringLiteral("Image generation failed: provider "
                                                      "returned an unreadable image."));
                }
                else
                {
                    pbLogRequest(m_obj, kind, requestId, elapsed, true);
                    emit resultReady(requestId, result);
                }
            }
            else
            {
                const QString url = first.value(QStringLiteral("url")).toString();
                if (url.isEmpty())
                {
                    pbLogRequest(m_obj, kind, requestId, elapsed, false,
                                 QStringLiteral("result without image data"));
                    emit errorOccurred(requestId,
                                       QStringLiteral("Image generation failed: provider "
                                                      "returned a result without image data."));
                }
                else
                {
                    /* never QImage::load(url): download asynchronously */
                    fetchImage(QUrl(url), requestId, startedAt);
                }
            }
        }
    }

    reply->deleteLater();
    if (m_replies.isEmpty())
    {
        m_busy = false;
        emit busyChanged(false);
    }
}

void GenerationService::fetchImage(const QUrl &url, const QString &requestId,
                                   qint64 startedAt)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(m_timeoutMs);
    if (!m_apiKey.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());

    QNetworkReply *reply = m_network.get(request);
    reply->setProperty("pbRequestId", requestId);
    reply->setProperty("pbStartedAt", startedAt);
    reply->setProperty("pbIsImageFetch", true);
    m_replies.append(reply);

    handleSslErrors(reply);
    connect(reply, &QNetworkReply::finished, this, &GenerationService::onReplyFinished);
}

void GenerationService::handleSslErrors(QNetworkReply *reply) const
{
    const bool insecure = m_insecureTls;
    /* Always record the failure detail so errors stay truthful. */
    connect(reply, &QNetworkReply::sslErrors, reply,
            [reply, insecure](const QList<QSslError> &errors) {
        reply->setProperty("pbSslDetail", sslErrorDetail(errors));
        /* A7: bypass certificate validation ONLY with the explicit
         * paintbrush-image-insecure / paintbrush-dev-insecure opt-in.
         * Default: Qt aborts the request and onReplyFinished surfaces a
         * truthful TLS error. */
        if (insecure)
            reply->ignoreSslErrors();
    });
}

void GenerationService::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged(busy);
}

void GenerationService::cancelAll()
{
    ++m_cancelEpoch; /* invalidate in-flight local workers */
    for (QNetworkReply *reply : m_replies)
        reply->abort();
    m_replies.clear();
    if (m_busy)
    {
        m_busy = false;
        emit busyChanged(false);
    }
}

} // namespace paintbrush