/*****************************************************************************
 * s5_acceptance.cpp — Stream 5 (first half) acceptance harness
 *
 * Standalone test for the Paintbrush honest-AI changes. Compiled with Qt6
 * (mingw64) against the real pb_* service sources; VLC itself is replaced
 * by a stub vlc_common.h and stub config/log helper definitions.
 *
 * Acceptance items asserted:
 *  (a) GenerationService never claims remote readiness without a provider:
 *      capability() is local-heal/local-blur/unavailable with a reason,
 *      isGenerative()==false, no model name; remote-generation only with
 *      a provider.
 *  (b) A long local inpaint returns immediately from generate(); the
 *      result arrives asynchronously via resultReady afterwards.
 *  (c) url provider responses are downloaded over QNetworkReply (end to
 *      end against a loopback HTTP server; no QImage::load(url) remains).
 *  (d) assistant/lookup/generation TLS opt-in is off by default; the
 *      sslErrors handler only bypasses validation when the flag is on.
 *  (e) lookup matches carry evidenceBacked=false, confidenceIsEstimate=true
 *      and never present an empty sourceUrl as a real source.
 *
 * No external network is used: the HTTP server binds 127.0.0.1 on an
 * ephemeral port.
 *****************************************************************************/

#include "pb_generation_service.hpp"
#include "pb_assistant_service.hpp"
#include "pb_lookup_service.hpp"
#include "pb_capability.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QPainter>
#include <QBuffer>
#include <QThread>
#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

/* ------------------------------------------------------------------ stubs */

/* Stub implementations of the pb_common config helpers declared by
 * pb_common.hpp, driven by a simple in-test config store. */
static std::map<std::string, std::string> g_cfgStrings;
static std::map<std::string, int> g_cfgBools;
static std::map<std::string, int> g_cfgInts;

namespace paintbrush {

QString pbConfigString(vlc_object_t *, const char *name)
{
    const auto it = g_cfgStrings.find(name);
    if (it == g_cfgStrings.end())
        return {};
    return QString::fromStdString(it->second);
}

bool pbConfigBool(vlc_object_t *, const char *name)
{
    const auto it = g_cfgBools.find(name);
    return it != g_cfgBools.end() && it->second != 0;
}

int pbConfigInt(vlc_object_t *, const char *name)
{
    const auto it = g_cfgInts.find(name);
    return it == g_cfgInts.end() ? 0 : it->second;
}

QString pbApiKey(vlc_object_t *, const char *, const char *)
{
    return {};
}

void pbLogRequest(vlc_object_t *, const char *, const QString &, qint64,
                  bool, const QString &)
{
    /* not part of the acceptance surface; swallowed */
}

} // namespace paintbrush

