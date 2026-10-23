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
#include <QPainter>
#include <QTimer>
#include <QUrl>
#include <QSslError>
#include <QDateTime>
#include <QCryptographicHash>
#include <limits>

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
    /* TLS opt-in for trusted local dev endpoints with self-signed
     * certificates. Default false: certificate validation is enforced. */
    m_insecure = pbConfigBool(obj, "paintbrush-vlm-insecure");

    if (m_baseUrl.isEmpty())
    {
        /* No dedicated VLM endpoint: fall back to the assistant gateway.
         * It is configured out of the box (LiteLLM Edge) and the default
         * model accepts image parts, so "Ask about this" works immediately. */
        m_baseUrl = pbConfigString(obj, "paintbrush-assistant-base-url").trimmed();
        m_model = pbConfigString(obj, "paintbrush-assistant-model").trimmed();
        if (m_apiKey.isEmpty())
            m_apiKey = pbApiKey(obj, "PAINTBRUSH_ASSISTANT_API_KEY",
                                "paintbrush-assistant-api-key");
    }

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

/* The AI sees an opaque image: a transparent canvas/crop would be read as
 * a blank (or black) picture by the vision sidecar. Composite onto white. */
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

/* Static system prompt: what the VLM is, what it receives and how it must
 * answer. It stays the second text part, after the user's question. */
static const char *systemPromptText()
{
    return
        "You are a helpful creative assistant inside a paint program. "
        "The user selected an object in a picture or video frame and asks a question about it.\n"
        "You receive: (1) a crop of the selected object, (2) the full contextual frame.\n"
        "Answer concisely and honestly. Clearly separate:\n"
        "- what you can see (visible evidence),\n"
        "- what you are inferring (likely),\n"
        "- when you are not sure (uncertainty).\n"
        "Look for specific identifying details and report them when visible: logos and printed "
        "text (read letter-by-letter, e.g. an 'M' emblem, brand names), character or clothing "
        "attributes (hat, color, pattern, accessories), colors, shapes, and materials.\n"
        "For brand/product questions, only name a specific model/SKU when the visible "
        "evidence supports it; otherwise describe the category. Do not invent details.\n"
        "Reply in JSON with exactly three fields: {\"answer\": \"...\", \"evidence\": \"...\", "
        "\"uncertainty\": \"...\"} where uncertainty is a short phrase like \"low\", "
        "\"medium\", \"high\" followed by a brief reason. If the user asked a question that "
        "does not fit this shape, put your full reply in answer and leave evidence empty.";
}

/* One user turn: the question VERBATIM first, then the static system
 * prompt, then the crop and context images. */
QJsonArray VisionService::buildUserContent(const QImage &crop, const QImage &context,
                                           const QString &question)
{
    QJsonArray content;

    QJsonObject questionPart;
    questionPart[QStringLiteral("type")] = QStringLiteral("text");
    questionPart[QStringLiteral("text")] = question; /* verbatim, first part */
    content.append(questionPart);

    QJsonObject promptPart;
    promptPart[QStringLiteral("type")] = QStringLiteral("text");
    promptPart[QStringLiteral("text")] = QString::fromUtf8(systemPromptText());
    content.append(promptPart);

    if (!crop.isNull())
    {
        QJsonObject imgPart;
        imgPart[QStringLiteral("type")] = QStringLiteral("image_url");
        QJsonObject urlObj;
        urlObj[QStringLiteral("url")] =
            QString::fromLatin1(pngDataUrl(capped(opaque(crop), 768)));
        imgPart[QStringLiteral("image_url")] = urlObj;
        content.append(imgPart);
    }
    if (!context.isNull())
    {
        QJsonObject imgPart;
        imgPart[QStringLiteral("type")] = QStringLiteral("image_url");
        QJsonObject urlObj;
        urlObj[QStringLiteral("url")] =
            QString::fromLatin1(pngDataUrl(capped(opaque(context), 1024)));
        imgPart[QStringLiteral("image_url")] = urlObj;
        content.append(imgPart);
    }

    return content;
}

