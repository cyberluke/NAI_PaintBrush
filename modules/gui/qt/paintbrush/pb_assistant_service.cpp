/*****************************************************************************
 * pb_assistant_service.cpp : NAI Paintbrush — AI assistant chat provider
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *****************************************************************************/

#include "pb_assistant_service.hpp"
#include "pb_common.hpp"

#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonArray>
#include <QBuffer>
#include <QUrl>
#include <QSslError>
#include <QDateTime>

namespace paintbrush {

namespace {

/* Cap the long edge so payloads stay small; keep the aspect ratio. */
QImage cappedImage(const QImage &image, int maxEdge)
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

QByteArray imageDataUrl(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return "data:image/png;base64," + bytes.toBase64();
}

/* Rough token estimate for a text string (≈4 chars/token). */
int estimateTextTokens(const QString &text)
{
    return qMax(1, text.size() / 4);
}

/* Rough token estimate for an image: fixed overhead + tiles of 512 px. */
int estimateImageTokens(const QImage &image)
{
    if (image.isNull())
        return 0;
    const int tilesX = qMax(1, (image.width() + 511) / 512);
    const int tilesY = qMax(1, (image.height() + 511) / 512);
    return 85 + 170 * tilesX * tilesY;
}

} // namespace

AssistantService::AssistantService(QObject *parent)
    : QObject(parent)
{
}

AssistantService::~AssistantService()
{
    cancelAll();
}

void AssistantService::configure(vlc_object_t *obj)
{
    m_obj = obj;

    /* The persisted gateway configuration. The defaults wire the in-app
     * assistant to the LiteLLM Edge gateway out of the box. */
    m_baseUrl = pbConfigString(obj, "paintbrush-assistant-base-url").trimmed();
    m_model = pbConfigString(obj, "paintbrush-assistant-model").trimmed();
    m_timeoutMs = pbConfigInt(obj, "paintbrush-assistant-timeout") * 1000;
    if (m_timeoutMs <= 0)
        m_timeoutMs = 120000;
    m_apiKey = pbApiKey(obj, "PAINTBRUSH_ASSISTANT_API_KEY",
                        "paintbrush-assistant-api-key");
    m_reasoning = pbConfigBool(obj, "paintbrush-assistant-reasoning");

    m_contextLimit = pbConfigInt(obj, "paintbrush-assistant-context-limit");
    m_inputLimit = pbConfigInt(obj, "paintbrush-assistant-input-limit");
    m_outputLimit = pbConfigInt(obj, "paintbrush-assistant-output-limit");
    if (m_contextLimit <= 0)
        m_contextLimit = 524288;
    if (m_inputLimit <= 0)
        m_inputLimit = 442368;
    if (m_outputLimit <= 0)
        m_outputLimit = 65536;

    m_compactionAuto = pbConfigBool(obj, "paintbrush-assistant-compaction-auto");
    m_compactionThreshold = pbConfigInt(obj, "paintbrush-assistant-compaction-threshold");
    if (m_compactionThreshold <= 0 || m_compactionThreshold > 100)
        m_compactionThreshold = 90;
    m_compactionPrune = pbConfigBool(obj, "paintbrush-assistant-compaction-prune");
    m_compactionTailTurns = pbConfigInt(obj, "paintbrush-assistant-compaction-tail-turns");
    if (m_compactionTailTurns < 1)
        m_compactionTailTurns = 4;
    m_compactionPreserveRecent =
        pbConfigInt(obj, "paintbrush-assistant-compaction-preserve-recent");
    if (m_compactionPreserveRecent < 0)
        m_compactionPreserveRecent = 16536;
    m_compactionReserved = pbConfigInt(obj, "paintbrush-assistant-compaction-reserved");
    if (m_compactionReserved < 0)
        m_compactionReserved = 20000;

    if (m_baseUrl.isEmpty())
    {
        m_unavailableReason =
            QStringLiteral("The AI assistant is unavailable: no chat endpoint is configured.");
        emit availabilityChanged(false, m_unavailableReason);
        return;
    }
    emit availabilityChanged(true, {});
}

int AssistantService::estimateTokens(const QJsonObject &message) const
{
    int tokens = 0;

    const QJsonValue content = message[QStringLiteral("content")];
    if (content.isString())
    {
        tokens += estimateTextTokens(content.toString());
    }
    else if (content.isArray())
    {
        /* multimodal content parts: text + image_url */
        const QJsonArray parts = content.toArray();
        for (const QJsonValue &part : parts)
        {
            const QJsonObject partObj = part.toObject();
            if (partObj[QStringLiteral("type")].toString() == QStringLiteral("text"))
                tokens += estimateTextTokens(partObj[QStringLiteral("text")].toString());
            else if (partObj[QStringLiteral("type")].toString() == QStringLiteral("image_url"))
                tokens += 512; /* conservative image budget (data-URL form) */
        }
    }
    return qMax(1, tokens);
}

void AssistantService::compactConversation()
{
    if (!m_compactionAuto || m_history.isEmpty())
        return;

    const int inputBudget = qMax(1, m_inputLimit - m_compactionReserved);
    const int thresholdTokens = inputBudget * m_compactionThreshold / 100;

    int current = 0;
    for (const QJsonObject &message : m_history)
        current += estimateTokens(message);

    /* keep at least the tail turns as a hard floor */
    const int minMessages = m_compactionTailTurns * 2;

    while (m_history.size() > minMessages && current > thresholdTokens)
    {
        const QJsonObject oldest = m_history.first();
        const int oldestTokens = estimateTokens(oldest);

        /* stop pruning once dropping the oldest message would push the
         * remaining recent context below the preserved-token floor */
        if (current - oldestTokens < m_compactionPreserveRecent)
            break;

        m_history.removeFirst();
        current -= oldestTokens;
    }
}

QByteArray AssistantService::buildPayload(const QString &text,
                                          const QImage &image) const
{
    QJsonObject userMsg;
    userMsg[QStringLiteral("role")] = QStringLiteral("user");

    if (image.isNull())
    {
        userMsg[QStringLiteral("content")] = text;
    }
    else
    {
        QJsonArray parts;
        QJsonObject textPart;
        textPart[QStringLiteral("type")] = QStringLiteral("text");
        textPart[QStringLiteral("text")] = text;
        parts.append(textPart);

        QJsonObject imgPart;
        imgPart[QStringLiteral("type")] = QStringLiteral("image_url");
        QJsonObject urlObj;
        urlObj[QStringLiteral("url")] =
            QString::fromLatin1(imageDataUrl(cappedImage(image, 1024)));
        imgPart[QStringLiteral("image_url")] = urlObj;
        parts.append(imgPart);

        userMsg[QStringLiteral("content")] = parts;
    }

    /* compact the previous history before appending the new turn */
    QList<QJsonObject> messages = m_history;
    messages.append(userMsg);

    QJsonArray messagesArray;
    for (const QJsonObject &message : messages)
        messagesArray.append(message);

    QJsonObject payload;
    payload[QStringLiteral("model")] =
        m_model.isEmpty() ? QStringLiteral("default") : m_model;
    payload[QStringLiteral("messages")] = messagesArray;
    payload[QStringLiteral("max_tokens")] = m_outputLimit;
    payload[QStringLiteral("temperature")] = 0.4;
    if (m_reasoning)
        payload[QStringLiteral("reasoning")] = QJsonObject{
            {QStringLiteral("effort"), QStringLiteral("high")}
        };

    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

QString AssistantService::extractContent(const QByteArray &json)
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
    {
        const QJsonValue content = message.toObject()[QStringLiteral("content")];
        if (content.isString())
            return content.toString();
        if (content.isArray())
        {
            /* reasoning models may return content as an array of parts */
            QString result;
            const QJsonArray parts = content.toArray();
            for (const QJsonValue &part : parts)
            {
                const QJsonObject partObj = part.toObject();
                if (partObj[QStringLiteral("type")].toString() == QStringLiteral("text"))
                    result += partObj[QStringLiteral("text")].toString();
                else if (partObj.contains(QStringLiteral("text")))
                    result += partObj[QStringLiteral("text")].toString();
            }
            return result;
        }
    }
    return choice[QStringLiteral("text")].toString();
}

void AssistantService::chat(const QString &text, const QImage &image,
                            const QString &conversationId)
{
    if (!isConfigured())
    {
        emit errorOccurred(conversationId, m_unavailableReason);
        return;
    }

    compactConversation();

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

    QNetworkReply *reply = m_network.post(request, buildPayload(text, image));
    reply->setProperty("pbConversationId", conversationId);
    reply->setProperty("pbRequestText", text);
    reply->setProperty("pbStartedAt", QDateTime::currentMSecsSinceEpoch());
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
    connect(reply, &QNetworkReply::finished, this, &AssistantService::onReplyFinished);
}

void AssistantService::onReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    m_replies.removeAll(reply);
    const QString conversationId = reply->property("pbConversationId").toString();
    const qint64 startedAt = reply->property("pbStartedAt").toLongLong();
    const qint64 elapsed = startedAt > 0
        ? QDateTime::currentMSecsSinceEpoch() - startedAt : 0;

