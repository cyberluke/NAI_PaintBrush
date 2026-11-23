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
#include <algorithm>

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
    /* A7: TLS validation stays ON unless the operator explicitly opts into
     * self-signed local endpoints (per-service or shared dev flag). */
    m_insecureTls = pbConfigBool(obj, "paintbrush-assistant-insecure")
                 || pbConfigBool(obj, "paintbrush-dev-insecure");

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

QJsonObject AssistantService::buildUserMessage(const QString &text,
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
    return userMsg;
}

QByteArray AssistantService::buildPayload(const QString &text,
                                          const QImage &image) const
{
    /* compact the previous history before appending the new turn */
    QList<QJsonObject> messages = m_history;
    messages.append(buildUserMessage(text, image));

    QJsonArray messagesArray;
    for (const QJsonObject &message : messages)
        messagesArray.append(message);

    QJsonObject payload;
    payload[QStringLiteral("model")] =
        m_model.isEmpty() ? QStringLiteral("default") : m_model;
    payload[QStringLiteral("messages")] = messagesArray;
    payload[QStringLiteral("max_tokens")] = qMin(m_outputLimit, 8192);
    payload[QStringLiteral("temperature")] = 0.4;
    /* Do NOT send a `reasoning` object: strict OpenAI-compatible servers
     * (Azure AI Foundry, DeepSeek, vLLM) reject unknown request fields with a
     * 400, which made the companion look silent. Reasoning is requested by the
     * model profile itself and parsed from `reasoning_content` deltas. */

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
    {
        request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());
        /* Azure AI Foundry's OpenAI-compatible endpoint accepts api-key; a
         * key-only endpoint otherwise answers 401 and the companion looks
         * dead. Sending both is harmless on plain OpenAI/LiteLLM. */
        request.setRawHeader("api-key", m_apiKey.toUtf8());
    }

    QNetworkReply *reply = m_network.post(request, buildPayload(text, image));
    reply->setProperty("pbConversationId", conversationId);
    reply->setProperty("pbRequestText", text);
    reply->setProperty("pbStartedAt", QDateTime::currentMSecsSinceEpoch());
    m_replies.append(reply);

    /* A7: no more unconditional ignoreSslErrors(). Certificate validation
     * is on by default; only the explicit paintbrush-assistant-insecure /
     * paintbrush-dev-insecure opt-in bypasses it for self-signed local
     * endpoints. */
    handleSslErrors(reply);

    if (!m_busy)
    {
        m_busy = true;
        emit busyChanged(true);
    }
    connect(reply, &QNetworkReply::finished, this, &AssistantService::onReplyFinished);
}

/* ===================================================================== */
/* Streaming OpenAI-compatible chat                                      */
/* ===================================================================== */

