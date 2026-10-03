/*****************************************************************************
 * pb_lookup_service.cpp : NAI Paintbrush — object/product lookup
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_lookup_service.hpp"
#include "pb_common.hpp"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QBuffer>
#include <QPainter>
#include <QUrl>
#include <QSslError>

namespace paintbrush {

LookupService::LookupService(QObject *parent)
    : QObject(parent)
{
}

void LookupService::configure(vlc_object_t *obj)
{
    m_obj = obj;

    /* Reuse the assistant gateway: it is the configured chat endpoint and
     * (through the SSEProxy vision sidecar) accepts image parts, which is
     * exactly what product identification needs. A dedicated lookup vendor
     * can still be wired here later without touching the editor. */
    m_baseUrl = pbConfigString(obj, "paintbrush-assistant-base-url").trimmed();
    m_model = pbConfigString(obj, "paintbrush-assistant-model").trimmed();
    m_apiKey = pbApiKey(obj, "PAINTBRUSH_ASSISTANT_API_KEY",
                        "paintbrush-assistant-api-key");
    m_timeoutMs = pbConfigInt(obj, "paintbrush-assistant-timeout") * 1000;
    if (m_timeoutMs <= 0)
        m_timeoutMs = 120000;

    if (m_baseUrl.isEmpty())
    {
        m_configured = false;
        m_providerName = QStringLiteral("none");
        m_unavailableReason =
            QStringLiteral("Object lookup is unavailable: no chat gateway is configured.");
        emit availabilityChanged(false, m_unavailableReason);
        return;
    }

    m_configured = true;
    m_providerName = QStringLiteral("assistant-gateway");
    emit availabilityChanged(true, {});
}

/* Cap the long edge so payloads stay small. */
static QImage capped(const QImage &image, int maxEdge)
{
    if (image.isNull())
        return {};
    if (qMax(image.width(), image.height()) <= maxEdge)
        return image;
    const qreal scale = qreal(maxEdge) / qreal(qMax(image.width(), image.height()));
    return image.scaled(qMax(1, int(image.width() * scale)),
                        qMax(1, int(image.height() * scale)),
                        Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

/* The AI sees an opaque image: composite onto white so a transparent
 * crop is not read as a blank picture. */
static QImage opaque(const QImage &image)
{
    if (image.isNull())
        return {};
    if (image.format() == QImage::Format_RGB32 ||
        image.format() == QImage::Format_RGB888)
        return image;
    QImage out(image.size(), QImage::Format_ARGB32);
    out.fill(Qt::white);
    QPainter p(&out);
    p.drawImage(0, 0, image);
    p.end();
    return out;
}

static QByteArray pngDataUrl(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return "data:image/png;base64," + bytes.toBase64();
}

bool LookupService::lookup(const QImage &objectCrop, const QString &vlmDescription,
                           const QString &ocrText, const QString &query)
{
    if (!m_configured)
    {
        emit errorOccurred(m_unavailableReason);
        return false;
    }
    if (objectCrop.isNull())
    {
        emit errorOccurred(QStringLiteral("Object lookup needs the selected object."));
        return false;
    }

    QJsonArray content;

    QJsonObject textPart;
    textPart[QStringLiteral("type")] = QStringLiteral("text");
    QString prompt =
        QStringLiteral(
            "You are a product identification assistant inside a paint program. "
            "The user selected an object in a picture or video frame.\n"
            "Identify the object: what it is, and — when the visible evidence "
            "supports it — the likely brand and model/SKU. Never invent a brand "
            "or model you cannot see; otherwise describe the category and obvious "
            "features (colors, materials, logos, text).\n"
            "Reply in JSON with exactly this shape:\n"
            "{\"title\": \"...\", \"description\": \"...\", \"confidence\": 0.0, "
            "\"evidence\": \"...\", \"metadata\": {\"category\": \"...\", "
            "\"brand\": \"...\", \"model\": \"...\"}}\n"
            "confidence is 0..1. metadata fields may be empty strings when unknown.");
    if (!query.isEmpty())
        prompt += QStringLiteral("\nUser question: %1").arg(query);
    if (!vlmDescription.isEmpty())
        prompt += QStringLiteral("\nVision model description: %1").arg(vlmDescription);
    if (!ocrText.isEmpty())
        prompt += QStringLiteral("\nOCR text found in/near the object: %1").arg(ocrText);
    textPart[QStringLiteral("text")] = prompt;
    content.append(textPart);

    QJsonObject imgPart;
    imgPart[QStringLiteral("type")] = QStringLiteral("image_url");
    QJsonObject urlObj;
    urlObj[QStringLiteral("url")] =
        QString::fromLatin1(pngDataUrl(capped(opaque(objectCrop), 768)));
    imgPart[QStringLiteral("image_url")] = urlObj;
    content.append(imgPart);

    QJsonObject userMsg;
    userMsg[QStringLiteral("role")] = QStringLiteral("user");
    userMsg[QStringLiteral("content")] = content;

    QJsonArray messages;
    messages.append(userMsg);

    QJsonObject payload;
    payload[QStringLiteral("model")] =
        m_model.isEmpty() ? QStringLiteral("default") : m_model;
    payload[QStringLiteral("messages")] = messages;
    payload[QStringLiteral("max_tokens")] = 512;
    payload[QStringLiteral("temperature")] = 0.2;

    QUrl url(m_baseUrl);
    QString path = url.path();
    if (!path.endsWith(QStringLiteral("/chat/completions")))
    {
        if (!path.endsWith(QLatin1Char('/')))
            path += QLatin1Char('/');
        url.setPath(path + QStringLiteral("chat/completions"));
    }

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setTransferTimeout(m_timeoutMs);
    if (!m_apiKey.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());

    m_requestId++;
    const int requestId = m_requestId;

    QNetworkReply *reply = m_network.post(request,
                                          QJsonDocument(payload).toJson(QJsonDocument::Compact));
    reply->setProperty("pbLookupRequestId", requestId);
    m_replies.append(reply);

    connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) {
        reply->ignoreSslErrors();
    });
    connect(reply, &QNetworkReply::finished, this, [this, requestId]() {
        onReplyFinished(requestId);
    });
    return true;
}

