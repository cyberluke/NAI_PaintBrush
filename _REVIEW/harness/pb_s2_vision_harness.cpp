/*****************************************************************************
 * pb_vision_service_harness.cpp — Stream 2 acceptance harness (standalone)
 *****************************************************************************
 * Verifies the VLM truth-telling fixes in pb_vision_service.* without a VLC
 * build and without external network:
 *
 *   (a) the user's question reaches the payload VERBATIM as the first part,
 *   (b) a follow-up with the same session id builds >=2 user turns,
 *   (c) with the insecure flag off, an sslErrors signal cancels and reports
 *       an error instead of swallowing it.
 *
 * Build (MSYS2 mingw64, Qt6):
 *   moc pb_vision_service.hpp -o moc_pb_vision_service.cpp
 *   g++ -std=c++17 -w -O1 \
 *       -I<worktree>/include -I<worktree>/modules/gui/qt/paintbrush \
 *       -I<mingw64>/include/qt6 -I<mingw64>/include/qt6/QtCore \
 *       -I<mingw64>/include/qt6/QtNetwork -I<mingw64>/include/qt6/QtGui \
 *       pb_vision_service.cpp moc_pb_vision_service.cpp \
 *       pb_vision_service_harness.cpp -o pb_vision_service_harness.exe \
 *       -L<mingw64>/lib -lQt6Network -lQt6Gui -lQt6Core
 *
 * Run from the worktree root with <mingw64>/bin on PATH (Qt6 + OpenSSL DLLs)
 * and python on PATH (MSYS2). Requires pb_harness_cert.pem /
 * pb_harness_key.pem (self-signed cert) and pb_harness_tls_server.py next
 * to the executable; the TLS server subprocess uses Python's ssl module
 * because Qt's QSslServer cannot complete a handshake in this environment
 * when the client pauses on sslErrors.
 *
 * The pb_common helpers are stubbed; private members of the service are
 * exposed with `#define private public` for this test TU only.
 *****************************************************************************/

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QProcess>
#include <QtCore/QElapsedTimer>
#include <QtCore/QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImage>
#include <QPainter>

/* Expose the private members under test (buildPayload, buildUserContent,
 * appendMessage, makeSessionId, onSslErrors, m_insecure, m_sessions). */
#define private public
#include "pb_vision_service.hpp"
#undef private

/* ---- Stubs for paintbrush::pb_common helpers (no VLC runtime) ---- */
namespace paintbrush {
static QString g_baseUrl;
static bool g_insecure = false;
static QString g_apiKey;

QString pbConfigString(vlc_object_t *, const char *name)
{
    const QByteArray n(name);
    if (n == "paintbrush-vlm-base-url")
        return g_baseUrl;
    if (n == "paintbrush-vlm-model")
        return QStringLiteral("test-model");
    return {};
}

bool pbConfigBool(vlc_object_t *, const char *name)
{
    if (QByteArray(name) == "paintbrush-vlm-insecure")
        return g_insecure;
    return false;
}

int pbConfigInt(vlc_object_t *, const char *name)
{
    if (QByteArray(name) == "paintbrush-vlm-timeout")
        return 30;
    return 0;
}

QString pbApiKey(vlc_object_t *, const char *, const char *)
{
    return g_apiKey;
}

void pbLogRequest(vlc_object_t *, const char *, const QString &,
                  qint64, bool, const QString &)
{
}
} // namespace paintbrush

/* ---- TLS server with a self-signed certificate ---- */
/* Drives pb_harness_tls_server.py (real OpenSSL TLS via Python's ssl
 * module). Qt's QSslServer cannot complete a handshake in this environment
 * (Qt 6.11/MSYS2) when the client pauses on sslErrors, so the test server
 * must be a proper TLS implementation. */
class TlsServer
{
public:
    bool start(const QString &dir, const QString &python)
    {
        m_proc.setProgram(python);
        m_proc.setArguments({QStringLiteral("pb_harness_tls_server.py"),
                             dir + QLatin1String("/pb_harness_cert.pem"),
                             dir + QLatin1String("/pb_harness_key.pem"),
                             QStringLiteral("0")});
        m_proc.setWorkingDirectory(dir);
        m_proc.setProcessChannelMode(QProcess::SeparateChannels);
        m_proc.start();
        if (!m_proc.waitForStarted(5000))
            return false;

        QByteArray buffer;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 10000)
        {
            if (m_proc.waitForReadyRead(200))
            {
                buffer += m_proc.readAllStandardOutput();
                const int nl = buffer.indexOf('\n');
                if (nl >= 0)
                {
                    const QByteArray line = buffer.left(nl).trimmed();
                    if (line.startsWith("READY "))
                    {
                        m_port = line.mid(6).trimmed().toUShort();
                        qInfo().noquote()
                            << "server: python TLS server on 127.0.0.1:"
                            << m_port;
                        return m_port != 0;
                    }
                }
            }
            if (m_proc.state() != QProcess::Running)
                return false;
        }
        return false;
    }
    quint16 port() const { return m_port; }
    ~TlsServer()
    {
        if (m_proc.state() != QProcess::NotRunning)
        {
            m_proc.kill();
            m_proc.waitForFinished(5000);
        }
    }