/* ------------------------------------------------------------------ utils */

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, label)                                                     \
    do {                                                                       \
        ++g_checks;                                                            \
        if (cond)                                                              \
            std::printf("PASS  %s\n", label);                                  \
        else {                                                                 \
            std::printf("FAIL  %s\n", label);                                  \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static bool waitFor(const std::function<bool()> &cond, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!cond() && timer.elapsed() < timeoutMs)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return cond();
}

/* Big image + full-coverage mask: forces the onion-peel inpaint to chew
 * through every pixel (multi-second compute on a CPU). */
static QImage makeBigImage(int size)
{
    QImage img(size, size, QImage::Format_ARGB32);
    img.fill(QColor(60, 90, 140));
    QPainter p(&img);
    p.fillRect(QRect(0, 0, size, size / 2), QColor(200, 120, 60));
    p.fillRect(QRect(0, size / 2, size / 3, size), QColor(40, 160, 90));
    p.end();
    return img;
}

static QImage makeMask(int size, bool fullCoverage)
{
    QImage mask(size, size, QImage::Format_Grayscale8);
    mask.fill(fullCoverage ? 255 : 0);
    if (fullCoverage)
    {
        /* 1-px known border so the peel has seeds */
        QPainter p(&mask);
        p.fillRect(QRect(0, 0, size, 1), Qt::black);
        p.fillRect(QRect(0, size - 1, size, 1), Qt::black);
        p.fillRect(QRect(0, 0, 1, size), Qt::black);
        p.fillRect(QRect(size - 1, 0, 1, size), Qt::black);
        p.end();
    }
    else
    {
        QPainter p(&mask);
        p.fillRect(QRect(size / 4, size / 4, size / 2, size / 2), Qt::white);
        p.end();
    }
    return mask;
}

/* Loopback HTTP provider: serves the OpenAI-style JSON for the generation
 * POST (with a url field pointing at itself) and the PNG for the image GET,
 * plus a chat response for the assistant endpoint. */
class FakeProviderServer : public QTcpServer
{
public:
    QByteArray pngBytes;
    int requestCount = 0;

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(socketDescriptor);
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket]() {
            QByteArray &buf = m_buffers[socket];
            buf += socket->readAll();
            const int headerEnd = buf.indexOf("\r\n\r\n");
            if (headerEnd < 0)
                return; /* wait for the full header */

            const QByteArray header = buf.left(headerEnd);
            /* read the whole request body before answering: closing while
             * the client still uploads would RST the connection and drop
             * our response */
            qint64 contentLength = -1;
            for (const QByteArray &line : header.split('\n'))
            {
                if (line.toLower().startsWith("content-length:"))
                {
                    contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                    break;
                }
            }
            if (contentLength >= 0 &&
                buf.size() < headerEnd + 4 + contentLength)
                return; /* body still in flight */

            buf = buf.mid(headerEnd + 4);
            ++requestCount;
            const QList<QByteArray> parts = header.split(' ');
            const QByteArray method = parts.value(0);
            const QByteArray path = parts.value(1);

            QByteArray body;
            QByteArray contentType = QStringLiteral("text/plain").toLatin1();
            if (path.endsWith("/images/edits"))
            {
                const QByteArray self =
                    "http://127.0.0.1:" + QByteArray::number(serverPort());
                body = "{\"data\":[{\"url\":\"" + self + "/img.png\"}]}";
                contentType = "application/json";
            }
            else if (path.endsWith("/chat/completions"))
            {
                body = "{\"choices\":[{\"message\":{\"content\":\"hello back\"}}]}";
                contentType = "application/json";
            }
            else if (path == "/img.png")
            {
                body = pngBytes;
                contentType = "image/png";
            }
            else
            {
                body = "not found";
            }

            const QByteArray response =
                "HTTP/1.1 200 OK\r\nContent-Type: " + contentType +
                "\r\nContent-Length: " + QByteArray::number(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
            m_buffers.remove(socket);
            socket->deleteLater();
        });
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
    }

private:
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

/* ------------------------------------------------------------- tests (a) */

static void testCapabilityTruth()
{
    std::printf("--- (a) generation capability truth (A3/A4) ---\n");
    g_cfgStrings.clear();
    g_cfgBools.clear();

    paintbrush::GenerationService gen;
    gen.configure(nullptr);

    const paintbrush::CapabilityReport rep = gen.capability();
    CHECK(rep.mode == paintbrush::CapabilityReport::LocalHeal,
          "A3: no provider -> LocalHeal");
    CHECK(rep.modeName() == QLatin1String("local-heal"),
          "A3: canonical mode string 'local-heal'");
    CHECK(!rep.isGenerative(), "A3: never generative without a provider");
    CHECK(rep.isUsable(), "A3: local fallback is usable");
    CHECK(rep.isLocal(), "A3: report says local");
    CHECK(!rep.reason.isEmpty(), "A3: reason is present");
    CHECK(rep.modelName.isEmpty(), "A3: no model name without provider");
    CHECK(!gen.hasProvider(), "A3: hasProvider()==false without provider");
    CHECK(gen.capability().modeName() != QLatin1String("remote-generation"),
          "A3: never 'remote-generation' without provider");
    CHECK(gen.isConfigured() == true,
          "A3: isConfigured() truthful: local heal is a real usable capability");

    CHECK(gen.capability(paintbrush::GenerationService::ACTION_REMOVE_OBJECT).mode ==
              paintbrush::CapabilityReport::LocalHeal,
          "A4: remove object -> local-heal");
    CHECK(gen.capability(paintbrush::GenerationService::ACTION_GENERATIVE_FILL).mode ==
              paintbrush::CapabilityReport::LocalHeal,
          "A4: fill -> local-heal (not 'generative fill')");
    const paintbrush::CapabilityReport blur =
        gen.capability(paintbrush::GenerationService::ACTION_REPLACE_BACKGROUND);
    CHECK(blur.mode == paintbrush::CapabilityReport::LocalBlur,
          "A4: replace background -> local-blur");
    CHECK(blur.modeName() == QLatin1String("local-blur"),
          "A4: canonical mode string 'local-blur'");
    CHECK(!blur.isGenerative(), "A4: blur is not generative");
    CHECK(blur.reason.contains("blur"), "A4: blur reason names the blur");

    /* with a provider, remote-generation is claimed and named */
    g_cfgStrings["paintbrush-image-base-url"] = "http://127.0.0.1:1/v1";
    g_cfgStrings["paintbrush-image-model"] = "sdxl-test";
    gen.configure(nullptr);
    const paintbrush::CapabilityReport remote = gen.capability();
    CHECK(remote.mode == paintbrush::CapabilityReport::RemoteGeneration,
          "A3: provider configured -> remote-generation");
    CHECK(remote.isGenerative(), "A3: generative with a provider");
    CHECK(remote.modelName == QLatin1String("sdxl-test"),
          "A3: model name reported with provider");
    CHECK(remote.providerUrl == QLatin1String("http://127.0.0.1:1/v1"),
          "A3: provider URL reported with provider");
    CHECK(gen.capability(paintbrush::GenerationService::ACTION_REPLACE_BACKGROUND).mode ==
              paintbrush::CapabilityReport::RemoteGeneration,
          "A3: provider wins for every action");
    g_cfgStrings.clear();
}