/* Append one message to a session, dropping the oldest turn pair when the
 * bounded history is full (a lone survivor is dropped on its own). */
void VisionService::appendMessage(VisionSession &session, const QString &role,
                                  const QJsonArray &content)
{
    VisionMessage message;
    message.role = role;
    message.content = content;
    session.messages.append(message);

    while (session.messages.size() > VisionSession::maxMessages)
    {
        session.messages.removeFirst();
        if (session.messages.size() > VisionSession::maxMessages)
            session.messages.removeFirst();
    }
}

/* Stable session key for callers that pass no conversation id: content hash
 * of crop + context + question. */
QString VisionService::makeSessionId(const QImage &crop, const QImage &context,
                                     const QString &question)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayLiteral("pb-vision-session"));
    hash.addData(question.toUtf8());
    if (!crop.isNull())
    {
        hash.addData(QByteArrayLiteral("crop"));
        hash.addData(QByteArray::number(crop.width()));
        hash.addData(QByteArray::number(crop.height()));
        hash.addData(QByteArray(
            reinterpret_cast<const char *>(crop.constBits()),
            qMin<qint64>(crop.sizeInBytes(), 1 << 16)));
    }
    if (!context.isNull())
    {
        hash.addData(QByteArrayLiteral("context"));
        hash.addData(QByteArray::number(context.width()));
        hash.addData(QByteArray::number(context.height()));
        hash.addData(QByteArray(
            reinterpret_cast<const char *>(context.constBits()),
            qMin<qint64>(context.sizeInBytes(), 1 << 16)));
    }
    return QString::fromLatin1(hash.result().toHex());
}

/* Assemble the payload for one request. When sessionId names an existing
 * session, the messages array carries the bounded history (including the
 * turn already appended by ask()); otherwise a single-shot user message is
 * built. */
QByteArray VisionService::buildPayload(const QImage &crop, const QImage &context,
                                       const QString &question,
                                       const QString &sessionId) const
{
    QJsonArray messages;

    if (!sessionId.isEmpty())
    {
        const auto it = m_sessions.constFind(sessionId);
        if (it != m_sessions.constEnd())
        {
            for (const VisionMessage &message : it->messages)
            {
                QJsonObject msg;
                msg[QStringLiteral("role")] = message.role;
                msg[QStringLiteral("content")] = message.content;
                messages.append(msg);
            }
        }
    }

    if (messages.isEmpty())
    {
        QJsonObject userMsg;
        userMsg[QStringLiteral("role")] = QStringLiteral("user");
        userMsg[QStringLiteral("content")] = buildUserContent(crop, context, question);
        messages.append(userMsg);
    }

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

    /* conversationId is the session key: a follow-up reusing it appends a
     * new turn to the same conversation instead of a fresh single shot. */
    QString sessionId = conversationId;
    if (sessionId.isEmpty())
        sessionId = makeSessionId(objectCrop, contextFrame, question);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    VisionSession &session = m_sessions[sessionId];
    if (session.id.isEmpty())
        session.id = sessionId;
    session.lastUsedMs = now;
    appendMessage(session, QStringLiteral("user"),
                  buildUserContent(objectCrop, contextFrame, question));

    /* Bound the session store: evict the least recently used session. */
    while (m_sessions.size() > maxSessions)
    {
        QString victim;
        qint64 oldest = std::numeric_limits<qint64>::max();
        for (auto it = m_sessions.constBegin(); it != m_sessions.constEnd(); ++it)
        {
            if (it.value().lastUsedMs < oldest)
            {
                oldest = it.value().lastUsedMs;
                victim = it.key();
            }
        }
        if (victim.isEmpty())
            break;
        m_sessions.remove(victim);
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
                                                                question,
                                                                sessionId));
    reply->setProperty("pbConversationId", sessionId);
    reply->setProperty("pbStartedAt",
                       QDateTime::currentMSecsSinceEpoch());
    m_replies.append(reply);

    /* TLS is validated by default. paintbrush-vlm-insecure explicitly opts
     * into ignoring certificate errors for trusted local dev endpoints. */
    connect(reply, &QNetworkReply::sslErrors, this,
            [this, reply](const QList<QSslError> &errors) {
                onSslErrors(reply, errors);
            });

    if (!m_busy)
    {
        m_busy = true;
        emit busyChanged(true);
    }
    connect(reply, &QNetworkReply::finished, this, &VisionService::onReplyFinished);
}