private:
    QProcess m_proc;
    quint16 m_port = 0;
};

static int s_failures = 0;
#define CHECK(cond, msg) \
    do { \
        if (cond) { qInfo().noquote() << "PASS:" << msg; } \
        else { ++s_failures; qCritical().noquote() << "FAIL:" << msg; } \
    } while (0)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QImage crop(16, 16, QImage::Format_RGB32);
    crop.fill(Qt::red);
    QImage context(32, 24, QImage::Format_RGB32);
    context.fill(Qt::blue);

    /* ---------- (a) A1: the question reaches the payload verbatim ---------- */
    paintbrush::VisionService service;
    const QString question = QStringLiteral("What brand is this handbag?");
    const QByteArray payload = service.buildPayload(crop, context, question, QString());

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
    CHECK(parseError.error == QJsonParseError::NoError,
          "A1: buildPayload output parses as JSON");
    const QJsonArray msgs = doc.object().value(QLatin1String("messages")).toArray();
    CHECK(msgs.size() == 1, "A1: single-shot payload has exactly one message");
    const QJsonObject userMsg = msgs.first().toObject();
    CHECK(userMsg.value(QLatin1String("role")).toString() == QLatin1String("user"),
          "A1: the message role is user");
    const QJsonArray content = userMsg.value(QLatin1String("content")).toArray();
    CHECK(content.size() == 4,
          "A1: content = [question, systemPrompt, cropImg, contextImg]");
    CHECK(content.at(0).toObject().value(QLatin1String("type")).toString()
                  == QLatin1String("text")
              && content.at(0).toObject().value(QLatin1String("text")).toString()
                     == question,
          "A1: question appears VERBATIM as the FIRST content part");
    CHECK(content.at(1).toObject().value(QLatin1String("type")).toString()
                  == QLatin1String("text")
              && content.at(1).toObject().value(QLatin1String("text")).toString()
                     .startsWith(QLatin1String("You are a helpful creative assistant")),
          "A1: system prompt is the second text part");
    CHECK(content.at(2).toObject().value(QLatin1String("type")).toString()
                  == QLatin1String("image_url")
              && content.at(3).toObject().value(QLatin1String("type")).toString()
                     == QLatin1String("image_url"),
          "A1: crop and context images are the third and fourth parts");

    /* ---------- (b) A2: follow-up appends to the same session ---------- */
    paintbrush::g_baseUrl = QStringLiteral("http://127.0.0.1:9/chat/completions");
    paintbrush::VisionService multi;
    multi.configure(nullptr);
    const QString q1 = QStringLiteral("What color is the hat?");
    const QString q2 = QStringLiteral("And what material is it made of?");
    multi.ask(crop, context, q1, QStringLiteral("session-A"));
    multi.ask(crop, context, q2, QStringLiteral("session-A"));
    CHECK(multi.sessionCount() == 1,
          "A2: two asks with the same id share ONE session");

    const QJsonDocument doc2 = QJsonDocument::fromJson(
        multi.buildPayload(crop, context, q2, QStringLiteral("session-A")));
    const QJsonArray msgs2 = doc2.object().value(QLatin1String("messages")).toArray();
    CHECK(msgs2.size() == 2,
          "A2: follow-up payload carries 2 user turns (prior + current)");
    CHECK(msgs2.at(0).toObject().value(QLatin1String("role")).toString()
                  == QLatin1String("user")
              && msgs2.at(1).toObject().value(QLatin1String("role")).toString()
                     == QLatin1String("user"),
          "A2: both turns are user messages");
    CHECK(msgs2.at(0).toObject().value(QLatin1String("content")).toArray().at(0)
              .toObject().value(QLatin1String("text")).toString() == q1,
          "A2: first turn carries the PRIOR question verbatim");
    CHECK(msgs2.at(1).toObject().value(QLatin1String("content")).toArray().at(0)
              .toObject().value(QLatin1String("text")).toString() == q2,
          "A2: second turn carries the CURRENT question verbatim");

    /* bounded history: 12 asks -> capped at VisionSession::maxMessages */
    for (int i = 0; i < 12; ++i)
        multi.ask(crop, context, QStringLiteral("turn %1").arg(i),
                  QStringLiteral("session-B"));
    const QJsonDocument docB = QJsonDocument::fromJson(
        multi.buildPayload(crop, context, QStringLiteral("x"),
                           QStringLiteral("session-B")));
    CHECK(docB.object().value(QLatin1String("messages")).toArray().size()
                  == paintbrush::VisionSession::maxMessages,
          "A2: bounded history caps at 8 messages");

    /* empty conversationId -> stable generated session key */
    paintbrush::VisionService gen;
    gen.configure(nullptr);
    gen.ask(crop, context, q1, QString());
    gen.ask(crop, context, q1, QString());
    CHECK(gen.sessionCount() == 1,
          "A2: empty conversationId maps to a stable session key");

    multi.clearSession(QStringLiteral("session-A"));
    CHECK(multi.sessionCount() == 1, "A2: clearSession removes one session");

    /* Let the pending dummy asks (no server on 127.0.0.1:9) drain so the
     * teardown stays quiet. */
    {
        QEventLoop settle;
        QTimer::singleShot(300, &settle, &QEventLoop::quit);
        settle.exec();
    }

    /* ---------- (c) A7: TLS validation scope-limited ---------- */
    const QString dir = QCoreApplication::applicationDirPath();
    const QString python = QStandardPaths::findExecutable(
        QStringLiteral("python"));
    TlsServer server;
    if (python.isEmpty() || !server.start(dir, python))
    {
        qCritical() << "Cannot start python TLS test server (python or "
                       "cert/key missing?):" << dir;
        ++s_failures;
    }
    else
    {
        paintbrush::g_baseUrl =
            QStringLiteral("https://127.0.0.1:%1/chat/completions")
                .arg(server.port());

        /* phase 1: insecure OFF -> sslErrors must abort + report truthfully */
        paintbrush::g_insecure = false;
        paintbrush::VisionService secure;
        secure.configure(nullptr);
        int outcome1 = 0;
        QString message1;
        QEventLoop loop1;
        QObject::connect(&secure, &paintbrush::VisionService::errorOccurred,
                         &loop1, [&](const QString &, const QString &m) {
                             outcome1 = 1;
                             message1 = m;
                             loop1.quit();
                         });
        QObject::connect(&secure, &paintbrush::VisionService::responseReady,
                         &loop1, [&](const QString &, const QString &, const QString &,
                                     const QString &, const QString &) {
                             outcome1 = 2;
                             loop1.quit();
                         });
        QTimer::singleShot(15000, &loop1, &QEventLoop::quit);
        qInfo().noquote() << "phase1: asking (insecure=false)...";
        secure.ask(crop, context, QStringLiteral("TLS probe"),
                   QStringLiteral("tls-1"));
        loop1.exec();
        qInfo().noquote() << "phase1: done, outcome=" << outcome1;
        CHECK(outcome1 == 1, "A7: insecure OFF -> errorOccurred, no responseReady");
        CHECK(message1.contains(QLatin1String("TLS")),
              "A7: error message states TLS certificate validation failure");
        CHECK(!message1.contains(QLatin1String("did not respond")),
              "A7: error is the TLS truth, not a generic failure");
        CHECK(!secure.m_insecure,
              "A7: configure() leaves the insecure flag OFF by default");

        /* phase 2: opt-in ON -> the REAL ignoreSslErrors() branch runs
         * end-to-end against the self-signed endpoint: handshake resumes,
         * request succeeds, answer parses, assistant turn is stored. */
        paintbrush::g_insecure = true;
        paintbrush::VisionService insecure;
        insecure.configure(nullptr);
        CHECK(insecure.m_insecure,
              "A7: paintbrush-vlm-insecure=true turns the opt-in flag ON");

        int outcome2 = 0;
        QString answer2;
        QEventLoop loop2;
        QObject::connect(&insecure, &paintbrush::VisionService::errorOccurred,
                         &loop2, [&](const QString &, const QString &m) {
                             outcome2 = 1;
                             qWarning() << "unexpected error:" << m;
                             loop2.quit();
                         });
        QObject::connect(&insecure, &paintbrush::VisionService::responseReady,
                         &loop2, [&](const QString &, const QString &answer,
                                     const QString &, const QString &, const QString &) {
                             outcome2 = 2;
                             answer2 = answer;
                             loop2.quit();
                         });
        QTimer::singleShot(15000, &loop2, &QEventLoop::quit);
        qInfo().noquote() << "phase2: asking (insecure=true)...";
        insecure.ask(crop, context, QStringLiteral("TLS probe 2"),
                     QStringLiteral("tls-2"));
        loop2.exec();
        qInfo().noquote() << "phase2: done, outcome=" << outcome2;
        CHECK(outcome2 == 2,
              "A7: opt-in ON -> request succeeds against self-signed endpoint");
        CHECK(answer2 == QLatin1String("ok from harness"),
              "A7: response parsed through the happy path");

        const QJsonDocument docT = QJsonDocument::fromJson(
            insecure.buildPayload(crop, context, QStringLiteral("x"),
                                  QStringLiteral("tls-2")));
        const QJsonArray msgsT = docT.object().value(QLatin1String("messages")).toArray();
        CHECK(msgsT.size() == 2
                  && msgsT.at(1).toObject().value(QLatin1String("role")).toString()
                         == QLatin1String("assistant"),
              "A2: successful reply stored back as an assistant turn (real conversation)");
    }

    qInfo().noquote() << "----";
    if (s_failures == 0)
        qInfo().noquote() << "ALL ACCEPTANCE CHECKS PASSED";
    else
        qCritical().noquote() << s_failures << "CHECK(S) FAILED";
    return s_failures == 0 ? 0 : 1;
}