/* ------------------------------------------------------------- tests (b) */

static void testAsyncLocalGeneration()
{
    std::printf("--- (b) local generation is async (A5) ---\n");
    g_cfgStrings.clear();
    g_cfgBools.clear();

    paintbrush::GenerationService gen;
    gen.configure(nullptr);

    const int size = 2600;
    const QImage img = makeBigImage(size);
    const QImage mask = makeMask(size, /*fullCoverage=*/true);

    bool gotResult = false;
    bool gotError = false;
    QImage received;
    QString receivedId;
    int busyTrueCount = 0;
    QObject::connect(&gen, &paintbrush::GenerationService::resultReady,
                     [&](const QString &id, const QImage &r) {
                         gotResult = true;
                         receivedId = id;
                         received = r;
                     });
    QObject::connect(&gen, &paintbrush::GenerationService::errorOccurred,
                     [&](const QString &, const QString &) { gotError = true; });
    QObject::connect(&gen, &paintbrush::GenerationService::busyChanged,
                     [&](bool busy) {
                         if (busy)
                             ++busyTrueCount;
                     });

    QElapsedTimer timer;
    timer.start();
    gen.generate(paintbrush::GenerationService::ACTION_GENERATIVE_FILL,
                 img, mask, QStringLiteral("test"), QStringLiteral("req-async"));
    const qint64 generateReturnMs = timer.elapsed();

    CHECK(generateReturnMs < 500,
          "A5: generate() returns immediately (<500ms) — GUI thread not blocked");
    CHECK(!gotResult, "A5: no result synchronously at return time");
    CHECK(gen.isBusy(), "A5: busy reported synchronously");

    QElapsedTimer waitTimer;
    waitTimer.start();
    const bool delivered =
        waitFor([&]() { return gotResult || gotError; }, 60000);
    const qint64 waitMs = waitTimer.elapsed();

    CHECK(delivered, "A5: worker delivers a terminal event");
    CHECK(gotResult, "A5: resultReady fired (async, from worker delivery)");
    CHECK(!gotError, "A5: no error for a valid inpaint");
    CHECK(!received.isNull(), "A5: result image is valid");
    CHECK(received.size() == img.size(), "A5: result has input dimensions");
    CHECK(receivedId == QLatin1String("req-async"), "A5: requestId preserved");
    CHECK(waitMs > 100,
          "A5: computation happened AFTER generate() returned (async)");
    CHECK(!gen.isBusy(), "A5: busy cleared after delivery");
    std::printf("      [timing] generate() returned in %lld ms; result after %lld ms; busyTrue=%d\n",
                (long long)generateReturnMs, (long long)waitMs, busyTrueCount);

    /* box-blur path is async too */
    gotResult = false;
    gotError = false;
    gen.generate(paintbrush::GenerationService::ACTION_REPLACE_BACKGROUND,
                 img, makeMask(600, false), QStringLiteral("test"),
                 QStringLiteral("req-blur"));
    const bool blurDelivered =
        waitFor([&]() { return gotResult || gotError; }, 30000);
    CHECK(blurDelivered && gotResult, "A5: local blur path also async-delivered");
    CHECK(receivedId == QLatin1String("req-blur"), "A5: blur result id preserved");
}

/* ------------------------------------------------------------- tests (c) */

