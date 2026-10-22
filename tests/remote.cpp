#include "test-helpers.hpp"
#include "dock.hpp"
#include "docx.hpp"
#include "locale.hpp"
#include "notes.hpp"
#include "remote.hpp"
#include "web-document.hpp"
#include "vendor/obs-websocket-api.h"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QKeyEvent>
#include <QPushButton>
#include <QSettings>
#include <QScrollBar>
#include <QSpinBox>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextBlock>
#include <QTextEdit>
#include <QTextList>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <cstdio>
#include <future>

const char *obs_module_text(const char *key) {
    if (qstrcmp(key, "CharacterCount") == 0) return "%1 chars";
    if (qstrcmp(key, "CharacterRemaining") == 0) return "Used: %1 / %2; remaining: %3";
    if (qstrcmp(key, "ExportFilter") == 0) return "HTML (*.html);;Plain text (*.txt)";
    return key;
}
namespace {
using scenepad::test::require;
using scenepad::test::checks;
class StyleEvents final : public QObject {
public:
    int changes = 0;
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) ++changes;
        return false;
    }
};
obs_websocket_request_callback callback{};
bool registered = false;
QList<obs_source_t *> frontendScenes;
QList<QPair<QString, QJsonObject>> published;
void mockApi()
{
    auto *handler = obs_get_proc_handler();
    proc_handler_add(handler, "bool obs_websocket_api_get_ph(out ptr ph)", [](void *, calldata_t *data) {
        calldata_set_ptr(data, "ph", obs_get_proc_handler());
    }, nullptr);
    proc_handler_add(handler, "bool vendor_register(in string name, out ptr vendor)", [](void *, calldata_t *data) {
        calldata_set_ptr(data, "vendor", obs_get_proc_handler());
    }, nullptr);
    proc_handler_add(handler, "bool vendor_request_register(in ptr vendor, in string type, in ptr callback, out bool success)", [](void *, calldata_t *data) {
        callback = *static_cast<obs_websocket_request_callback *>(calldata_ptr(data, "callback"));
        registered = true; calldata_set_bool(data, "success", true);
    }, nullptr);
    proc_handler_add(handler, "bool vendor_request_unregister(in ptr vendor, in string type, out bool success)", [](void *, calldata_t *data) {
        registered = false; calldata_set_bool(data, "success", true);
    }, nullptr);
    proc_handler_add(handler, "bool vendor_event_emit(in ptr vendor, in string type, in ptr data, out bool success)", [](void *, calldata_t *data) {
        auto *event = static_cast<obs_data_t *>(calldata_ptr(data, "data"));
        published.append({QString::fromUtf8(calldata_string(data, "type")),
            QJsonDocument::fromJson(obs_data_get_string(event, "payload")).object()});
        calldata_set_bool(data, "success", true);
    }, nullptr);
}
std::future<QJsonObject> workerRequest(const QJsonObject &value)
{
    const auto function = callback;
    return std::async(std::launch::async, [function, value] {
        OBSDataAutoRelease input = obs_data_create(), output = obs_data_create();
        obs_data_set_string(input, "payload", QJsonDocument(value).toJson(QJsonDocument::Compact).constData());
        function.callback(input, output, function.priv_data);
        return QJsonDocument::fromJson(obs_data_get_string(output, "payload")).object();
    });
}
using scenepad::test::wait;
template<class Predicate> bool waitUntil(Predicate ready)
{
    QElapsedTimer clock; clock.start();
    while (!ready() && clock.elapsed() < 2000) wait(2);
    return ready();
}
QJsonObject changed(QJsonObject document, const QString &text)
{
    auto blocks = document.value("blocks").toArray();
    auto block = blocks[0].toObject();
    auto runs = block.value("runs").toArray();
    auto run = runs[0].toObject(); run.insert("text", text); runs[0] = run; block.insert("runs", runs); blocks[0] = block;
    document.insert("blocks", blocks); return document;
}
QByteArray http(quint16 port, const QByteArray &request)
{
    QTcpSocket socket; socket.connectToHost(QHostAddress::LocalHost, port);
    require(socket.waitForConnected(1000), "HTTP connects"); socket.write(request); socket.flush();
    QByteArray result; QElapsedTimer clock; clock.start();
    while (clock.elapsed() < 4000) {
        QApplication::processEvents(); result += socket.readAll();
        if (socket.state() == QAbstractSocket::UnconnectedState) break;
        QThread::msleep(1);
    }
    return result;
}
}

// Supply the frontend scene order without an OBS main window.
void obs_frontend_get_scenes(obs_frontend_source_list *list)
{
    for (auto *scene : frontendScenes) {
        auto *reference = obs_source_get_ref(scene);
        da_push_back(list->sources, &reference);
    }
}

QJsonObject body(const QByteArray &response)
{ return QJsonDocument::fromJson(response.mid(response.indexOf("\r\n\r\n") + 4)).object(); }

