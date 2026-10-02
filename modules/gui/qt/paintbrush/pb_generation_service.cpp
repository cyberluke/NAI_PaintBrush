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

    if (m_baseUrl.isEmpty())
    {
        m_unavailableReason =
            QStringLiteral("Image generation is unavailable: image provider is not configured.");
        emit availabilityChanged(false, m_unavailableReason);
        return;
    }
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

QImage GenerationService::resultFromJson(const QByteArray &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return {};
    const QJsonObject root = doc.object();
    const QJsonArray data = root[QStringLiteral("data")].toArray();
    if (data.isEmpty())
        return {};

    const QJsonObject first = data.first().toObject();
    const QString b64 = first[QStringLiteral("b64_json")].toString();
    if (!b64.isEmpty())
    {
        QImage image;
        image.loadFromData(QByteArray::fromBase64(b64.toLatin1()));
        return image;
    }
    const QString url = first[QStringLiteral("url")].toString();
    if (!url.isEmpty())
    {
        QImage image;
        image.load(url);
        return image;
    }
    return {};
}

void GenerationService::generate(Action action, const QImage &image,
                                 const QImage &mask, const QString &prompt,
                                 const QString &requestId)
{
    if (!isConfigured())
    {
        emit errorOccurred(requestId, m_unavailableReason);
        return;
    }
    if (image.isNull())
    {
        emit errorOccurred(requestId,
                           QStringLiteral("Image generation needs an image."));
        return;
    }

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
    m_replies.append(reply);

    connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) {
        reply->ignoreSslErrors();
    });

    if (!m_busy)
    {
        m_busy = true;
        emit busyChanged(true);
    }
    connect(reply, &QNetworkReply::finished, this, &GenerationService::onReplyFinished);
}

void GenerationService::onReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    m_replies.removeAll(reply);
    const QString requestId = reply->property("pbRequestId").toString();

    const QByteArray data = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();

    if (error != QNetworkReply::NoError)
    {
        QString detail = QString::fromUtf8(data).left(300);
        if (detail.isEmpty())
            detail = reply->errorString();
        emit errorOccurred(requestId,
                           QStringLiteral("Image generation failed: provider did not respond (%1). %2")
                               .arg(int(error)).arg(detail));
    }
    else
    {
        QImage result = resultFromJson(data);
        if (result.isNull())
        {
            emit errorOccurred(requestId,
                               QStringLiteral("Image generation failed: provider returned "
                                              "an unreadable result."));
        }
        else
        {
            emit resultReady(requestId, result);
        }
    }

    reply->deleteLater();
    if (m_replies.isEmpty())
    {
        m_busy = false;
        emit busyChanged(false);
    }
}

void GenerationService::cancelAll()
{
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