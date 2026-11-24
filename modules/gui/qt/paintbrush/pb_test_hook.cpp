/*****************************************************************************
 * pb_test_hook.cpp : NAI Paintbrush — test-only loopback control channel
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#include "pb_test_hook.hpp"
#include "pb_controller.hpp"
#include "pb_canvas_item.hpp"
#include "pb_document.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QDebug>

namespace paintbrush {

PaintbrushTestHook::PaintbrushTestHook(PaintbrushController *controller,
                                       QObject *parent)
    : QObject(parent)
    , m_controller(controller)
    , m_server(new QTcpServer(this))
{
    if (!m_controller || !qEnvironmentVariableIsSet("PB_TEST_HOOK"))
        return;

    m_token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_endpointPath = qEnvironmentVariable("PB_TEST_HOOK_FILE");
    if (m_endpointPath.isEmpty())
        m_endpointPath = QDir::temp().filePath(
            QStringLiteral("nai-paintbrush-test-%1.json")
                .arg(QCoreApplication::applicationPid()));

    if (!m_server->listen(QHostAddress::LocalHost, 0))
    {
        qWarning() << "paintbrush test hook: listen failed:"
                   << m_server->errorString();
        return;
    }

    connect(m_server, &QTcpServer::newConnection, this,
            &PaintbrushTestHook::onNewConnection);

    QJsonObject endpoint{
        {QStringLiteral("pid"), double(QCoreApplication::applicationPid())},
        {QStringLiteral("port"), m_server->serverPort()},
        {QStringLiteral("token"), m_token},
        {QStringLiteral("endpoint"), m_endpointPath},
    };
    QDir().mkpath(QFileInfo(m_endpointPath).absolutePath());
    QFile file(m_endpointPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        file.write(QJsonDocument(endpoint).toJson(QJsonDocument::Compact));
        file.write("\n");
        file.close();
    }
    qInfo() << "paintbrush test hook: 127.0.0.1:" << m_server->serverPort()
            << "endpoint" << m_endpointPath;
}

PaintbrushTestHook::~PaintbrushTestHook()
{
    if (m_server && m_server->isListening())
        m_server->close();
    if (!m_endpointPath.isEmpty())
        QFile::remove(m_endpointPath);
}

void PaintbrushTestHook::onNewConnection()
{
    while (QTcpSocket *socket = m_server->nextPendingConnection())
    {
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            processBuffer(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void PaintbrushTestHook::processBuffer(QTcpSocket *socket)
{
    if (isBusy())
        return;
    QByteArray &buffer = m_buffers[socket];
    buffer.append(socket->readAll());
    for (;;)
    {
        const int newline = buffer.indexOf('\n');
        if (newline < 0)
            break;
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (line.isEmpty())
            continue;
        QJsonParseError error;
        const QJsonDocument document =
            QJsonDocument::fromJson(line, &error);
        QJsonObject response;
        if (error.error != QJsonParseError::NoError || !document.isObject())
        {
            response = QJsonObject{
                {QStringLiteral("ok"), false},
                {QStringLiteral("error"),
                 QStringLiteral("invalid JSON: %1").arg(error.errorString())},
            };
            send(socket, response);
            continue;
        }
        setBusy(true);
        dispatch(socket, document.object());
        setBusy(false);
        if (m_buffers.contains(socket) && m_buffers[socket].contains('\n'))
            processBuffer(socket);
        break;
    }
}

void PaintbrushTestHook::send(QTcpSocket *socket, const QJsonObject &response)
{
    if (!socket)
        return;
    QByteArray line = QJsonDocument(response).toJson(QJsonDocument::Compact);
    line.append('\n');
    socket->write(line);
    socket->flush();
}

bool PaintbrushTestHook::waitUntil(
    int timeoutMs, const std::function<bool()> &done) const
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs)
    {
        QEventLoop loop;
        QTimer::singleShot(50, &loop, &QEventLoop::quit);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    return done();
}

QJsonObject PaintbrushTestHook::stateObject() const
{
    QJsonObject state;
    PaintbrushCanvasItem *canvas = m_controller->canvas();
    PaintbrushDocument *document = m_controller->document();
    const QSize canvasSize = document->canvasSize();

    state.insert(QStringLiteral("pid"),
                 double(QCoreApplication::applicationPid()));
    state.insert(QStringLiteral("appReady"), canvas != nullptr);
    state.insert(QStringLiteral("canvasW"), canvasSize.width());
    state.insert(QStringLiteral("canvasH"), canvasSize.height());
    state.insert(QStringLiteral("hasDocument"), m_controller->hasOpenDocument());
    state.insert(QStringLiteral("layerCount"), document->layerCount());
    const CanvasLayer *current = document->currentLayer();
    state.insert(QStringLiteral("currentLayer"),
                 current ? current->name : QString());
    state.insert(QStringLiteral("canUndo"), m_controller->canUndo());
    state.insert(QStringLiteral("canRedo"), m_controller->canRedo());
    state.insert(QStringLiteral("generationBusy"), m_controller->generationBusy());
    state.insert(QStringLiteral("generationStatusText"),
                 m_controller->generationStatusText());
    state.insert(QStringLiteral("lamaBackend"),
                 m_controller->testLamaBackend());
    state.insert(QStringLiteral("generationUnavailableReason"),
                 m_controller->generationUnavailableReason());
    state.insert(QStringLiteral("statusMessage"), m_controller->statusMessage());
    state.insert(QStringLiteral("objectSelected"), m_controller->objectSelected());
    state.insert(QStringLiteral("objectMaskBoundsText"),
                 m_controller->objectMaskBoundsText());
    state.insert(QStringLiteral("hasSelection"),
                 canvas ? canvas->hasSelection() : false);

    QJsonArray layers;
    const QList<CanvasLayer> &all = document->layers();
    const int first = qMax(0, all.size() - 4);
    for (int i = first; i < all.size(); ++i)
    {
        const CanvasLayer &layer = all.at(i);
        layers.append(QJsonObject{
            {QStringLiteral("index"), i},
            {QStringLiteral("name"), layer.name},
            {QStringLiteral("type"), layer.type},
            {QStringLiteral("visible"), layer.visible},
            {QStringLiteral("w"), layer.raster.width()},
            {QStringLiteral("h"), layer.raster.height()},
        });
    }
    state.insert(QStringLiteral("layers"), layers);
    return state;
}

void PaintbrushTestHook::dispatch(QTcpSocket *socket, const QJsonObject &request)
{
    const QString token = request.value(QStringLiteral("token")).toString();
    const QString command = request.value(QStringLiteral("cmd")).toString();
    const QJsonObject args = request.value(QStringLiteral("args")).toObject();
    QJsonObject response{
        {QStringLiteral("id"), request.value(QStringLiteral("id"))},
        {QStringLiteral("cmd"), command},
    };

    if (token != m_token)
    {
        response.insert(QStringLiteral("ok"), false);
        response.insert(QStringLiteral("error"), QStringLiteral("bad token"));
        send(socket, response);
        return;
    }

    auto fail = [&](const QString &message) {
        response.insert(QStringLiteral("ok"), false);
        response.insert(QStringLiteral("error"), message);
        send(socket, response);
    };
    auto succeed = [&](const QJsonObject &extra = QJsonObject()) {
        response.insert(QStringLiteral("ok"), true);
        for (auto it = extra.begin(); it != extra.end(); ++it)
            response.insert(it.key(), it.value());
        response.insert(QStringLiteral("state"), stateObject());
        send(socket, response);
    };

    if (command == QStringLiteral("hello") || command == QStringLiteral("state"))
    {
        succeed();
        return;
    }
    if (command == QStringLiteral("wait_ready"))
    {
        const int timeout = args.value(QStringLiteral("timeoutMs")).toInt(120000);
        const bool ready = waitUntil(timeout, [this]() {
            PaintbrushCanvasItem *canvas = m_controller->canvas();
            return canvas && canvas->width() > 0.0
                   && m_controller->hasOpenDocument();
        });
        response.insert(QStringLiteral("ready"), ready);
        succeed();
        return;
    }
    if (command == QStringLiteral("wait_busy"))
    {
        const int timeout = args.value(QStringLiteral("timeoutMs")).toInt(30000);
        const bool busy = waitUntil(timeout, [this]() {
            return m_controller->generationBusy();
        });
        response.insert(QStringLiteral("busy"), busy);
        succeed();
        return;
    }
    if (command == QStringLiteral("wait_idle"))
    {
        const int timeout = args.value(QStringLiteral("timeoutMs")).toInt(600000);
        const bool idle = waitUntil(timeout, [this]() {
            return !m_controller->generationBusy();
        });
        response.insert(QStringLiteral("idle"), idle);
        succeed();
        return;
    }
    if (command == QStringLiteral("wait_ms"))
    {
        const int ms = qBound(0, args.value(QStringLiteral("ms")).toInt(1000),
                              120000);
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        succeed();
        return;
    }
    if (command == QStringLiteral("open_image"))
    {
        const QString path = args.value(QStringLiteral("path")).toString();
        if (!QFileInfo::exists(path))
            return fail(QStringLiteral("image not found: %1").arg(path));
        m_controller->openImageFile(QUrl::fromLocalFile(path));
        succeed(QJsonObject{{QStringLiteral("path"), path}});
        return;
    }
    if (command == QStringLiteral("open_project"))
    {
        const QString path = args.value(QStringLiteral("path")).toString();
        if (!QFileInfo::exists(path))
            return fail(QStringLiteral("project not found: %1").arg(path));
        m_controller->openProject(QUrl::fromLocalFile(path));
        succeed(QJsonObject{{QStringLiteral("path"), path}});
        return;
    }
    if (command == QStringLiteral("save_project"))
    {
        const QString path = args.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            return fail(QStringLiteral("save_project needs a path"));
        QDir().mkpath(QFileInfo(path).absolutePath());
        m_controller->saveProject(QUrl::fromLocalFile(path));
        succeed(QJsonObject{{QStringLiteral("path"), path}});
        return;
    }
    if (command == QStringLiteral("new_canvas"))
    {
        m_controller->newBlankCanvas(
            args.value(QStringLiteral("width")).toInt(1280),
            args.value(QStringLiteral("height")).toInt(720));
        succeed();
        return;
    }
    if (command == QStringLiteral("set_object_mask"))
    {
        const QString path = args.value(QStringLiteral("path")).toString();
        QImage mask(path);
        if (mask.isNull())
            return fail(QStringLiteral("mask not readable: %1").arg(path));
        mask = mask.convertToFormat(QImage::Format_Grayscale8);
        const QSize canvasSize = m_controller->document()->canvasSize();
        if (mask.size() != canvasSize)
            mask = mask.scaled(canvasSize, Qt::IgnoreAspectRatio,
                               Qt::FastTransformation);
        const bool applied = m_controller->testApplyObjectMask(mask);
        succeed(QJsonObject{
            {QStringLiteral("applied"), applied},
            {QStringLiteral("maskW"), mask.width()},
            {QStringLiteral("maskH"), mask.height()},
        });
        return;
    }
    if (command == QStringLiteral("set_selection_rect"))
    {
        PaintbrushCanvasItem *canvas = m_controller->canvas();
        if (!canvas)
            return fail(QStringLiteral("canvas is not ready"));
        const QSize size = m_controller->document()->canvasSize();
        QRect rect(args.value(QStringLiteral("x")).toInt(),
                   args.value(QStringLiteral("y")).toInt(),
                   args.value(QStringLiteral("w")).toInt(),
                   args.value(QStringLiteral("h")).toInt());
        rect = rect.intersected(QRect(QPoint(0, 0), size));
        if (rect.isEmpty())
            return fail(QStringLiteral("selection rect is empty"));
        QImage mask(size, QImage::Format_Grayscale8);
        mask.fill(0);
        for (int y = rect.top(); y <= rect.bottom(); ++y)
        {
            uchar *line = mask.scanLine(y);
            for (int x = rect.left(); x <= rect.right(); ++x)
                line[x] = 255;
        }
        canvas->setSelectionFromMask(mask);
        succeed(QJsonObject{
            {QStringLiteral("x"), rect.x()},
            {QStringLiteral("y"), rect.y()},
            {QStringLiteral("w"), rect.width()},
            {QStringLiteral("h"), rect.height()},
        });
        return;
    }
    if (command == QStringLiteral("set_selection_mask"))
    {
        PaintbrushCanvasItem *canvas = m_controller->canvas();
        if (!canvas)
            return fail(QStringLiteral("canvas is not ready"));
        const QString path = args.value(QStringLiteral("path")).toString();
        QImage mask(path);
        if (mask.isNull())
            return fail(QStringLiteral("mask not readable: %1").arg(path));
        mask = mask.convertToFormat(QImage::Format_Grayscale8);
        const QSize size = m_controller->document()->canvasSize();
        if (mask.size() != size)
            mask = mask.scaled(size, Qt::IgnoreAspectRatio,
                               Qt::FastTransformation);
        for (int y = 0; y < mask.height(); ++y)
        {
            uchar *line = mask.scanLine(y);
            for (int x = 0; x < mask.width(); ++x)
                line[x] = line[x] >= 128 ? 255 : 0;
        }
        canvas->setSelectionFromMask(mask);
        succeed(QJsonObject{{QStringLiteral("path"), path}});
        return;
    }
    if (command == QStringLiteral("selection_clear"))
    {
        PaintbrushCanvasItem *canvas = m_controller->canvas();
        if (!canvas)
            return fail(QStringLiteral("canvas is not ready"));
        canvas->selectionClear();
        succeed();
        return;
    }
    if (command == QStringLiteral("remove_object"))
    {
        m_controller->removeObject(args.value(QStringLiteral("prompt")).toString());
        succeed();
        return;
    }
    if (command == QStringLiteral("fill"))
    {
        m_controller->generativeFill(args.value(QStringLiteral("prompt")).toString());
        succeed();
        return;
    }
    if (command == QStringLiteral("timed_fill")
        || command == QStringLiteral("timed_remove"))
    {
        const QString prompt = args.value(QStringLiteral("prompt")).toString();
        const int timeout = args.value(QStringLiteral("timeoutMs")).toInt(600000);
        QElapsedTimer timer;
        timer.start();
        if (command == QStringLiteral("timed_fill"))
            m_controller->generativeFill(prompt);
        else
            m_controller->removeObject(prompt);
        const bool becameBusy = waitUntil(2000, [this]() {
            return m_controller->generationBusy();
        });
        const bool idle = waitUntil(timeout, [this]() {
            return !m_controller->generationBusy();
        });
        response.insert(QStringLiteral("becameBusy"), becameBusy);
        response.insert(QStringLiteral("idle"), idle);
        response.insert(QStringLiteral("elapsedMs"),
                        double(timer.elapsed()));
        succeed();
        return;
    }
    if (command == QStringLiteral("fill_cancel_then_fill"))
    {
        const QString prompt = args.value(QStringLiteral("prompt")).toString();
        const int cancelAfter =
            qBound(0, args.value(QStringLiteral("cancelAfterMs")).toInt(50),
                   5000);
        const int graceMs =
            qBound(0, args.value(QStringLiteral("graceMs")).toInt(4000),
                   60000);
        const int timeout =
            args.value(QStringLiteral("timeoutMs")).toInt(600000);

        /* A: start, wait while genuinely busy, cancel through the real
         * controller action, then make sure the cancellation was observed. */
        QElapsedTimer aTimer;
        aTimer.start();
        m_controller->generativeFill(prompt);
        const bool aBusy = waitUntil(3000, [this]() {
            return m_controller->generationBusy();
        });
        {
            QEventLoop loop;
            QTimer::singleShot(cancelAfter, &loop, &QEventLoop::quit);
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
        const bool cancelledWhileBusy = m_controller->generationBusy();
        if (cancelledWhileBusy)
            m_controller->cancelGeneration();
        const int aElapsed = int(aTimer.elapsed());
        const int layersAfterCancel = m_controller->document()->layerCount();

        /* Let any in-flight worker finish and prove it cannot commit. */
        {
            QEventLoop loop;
            QTimer::singleShot(1500, &loop, &QEventLoop::quit);
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
        const int layersAfterCancelGrace =
            m_controller->document()->layerCount();
        const QString statusAfterCancel = m_controller->statusMessage();

        /* B: the only result allowed to publish. */
        const int layersBeforeB = m_controller->document()->layerCount();
        QElapsedTimer bTimer;
        bTimer.start();
        m_controller->generativeFill(prompt);
        const bool bBusy = waitUntil(3000, [this]() {
            return m_controller->generationBusy();
        });
        const bool bIdle = waitUntil(timeout, [this]() {
            return !m_controller->generationBusy();
        });
        const int bElapsed = int(bTimer.elapsed());
        const int layersAfterB = m_controller->document()->layerCount();
        const QString statusAfterB = m_controller->statusMessage();

        /* Grace: a stale A result arriving late would add a layer here. */
        {
            QEventLoop loop;
            QTimer::singleShot(graceMs, &loop, &QEventLoop::quit);
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
        const int layersAfterGrace = m_controller->document()->layerCount();
        const QString statusAfterGrace = m_controller->statusMessage();

        response.insert(QStringLiteral("aBusy"), aBusy);
        response.insert(QStringLiteral("cancelledWhileBusy"), cancelledWhileBusy);
        response.insert(QStringLiteral("aElapsedMs"), aElapsed);
        response.insert(QStringLiteral("layersAfterCancel"), layersAfterCancel);
        response.insert(QStringLiteral("layersAfterCancelGrace"),
                        layersAfterCancelGrace);
        response.insert(QStringLiteral("statusAfterCancel"), statusAfterCancel);
        response.insert(QStringLiteral("layersBeforeB"), layersBeforeB);
        response.insert(QStringLiteral("bBusy"), bBusy);
        response.insert(QStringLiteral("bIdle"), bIdle);
        response.insert(QStringLiteral("bElapsedMs"), bElapsed);
        response.insert(QStringLiteral("layersAfterB"), layersAfterB);
        response.insert(QStringLiteral("statusAfterB"), statusAfterB);
        response.insert(QStringLiteral("layersAfterGrace"), layersAfterGrace);
        response.insert(QStringLiteral("statusAfterGrace"), statusAfterGrace);
        succeed();
        return;
    }
    if (command == QStringLiteral("cancel"))
    {
        m_controller->cancelGeneration();
        succeed();
        return;
    }
    if (command == QStringLiteral("undo"))
    {
        m_controller->undo();
        succeed();
        return;
    }
    if (command == QStringLiteral("redo"))
    {
        m_controller->redo();
        succeed();
        return;
    }
    if (command == QStringLiteral("export_canvas"))
    {
        const QString path = args.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            return fail(QStringLiteral("export_canvas needs a path"));
        const bool alpha = args.value(QStringLiteral("alpha")).toBool(false);
        QImage image = alpha
            ? DocumentRenderer::render(*m_controller->document())
            : m_controller->flattenedPreview();
        if (image.isNull())
            return fail(QStringLiteral("canvas has no content"));
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (!image.save(path, "PNG"))
            return fail(QStringLiteral("could not write %1").arg(path));
        QFile file(path);
        QByteArray digest;
        if (file.open(QIODevice::ReadOnly))
        {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(&file);
            digest = hash.result().toHex();
        }
        succeed(QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("w"), image.width()},
            {QStringLiteral("h"), image.height()},
            {QStringLiteral("sha256"), QString::fromLatin1(digest)},
        });
        return;
    }
    if (command == QStringLiteral("export_layer"))
    {
        const QString path = args.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            return fail(QStringLiteral("export_layer needs a path"));
        const QList<CanvasLayer> &layers = m_controller->document()->layers();
        int index = args.value(QStringLiteral("index")).toInt(-1);
        if (index < 0)
            index = layers.size() + index;
        if (index < 0 || index >= layers.size())
            return fail(QStringLiteral("layer index out of range"));
        const CanvasLayer &layer = layers.at(index);
        if (layer.raster.isNull())
            return fail(QStringLiteral("layer has no raster"));
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (!layer.raster.save(path, "PNG"))
            return fail(QStringLiteral("could not write %1").arg(path));
        QFile file(path);
        QByteArray digest;
        if (file.open(QIODevice::ReadOnly))
        {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(&file);
            digest = hash.result().toHex();
        }
        succeed(QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("index"), index},
            {QStringLiteral("name"), layer.name},
            {QStringLiteral("type"), layer.type},
            {QStringLiteral("w"), layer.raster.width()},
            {QStringLiteral("h"), layer.raster.height()},
            {QStringLiteral("sha256"), QString::fromLatin1(digest)},
        });
        return;
    }

    fail(QStringLiteral("unknown command: %1").arg(command));
}

} // namespace paintbrush