void checkAppResources(const QUrl &url)
{
    const auto port = static_cast<quint16>(url.port());
    const auto get = [&](const QByteArray &path) { return http(port, "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n"); };
    const auto html = get("/");
    require(html.contains("manifest-src 'self'") && html.contains("img-src 'self'") && html.contains("rel=\"manifest\""), "app metadata and same-origin icons are allowed");
    const auto response = get("/manifest.webmanifest");
    const auto manifest = body(response);
    require(response.startsWith("HTTP/1.1 200") && response.contains("application/manifest+json") && response.contains("Cache-Control: no-store"), "manifest has its own MIME type and fresh content");
    require(manifest.value("name") == "ScenePad" && manifest.value("display") == "standalone" && manifest.value("start_url") == "/" && manifest.value("scope") == "/", "app identity and startup scope are valid");
    require(!response.contains(url.fragment().toUtf8()) && manifest.value("icons").toArray().size() == 2, "public metadata does not disclose the connection capability");
    for (const int size : {192, 512}) {
        const auto icon = get("/icon-" + QByteArray::number(size) + ".png");
        const auto image = QImage::fromData(icon.mid(icon.indexOf("\r\n\r\n") + 4));
        require(icon.startsWith("HTTP/1.1 200") && icon.contains("Content-Type: image/png") && image.size() == QSize(size, size), "PNG app icons are served at their declared sizes");
    }
    require(get("/icon.svg").contains("Content-Type: image/svg+xml"), "vector favicon has the correct MIME type");
    require(get("/service-worker.js").startsWith("HTTP/1.1 404"), "the lightweight app does not install an offline worker");
    QElapsedTimer clock; clock.start();
    for (int i=0; i<100; ++i) {
        require(get(i%2 ? "/manifest.webmanifest" : "/icon-192.png").startsWith("HTTP/1.1 200"), "static app resources remain available");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    require(clock.elapsed() < 10000, "static app requests remain responsive");
    std::printf("PASS PWA HTTP: manifest, icon sizes, MIME, policy and capability privacy; requests=100 elapsed=%lldms\n", clock.elapsed());
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (!obs_startup("en-US", nullptr, nullptr)) return 1;
    int result = 0;
    try {
        QTemporaryDir temp;
        QDir().mkpath(temp.filePath("obs-scenepad"));
        QDir().mkpath(temp.filePath("obs-websocket"));
        const auto settingsPath = temp.filePath("obs-scenepad/settings.ini");
        {
            QSettings settings(settingsPath, QSettings::IniFormat);
            settings.setValue("remote/access", QString(64, 'a'));
        }
        scenepad::Dock dock(settingsPath);
        auto &notes = dock.notes();
        auto *remote = dock.remote();
        mockApi(); remote->initialize();
        require(registered, "vendor request registers through the OBS WebSocket API");
        require(!remote->handle({}).value("ok").toBool(), "remote access is disabled by default");
        require(!QSettings(settingsPath, QSettings::IniFormat).value("remote/enabled", false).toBool(), "disabled preference defaults to false");
        auto *qrButton = dock.findChild<QToolButton *>("remoteQrButton");
        require(qrButton && qrButton->isHidden(), "QR entry is hidden while LAN access is disabled");
        require(remote->listen("127.0.0.1", 0), "bind loopback on an available port");
        require(!qrButton->isHidden() && !dock.findChild<QLabel *>("remoteQrOverlay"), "LAN access exposes only the collapsed QR entry");
        const QUrl url(remote->url());
        require(url.fragment().size() == 22 && !url.fragment().contains('=') &&
            QByteArray::fromBase64(url.fragment().toLatin1(), QByteArray::Base64UrlEncoding).size() == 16,
            "capability is a URL-safe 128-bit link fragment without padding");
        checkAppResources(url);
        if (app.arguments().value(1) == "--pwa-only") { dock.shutdown(); obs_shutdown(); return 0; }
        const auto port = static_cast<quint16>(url.port());
        auto request = [&](const char *action, QJsonObject data = {}) {
            data.insert("action", action); data.insert("access", url.fragment()); return remote->handle(data);
        };
        const auto denied = remote->handle({{"action", "Open"}, {"access", "wrong"}});
        require(denied.value("code") == "access", "unrelated OBS clients cannot access notes without the link capability");
        auto first = request("Open", {{"name", "Phone"}});
        auto second = request("Open", {{"name", "Tablet"}});
        const auto session = first.value("session").toString();
        const auto session2 = second.value("session").toString();
        require(first.value("speed").toDouble() == 0.5, "remote state starts at 0.5x");
        for (int i = 0; i < 1000; ++i) {
            const double speed = (1 + i % 50) / 10.0;
            const auto state = request("Scroll", {{"session", session}, {"command", "speed"}, {"speed", speed}});
            require(state.value("ok").toBool() && state.value("speed").toDouble() == speed && dock.speed() == speed,
                "remote speed cycles cover every 0.1-5.0x step");
        }
        for (const double speed : {-1.0, 0.0, 0.09, 5.01, 9.0}) {
            require(request("Scroll", {{"session", session}, {"command", "speed"}, {"speed", speed}}).value("code") == "range",
                "remote rejects out-of-range speeds");
            require(dock.speed() == 5.0, "invalid remote speeds preserve the previous setting");
        }
        dock.setSpeed(0.5);
        std::printf("PASS SPEED REMOTE: default, all 50 steps, 1000 requests and invalid range guards\n");
        auto worker = workerRequest({{"access", url.fragment()}, {"action", "GetState"}, {"session", session}});
        while (worker.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) wait(2);
        require(worker.get().value("ok").toBool(), "WebSocket worker requests execute safely on the Qt thread");
        require(first.value("ok").toBool() && session != session2, "server issues separate device sessions");
        require(request("GetState", {{"session", "invalid"}}).value("code") == "session", "unknown sessions fail");
        OBSSceneAutoRelease a = obs_scene_create("Remote A"), b = obs_scene_create("Remote B");
        auto *source = obs_scene_get_source(a);
        const auto id = QString::fromUtf8(obs_source_get_uuid(source));
        const auto bid = QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(b)));
        frontendScenes = {obs_scene_get_source(b), source};
        auto *group = obs_scene_add_group(a, "Remote group");
        require(group != nullptr, "group source fixture is created");
        const auto listed = request("GetState", {{"session", session}}).value("scenes").toArray();
        QSet<QString> listedIds;
        for (const auto &entry : listed) listedIds.insert(entry.toObject().value("id").toString());
        require(listed.size() == 2 && listedIds == QSet<QString>{id, bid}, "scene dropdown excludes group sources");
        require(listed[0].toObject().value("id") == bid && listed[1].toObject().value("id") == id,
            "scene dropdown follows frontend order instead of creation order");
        frontendScenes = {source, obs_scene_get_source(b)};
        const auto reordered = request("GetState", {{"session", session}}).value("scenes").toArray();
        require(reordered[0].toObject().value("id") == id && reordered[1].toObject().value("id") == bid,
            "scene dropdown reflects frontend reordering");
        const auto groupId = QString::fromUtf8(obs_source_get_uuid(obs_sceneitem_get_source(group)));
        require(request("GetDocument", {{"session", session}, {"scene", groupId}}).value("code") == "scene",
            "group sources cannot be opened as script scenes");
        obs_sceneitem_remove(group);
        notes.write(source, {QString::fromUtf8("<p><b>Original 😀</b></p>")});
        notes.write(obs_scene_get_source(b), {"<p>Other scene</p>"});
        dock.bindScene(source);
        dock.resize(500, 400); dock.show(); wait(20);
        auto *editor = dock.findChild<QTextEdit *>("noteEditor");
        const QRect textArea(editor->viewport()->mapTo(&dock, QPoint()), editor->viewport()->size());
        require(!textArea.intersects(qrButton->geometry()) && dock.rect().contains(qrButton->geometry()),
            "QR button has its own space outside the text viewport");
        qrButton->click();
        auto *popup = dock.findChild<QLabel *>("remoteQrOverlay");
        require(popup && popup->isVisible() && !popup->pixmap().isNull(), "QR opens without a nested event loop");
        require(!popup->isWindow() && !QApplication::activePopupWidget() &&
            !QWidget::mouseGrabber() && !QWidget::keyboardGrabber(), "QR never grabs input or opens a native popup");
        const QRect visibleArea(editor->viewport()->mapToGlobal(QPoint()), editor->viewport()->size());
        require(popup->width() == popup->height() && visibleArea.contains(QRect(popup->mapToGlobal(QPoint()), popup->size())),
            "square QR popup fits inside the document viewport");
        const auto cachedImage = popup->pixmap().cacheKey();
        qrButton->click();
        require(!popup->isVisible(), "clicking the QR button again hides the popup");
        qrButton->click();
        require(popup == dock.findChild<QLabel *>("remoteQrOverlay") && popup->pixmap().cacheKey() == cachedImage,
            "reopening reuses the popup and its image");
        dock.resize(500, 140); wait(20);
        require(!popup->isVisible(), "resizing closes a popup sized for the previous viewport");
        qrButton->click();
        const QRect shortArea(editor->viewport()->mapToGlobal(QPoint()), editor->viewport()->size());
        require(popup->isVisible() && popup->width() == popup->height() &&
            shortArea.contains(QRect(popup->mapToGlobal(QPoint()), popup->size())),
            "QR popup stays within a short document viewport");
        remote->hideQr();
        QElapsedTimer resizeClock; resizeClock.start();
        for (int i=0; i<100; ++i) {
            dock.resize(280+i%7*57, 140+i%9*33); wait(2);
            qrButton->click();
            const QRect area(editor->viewport()->mapToGlobal(QPoint()),editor->viewport()->size());
            require(popup->parentWidget()==editor && popup->isVisible() && popup->width()==popup->height() && popup->pixmap().size()==popup->size() && area.contains(QRect(popup->mapToGlobal(QPoint()),popup->size())),"square QR remains fully inside the document at changing sizes");
            remote->hideQr();
        }
        require(resizeClock.elapsed()<10000,"QR resize/open pressure remains bounded");
        std::printf("PASS QR BOUNDS: 100 resize/open cycles, square image inside document, elapsed=%lldms\n",resizeClock.elapsed());
        dock.resize(500, 400); wait(20);
        qrButton->click(); require(dock.grab().save("preview-qr-bounds.png"),"save in-document QR evidence"); remote->hideQr();
        auto document = request("GetDocument", {{"session", session}, {"scene", id}});
        require(request("ExportHtml", {{"session", session}, {"scene", id}}).value("html").toString() == notes.read(source).html,
            "HTML exports preserve the complete saved document");
        require(request("ExportText", {{"session", session}, {"scene", id}}).value("text").toString() == QString::fromUtf8("Original 😀"),
            "text export preserves Unicode and removes markup");
        require(scenepad::plainText(QString::fromUtf8("<p><b>剧本 😀</b><br>Next</p><p>End<img src='missing'></p>")) ==
            QString::fromUtf8("剧本 😀\nNext\nEnd"), "plain export retains paragraphs and soft breaks without image placeholders");
        require(request("ExportHtml", {{"session", "invalid"}, {"scene", id}}).value("code") == "session", "HTML exports require an authenticated device session");
        require(request("ExportText", {{"session", "invalid"}, {"scene", id}}).value("code") == "session", "text exports share the authenticated session check");
        auto editInput = QJsonObject{{"session", session}, {"scene", id}, {"epoch", document.value("epoch")}, {"revision", document.value("revision")}};
        require(document.value("editable").toBool() && document.value("blocks").toArray().size() == 1, "simple rich text is editable");
        require(request("AcquireEdit", editInput).value("ok").toBool(), "one device acquires the editing lease");
        auto *lock = dock.findChild<QToolButton *>("readMode");
        require(editor->isReadOnly(), "desktop editor locks while a remote device edits");
        require(lock->isChecked(), "remote editing shows the closed lock icon");
        dock.flush(false);
        require(!dock.isLocked(), "a remote editing lease does not change the shared lock");
        auto competitor = editInput; competitor.insert("session", session2);
        require(request("AcquireEdit", competitor).value("code") == "busy", "another device cannot overwrite the lease");
        auto saveInput = editInput; saveInput.insert("document", changed(document, QString::fromUtf8("Updated 🌍")));
        auto saved = request("SaveDocument", saveInput);
        require(saved.value("ok").toBool() && saved.value("revision").toInt() > document.value("revision").toInt(), "save advances the revision");
        require(editor->toPlainText() == QString::fromUtf8("Updated 🌍") && editor->document()->find("Updated").charFormat().fontWeight() == QFont::Bold,
            "remote save updates the dock and preserves rich text and surrogate pairs");
        require(request("SaveDocument", saveInput).value("code") == "conflict", "stale saves cannot overwrite newer text");
        const auto beforeInvalid = notes.read(source).html;
        saveInput.insert("revision", saved.value("revision"));
        saveInput.insert("document", changed(document, QString(10001, 'x')));
        require(request("SaveDocument", saveInput).value("code") == "limit" && notes.read(source).html == beforeInvalid, "oversized input leaves notes unchanged");
        request("ReleaseEdit", {{"session", session}});
        require(!editor->isReadOnly() && !lock->isChecked(), "lease release restores the open lock icon and local editing");
        auto other = request("GetDocument", {{"session", session}, {"scene", bid}});
        QJsonObject otherEdit{{"session", session}, {"scene", bid}, {"epoch", other.value("epoch")}, {"revision", other.value("revision")}};
        request("AcquireEdit", otherEdit); otherEdit.insert("document", changed(other, "Background edit"));
        require(request("SaveDocument", otherEdit).value("ok").toBool() && dock.currentSceneId() == id && editor->toPlainText() == QString::fromUtf8("Updated 🌍"),
            "editing another scene never switches the dock or OBS output");
        require(!dock.isLocked(), "remote saves leave the shared lock unchanged");
        dock.bindScene(obs_scene_get_source(b)); request("ReleaseEdit", {{"session", session}});
        require(!editor->isReadOnly(), "releasing a lease restores editing after scene changes");
        dock.bindScene(source);
        const auto previous = request("GetDocument", {{"session", session}, {"scene", id}}).value("revision").toInt();
        editor->insertPlainText("Local edit");
        require(request("GetDocument", {{"session", session}, {"scene", id}}).value("revision").toInt() > previous, "unsaved desktop edits invalidate remote versions immediately");
        const auto localDocument = request("GetDocument", {{"session", session}, {"scene", id}});
        const auto localAttempt = QJsonObject{{"session", session}, {"scene", id}, {"epoch", localDocument.value("epoch")}, {"revision", localDocument.value("revision")}};
        require(request("GetState", {{"session", session}}).value("editor") == "OBS" &&
            request("AcquireEdit", localAttempt).value("code") == "busy", "desktop typing holds an editing turn");
        wait(2100);
        require(request("GetState", {{"session", session}}).value("editor").toString().isEmpty(), "desktop editing turn expires after typing stops");
        QFile fixture(app.arguments().value(1));
        require(fixture.open(QIODevice::ReadOnly), "Word upload fixture opens");
        const auto word = fixture.readAll();
        require(scenepad::importDocx(QFileInfo(fixture.fileName()).dir().filePath("long-line.docx")).error == "TextLimit",
            "Word import stops at the paragraph limit before loading the text");
        auto uploadInput = QJsonObject{{"session", session}, {"scene", bid}, {"epoch", document.value("epoch")}};
        auto uploadDoc = request("GetDocument", {{"session", session}, {"scene", bid}});
        uploadInput.insert("epoch", uploadDoc.value("epoch")); uploadInput.insert("revision", uploadDoc.value("revision"));
        auto headers = [&](QJsonObject input, qint64 length, QByteArray extra = {}) {
            return "POST /import.docx HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/vnd.openxmlformats-officedocument.wordprocessingml.document\r\nContent-Length: " + QByteArray::number(length) +
                "\r\nX-ScenePad-Access: " + input.value("access").toString(url.fragment()).toUtf8() + "\r\nX-ScenePad-Session: " + input.value("session").toString().toUtf8() +
                "\r\nX-ScenePad-Scene: " + input.value("scene").toString().toUtf8() + "\r\nX-ScenePad-Epoch: " + input.value("epoch").toString().toUtf8() +
                "\r\nX-ScenePad-Revision: " + QByteArray::number(input.value("revision").toInt()) + "\r\n" + extra + "\r\n";
        };
        const auto originalNote = notes.read(obs_scene_get_source(b)).html;
        const auto uploadFiles = [] { return QDir(QDir::tempPath()).entryList({"scenepad-upload-*"}, QDir::Dirs | QDir::NoDotAndDotDot); };
        const auto filesBefore = uploadFiles();
        require(body(http(port, headers(uploadInput, word.size()) + word)).value("code") == "lease", "Word uploads require an editing lease");
        require(request("AcquireEdit", uploadInput).value("ok").toBool(), "Word upload acquires the existing single-editor lease");
        auto unauthenticated = uploadInput; unauthenticated.insert("access", "wrong");
        require(body(http(port, headers(unauthenticated, word.size()) + word)).value("code") == "access", "Word uploads reject invalid capabilities");
        auto unknownSession = uploadInput; unknownSession.insert("session", "invalid");
        require(body(http(port, headers(unknownSession, word.size()) + word)).value("code") == "session", "Word uploads require a server-issued session");
        require(body(http(port, headers(uploadInput, 5 * scenepad::maxRemoteText + 1))).value("code") == "size", "oversized uploads are rejected from their headers");
        require(body(http(port, headers(uploadInput, word.size(), "Content-Length: 1\r\n") + word)).value("code") == "format", "ambiguous duplicate lengths are rejected");
        require(body(http(port, headers(uploadInput, word.size(), "Transfer-Encoding: chunked\r\n") + word)).value("code") == "format", "chunked uploads are not accepted");
        require(body(http(port, headers(uploadInput, word.size(), "Origin: http://untrusted.example\r\n") + word)).value("code") == "format", "cross-origin uploads are rejected");
        require(body(http(port, headers(uploadInput, 6) + "broken")).value("code") == "word", "invalid Word data is reported without replacing notes");
        require(notes.read(obs_scene_get_source(b)).html == originalNote && uploadFiles() == filesBefore, "rejected uploads preserve the note and leave no temporary files");
        QTcpSocket pending; pending.connectToHost(QHostAddress::LocalHost, port); require(pending.waitForConnected(1000), "streaming upload connects");
        pending.write(headers(uploadInput, word.size()) + word.left(10)); pending.flush();
        require(waitUntil([&] { return uploadFiles().size() == filesBefore.size() + 1; }), "partial authenticated uploads use one disposable file");
        require(body(http(port, headers(uploadInput, word.size()) + word)).value("code") == "busy", "only one Word upload can run at a time");
        remote->noteChanged(bid);
        pending.write(word.mid(10)); pending.flush();
        require(waitUntil([&] { return pending.state() == QAbstractSocket::UnconnectedState; }), "completed upload receives its response");
        require(body(pending.readAll()).value("code") == "conflict" && notes.read(obs_scene_get_source(b)).html == originalNote,
            "a revision change during upload prevents replacement");
        uploadDoc = request("GetDocument", {{"session", session}, {"scene", bid}}); uploadInput.insert("revision", uploadDoc.value("revision"));
        QTcpSocket interrupted; interrupted.connectToHost(QHostAddress::LocalHost, port); require(interrupted.waitForConnected(1000), "interrupted upload connects");
        interrupted.write(headers(uploadInput, word.size()) + word.left(10)); interrupted.flush();
        require(waitUntil([&] { return uploadFiles().size() == filesBefore.size() + 1; }), "interrupted upload starts");
        interrupted.abort();
        require(waitUntil([&] { return uploadFiles() == filesBefore; }) && notes.read(obs_scene_get_source(b)).html == originalNote, "disconnecting removes the upload without changing the note");
        const auto imported = body(http(port, headers(uploadInput, word.size()) + word));
        if (!imported.value("ok").toBool()) std::fprintf(stderr, "Upload failure code: %s\n", imported.value("code").toString().toUtf8().constData());
        require(imported.value("ok").toBool() && imported.value("revision").toInt() > uploadDoc.value("revision").toInt(), "Word upload commits once and returns its new document revision");
        QTextDocument importedText; importedText.setHtml(notes.read(obs_scene_get_source(b)).html);
        require(importedText.toPlainText().contains("Word import test") && importedText.find("Word import test").charFormat().fontPointSize() == 22,
            "Word uploads reuse the native rich text parser");
        require(dock.currentSceneId() == id && !dock.isLocked() && uploadFiles() == filesBefore,
            "Word import preserves scene selection and the shared lock and removes its file");
        require(body(http(port, headers(uploadInput, word.size()) + word)).value("code") == "conflict", "repeated uploads with stale revisions cannot overwrite notes");
        uploadInput.insert("revision", imported.value("revision"));
        QFile largeFixture(QFileInfo(fixture.fileName()).dir().filePath("long.docx"));
        require(largeFixture.open(QIODevice::ReadOnly), "larger Word fixture opens"); const auto largeWord = largeFixture.readAll();
        const auto largeImport = body(http(port, headers(uploadInput, largeWord.size()) + largeWord));
        require(largeWord.size() > 8192 && largeImport.value("ok").toBool() && largeImport.value("blocks").toArray().size() > 1000,
            "Word bodies larger than the header buffer stream and parse successfully");
        request("ReleaseEdit", {{"session", session}});
        notes.write(obs_scene_get_source(b), {"<table><tr><td>Complex</td></tr></table>"}); remote->noteChanged(bid);
        uploadDoc = request("GetDocument", {{"session", session}, {"scene", bid}}); uploadInput.insert("revision", uploadDoc.value("revision"));
        require(request("AcquireEdit", uploadInput).value("code") == "format", "normal editing does not simplify complex notes");
        uploadInput.insert("replace", true);
        require(request("AcquireEdit", uploadInput).value("ok").toBool(), "explicit imports can replace complex notes");
        uploadInput.insert("document", changed(document, "Imported text"));
        require(request("ReplaceDocument", uploadInput).value("ok").toBool(), "HTML and text imports use the same validated replacement path");
        request("ReleaseEdit", {{"session", session}});
        auto takeoverDoc = request("GetDocument", {{"session", session}, {"scene", id}});
        auto takeoverInput = QJsonObject{{"session", session}, {"scene", id}, {"epoch", takeoverDoc.value("epoch")}, {"revision", takeoverDoc.value("revision")}};
        require(request("AcquireEdit", takeoverInput).value("ok").toBool(), "web editing starts while the shared lock is open");
        QTcpSocket takenUpload; takenUpload.connectToHost(QHostAddress::LocalHost, port); require(takenUpload.waitForConnected(1000), "takeover upload connects");
        takenUpload.write(headers(takeoverInput, word.size()) + word.left(10)); takenUpload.flush();
        require(waitUntil([&] { return uploadFiles() != filesBefore; }), "partial upload starts before locking");
        dock.command(scenepad::Command::ToggleLock);
        require(waitUntil([&] { return takenUpload.state() == QAbstractSocket::UnconnectedState && uploadFiles() == filesBefore; }), "local reclaim aborts and removes an in-progress upload");
        require(!editor->isReadOnly() && !dock.isLocked() && !lock->isChecked(), "one unlock click reclaims local editing from the browser");
        require(request("GetState", {{"session", session2}}).value("localEditing").toBool() &&
            !request("KeepAlive", {{"session", session}}).value("editing").toBool() &&
            request("SaveDocument", takeoverInput).value("code") == "busy", "reclaimed leases cannot save or immediately reacquire");
        dock.command(scenepad::Command::ToggleLock);
        require(editor->isReadOnly() && dock.isLocked(), "the desktop button locks editing on all devices");
        require(request("GetState", {{"session", session2}}).value("locked").toBool() && request("GetDocument", {{"session", session2}, {"scene", id}}).value("ok").toBool(),
            "the shared lock keeps reader sessions connected");
        require(request("AcquireEdit", takeoverInput).value("code") == "locked" && request("SaveDocument", takeoverInput).value("code") == "locked" &&
            request("CheckImport", takeoverInput).value("code") == "locked", "the shared lock blocks new leases, late saves and imports");
        dock.command(scenepad::Command::ToggleLock);
        require(!editor->isReadOnly() && !dock.isLocked(), "unlocking restores local editing");
        require(request("AcquireEdit", takeoverInput).value("ok").toBool(), "unlocking allows a webpage to edit again");
        request("ReleaseEdit", {{"session", session}});
        remote->resetCollection();
        require(request("AcquireEdit", editInput).value("code") == "conflict", "collection changes invalidate old revisions");
        remote->setCollectionChanging(true);
        require(request("GetDocument", {{"session", session}, {"scene", id}}).value("code") == "loading", "requests pause while OBS changes collections");
        remote->setCollectionChanging(false);
        auto current = request("GetDocument", {{"session", session}, {"scene", id}});
        editInput.insert("epoch", current.value("epoch")); editInput.insert("revision", current.value("revision"));
        request("AcquireEdit", editInput); wait(16100);
        require(!editor->isReadOnly(), "an abandoned editing lease expires without another request");
        auto delayed = editInput; delayed.insert("action", "AcquireEdit"); delayed.insert("access", url.fragment());
        auto timeout = workerRequest(delayed);
        const auto timeoutResult = timeout.get(); wait(10);
        require(timeoutResult.value("code") == "timeout" && !editor->isReadOnly(), "timed-out queued writes never run later");
        const auto page = http(port, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
        require(page.startsWith("HTTP/1.1 200") && page.contains("contenteditable") && page.contains("frame-ancestors 'none'"), "embedded page and security headers are served");
        const auto configuration = http(port, "GET /config.json HTTP/1.1\r\nHost: localhost\r\n\r\n");
        require(body(configuration).value("labels").toObject().contains("RemoteDevice") &&
            !configuration.contains(url.fragment().toUtf8()) && !configuration.contains("Updated"), "public configuration contains no capability or notes");
        require(body(configuration).value("port").toInt() == 4455, "OBS default WebSocket port is used without configuration");
        QFile websocketConfig(temp.filePath("obs-websocket/config.json"));
        require(websocketConfig.open(QIODevice::WriteOnly), "isolated OBS settings fixture opens");
        websocketConfig.write("{\"server_port\":4567,\"server_password\":\"fixture-only\"}"); websocketConfig.close();
        const auto customConfig = http(port, "GET /config.json HTTP/1.1\r\nHost: localhost\r\n\r\n");
        require(body(customConfig).value("port").toInt() == 4567 && !customConfig.contains("fixture-only") &&
            http(port, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n").contains("ws://127.0.0.1:4567"),
            "browser connection and security policy follow the OBS port without disclosing its password");
        require(websocketConfig.open(QIODevice::WriteOnly | QIODevice::Truncate), "OBS settings fixture can change");
        websocketConfig.write("{\"server_port\":4678}"); websocketConfig.close();
        require(body(http(port, "GET /config.json HTTP/1.1\r\nHost: localhost\r\n\r\n")).value("port").toInt() == 4678,
            "refreshing the page follows saved OBS port changes without restarting ScenePad");
        require(http(port, "GET /../../src/plugin.cpp HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 404"), "HTTP cannot traverse into files");
        require(http(port, "POST / HTTP/1.1\r\n\r\n").startsWith("HTTP/1.1 405"), "HTTP rejects unknown POST endpoints");
        QTcpSocket partial; partial.connectToHost(QHostAddress::LocalHost, port); partial.waitForConnected(1000); partial.write("GET / HTTP/1.1\r\n");partial.flush();wait(3300);
        require(partial.state() == QAbstractSocket::UnconnectedState, "partial requests time out");
        for (int i = 0; i < 14; ++i) require(request("Open").value("ok").toBool(), "bounded reader sessions are accepted");
        require(request("Open").value("code") == "capacity", "session limit bounds memory use");
        const int widthWithQr = editor->viewport()->width();
        remote->stop(); wait(20);
        require(!editor->isReadOnly() && request("GetState").value("code") == "disabled", "disabling remote access closes sessions and releases editing");
        require(editor->viewport()->width() == widthWithQr, "disabling LAN access preserves the fixed side space");

        require(qrButton->isHidden(), "stopping LAN access hides the QR entry");
        QSettings settings(settingsPath, QSettings::IniFormat);
        settings.setValue("remote/address", "127.0.0.1"); settings.setValue("remote/port", port); settings.sync();
        std::exception_ptr dialogFailure;
        QString retainedSession;
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            try {
                require(dialog && dialog->objectName() == "remoteSettings", "remote settings opens");
                require(!dialog->findChild<QSpinBox *>("remoteWebSocketPort"), "WebSocket port is configured only in OBS");
                require(dialog->findChildren<QPushButton *>().size() == 3, "settings has three buttons");
                require(dialog->findChild<QPushButton *>("remoteReset")->geometry().right() <
                    dialog->findChild<QPushButton *>("remoteCopy")->geometry().left(), "reset is separated on the left");
                auto *enabled = dialog->findChild<QCheckBox *>("remoteEnabled"); enabled->setChecked(true);
                require(remote->url().isEmpty() && !settings.value("remote/enabled", false).toBool(), "changes wait for Apply");
                auto *apply = dialog->findChild<QPushButton *>("remoteApply"); apply->click();
                require(!remote->url().isEmpty() && settings.value("remote/enabled").toBool(),
                    "Apply activates and saves the settings");
                auto *qr = dialog->findChild<QLabel *>("remoteQR");
                require(!qr->isHidden() && !qr->pixmap().isNull(), "QR is shown directly without a toggle");
                wait(20);
                auto *address = dialog->findChild<QComboBox *>("remoteAddress");
                if (qr->height() != qr->width() || qr->width() != address->width() || qr->pixmap().size() != qr->size())
                    std::fprintf(stderr, "QR=%dx%d image=%dx%d address=%dx%d dialog=%dx%d\n", qr->width(), qr->height(),
                        qr->pixmap().width(), qr->pixmap().height(), address->width(), address->height(), dialog->width(), dialog->height());
                require(qr->height() == qr->width(), "QR stays square");
                require(qr->width() == address->width(), "QR matches the address field width");
                require(qr->pixmap().size() == qr->size(), "QR image is not clipped");
                dialog->resize(dialog->width() + 60, dialog->height() + 60); wait(20);
                require(qr->height() == qr->width() && qr->width() == address->width(), "QR stays square after resizing");
                const int buttonY = apply->y();
                dialog->findChild<QSpinBox *>("remotePort")->stepUp(); wait(20);
                auto *status = dialog->findChild<QLabel *>("remoteStatus");
                require(!status->text().isEmpty() && status->alignment() == Qt::AlignCenter && apply->y() == buttonY,
                    "centered status messages use a reserved row without moving buttons");
                dialog->findChild<QSpinBox *>("remotePort")->stepDown();
                retainedSession = request("Open").value("session").toString();
                require(!retainedSession.isEmpty(), "applied settings allow new connections");
                apply->click(); dialog->reject();
            } catch (...) { dialogFailure = std::current_exception(); if (dialog) dialog->reject(); }
        });
        remote->showSettings(); if (dialogFailure) std::rethrow_exception(dialogFailure);
        require(request("GetState", {{"session", retainedSession}}).value("ok").toBool(), "unchanged Apply preserves remote sessions");
        QTimer::singleShot(0, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            try {
                require(dialog, "settings reopens");
                const auto previousUrl = remote->url();
                dialog->findChild<QPushButton *>("remoteReset")->click();
                require(remote->url() == previousUrl && request("GetState", {{"session", retainedSession}}).value("ok").toBool(),
                    "link resets wait for Apply");
                auto *apply = dialog->findChild<QPushButton *>("remoteApply"); apply->click();
                require(remote->url() != previousUrl && request("Open").value("code") == "access", "applied reset revokes old links");
                dialog->findChild<QCheckBox *>("remoteEnabled")->setChecked(false);
                require(!remote->url().isEmpty() && settings.value("remote/enabled").toBool(), "disabling also waits for Apply");
                apply->click(); dialog->reject();
            } catch (...) { dialogFailure = std::current_exception(); if (dialog) dialog->reject(); }
        });
        remote->showSettings(); if (dialogFailure) std::rethrow_exception(dialogFailure);
        require(remote->url().isEmpty() && !settings.value("remote/enabled").toBool(), "Apply disables remote access");

        const QString rich = "<p style='text-align:right;margin-top:17px'><span style='font-size:20pt;color:#cc2222'>Hello</span></p><ol><li>One</li><li>Two</li></ol>";
        auto web = scenepad::webDocument(rich); QString output;
        require(scenepad::applyWebDocument(rich, web, output) && output == rich, "unchanged documents preserve their original HTML exactly");
        require(scenepad::applyWebDocument(rich, changed(web, "Changed"), output), "paragraphs and basic lists round trip");
        QTextDocument roundTrip; roundTrip.setHtml(output);
        require(roundTrip.toPlainText().contains("Changed") && roundTrip.find("Changed").charFormat().fontPointSize() == 20 &&
            roundTrip.find("Changed").blockFormat().topMargin() == 17, "browser edits retain Qt font sizes and untouched paragraph layout");
        auto listBlocks = web.value("blocks").toArray();
        for (int i = 1; i < listBlocks.size(); ++i) {
            auto block = listBlocks[i].toObject(); block.insert("listGroup", 99); block.insert("listStart", 3); listBlocks[i] = block;
        }
        auto restartedList = web; restartedList.insert("blocks", listBlocks);
        require(scenepad::applyWebDocument(rich, restartedList, output), "browser list tools can start a new numbered list");
        roundTrip.setHtml(output);
        require(roundTrip.find("One").block().textList()->format().start() == 3, "imported list starts survive native storage");
        require(!scenepad::webDocument("<table><tr><td>Cell</td></tr></table>").value("editable").toBool(), "tables are read only in browsers");
        require(!scenepad::webDocument("<p><a href='https://example.com'>Link</a></p>").value("editable").toBool(), "unsupported links remain read only");
        QTextDocument special;
        special.setHtml("<p><b>Bold</b><a href='https://example.com'>Link</a><sup>Raised</sup></p>"
            "<table><tr><td>Cell one</td><td>Cell two</td></tr></table>"
            "<p><span style='font-size:24px;color:#cc2222'>Pixels</span><img src='missing'></p>");
        const auto specialHtml = special.toHtml();
        require(scenepad::basicHtml(special, output) && special.toHtml() == specialHtml,
            "basic conversion leaves the source document unchanged");
        roundTrip.setHtml(output);
        require(scenepad::webDocument(output).value("editable").toBool() &&
            roundTrip.rootFrame()->childFrames().isEmpty() && roundTrip.toPlainText().contains("Cell one\nCell two") &&
            !roundTrip.toPlainText().contains(QChar::ObjectReplacementCharacter) &&
            roundTrip.find("Bold").charFormat().fontWeight() == QFont::Bold &&
            !roundTrip.find("Link").charFormat().isAnchor() &&
            roundTrip.find("Raised").charFormat().verticalAlignment() == QTextCharFormat::AlignNormal &&
            roundTrip.find("Pixels").charFormat().fontPointSize() == 18,
            "basic conversion flattens tables and removes special layout while retaining common text formatting");
        for (const bool fragmented : {false, true}) {
            QString html = "<p>";
            if (fragmented) {
                for (int i = 0; i < 3000; ++i)
                    html += QString("<span style='font-size:%1pt;font-weight:%2'>%3</span>")
                        .arg(10 + i % 3 * 4).arg(i % 2 ? 700 : 400).arg("abc");
            } else html += QString(9999, 'W');
            html += "</p>";
            QElapsedTimer timing; timing.start();
            const auto data = scenepad::webDocument(html); const auto exportTime = timing.elapsed();
            require(data.value("editable").toBool(), "long-paragraph remote document remains editable");
            const auto runText = data.value("blocks").toArray().first().toObject().value("runs").toArray().first().toObject().value("text").toString();
            timing.restart();
            require(scenepad::applyWebDocument(html, changed(data, "x" + runText), output), "long-paragraph remote save succeeds");
            const auto saveTime = timing.elapsed();
            std::printf("PASS: remote %s export %lld ms, save %lld ms\n", fragmented ? "3000-format paragraph" : "9999-character paragraph", exportTime, saveTime);
            std::fflush(stdout);
            require(exportTime < 5000 && saveTime < 5000, "long-paragraph remote round trip exceeds 5 seconds");
            QTextDocument savedText; savedText.setHtml(output);
            require(savedText.toPlainText().startsWith("x" + runText) && savedText.characterCount() == (fragmented ? 9002 : 10001),
                "long-paragraph remote save preserves every character");
        }
        QString longHtml = "<p>" + QString("Paragraph</p><p>").repeated(9999) + "End</p>";
        QElapsedTimer performance; performance.start();
        const auto longDocument = scenepad::webDocument(longHtml);
        require(longDocument.value("blocks").toArray().size() == 10000 && performance.elapsed() < 5000, "large document export remains bounded");
        std::printf("PASS: 10000-paragraph export %lld ms\n", performance.elapsed());
        performance.restart();
        require(scenepad::applyWebDocument(longHtml, changed(longDocument, "Remote change"), output) && performance.elapsed() < 5000,
            "large document updates preserve a bounded save time");
        std::printf("PASS: 10000-paragraph save %lld ms\n", performance.elapsed());
        require(remote->listen("127.0.0.1", 0), "scroll synchronization starts on loopback");
        const auto scrollAccess = QUrl(remote->url()).fragment();
        const auto reader = remote->handle({{"action", "Open"}, {"access", scrollAccess}}).value("session").toString();
        auto scrollRequest = [&](const char *action, QJsonObject data = {}) {
            data.insert("action", action); data.insert("access", scrollAccess); data.insert("session", reader);
            return remote->handle(data);
        };
        notes.write(source, {longHtml});
        dock.bindScene(obs_scene_get_source(b)); dock.bindScene(source);
        auto *scroll = editor->verticalScrollBar(); scroll->setValue(scroll->minimum()); wait(250);
        const auto originalText = editor->toPlainText();
        QElapsedTimer textClock; textClock.start();
        for (int i = 0; i < 5; ++i)
            require(scrollRequest("ExportText", {{"scene", id}}).value("text").toString() == originalText, "large text exports retain all 10000 paragraphs");
        std::printf("TXT remote: 5 exports over 10000 paragraphs in %lld ms\n", textClock.elapsed());
        require(textClock.elapsed() < 5000, "large text exports remain bounded");
        require(scroll->maximum() > 0 && scrollRequest("GetState").value("position").toDouble(-1) == 0,
            "state includes the initial scroll position");
        scroll->setValue(scroll->maximum() / 2);
        const double middle = scrollRequest("GetState").value("position").toDouble(-1);
        require(middle > .49 && middle < .51, "state reports progress independently of document height");
        // Let the desktop editing turn expire before isolating scroll events.
        wait(3500);
        published.clear();
        for (int i = 0; i <= 100; ++i) scroll->setValue(static_cast<int>(static_cast<qint64>(scroll->maximum()) * i / 100));
        wait(250);
        require(published.size() == 1 && published[0].first == "Scroll" &&
            published[0].second.value("position").toDouble() == 1 && published[0].second.value("scene") == id &&
            !published[0].second.contains("scenes") && !published[0].second.contains("changed"),
            "scroll bursts coalesce into one compact event with the final position");
        wait(250);
        require(published.size() == 1, "idle scrolling does not produce recurring events");
        require(scrollRequest("Scroll", {{"command", "top"}}).value("position").toDouble(-1) == 0,
            "return-to-top replies include the final position immediately");
        wait(250);
        require(published.last().second.value("position").toDouble(-1) == 0,
            "return-to-top also notifies other devices");
        require(scrollRequest("Scroll", {{"command", "seek"}, {"scene", bid}, {"target", "50%"}}).value("code") == "scene",
            "remote seeking rejects a different scene");
        require(scrollRequest("Scroll", {{"command", "seek"}, {"scene", id}, {"target", "10001"}}).value("code") == "range",
            "remote seeking rejects invalid line numbers");
        require(scrollRequest("Scroll", {{"command", "seek"}, {"scene", id}, {"target", "5000"}}).value("ok").toBool() && editor->textCursor().blockNumber() == 4999,
            "remote seeking locates a logical paragraph");
        const auto sought = scrollRequest("Scroll", {{"command", "seek"}, {"scene", id}, {"target", "50%"}});
        require(sought.value("ok").toBool() && qAbs(sought.value("position").toDouble() - .5) < .01,
            "remote percentage seeking immediately returns the final position");
        QElapsedTimer popupClock; popupClock.start();
        qrButton->click();
        const auto largeImage = popup->pixmap().cacheKey();
        const auto qrPosition = popup->pos();
        scroll->setValue(scroll->maximum() / 2); wait(20);
        require(popup->isVisible() && popup->pos() == qrPosition, "scrolling text does not move the QR overlay");
        remote->hideQr();
        for (int i = 0; i < 100; ++i) {
            qrButton->click();
            require(popup->isVisible() && popup->pixmap().cacheKey() == largeImage, "rapid QR opens reuse the cached image");
            qrButton->click();
            require(!popup->isVisible(), "rapid QR closes leave no active popup");
            if (i % 10 == 0) QApplication::processEvents();
        }
        require(dock.findChildren<QLabel *>("remoteQrOverlay").size() == 1, "rapid QR toggles never accumulate popup objects");
        require(popupClock.elapsed() < 10000, "QR toggling remains bounded with 10000 paragraphs");
        std::printf("PASS: 100 QR open/close cycles over 10000 paragraphs in %lld ms\n", popupClock.elapsed());
        const auto appStyle = app.styleSheet();
        app.setStyleSheet("QWidget { font-size: 14px; }");
        const auto longParagraph = QString(10000, 'W');
        editor->setPlainText(longParagraph); dock.flush(false); wait(30);
        StyleEvents styleEvents; editor->installEventFilter(&styleEvents);
        editor->setFocus(); wait(20);
        auto *focused = QApplication::focusWidget();
        int heartbeats = 0;
        QTimer heartbeat; heartbeat.setInterval(1);
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
        heartbeat.start();
        popupClock.restart();
        qint64 cyclePeak = 0;
        for (int i = 0; i < 100; ++i) {
            QElapsedTimer cycle; cycle.start();
            qrButton->click(); wait(2);
            require(!QApplication::activePopupWidget() && !QWidget::mouseGrabber() &&
                !QWidget::keyboardGrabber() && QApplication::focusWidget() == focused,
                "visible QR leaves input and editor focus available");
            QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(editor, &escape);
            require(!popup->isVisible(), "Escape dismisses QR through the editor");
            QKeyEvent blocked(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
            QApplication::sendEvent(editor, &blocked);
            cyclePeak = qMax(cyclePeak, cycle.elapsed());
        }
        heartbeat.stop();
        require(heartbeats > 0, "event loop keeps processing timers while QR is visible");
        require(styleEvents.changes == 0, "focus and QR toggles never reset the editor style or font");
        require(editor->document()->blockCount() == 1 && editor->toPlainText() == longParagraph,
            "long-paragraph focus changes preserve the document");
        std::printf("QR: 100 cycles over %lld characters in %lld ms; peak %lld ms; no input grabs or style/font resets\n",
            static_cast<long long>(longParagraph.size()), popupClock.elapsed(), cyclePeak); std::fflush(stdout);
        require(popupClock.elapsed() < 10000, "long-paragraph QR toggling remains bounded");
        notes.write(source, {"<p>" + QString(1000000, 'W') + "</p>"});
        dock.bindScene(obs_scene_get_source(b)); dock.bindScene(source);
        require(editor->isReadOnly() && editor->toPlainText().isEmpty(), "oversized stored notes are not laid out or overwritten");
        QElapsedTimer rejection; rejection.start();
        for (int i = 0; i < 100; ++i)
            require(scrollRequest("GetDocument", {{"scene", dock.currentSceneId()}}).value("code") == "read",
                "oversized active documents are rejected without HTML conversion");
        require(rejection.elapsed() < 1000, "oversized reader requests must not repeatedly parse long text");
        std::printf("PASS: 100 oversized active-document checks in %lld ms\n", rejection.elapsed());
        editor->removeEventFilter(&styleEvents); app.setStyleSheet(appStyle);
        OBSSceneAutoRelease snapshotScene = obs_scene_create("Pending reader snapshot");
        auto *pendingSource = obs_scene_get_source(snapshotScene);
        const auto pendingId = QString::fromUtf8(obs_source_get_uuid(pendingSource));
        frontendScenes.append(pendingSource);
        dock.bindScene(pendingSource);
        editor->setPlainText("Pending live note");
        for (int i = 0; i < 10; ++i) {
            const auto snapshot = scrollRequest("GetDocument", {{"scene", pendingId}});
            require(snapshot.value("ok").toBool() && QJsonDocument(snapshot).toJson().contains("Pending live note"),
                "remote readers see pending desktop edits");
        }
        require(scrollRequest("ExportHtml", {{"scene", pendingId}}).value("html").toString().contains("Pending live note") &&
            notes.filePath(pendingSource).isEmpty(), "remote reads and exports do not create files or force saves");
        require(scrollRequest("ExportText", {{"scene", pendingId}}).value("text").toString().contains("Pending live note") &&
            notes.filePath(pendingSource).isEmpty(), "text exports read pending edits without forcing a disk or scene save");
        require(dock.flush(false) && notes.read(pendingSource).html.contains("Pending live note"), "pending reader snapshots retain normal autosave durability");
        dock.shutdown();
        require(!registered, "shutdown unregisters the vendor callback");
        std::printf("PASS: %d remote checks\n", checks);
    } catch (const std::exception &failure) { std::fprintf(stderr, "FAIL remote after %d checks: %s\n", checks, failure.what()); result = 1; }
    obs_shutdown(); return result;
}