void AssistantService::setToolDefinitions(const QByteArray &jsonArray)
{
    if (jsonArray.trimmed().isEmpty())
    {
        m_toolDefinitions.clear();
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(jsonArray);
    if (!doc.isArray())
        return;
    m_toolDefinitions = QJsonDocument(doc.array()).toJson(QJsonDocument::Compact);
}

QByteArray AssistantService::buildStreamPayload(const QJsonArray &messages) const
{
    QJsonObject payload;
    payload[QStringLiteral("model")] =
        m_model.isEmpty() ? QStringLiteral("default") : m_model;
    payload[QStringLiteral("messages")] = messages;
    payload[QStringLiteral("stream")] = true;
    payload[QStringLiteral("max_tokens")] = qMin(m_outputLimit, 8192);
    payload[QStringLiteral("temperature")] = 0.4;

    if (!m_stream.disableTools && !m_toolDefinitions.isEmpty())
    {
        const QJsonDocument tools = QJsonDocument::fromJson(m_toolDefinitions);
        if (tools.isArray() && !tools.array().isEmpty())
        {
            payload[QStringLiteral("tools")] = tools.array();
            payload[QStringLiteral("tool_choice")] = QStringLiteral("auto");
        }
    }
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

void AssistantService::chatStreaming(const QString &text, const QImage &image,
                                     const QString &conversationId,
                                     const QString &systemPrompt)
{
    if (!isConfigured())
    {
        emit streamError(conversationId, m_unavailableReason);
        return;
    }
    if (m_stream.reply)
    {
        emit streamError(conversationId,
                         QStringLiteral("The companion is still answering. "
                                        "Stop it before sending another message."));
        return;
    }

    resetStreamState();
    m_stream.conversationId = conversationId;
    m_stream.systemPrompt = systemPrompt.trimmed();

    QJsonArray messages;
    bool hasSystem = false;
    for (const QJsonObject &m : m_streamHistory)
    {
        if (m.value(QStringLiteral("role")).toString() == QStringLiteral("system"))
            hasSystem = true;
        messages.append(m);
    }
    if (!hasSystem && !m_stream.systemPrompt.isEmpty())
    {
        QJsonObject sys;
        sys[QStringLiteral("role")] = QStringLiteral("system");
        sys[QStringLiteral("content")] = m_stream.systemPrompt;
        messages.prepend(sys);
    }
    messages.append(buildUserMessage(text, image));
    m_stream.messages = messages;

    startStreamRequest(conversationId, 0);
}

void AssistantService::startStreamRequest(const QString &conversationId, int round)
{
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
    request.setRawHeader("Accept", "text/event-stream");
    request.setTransferTimeout(m_timeoutMs);
    if (!m_apiKey.isEmpty())
    {
        request.setRawHeader("Authorization", "Bearer " + m_apiKey.toUtf8());
        request.setRawHeader("api-key", m_apiKey.toUtf8());
    }

    QNetworkReply *reply = m_network.post(request,
                                          buildStreamPayload(m_stream.messages));
    reply->setProperty("pbConversationId", conversationId);
    reply->setProperty("pbRound", round);

    m_stream.reply = reply;
    m_stream.round = round;
    m_stream.startedAt = QDateTime::currentMSecsSinceEpoch();
    m_stream.buffer.clear();
    m_stream.raw.clear();
    m_stream.content.clear();
    m_stream.reasoning.clear();
    m_stream.finishReason.clear();
    m_stream.partialCalls.clear();
    m_stream.toolCalls = QJsonArray();
    m_stream.assistantToolMessage = QJsonObject();
    m_stream.sse = false;
    m_stream.sawDone = false;
    m_stream.cancelled = false;

    handleSslErrors(reply);
    connect(reply, &QNetworkReply::readyRead,
            this, &AssistantService::onStreamReadyRead);
    connect(reply, &QNetworkReply::finished,
            this, &AssistantService::onStreamFinished);

    if (!m_busy)
    {
        m_busy = true;
        emit busyChanged(true);
    }
    if (round == 0)
        emit streamStarted(conversationId);
    else
        emit streamRoundStarted(conversationId);
}

void AssistantService::onStreamReadyRead()
{
    if (!m_stream.reply)
        return;
    const QByteArray chunk = m_stream.reply->readAll();
    if (chunk.isEmpty())
        return;
    m_stream.raw.append(chunk);
    m_stream.buffer.append(chunk);

    for (;;)
    {
        const int nl = m_stream.buffer.indexOf('\n');
        if (nl < 0)
            break;
        QByteArray line = m_stream.buffer.left(nl);
        m_stream.buffer.remove(0, nl + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        parseStreamLine(line);
    }
}

void AssistantService::parseStreamLine(const QByteArray &line)
{
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return;

    if (trimmed.startsWith("data:"))
    {
        m_stream.sse = true;
        const QByteArray payload = trimmed.mid(5).trimmed();
        if (payload == "[DONE]")
        {
            m_stream.sawDone = true;
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(payload);
        if (doc.isObject())
            processStreamObject(doc.object());
    }
    /* A non-SSE server returns one JSON document; it is parsed from
     * m_stream.raw in onStreamFinished(). Do not mark it as SSE here. */
}

void AssistantService::processStreamObject(const QJsonObject &obj)
{
    if (obj.contains(QStringLiteral("error")))
    {
        const QJsonObject err = obj.value(QStringLiteral("error")).toObject();
        failStream(err.value(QStringLiteral("message")).toString(
                       QStringLiteral("The AI endpoint returned an error.")));
        return;
    }

    const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
        return;
    const QJsonObject choice = choices.at(0).toObject();
    const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();

    QString reasoning = delta.value(QStringLiteral("reasoning_content")).toString();
    if (reasoning.isEmpty())
        reasoning = delta.value(QStringLiteral("reasoning")).toString();
    if (!reasoning.isEmpty())
    {
        m_stream.reasoning += reasoning;
        emit reasoningDelta(m_stream.conversationId, reasoning);
    }

    const QJsonValue contentValue = delta.value(QStringLiteral("content"));
    QString content;
    if (contentValue.isString())
    {
        content = contentValue.toString();
    }
    else if (contentValue.isArray())
    {
        const QJsonArray parts = contentValue.toArray();
        for (const QJsonValue &part : parts)
        {
            const QJsonObject partObj = part.toObject();
            if (partObj.value(QStringLiteral("type")).toString()
                    == QStringLiteral("text"))
                content += partObj.value(QStringLiteral("text")).toString();
        }
    }
    if (!content.isEmpty())
    {
        m_stream.content += content;
        emit contentDelta(m_stream.conversationId, content);
    }

    const QJsonArray toolCalls = delta.value(QStringLiteral("tool_calls")).toArray();
    for (const QJsonValue &value : toolCalls)
    {
        const QJsonObject tc = value.toObject();
        const int index = tc.value(QStringLiteral("index")).toInt();
        QJsonObject call = m_stream.partialCalls.value(index);
        const QJsonObject oldFunction = call.value(QStringLiteral("function")).toObject();
        const bool hadName = !oldFunction.value(QStringLiteral("name")).toString().isEmpty();

        call[QStringLiteral("index")] = index;
        const QString id = tc.value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            call[QStringLiteral("id")] = id;
        const QString type = tc.value(QStringLiteral("type")).toString();
        if (!type.isEmpty())
            call[QStringLiteral("type")] = type;

        const QJsonObject fn = tc.value(QStringLiteral("function")).toObject();
        QJsonObject function = oldFunction;
        if (fn.contains(QStringLiteral("name")))
            function[QStringLiteral("name")] = fn.value(QStringLiteral("name")).toString();
        if (fn.contains(QStringLiteral("arguments")))
        {
            QString args = function.value(QStringLiteral("arguments")).toString();
            args += fn.value(QStringLiteral("arguments")).toString();
            function[QStringLiteral("arguments")] = args;
        }
        call[QStringLiteral("function")] = function;
        m_stream.partialCalls[index] = call;

        const QString name = function.value(QStringLiteral("name")).toString();
        if (!hadName && !name.isEmpty())
        {
            QString emitId = call.value(QStringLiteral("id")).toString();
            if (emitId.isEmpty())
                emitId = QStringLiteral("call_%1").arg(index);
            emit toolCallStarted(m_stream.conversationId, emitId, name);
        }
    }

    const QString finish = choice.value(QStringLiteral("finish_reason")).toString();
    if (!finish.isEmpty())
        m_stream.finishReason = finish;
}

void AssistantService::onStreamFinished()
{
    QNetworkReply *reply = m_stream.reply;
    if (!reply)
        return;

    const QByteArray rest = reply->readAll();
    if (!rest.isEmpty())
    {
        m_stream.raw.append(rest);
        m_stream.buffer.append(rest);
    }
    /* flush a final line that arrived without a trailing newline */
    if (!m_stream.buffer.isEmpty())
    {
        const QByteArray line = m_stream.buffer;
        m_stream.buffer.clear();
        parseStreamLine(line);
    }

    const QString conversationId = m_stream.conversationId;
    const bool cancelled = m_stream.cancelled;
    const QNetworkReply::NetworkError netError = reply->error();
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString errorDetail = reply->property("pbSslDetail").toString();

    m_stream.reply = nullptr;
    reply->deleteLater();

    if (cancelled)
    {
        resetStreamState();
        if (m_replies.isEmpty())
        {
            m_busy = false;
            emit busyChanged(false);
        }
        return;
    }

    if (netError != QNetworkReply::NoError || httpStatus >= 400)
    {
        if (errorDetail.isEmpty() && netError != QNetworkReply::NoError)
            m_stream.raw.append(reply->errorString().toUtf8());

        /* A non-SSE server may still have returned a complete JSON answer. */
        if (!m_stream.sse && m_stream.content.isEmpty())
        {
            const QJsonDocument doc = QJsonDocument::fromJson(m_stream.raw);
            if (doc.isObject()
                && doc.object().value(QStringLiteral("choices")).isArray())
            {
                /* fall through to the normal parse below */
            }
            else
            {
                QString detail;
                const QJsonObject root = doc.object();
                const QJsonObject errorObj =
                    root.value(QStringLiteral("error")).toObject();
                detail = errorObj.value(QStringLiteral("message")).toString();
                if (detail.isEmpty())
                    detail = root.value(QStringLiteral("message")).toString();
                if (detail.isEmpty())
                    detail = QString::fromUtf8(m_stream.raw).left(320);
                if (detail.isEmpty())
                    detail = errorDetail;
                if (detail.isEmpty())
                    detail = reply->errorString();

                QString message;
                if (httpStatus == 401 || httpStatus == 403)
                    message = QStringLiteral(
                        "The AI endpoint rejected the API key (HTTP %1). "
                        "Check Settings → AI.").arg(httpStatus);
                else if (httpStatus == 404)
                    message = QStringLiteral(
                        "The AI endpoint URL was not found (HTTP 404). "
                        "Check Settings → AI.");
                else if (httpStatus >= 400)
                    message = QStringLiteral("The AI endpoint returned HTTP %1: %2")
                                  .arg(httpStatus).arg(detail);
                else
                    message = QStringLiteral("The AI companion failed: %1").arg(detail);

                /* Some OpenAI-compatible models do not implement tools and
                 * answer 400. Retry the exact same turn once without tool
                 * definitions so plain chat still works everywhere. */
                if (httpStatus == 400 && !m_stream.disableTools
                    && !m_toolDefinitions.isEmpty()
                    && detail.contains(QStringLiteral("tool"), Qt::CaseInsensitive))
                {
                    m_stream.disableTools = true;
                    m_stream.sse = false;
                    m_stream.sawDone = false;
                    m_stream.raw.clear();
                    m_stream.buffer.clear();
                    m_stream.content.clear();
                    m_stream.reasoning.clear();
                    m_stream.finishReason.clear();
                    m_stream.partialCalls.clear();
                    m_stream.toolCalls = QJsonArray();
                    m_stream.assistantToolMessage = QJsonObject();
                    startStreamRequest(conversationId, m_stream.round);
                    return;
                }

                /* A text-only model rejects the image content part with a
                 * 400. Strip the image from the last user message and retry
                 * so the companion still answers in words. */
                if (httpStatus == 400 && !m_stream.disableImage
                    && (detail.contains(QStringLiteral("image"), Qt::CaseInsensitive)
                        || detail.contains(QStringLiteral("vision"), Qt::CaseInsensitive)))
                {
                    m_stream.disableImage = true;
                    for (int i = m_stream.messages.size() - 1; i >= 0; --i)
                    {
                        QJsonObject msg = m_stream.messages.at(i).toObject();
                        if (msg.value(QStringLiteral("role")).toString()
                                != QStringLiteral("user"))
                            continue;
                        const QJsonValue content =
                            msg.value(QStringLiteral("content"));
                        if (!content.isArray())
                            break;
                        QJsonArray parts;
                        const QJsonArray oldParts = content.toArray();
                        for (const QJsonValue &part : oldParts)
                        {
                            if (part.toObject().value(QStringLiteral("type")).toString()
                                != QStringLiteral("image_url"))
                                parts.append(part);
                        }
                        msg[QStringLiteral("content")] = parts;
                        m_stream.messages[i] = msg;
                        break;
                    }
                    m_stream.sse = false;
                    m_stream.sawDone = false;
                    m_stream.raw.clear();
                    m_stream.buffer.clear();
                    m_stream.content.clear();
                    m_stream.reasoning.clear();
                    m_stream.finishReason.clear();
                    m_stream.partialCalls.clear();
                    m_stream.toolCalls = QJsonArray();
                    m_stream.assistantToolMessage = QJsonObject();
                    startStreamRequest(conversationId, m_stream.round);
                    return;
                }

                failStream(message);
                return;
            }
        }
        else if (m_stream.content.isEmpty() && m_stream.partialCalls.isEmpty())
        {
            failStream(QStringLiteral("The AI companion failed: %1")
                           .arg(errorDetail.isEmpty() ? reply->errorString()
                                                      : errorDetail));
            return;
        }
    }

    /* Non-SSE JSON body: parse a regular chat completion message. */
    if (!m_stream.sse && m_stream.content.isEmpty()
        && m_stream.partialCalls.isEmpty() && !m_stream.raw.isEmpty())
    {
        const QJsonDocument doc = QJsonDocument::fromJson(m_stream.raw);
        if (doc.isObject())
        {
            const QJsonArray choices =
                doc.object().value(QStringLiteral("choices")).toArray();
            if (!choices.isEmpty())
            {
                const QJsonObject message =
                    choices.at(0).toObject()
                        .value(QStringLiteral("message")).toObject();
                const QString reasoning =
                    message.value(QStringLiteral("reasoning_content")).toString()
                    .isEmpty()
                    ? message.value(QStringLiteral("reasoning")).toString()
                    : message.value(QStringLiteral("reasoning_content")).toString();
                if (!reasoning.isEmpty())
                {
                    m_stream.reasoning += reasoning;
                    emit reasoningDelta(conversationId, reasoning);
                }
                const QString content = extractContent(m_stream.raw);
                if (!content.isEmpty())
                {
                    m_stream.content = content;
                    emit contentDelta(conversationId, content);
                }

                const QJsonArray calls =
                    message.value(QStringLiteral("tool_calls")).toArray();
                for (int i = 0; i < calls.size(); ++i)
                {
                    const QJsonObject tc = calls.at(i).toObject();
                    QJsonObject call = tc;
                    call[QStringLiteral("index")] = i;
                    m_stream.partialCalls[i] = call;
                    const QString name =
                        tc.value(QStringLiteral("function")).toObject()
                            .value(QStringLiteral("name")).toString();
                    if (!name.isEmpty())
                        emit toolCallStarted(conversationId,
                                             tc.value(QStringLiteral("id")).toString(),
                                             name);
                }
            }
        }
    }

    if (!m_stream.partialCalls.isEmpty())
    {
        finishStreamRound();
        return;
    }

    if (m_stream.content.isEmpty())
    {
        failStream(QStringLiteral("The AI endpoint returned an empty response."));
        return;
    }

    finishStreamAnswer(m_stream.content);
}

void AssistantService::finishStreamRound()
{
    QList<int> keys = m_stream.partialCalls.keys();
    std::sort(keys.begin(), keys.end());

    QJsonArray toolCalls;
    QVariantList calls;
    for (int index : keys)
    {
        QJsonObject call = m_stream.partialCalls.value(index);
        const QString name = call.value(QStringLiteral("function")).toObject()
                                 .value(QStringLiteral("name")).toString();
        if (name.isEmpty())
            continue;
        QString id = call.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            id = QStringLiteral("call_%1").arg(index);
        call[QStringLiteral("id")] = id;
        call[QStringLiteral("type")] = QStringLiteral("function");
        toolCalls.append(call);

        calls.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("arguments"),
             call.value(QStringLiteral("function")).toObject()
                 .value(QStringLiteral("arguments")).toString()},
        });
    }

    if (calls.isEmpty())
    {
        finishStreamAnswer(m_stream.content);
        return;
    }

    QJsonObject assistant;
    assistant[QStringLiteral("role")] = QStringLiteral("assistant");
    assistant[QStringLiteral("content")] =
        m_stream.content.isEmpty() ? QJsonValue(QJsonValue::Null)
                                   : QJsonValue(m_stream.content);
    assistant[QStringLiteral("tool_calls")] = toolCalls;
    m_stream.messages.append(assistant);
    m_stream.assistantToolMessage = assistant;
    m_stream.toolCalls = toolCalls;

    emit toolCallsReady(m_stream.conversationId, calls);
    /* wait for submitToolResults(); busy stays true */
}

void AssistantService::submitToolResults(const QString &conversationId,
                                         const QVariantList &results)
{
    if (m_stream.reply || m_stream.conversationId != conversationId
        || m_stream.assistantToolMessage.isEmpty())
        return;

    for (const QVariant &value : results)
    {
        const QVariantMap result = value.toMap();
        QJsonObject msg;
        msg[QStringLiteral("role")] = QStringLiteral("tool");
        msg[QStringLiteral("tool_call_id")] =
            result.value(QStringLiteral("tool_call_id")).toString();
        msg[QStringLiteral("content")] =
            result.value(QStringLiteral("content")).toString();
        m_stream.messages.append(msg);
    }

    m_stream.assistantToolMessage = QJsonObject();
    m_stream.partialCalls.clear();
    m_stream.toolCalls = QJsonArray();
    m_stream.content.clear();
    m_stream.reasoning.clear();
    m_stream.finishReason.clear();

    if (m_stream.round >= 6)
    {
        failStream(QStringLiteral("The model requested too many tool rounds."));
        return;
    }
    startStreamRequest(conversationId, m_stream.round + 1);
}

void AssistantService::finishStreamAnswer(const QString &content)
{
    if (!content.isEmpty())
    {
        QJsonObject assistant;
        assistant[QStringLiteral("role")] = QStringLiteral("assistant");
        assistant[QStringLiteral("content")] = content;
        m_stream.messages.append(assistant);
    }

    m_streamHistory.clear();
    for (const QJsonValue &value : m_stream.messages)
        m_streamHistory.append(value.toObject());
    while (m_streamHistory.size() > 40)
    {
        /* remove whole oldest turns so an assistant tool_calls message is
         * never separated from its tool results */
        const int start = (!m_streamHistory.isEmpty()
                           && m_streamHistory.first()
                                  .value(QStringLiteral("role")).toString()
                                  == QStringLiteral("system")) ? 1 : 0;
        int nextUser = -1;
        for (int i = start + 1; i < m_streamHistory.size(); ++i)
        {
            if (m_streamHistory.at(i).value(QStringLiteral("role")).toString()
                == QStringLiteral("user"))
            {
                nextUser = i;
                break;
            }
        }
        if (nextUser <= start)
            break;
        for (int i = 0; i < nextUser - start; ++i)
            m_streamHistory.removeAt(start);
    }

    const QString conversationId = m_stream.conversationId;
    const qint64 elapsed = m_stream.startedAt > 0
        ? QDateTime::currentMSecsSinceEpoch() - m_stream.startedAt : 0;
    pbLogRequest(m_obj, "assistant", conversationId, elapsed, true);
    emit streamFinished(conversationId, content);
    resetStreamState();
    if (m_replies.isEmpty())
    {
        m_busy = false;
        emit busyChanged(false);
    }
}

void AssistantService::failStream(const QString &message)
{
    const QString conversationId = m_stream.conversationId;
    const qint64 elapsed = m_stream.startedAt > 0
        ? QDateTime::currentMSecsSinceEpoch() - m_stream.startedAt : 0;
    pbLogRequest(m_obj, "assistant", conversationId, elapsed, false,
                 message.left(200));
    emit streamError(conversationId, message);
    resetStreamState();
    if (m_replies.isEmpty())
    {
        m_busy = false;
        emit busyChanged(false);
    }
}

void AssistantService::cancelStreaming(const QString &conversationId)
{
    if (!m_stream.reply)
        return;
    if (!conversationId.isEmpty()
        && m_stream.conversationId != conversationId)
        return;

    QNetworkReply *reply = m_stream.reply;
    m_stream.reply = nullptr;
    m_stream.cancelled = true;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    resetStreamState();
    if (m_replies.isEmpty())
    {
        m_busy = false;
        emit busyChanged(false);
    }
}

void AssistantService::resetStreamState()
{
    if (m_stream.reply)
    {
        QNetworkReply *reply = m_stream.reply;
        m_stream.reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    m_stream = StreamState{};
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

        /* A7: TLS validation failures are surfaced truthfully instead of
         * being silently ignored. (Qt5/Qt6-safe: the failure detail is
         * recorded by the sslErrors handler, never read back from a
         * getter that only Qt5 had.) */
        const QString sslDetail = reply->property("pbSslDetail").toString();
        const bool tlsFailure = !sslDetail.isEmpty() ||
                                error == QNetworkReply::SslHandshakeFailedError;

        if (tlsFailure)
        {
            pbLogRequest(m_obj, "assistant", conversationId, elapsed, false,
                         QStringLiteral("TLS: %1").arg(sslDetail));
            emit errorOccurred(conversationId,
                QStringLiteral("The AI assistant failed: TLS certificate validation "
                               "failed. %1 Enable the paintbrush-dev-insecure option "
                               "only for trusted self-signed local endpoints.")
                    .arg(sslDetail.isEmpty() ? detail : sslDetail));
        }
        else
        {
            pbLogRequest(m_obj, "assistant", conversationId, elapsed, false, detail);
            emit errorOccurred(conversationId,
                               QStringLiteral("The AI assistant failed: the chat endpoint did "
                                              "not respond (%1). %2")
                                   .arg(int(error)).arg(detail));
        }
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
    if (m_replies.isEmpty() && !m_stream.reply)
    {
        m_busy = false;
        emit busyChanged(false);
    }
}

void AssistantService::resetConversation()
{
    cancelStreaming(QString());
    m_history.clear();
    m_streamHistory.clear();
}

void AssistantService::cancelAll()
{
    cancelStreaming(QString());
    for (QNetworkReply *reply : m_replies)
        reply->abort();
    m_replies.clear();
    if (m_busy)
    {
        m_busy = false;
        emit busyChanged(false);
    }
}

void AssistantService::handleSslErrors(QNetworkReply *reply) const
{
    const bool insecure = m_insecureTls;
    /* Always record the failure detail so errors stay truthful. */
    connect(reply, &QNetworkReply::sslErrors, reply,
            [reply, insecure](const QList<QSslError> &errors) {
        reply->setProperty("pbSslDetail", sslErrorDetail(errors));
        /* A7: bypass certificate validation ONLY with the explicit
         * paintbrush-assistant-insecure / paintbrush-dev-insecure opt-in.
         * Default: Qt aborts the request and onReplyFinished surfaces a
         * truthful TLS error. */
        if (insecure)
            reply->ignoreSslErrors();
    });
}

} // namespace paintbrush