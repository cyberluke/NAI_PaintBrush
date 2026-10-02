/*****************************************************************************
 * pb_vision_service.cpp : NAI Paintbrush — Ask about this (VLM provider)
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_vision_service.hpp"
#include "pb_common.hpp"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QBuffer>
#include <QTimer>
#include <QUrl>
#include <QSslError>

namespace paintbrush {

VisionService::VisionService(QObject *parent)
    : QObject(parent)
{
}

VisionService::~VisionService()
{
    cancelAll();
}

void VisionService::configure(vlc_object_t *obj)
{
    m_obj = obj;
    m_baseUrl = pbConfigString(obj, "paintbrush-vlm-base-url").trimmed();
    m_model = pbConfigString(obj, "paintbrush-vlm-model").trimmed();
    m_timeoutMs = pbConfigInt(obj, "paintbrush-vlm-timeout") * 1000;
    if (m_timeoutMs <= 0)
        m_timeoutMs = 60000;
    m_apiKey = pbApiKey(obj, "PAINTBRUSH_VLM_API_KEY", "paintbrush-vlm-api-key");

    if (m_baseUrl.isEmpty())
    {
        m_unavailableReason =
            QStringLiteral("Ask about this is unavailable: vision endpoint is not configured.");
        emit availabilityChanged(false, m_unavailableReason);
        return;
    }
    emit availabilityChanged(true, {});
}

/* Cap the long edge so payloads stay small; keep the aspect ratio. */
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

static QByteArray pngDataUrl(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return "data:image/png;base64," + bytes.toBase64();
}

QByteArray VisionService::buildPayload(const QImage &crop, const QImage &context,
                                       const QString &question) const
{
    QJsonArray content;

    QJsonObject textPart;
    textPart[QStringLiteral("type")] = QStringLiteral("text");
    textPart[QStringLiteral("text")] =
        QStringLiteral(
            "You are a helpful creative assistant inside a paint program. "
            "The user selected an object in a picture or video frame and asks a question about it.\n"
            "You receive: (1) a crop of the selected object, (2) the full contextual frame.\n"
            "Answer concisely and honestly. Clearly separate:\n"
            "- what you can see (visible evidence),\n"
            "- what you are inferring (likely),\n"
            "- when you are not sure (uncertainty).\n"
            "For brand/product questions, only name a specific model/SKU when the visible "
            "evidence supports it; otherwise describe the category. Do not invent details.\n"
            "Reply in JSON with exactly three fields: {\"answer\": \"...\", \"evidence\": \"...\", "
            "\"uncertainty\": \"...\"} where uncertainty is a short phrase like \"low\", "
            "\"medium\", \"high\" followed by a brief reason. If the user asked a question that "
            "does not fit this shape, put your full reply in answer and leave evidence empty.");
    content.append(textPart);

    if (!crop.isNull())
    {
        QJsonObject imgPart;
        imgPart[QStringLiteral("type")] = QStringLiteral("image_url");
        QJsonObject urlObj;
        urlObj[QStringLiteral("url")] =
            QString::fromLatin1(pngDataUrl(capped(crop, 768)));
        imgPart[QStringLiteral("image_url")] = urlObj;
        content.append(imgPart);
    }
    if (!context.isNull())
    {
        QJsonObject imgPart;
        imgPart[QStringLiteral("type")] = QStringLiteral("image_url");
        QJsonObject urlObj;
        urlObj[QStringLiteral("url")] =
            QString::fromLatin1(pngDataUrl(capped(context, 1024)));
        imgPart[QStringLiteral("image_url")] = urlObj;
        content.append(imgPart);
    }

    QJsonObject userMsg;
    userMsg[QStringLiteral("role")] = QStringLiteral("user");
    userMsg[QStringLiteral("content")] = content;

    QJsonArray messages;
    messages.append(userMsg);

    QJsonObject payload;
    payload[QStringLiteral("model")] = m_model.isEmpty()
                                           ? QStringLiteral("default")
                                           : m_model;
    payload[QStringLiteral("messages")] = messages;
    payload[QStringLiteral("max_tokens")] = 512;
    payload[QStringLiteral("temperature")] = 0.4;

    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

QString VisionService::extractContent(const QByteArray &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return {};
    const QJsonObject root = doc.object();
    const QJsonArray choices = root[QStringLiteral("choices")].toArray();
    if (choices.isEmpty())
        return {};
    const QJsonObject choice = choices.first().toObject();
    const QJsonValue message = choice[QStringLiteral("message")];
    if (message.isObject())
        return message.toObject()[QStringLiteral("content")].toString();
    return choice[QStringLiteral("text")].toString();
}

void VisionService::parseStructured(const QString &content, QString *answer,
                                    QString *evidence, QString *uncertainty)
{
    *answer = content.trimmed();
    *evidence = {};
    *uncertainty = {};

    const int start = content.indexOf('{');
    const int end = content.lastIndexOf('}');
    if (start < 0 || end <= start)
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(
        content.mid(start, end - start + 1).toUtf8());
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    *answer = obj[QStringLiteral("answer")].toString(*answer);
    *evidence = obj[QStringLiteral("evidence")].toString();
    *uncertainty = obj[QStringLiteral("uncertainty")].toString();
}

void VisionService::ask(const QImage &objectCrop, const QImage &contextFrame,
                        const QString &question, const QString &conversationId)
{
    if (!isConfigured())
    {
        emit errorOccurred(conversationId, m_unavailableReason);
        return;
    }
    if (objectCrop.isNull() && contextFrame.isNull())
    {
        emit errorOccurred(conversationId,
                           QStringLiteral("Ask about this needs an image."));
        return;
    }

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

    QNetworkReply *reply = m_network.post(request, buildPayload(objectCrop,
                                                                contextFrame,
                                                                question));
    reply->setProperty("pbConversationId", conversationId);
    m_replies.append(reply);

    /* local endpoints often use self-signed certificates; log instead of
     * failing hard so local providers work out of the box */
    connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError> &) {
        reply->ignoreSslErrors();
    });

    if (!m_busy)
    {
        m_busy = true;
        emit busyChanged(true);
    }
    connect(reply, &QNetworkReply::finished, this, &VisionService::onReplyFinished);
}

void VisionService::onReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    m_replies.removeAll(reply);
    const QString conversationId = reply->property("pbConversationId").toString();

    const QByteArray data = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();

    if (error != QNetworkReply::NoError)
    {
        QString detail = QString::fromUtf8(data).left(300);
        if (detail.isEmpty())
            detail = reply->errorString();
        emit errorOccurred(conversationId,
                           QStringLiteral("Ask about this failed: vision endpoint did not respond (%1). %2")
                               .arg(int(error)).arg(detail));
    }
    else
    {
        const QString content = extractContent(data);
        if (content.isEmpty())
        {
            emit errorOccurred(conversationId,
                               QStringLiteral("Ask about this failed: the vision endpoint "
                                              "returned an empty response."));
        }
        else
        {
            QString answer, evidence, uncertainty;
            parseStructured(content, &answer, &evidence, &uncertainty);
            emit responseReady(conversationId, answer, evidence, uncertainty,
                               content);
        }
    }

    reply->deleteLater();
    if (m_replies.isEmpty())
    {
        m_busy = false;
        emit busyChanged(false);
    }
}

void VisionService::cancelAll()
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