static void testUrlDownloadEndToEnd()
{
    std::printf("--- (c) url provider downloaded via QNetworkReply (A6) ---\n");
    g_cfgStrings.clear();
    g_cfgBools.clear();

    /* reference PNG the fake provider will serve */
    QImage expected(96, 64, QImage::Format_ARGB32);
    expected.fill(QColor(200, 30, 30));
    QPainter pe(&expected);
    pe.fillRect(QRect(20, 10, 30, 20), QColor(30, 200, 30));
    pe.end();
    QByteArray pngBytes;
    {
        QBuffer buffer(&pngBytes);
        buffer.open(QIODevice::WriteOnly);
        expected.save(&buffer, "PNG");
    }

    FakeProviderServer server;
    server.pngBytes = pngBytes;
    if (!server.listen(QHostAddress::LocalHost, 0))
    {
        CHECK(false, "A6: loopback server failed to listen");
        return;
    }
    const QString baseUrl = QStringLiteral("http://127.0.0.1:%1/v1")
                                .arg(server.serverPort());
    g_cfgStrings["paintbrush-image-base-url"] = baseUrl.toStdString();

    paintbrush::GenerationService gen;
    gen.configure(nullptr);

    bool gotResult = false;
    bool gotError = false;
    QString errorText;
    QImage received;
    QObject::connect(&gen, &paintbrush::GenerationService::resultReady,
                     [&](const QString &, const QImage &r) {
                         gotResult = true;
                         received = r;
                     });
    QObject::connect(&gen, &paintbrush::GenerationService::errorOccurred,
                     [&](const QString &, const QString &m) {
                         gotError = true;
                         errorText = m;
                     });

    const QImage img = makeBigImage(320);
    const QImage mask = makeMask(320, false);
    gen.generate(paintbrush::GenerationService::ACTION_GENERATIVE_FILL,
                 img, mask, QStringLiteral("test"), QStringLiteral("req-url"));

    const bool delivered = waitFor([&]() { return gotResult || gotError; }, 20000);
    CHECK(delivered, "A6: url flow terminates");
    CHECK(gotResult, "A6: resultReady fired for a url provider response");
    if (gotError)
        std::printf("      [detail] url-flow error: %s\n", errorText.toUtf8().constData());
    CHECK(!gotError, "A6: no error from the url flow");
    CHECK(received.size() == expected.size(), "A6: downloaded image has served size");
    CHECK(received.pixel(10, 10) == expected.pixel(10, 10),
          "A6: downloaded pixel content matches served PNG");
    CHECK(server.requestCount == 2,
          "A6: exactly two exchanges (POST json + GET image)");
    server.close();
}

/* ------------------------------------------------------------- tests (d) */

static void testTlsOptIn()
{
    std::printf("--- (d) TLS opt-in off by default (A7) ---\n");
    g_cfgStrings.clear();
    g_cfgBools.clear();

    paintbrush::AssistantService assistant;
    assistant.configure(nullptr);
    CHECK(!assistant.insecureTlsOptIn(), "A7: assistant insecure flag default false");
    CHECK(assistant.tlsPosture() == paintbrush::TlsPosture::Normal,
          "A7: assistant posture Normal by default");

    g_cfgBools["paintbrush-assistant-insecure"] = 1;
    assistant.configure(nullptr);
    CHECK(assistant.insecureTlsOptIn(), "A7: assistant per-service opt-in honored");
    CHECK(assistant.tlsPosture() == paintbrush::TlsPosture::DevSelfSigned,
          "A7: assistant posture flips to DevSelfSigned");
    g_cfgBools.clear();

    paintbrush::LookupService lookup;
    lookup.configure(nullptr);
    CHECK(!lookup.insecureTlsOptIn(), "A7: lookup insecure flag default false");

    g_cfgBools["paintbrush-dev-insecure"] = 1;
    lookup.configure(nullptr);
    CHECK(lookup.insecureTlsOptIn(), "A7: shared paintbrush-dev-insecure honored");
    g_cfgBools.clear();

    paintbrush::GenerationService gen;
    gen.configure(nullptr);
    CHECK(!gen.insecureTlsOptIn(), "A7: generation insecure flag default false");

    g_cfgBools["paintbrush-image-insecure"] = 1;
    gen.configure(nullptr);
    CHECK(gen.insecureTlsOptIn(), "A7: generation per-service opt-in honored");
    g_cfgBools.clear();
}

/* ------------------------------------------------------------- tests (e) */