void VisionService::onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors)
{
    if (m_insecure)
    {
        /* Explicit opt-in (paintbrush-vlm-insecure): allow trusted local
         * development endpoints with self-signed certificates. */
        reply->ignoreSslErrors();
        return;
    }

    /* Default: a certificate validation failure is a real error. Record why
     * and abort; onReplyFinished() reports it truthfully. */
    QStringList details;
    details.reserve(errors.size());
    for (const QSslError &error : errors)
        details << error.errorString();
    reply->setProperty("pbSslFailure", details.join(QStringLiteral("; ")));
    reply->abort();
}

void VisionService::onReplyFinished()
{
    auto *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply)
        return;
    m_replies.removeAll(reply);
    const QString conversationId = reply->property("pbConversationId").toString();
    const qint64 startedAt = reply->property("pbStartedAt").toLongLong();
    const qint64 elapsed = startedAt > 0
        ? QDateTime::currentMSecsSinceEpoch() - startedAt : 0;

    const QNetworkReply::NetworkError error = reply->error();
    const QString sslDetail = reply->property("pbSslFailure").toString();

    /* After an SSL abort or a refused connection the reply device is
     * closed; only read the body when it can still deliver data. */
    QByteArray data;
    if ((error == QNetworkReply::NoError || sslDetail.isEmpty())
        && reply->isOpen())
        data = reply->readAll();

    if (error != QNetworkReply::NoError)
    {
        QString message;
        if (!sslDetail.isEmpty())
        {
            message = QStringLiteral(
                          "Ask about this failed: TLS certificate validation failed (%1). "
                          "Enable paintbrush-vlm-insecure only for trusted local development "
                          "endpoints.")
                          .arg(sslDetail);
        }
        else
        {
            QString detail = QString::fromUtf8(data).left(300);
            if (detail.isEmpty())
                detail = reply->errorString();
            message = QStringLiteral("Ask about this failed: vision endpoint did not respond (%1). %2")
                          .arg(int(error)).arg(detail);
        }
        pbLogRequest(m_obj, "vision", conversationId, elapsed, false, message);
        emit errorOccurred(conversationId, message);
    }
    else
    {
        const QString content = extractContent(data);
        if (content.isEmpty())
        {
            pbLogRequest(m_obj, "vision", conversationId, elapsed, false,
                         QStringLiteral("empty response"));
            emit errorOccurred(conversationId,
                               QStringLiteral("Ask about this failed: the vision endpoint "
                                              "returned an empty response."));
        }
        else
        {
            pbLogRequest(m_obj, "vision", conversationId, elapsed, true);
            QString answer, evidence, uncertainty;
            parseStructured(content, &answer, &evidence, &uncertainty);

            /* Store the assistant turn so a follow-up on the same session
             * carries the real conversation (user + assistant). */
            if (!conversationId.isEmpty())
            {
                auto it = m_sessions.find(conversationId);
                if (it != m_sessions.end())
                {
                    QJsonArray parts;
                    QJsonObject textPart;
                    textPart[QStringLiteral("type")] = QStringLiteral("text");
                    textPart[QStringLiteral("text")] = content;
                    parts.append(textPart);
                    appendMessage(it.value(), QStringLiteral("assistant"), parts);
                    it.value().lastUsedMs = QDateTime::currentMSecsSinceEpoch();
                }
            }
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

void VisionService::clearSession(const QString &sessionId)
{
    if (!sessionId.isEmpty())
        m_sessions.remove(sessionId);
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