void LookupService::onReplyFinished(int requestId)
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    m_replies.removeAll(reply);

    const QByteArray data = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();

    if (error != QNetworkReply::NoError)
    {
        QString detail = QString::fromUtf8(data).left(300);
        if (detail.isEmpty())
            detail = reply->errorString();
        emit errorOccurred(QStringLiteral("Object lookup failed: the chat endpoint did not respond (%1). %2")
                               .arg(int(error)).arg(detail));
    }
    else
    {
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        QJsonObject root = doc.object();
        QJsonArray choices = root[QStringLiteral("choices")].toArray();
        QString content;
        if (!choices.isEmpty())
        {
            const QJsonObject choice = choices.first().toObject();
            const QJsonValue message = choice[QStringLiteral("message")];
            if (message.isObject())
                content = message.toObject()[QStringLiteral("content")].toString();
            else
                content = choice[QStringLiteral("text")].toString();
        }

        QVariantList matches;
        if (!content.isEmpty())
        {
            /* try to extract the JSON object the prompt asked for */
            const int start = content.indexOf('{');
            const int end = content.lastIndexOf('}');
            if (start >= 0 && end > start)
            {
                const QJsonDocument objDoc = QJsonDocument::fromJson(
                    content.mid(start, end - start + 1).toUtf8());
                if (objDoc.isObject())
                {
                    const QJsonObject obj = objDoc.object();
                    QVariantMap match;
                    match[QStringLiteral("title")] = obj[QStringLiteral("title")].toString();
                    match[QStringLiteral("description")] =
                        obj[QStringLiteral("description")].toString();
                    match[QStringLiteral("confidence")] =
                        obj[QStringLiteral("confidence")].toDouble(0.0);
                    match[QStringLiteral("evidence")] =
                        obj[QStringLiteral("evidence")].toString();
                    match[QStringLiteral("metadata")] =
                        obj[QStringLiteral("metadata")].toObject().toVariantMap();
                    if (!match[QStringLiteral("title")].toString().isEmpty())
                        matches.append(match);
                }
            }
            if (matches.isEmpty())
            {
                /* model did not follow the JSON shape: surface the answer as
                 * a single plain match so the user still gets the result */
                QVariantMap match;
                match[QStringLiteral("title")] = content.trimmed().left(120);
                match[QStringLiteral("description")] = content.trimmed();
                match[QStringLiteral("confidence")] = 0.0;
                matches.append(match);
            }
        }
        if (matches.isEmpty())
        {
            emit errorOccurred(QStringLiteral("Object lookup returned no identification."));
        }
        else
        {
            m_matches = matches;
            emit lookupResult(requestId, matches);
        }
    }

    reply->deleteLater();
}

QVariantList LookupService::matches() const
{
    return m_matches;
}

} // namespace paintbrush