    const QByteArray data = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();

    if (error != QNetworkReply::NoError)
    {
        QString detail = QString::fromUtf8(data).left(300);
        if (detail.isEmpty())
            detail = reply->errorString();
        pbLogRequest(m_obj, "assistant", conversationId, elapsed, false, detail);
        emit errorOccurred(conversationId,
                           QStringLiteral("The AI assistant failed: the chat endpoint did "
                                          "not respond (%1). %2")
                               .arg(int(error)).arg(detail));
    }
    else
    {
        const QString content = extractContent(data);
        if (content.isEmpty())
        {
            pbLogRequest(m_obj, "assistant", conversationId, elapsed, false,
                         QStringLiteral("empty response"));
            emit errorOccurred(conversationId,
                               QStringLiteral("The AI assistant failed: the chat endpoint "
                                              "returned an empty response."));
        }
        else
        {
            pbLogRequest(m_obj, "assistant", conversationId, elapsed, true);
            /* remember the successful exchange for follow-up turns */
            QJsonObject userMsg;
            userMsg[QStringLiteral("role")] = QStringLiteral("user");
            userMsg[QStringLiteral("content")] =
                reply->property("pbRequestText").toString();
            if (!userMsg[QStringLiteral("content")].toString().isEmpty())
                m_history.append(userMsg);

            QJsonObject assistantMsg;
            assistantMsg[QStringLiteral("role")] = QStringLiteral("assistant");
            assistantMsg[QStringLiteral("content")] = content;
            m_history.append(assistantMsg);

            emit responseReady(conversationId, content);
        }
    }

    reply->deleteLater();
    if (m_replies.isEmpty())
    {
        m_busy = false;
        emit busyChanged(false);
    }
}

void AssistantService::resetConversation()
{
    m_history.clear();
}

void AssistantService::cancelAll()
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