/*****************************************************************************
 * pb_test_hook.hpp : NAI Paintbrush — test-only loopback control channel
 *****************************************************************************
 * Copyright (C) 2026 NAI Paintbrush authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *****************************************************************************/

#ifndef PB_TEST_HOOK_HPP
#define PB_TEST_HOOK_HPP

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QString>

#include <functional>

class QJsonObject;
class QTcpServer;
class QTcpSocket;

namespace paintbrush {

class PaintbrushController;

/* Test-only (PB_TEST_HOOK) loopback JSON channel for the real application
 * process. It exists so acceptance tests can drive the REAL canvas and the
 * REAL controller actions without any desktop input, cursor movement,
 * foreground stealing or injected keystrokes.
 *
 * It is created only when the PB_TEST_HOOK environment variable is set, listens
 * on 127.0.0.1 with an ephemeral port, and publishes {pid, port, token} to the
 * file named by PB_TEST_HOOK_FILE. Every request must carry the token.
 *
 * Nothing here changes product behaviour: the commands below call the same
 * public controller/canvas entry points a QML button would call. */
class PaintbrushTestHook : public QObject
{
    Q_OBJECT

public:
    explicit PaintbrushTestHook(PaintbrushController *controller,
                                QObject *parent = nullptr);
    ~PaintbrushTestHook() override;

private slots:
    void onNewConnection();

private:
    void processBuffer(QTcpSocket *socket);
    void dispatch(QTcpSocket *socket, const QJsonObject &request);
    void send(QTcpSocket *socket, const QJsonObject &response);
    QJsonObject stateObject() const;
    /* Pump GUI events (without user input) until done() or the timeout. */
    bool waitUntil(int timeoutMs, const std::function<bool()> &done) const;
    void setBusy(bool busy) { m_busy = busy; }
    bool isBusy() const { return m_busy; }

    PaintbrushController *m_controller = nullptr;
    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QString m_token;
    QString m_endpointPath;
    bool m_busy = false;
};

} // namespace paintbrush

#endif // PB_TEST_HOOK_HPP