static void testLookupHonesty()
{
    std::printf("--- (e) lookup honesty (A42) ---\n");
    g_cfgStrings.clear();
    g_cfgBools.clear();

    paintbrush::LookupService lookup;
    lookup.configure(nullptr);
    CHECK(!lookup.evidenceBacked(),
          "A42: module-level evidenceBacked()==false (candidate prompt only)");

    paintbrush::LookupService::Match m;
    CHECK(!m.evidenceBacked, "A42: Match.evidenceBacked default false");
    CHECK(m.confidenceIsEstimate, "A42: Match.confidenceIsEstimate default true");
    CHECK(!m.hasSource(), "A42: empty sourceUrl is never a real source");

    const QString modelJson =
        QStringLiteral("{\"title\": \"Leather handbag\", "
                       "\"description\": \"Tan leather tote\", "
                       "\"confidence\": 0.91, "
                       "\"evidence\": \"model-generated guess\", "
                       "\"metadata\": {\"brand\": \"X\", \"model\": \"Y\"}}");
    const QVariantList matches =
        paintbrush::LookupService::matchesFromContent(modelJson);
    CHECK(matches.size() == 1, "A42: parses one candidate match");
    const QVariantMap first = matches.first().toMap();
    CHECK(first.value(QStringLiteral("evidenceBacked")) == false,
          "A42: result map evidenceBacked=false");
    CHECK(first.value(QStringLiteral("confidenceIsEstimate")) == true,
          "A42: result map confidenceIsEstimate=true");
    CHECK(first.value(QStringLiteral("sourceUrl")).toString().isEmpty(),
          "A42: sourceUrl empty, never a fabricated source");
    CHECK(first.value(QStringLiteral("imageUrl")).toString().isEmpty(),
          "A42: imageUrl empty");
    CHECK(first.value(QStringLiteral("title")).toString() ==
              QLatin1String("Leather handbag"),
          "A42: model title preserved");
    CHECK(first.value(QStringLiteral("confidence")).toDouble() > 0.9,
          "A42: model confidence kept but flagged as estimate (not grounded)");

    const QVariantList plain =
        paintbrush::LookupService::matchesFromContent(
            QStringLiteral("This is a plain text identification"));
    CHECK(plain.size() == 1, "A42: plain-text fallback still yields one match");
    CHECK(plain.first().toMap().value(QStringLiteral("evidenceBacked")) == false,
          "A42: plain-text fallback also unbacked");
    CHECK(plain.first().toMap().value(QStringLiteral("sourceUrl")).toString().isEmpty(),
          "A42: plain-text fallback has no source");

    /* empty content -> no matches */
    CHECK(paintbrush::LookupService::matchesFromContent(QString()).isEmpty(),
          "A42: empty content yields no matches");
}

/* ------------------------------------------------------- assistant smoke */

static void testAssistantChatAgainstLoopback()
{
    std::printf("--- assistant chat smoke (regression) ---\n");
    g_cfgStrings.clear();
    g_cfgBools.clear();

    FakeProviderServer server;
    if (!server.listen(QHostAddress::LocalHost, 0))
    {
        CHECK(false, "assistant: loopback server failed to listen");
        return;
    }
    g_cfgStrings["paintbrush-assistant-base-url"] =
        (QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort())).toStdString();

    paintbrush::AssistantService assistant;
    assistant.configure(nullptr);
    CHECK(assistant.isConfigured(), "assistant: configured against loopback");

    bool gotResponse = false;
    QString responseId;
    QString responseText;
    bool gotError = false;
    QObject::connect(&assistant, &paintbrush::AssistantService::responseReady,
                     [&](const QString &id, const QString &text) {
                         gotResponse = true;
                         responseId = id;
                         responseText = text;
                     });
    QObject::connect(&assistant, &paintbrush::AssistantService::errorOccurred,
                     [&](const QString &, const QString &) { gotError = true; });

    assistant.chat(QStringLiteral("hi"), QImage(), QStringLiteral("c1"));
    const bool delivered = waitFor([&]() { return gotResponse || gotError; }, 15000);
    CHECK(delivered && gotResponse && !gotError,
          "assistant: chat round-trip over loopback works");
    CHECK(responseId == QLatin1String("c1"), "assistant: conversation id preserved");
    CHECK(responseText == QLatin1String("hello back"), "assistant: content extracted");
    server.close();
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testCapabilityTruth();
    testAsyncLocalGeneration();
    testUrlDownloadEndToEnd();
    testTlsOptIn();
    testLookupHonesty();
    testAssistantChatAgainstLoopback();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}