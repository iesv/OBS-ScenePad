#include "remote.hpp"
#include "dock.hpp"
#include "docx.hpp"
#include "text-limits.hpp"
#include "locale.hpp"
#include "notes.hpp"
#include "web-document.hpp"
#include "vendor/obs-websocket-api.h"
#include "vendor/qrcodegen.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkInterface>
#include <QPushButton>
#include <QRandomGenerator>
#include <QResizeEvent>
#include <QSemaphore>
#include <QSettings>
#include <QSpinBox>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QVBoxLayout>
#include <memory>
#include <cmath>

// Force the resources into the plugin and test executables from the static library.
static void initializeRemoteResources()
{
    static const bool initialized = [] { Q_INIT_RESOURCE(remote); return true; }();
    Q_UNUSED(initialized);
}

namespace scenepad {
namespace {
QString identifier() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString capability()
{
    QByteArray bytes(16, Qt::Uninitialized);
    auto *random = QRandomGenerator::system();
    for (int i = 0; i < bytes.size(); i += 4) {
        const auto value = random->generate();
        for (int j = 0; j < 4; ++j) bytes[i + j] = static_cast<char>(value >> (j * 8));
    }
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
QJsonObject error(const char *code) { return {{"ok", false}, {"code", code}}; }
OBSSourceAutoRelease sceneSource(const QString &id)
{
    OBSSourceAutoRelease source = obs_get_source_by_uuid(id.toUtf8().constData());
    if (!source || !obs_scene_from_source(source)) return {};
    return source;
}
QJsonArray scenes()
{
    QJsonArray result;
    // Preserve the Scenes dock order supplied by OBS.
    obs_frontend_source_list list{};
    obs_frontend_get_scenes(&list);
    for (size_t i = 0; i < list.sources.num; ++i) {
        auto *source = list.sources.array[i];
        result.append(QJsonObject{{"id", QString::fromUtf8(obs_source_get_uuid(source))},
            {"name", QString::fromUtf8(obs_source_get_name(source))}});
    }
    obs_frontend_source_list_free(&list);
    return result;
}
QPixmap qrPixmap(const QString &url, int maximumSide)
{
    const auto code = qrcodegen::QrCode::encodeText(url.toUtf8().constData(), qrcodegen::QrCode::Ecc::LOW);
    if (maximumSide < code.getSize() + 8) return {};
    const int side = maximumSide;
    const int scale = side / (code.getSize() + 8);
    const int offset = (side - code.getSize() * scale) / 2;
    QImage image(side, side, QImage::Format_RGB32);
    image.fill(Qt::white);
    for (int y = 0; y < code.getSize(); ++y)
        for (int x = 0; x < code.getSize(); ++x)
            if (code.getModule(x, y))
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx) image.setPixel(offset + x * scale + dx, offset + y * scale + dy, qRgb(0, 0, 0));
    return QPixmap::fromImage(image);
}

// Follow the form field width while keeping QR modules on whole pixels.
class QrLabel final : public QLabel {
public:
    explicit QrLabel(QWidget *parent) : QLabel(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(240);
        setAlignment(Qt::AlignCenter);
    }
    QSize sizeHint() const override { return {240, 240}; }
    QSize minimumSizeHint() const override { return {64, 64}; }
    void setLink(const QString &link)
    {
        if (link == link_) return;
        link_ = link;
        updateImage();
    }
protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        if (event->size().width() != event->oldSize().width()) {
            setFixedHeight(event->size().width());
            updateImage();
        }
    }
private:
    void updateImage() { if (link_.isEmpty()) clear(); else setPixmap(qrPixmap(link_, width())); }
    QString link_;
};
}

Remote::Remote(Dock &dock, QSettings &settings) : QObject(&dock), dock_(dock), settings_(settings), epoch_(identifier())
{
    initializeRemoteResources();
    access_ = settings_.value("remote/access").toString();
    if (access_.size() != 22) {
        access_ = capability();
        settings_.setValue("remote/access", access_);
    }
    maintenance_.setInterval(1000);
    connect(&maintenance_, &QTimer::timeout, this, [this] { expire(); });
    events_.setInterval(200);
    events_.setSingleShot(true);
    connect(&events_, &QTimer::timeout, this, [this] { publish(); });
    connect(&server_, &QTcpServer::newConnection, this, [this] { acceptConnections(); });
}

Remote::~Remote() { shutdown(); }

void Remote::initialize()
{
    if (registered_ || stopping_) return;
    vendor_ = obs_websocket_register_vendor("ScenePad");
    registered_ = vendor_ && obs_websocket_vendor_register_request(vendor_, "Request", request, this);
    if (registered_ && settings_.value("remote/enabled", false).toBool())
        listen(settings_.value("remote/address").toString(), static_cast<quint16>(settings_.value("remote/port", 8800).toUInt()));
}

void Remote::shutdown()
{
    if (stopping_.exchange(true)) return;
    stop();
    if (registered_) obs_websocket_vendor_unregister_request(vendor_, "Request");
    registered_ = false;
}

bool Remote::listen(const QString &address, quint16 port)
{
    stop();
    const QHostAddress host(address);
    if (stopping_ || host.protocol() != QAbstractSocket::IPv4Protocol || host == QHostAddress::AnyIPv4 || host.isMulticast()) return false;
    if (!server_.listen(host, port)) return false;
    maintenance_.start();
    dock_.setRemoteAvailable(true);
    return true;
}

void Remote::stop()
{
    hideQr();
    releaseEditor();
    server_.close();
    dock_.setRemoteAvailable(false);
    for (auto *socket : server_.findChildren<QTcpSocket *>()) { socket->abort(); socket->deleteLater(); }
    uploading_.clear();
    localUntil_ = 0;
    maintenance_.stop();
    events_.stop();
    stateDirty_ = false;
    sessions_.clear();
    changed_.clear();
    epoch_ = identifier();
    revisions_.clear();
}

QString Remote::url() const
{
    if (!server_.isListening()) return {};
    return QString("http://%1:%2/#%3").arg(server_.serverAddress().toString()).arg(server_.serverPort()).arg(access_);
}

void Remote::lockChanged()
{
    // Unlocking a remote editing turn returns control to OBS immediately.
    if (dock_.isLocked() || !editorSession_.isEmpty()) {
        localUntil_ = dock_.isLocked() ? 0 : now() + 2000;
        releaseEditor();
        if (uploading_) uploading_->abort();
    }
    stateChanged();
}

void Remote::releaseEditor()
{
    if (editorSession_.isEmpty()) return;
    editorSession_.clear();
    editorScene_.clear();
    leaseUntil_ = 0;
    dock_.setRemoteEditing(false);
    stateChanged();
}

bool Remote::sceneInUse(const QString &id) const
{
    // Active readers and editing turns protect their notes from expiry.
    if (id == editorScene_) return true;
    const auto time = now();
    for (const auto &session : sessions_)
        if (session.scene == id && time - session.seen <= 60000) return true;
    return false;
}

void Remote::expire()
{
    const auto time = now();
    if (localUntil_ && time >= localUntil_) { localUntil_ = 0; stateChanged(); }
    if (!editorSession_.isEmpty() && (time >= leaseUntil_ || !sceneSource(editorScene_))) releaseEditor();
    for (auto it = sessions_.begin(); it != sessions_.end();)
        if (time - it->seen > 60000) it = sessions_.erase(it); else ++it;
}

QJsonObject Remote::state()
{
    const bool localEditing = localUntil_ > now();
    return {{"epoch", epoch_}, {"scenes", collectionChanging_ ? QJsonArray() : scenes()}, {"activeScene", dock_.currentSceneId()},
        {"locked", dock_.isLocked()}, {"localEditing", localEditing},
        {"editor", localEditing ? QString("OBS") : sessions_.value(editorSession_).name},
        {"editorScene", localEditing ? dock_.currentSceneId() : editorScene_},
        {"scrolling", dock_.isScrolling() || dock_.isCountingDown()}, {"speed", dock_.speed()}, {"position", dock_.scrollProgress()}};
}

bool Remote::hideQr()
{
    if (!qrOverlay_) return false;
    const bool visible = qrOverlay_->isVisible();
    qrOverlay_->hide();
    return visible;
}

void Remote::toggleQr(QWidget *bounds)
{
    if (qrOverlay_ && qrOverlay_->isVisible()) { hideQr(); return; }
    const auto link = url();
    if (link.isEmpty()) return;
    const int side = qMin(240, qMin(bounds->width(), bounds->height()));
    if (side <= 0) return;
    // A child overlay displays QR without grabbing input or activating a window.
    if (!qrOverlay_) {
        qrOverlay_ = new QLabel(bounds->parentWidget());
        qrOverlay_->setObjectName("remoteQrOverlay");
        qrOverlay_->setFocusPolicy(Qt::NoFocus);
        qrOverlay_->setAttribute(Qt::WA_OpaquePaintEvent);
        qrOverlay_->setStyleSheet("QLabel { background: white; border: 0px; padding: 0px; margin: 0px; }");
    }
    if (qrLink_ != link || qrOverlay_->size() != QSize(side, side)) {
        const auto image = qrPixmap(link, side);
        if (image.isNull()) return;
        qrOverlay_->setFixedSize(side, side);
        qrOverlay_->setPixmap(image);
        qrLink_ = link;
    }
    qrOverlay_->move(bounds->mapTo(qrOverlay_->parentWidget(), QPoint(bounds->width() - side, bounds->height() - side)));
    qrOverlay_->raise();
    qrOverlay_->show();
}

QJsonObject Remote::document(const QString &id)
{
    auto source = sceneSource(id);
    if (!source) return error("scene");
    if (id == dock_.currentSceneId() && dock_.noteUnavailable()) return error("read");
    // Reject oversized active text before copying or parsing its HTML.
    if (id == dock_.currentSceneId() && dock_.textLength() > maxNoteCharacters) return error("size");
    QString failure;
    const auto html = dock_.readHtml(source, &failure);
    if (!failure.isEmpty()) return error("read");
    auto result = webDocument(html);
    if (result.isEmpty() || QJsonDocument(result).toJson(QJsonDocument::Compact).size() > 4 * maxRemoteText) return error("size");
    result.insert("ok", true);
    result.insert("revision", revisions_.value(id));
    result.insert("epoch", epoch_);
    result.insert("scene", id);
    return result;
}

QJsonObject Remote::handle(const QJsonObject &input)
{
    if (stopping_ || !server_.isListening()) return error("disabled");
    // A separate capability keeps unrelated OBS clients out of ScenePad sessions.
    const auto access = input.value("access").toString();
    unsigned difference = static_cast<unsigned>(access.size() ^ access_.size());
    for (int i = 0; i < access_.size(); ++i) difference |= access_.at(i).unicode() ^ (i < access.size() ? access.at(i).unicode() : 0);
    if (difference) return error("access");
    expire();
    const auto action = input.value("action").toString();
    if (collectionChanging_) return error("loading");
    if (action == "Open") {
        if (sessions_.size() >= 16) return error("capacity");
        const auto id = identifier();
        auto name = input.value("name").toString().simplified().left(32);
        name.remove(QChar::Null);
        if (name.isEmpty()) name = text("RemoteDevice");
        sessions_.insert(id, {name, now(), {}});
        auto result = state();
        result.insert("session", id);
        result.insert("ok", true);
        return result;
    }
    const auto session = input.value("session").toString();
    auto found = sessions_.find(session);
    if (found == sessions_.end()) return error("session");
    found->seen = now();
    if (action == "KeepAlive") {
        if (editorSession_ == session) leaseUntil_ = now() + 15000;
        return {{"ok", true}, {"editing", editorSession_ == session}, {"locked", dock_.isLocked()}, {"epoch", epoch_}};
    }
    if (action == "GetState") { auto result = state(); result.insert("ok", true); return result; }
    const auto id = input.value("scene").toString();
    if (action == "GetDocument") {
        auto result = document(id);
        if (result.value("ok").toBool()) found->scene = id;
        return result;
    }
    if (action == "ExportHtml" || action == "ExportText") {
        auto source = sceneSource(id);
        if (!source) return error("scene");
        QString failure;
        const auto html = dock_.readHtml(source, &failure);
        if (!failure.isEmpty()) return error("read");
        if (html.toUtf8().size() > 4 * maxRemoteText) return error("size");
        if (action == "ExportText") return {{"ok", true}, {"text", plainText(html)}};
        return {{"ok", true}, {"html", html}};
    }
    if (action == "ReleaseEdit") {
        if (editorSession_ == session) releaseEditor();
        auto result = state(); result.insert("ok", true); return result;
    }
    if (action == "AcquireEdit" || action == "SaveDocument" || action == "CheckImport" || action == "ReplaceDocument") {
        if (dock_.isLocked()) return error("locked");
        if (input.value("epoch").toString() != epoch_ || !input.value("revision").isDouble() ||
            input.value("revision").toDouble() != revisions_.value(id)) return error("conflict");
        if (!sceneSource(id)) return error("scene");
        if (localUntil_ > now()) return error("busy");
        if (!editorSession_.isEmpty() && (editorSession_ != session || editorScene_ != id)) return error("busy");
        if (action == "AcquireEdit") {
            if (!dock_.flush(false)) return error("save");
            const auto result = input.value("replace").toBool() ? QJsonObject{{"ok", true}} : document(id);
            if (!result.value("ok").toBool()) return result;
            if (!input.value("replace").toBool() && !result.value("editable").toBool()) return error("format");
            editorSession_ = session;
            editorScene_ = id;
            leaseUntil_ = now() + 15000;
            dock_.setRemoteEditing(true);
            stateChanged();
            return result;
        }
        if (editorSession_ != session || editorScene_ != id) return error("lease");
        if (action == "CheckImport") return {{"ok", true}};
        auto source = sceneSource(id);
        QString failure;
        const auto original = action == "ReplaceDocument" ? QString() : dock_.notes().read(source, &failure).html;
        if (!failure.isEmpty()) return error("read");
        QString html;
        bool limited = false;
        if (!applyWebDocument(original,
            input.value("document").toObject(), html, &limited)) return error(limited ? "limit" : "format");
        if (action == "ReplaceDocument") return replaceDocument(input, html);
        if (html != original) {
            if (!dock_.applyRemoteHtml(source, html)) return error("save");
            noteChanged(id);
        }
        leaseUntil_ = now() + 15000;
        return {{"ok", true}, {"revision", revisions_.value(id)}, {"epoch", epoch_}};
    }
    if (action == "Scroll") {
        const auto command = input.value("command").toString();
        if (command == "speed") {
            const double value = input.value("speed").toDouble();
            if (!std::isfinite(value) || value < 0.1 || value > 5) return error("range");
            dock_.setSpeed(value);
        } else if (command == "start") dock_.startScroll();
        else if (command == "pause") dock_.pauseScroll();
        else if (command == "top") dock_.command(Command::Top);
        else if (command == "seek") {
            if (id != dock_.currentSceneId()) return error("scene");
            if (localUntil_ > now() || !editorSession_.isEmpty()) return error("busy");
            if (!dock_.goToPosition(input.value("target").toString())) return error("range");
        }
        else return error("command");
        auto result = state(); result.insert("ok", true); return result;
    }
    return error("action");
}

QJsonObject Remote::replaceDocument(QJsonObject input, const QString &html, bool simplified)
{
    auto result = webDocument(html);
    if (result.isEmpty() || QJsonDocument(result).toJson(QJsonDocument::Compact).size() > 4 * maxRemoteText) return error("size");
    // Validate again after parsing so an expired lease or changed scene cannot overwrite a note.
    input.insert("action", "CheckImport");
    const auto checked = handle(input);
    if (!checked.value("ok").toBool()) return checked;
    const auto id = input.value("scene").toString();
    auto source = sceneSource(id);
    if (!dock_.applyRemoteHtml(source, html)) return error("save");
    noteChanged(id);
    leaseUntil_ = now() + 15000;
    result.insert("ok", true);
    result.insert("epoch", epoch_);
    result.insert("revision", revisions_.value(id));
    result.insert("scene", id);
    result.insert("simplified", simplified);
    return result;
}

void Remote::request(obs_data_t *input, obs_data_t *output, void *data)
{
    auto *self = static_cast<Remote *>(data);
    const auto pending = self->pending_;
    const QByteArray payload(obs_data_get_string(input, "payload"));
    QJsonObject result = error("timeout");
    if (payload.size() > 6 * maxRemoteText || self->stopping_) result = error("size");
    else if (pending->fetch_add(1) >= 8) { --*pending; result = error("capacity"); }
    else {
        const auto request = QJsonDocument::fromJson(payload);
        if (!request.isObject()) result = error("format");
        else if (QThread::currentThread() == self->thread()) result = self->handle(request.object());
        else {
            struct Work { QSemaphore done; std::atomic<int> status{0}; QJsonObject result; };
            auto work = std::make_shared<Work>();
            QMetaObject::invokeMethod(self, [self, work, input = request.object()] {
                int queued = 0;
                if (!work->status.compare_exchange_strong(queued, 1)) return;
                work->result = self->handle(input);
                work->done.release();
            }, Qt::QueuedConnection);
            if (work->done.tryAcquire(1, 3000)) result = work->result;
            else { int queued = 0; work->status.compare_exchange_strong(queued, 2); }
        }
        --*pending;
    }
    const auto json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    obs_data_set_string(output, "payload", json.constData());
}

void Remote::noteChanged(const QString &id)
{
    if (!server_.isListening() || id.isEmpty()) return;
    // Reserve a short editing turn while the desktop is typing.
    if (editorSession_.isEmpty() && id == dock_.currentSceneId() && !dock_.isLocked()) localUntil_ = now() + 2000;
    ++revisions_[id];
    changed_.insert(id);
    stateChanged();
}

void Remote::stateChanged()
{
    if (!server_.isListening()) return;
    stateDirty_ = true;
    if (!events_.isActive()) events_.start();
}

void Remote::scrollChanged()
{
    // Share the 200 ms event throttle without sending scene lists on each scroll.
    if (server_.isListening() && !sessions_.isEmpty() && !events_.isActive()) events_.start();
}

void Remote::resetCollection()
{
    releaseEditor();
    localUntil_ = 0;
    epoch_ = identifier();
    revisions_.clear();
    changed_.clear();
    stateChanged();
}

void Remote::setCollectionChanging(bool changing)
{
    collectionChanging_ = changing;
    if (changing) resetCollection();
    stateChanged();
}

void Remote::publish()
{
    if (!registered_ || !server_.isListening()) return;
    if (sessions_.isEmpty()) { changed_.clear(); stateDirty_ = false; return; }
    QJsonObject value;
    const char *type = stateDirty_ ? "Changed" : "Scroll";
    if (stateDirty_) {
        value = state();
        QJsonArray changed;
        for (const auto &id : changed_) changed.append(id);
        changed_.clear();
        value.insert("changed", changed);
    } else {
        value = {{"epoch", epoch_}, {"scene", dock_.currentSceneId()}, {"position", dock_.scrollProgress()}};
    }
    stateDirty_ = false;
    OBSDataAutoRelease event = obs_data_create();
    obs_data_set_string(event, "payload", QJsonDocument(value).toJson(QJsonDocument::Compact).constData());
    obs_websocket_vendor_emit_event(vendor_, type, event);
}

quint16 Remote::websocketPort() const
{
    // Follow OBS settings and its command-line override without polling.
    QCommandLineParser arguments;
    arguments.addOption(QCommandLineOption("websocket_port", QString(), "port"));
    arguments.parse(QCoreApplication::arguments());
    bool valid = false;
    const auto overridePort = arguments.value("websocket_port").toUShort(&valid);
    if (valid && overridePort) return overridePort;
    const QDir directory(QFileInfo(settings_.fileName()).absolutePath());
    QFile file(directory.filePath("../obs-websocket/config.json"));
    if (file.open(QIODevice::ReadOnly)) {
        const int port = QJsonDocument::fromJson(file.readAll()).object().value("server_port").toInt(4455);
        if (port > 0 && port <= 65535) return static_cast<quint16>(port);
    }
    return 4455;
}

QJsonObject Remote::config() const
{
    QJsonObject labels;
    const char *keys[] = {"ScenePad", "FindPlaceholder", "FindPrevious", "FindNext", "CloseSearch", "NoMatch", "Pause",
        "RemoteDevice", "RemotePassword", "RemoteRememberSession", "RemoteConnect", "RemoteDisconnect", "RemoteConnected", "RemoteDisconnected", "RemoteConnecting",
        "RemotePasswordRequired", "RemoteFollow", "RemoteLocked", "RemoteClickEdit", "RemoteReadOnly", "RemoteEditing", "RemoteSaved",
        "RemoteSaving", "RemoteConflict", "RemoteReload", "RemoteDownload", "RemoteLocal", "RemoteOBS", "RemoteTarget", "RemotePlay", "RemoteTop",
        "RemoteGuide", "RemoteBusy", "RemoteFormat", "RemoteDraft", "RemoteFailure", "RemoteStorageFailed", "RemoteNoDraft", "RemoteRetry", "RemoteScene", "RemoteLines",
        "Bold", "Italic", "Underline", "Strikethrough", "FormatPainter", "FormatPainterContinuous", "SelectedCharacters", "TextColor", "AlignLeft", "AlignCenter", "AlignRight", "Speed",
        "Font", "Highlight", "ClearFormat", "AlignJustify", "ListStyle", "RemoveList", "ListDisc", "ListCircle", "ListSquare", "ListDecimal",
        "ListLowerAlpha", "ListUpperAlpha", "ListLowerRoman", "ListUpperRoman", "IncreaseIndent", "DecreaseIndent", "InsertTime", "InsertMark", "NextMark", "ReplacePrompt",
        "RemoteImport", "RemoteExport", "RemoteFileSize", "RemoteHtmlNotice", "RemoteSelectText", "RemoteCountdown", "RemoteTooLarge", "RemoteFontSize",
        "RemoteWordSize", "RemoteImporting", "RemoteImportFailed", "RemoteFileType", "RemoteWordNotice", "WordOpenError", "WordSimplified",
        "TextLimit", "CharacterCount", "CharacterRemaining", "ExportHTML", "ExportTXT", "GoTo", "GoToPrompt", "GoToInvalid"};
    for (const auto *key : keys) labels.insert(key, text(key));
    return {{"port", websocketPort()}, {"labels", labels}, {"version", SCENEPAD_VERSION}};
}

void Remote::acceptConnections()
{
    while (server_.hasPendingConnections()) {
        auto *socket = server_.nextPendingConnection();
        if (server_.findChildren<QTcpSocket *>().size() > 16) { socket->abort(); socket->deleteLater(); continue; }
        struct Transfer {
            bool done = false;
            qint64 remaining = 0;
            QJsonObject input;
            std::unique_ptr<QTemporaryDir> directory;
            std::unique_ptr<QFile> file;
        };
        auto transfer = std::make_shared<Transfer>();
        socket->setReadBufferSize(8193);
        auto *timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, socket, [socket] { socket->abort(); socket->deleteLater(); });
        timeout->start(3000);
        connect(socket, &QTcpSocket::disconnected, this, [this, socket, transfer] {
            transfer->done = true;
            transfer->file.reset();
            transfer->directory.reset();
            if (uploading_ == socket) uploading_.clear();
            socket->deleteLater();
        });
        auto reply = [this, socket, timeout, transfer](const QByteArray &body, const QByteArray &type, const QByteArray &status) {
            transfer->done = true;
            transfer->file.reset();
            transfer->directory.reset();
            if (uploading_ == socket) uploading_.clear();
            const auto origin = QString("ws://%1:%2").arg(server_.serverAddress().toString()).arg(websocketPort()).toUtf8();
            socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(body.size()) +
                "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
                "Content-Security-Policy: default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self' " + origin +
                "; img-src 'self'; manifest-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'\r\n\r\n" + body);
            timeout->start(30000);
            socket->disconnectFromHost();
        };
        auto json = [reply](const QJsonObject &value) { reply(QJsonDocument(value).toJson(QJsonDocument::Compact), "application/json", "200 OK"); };
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, timeout, transfer, reply, json] {
            if (transfer->done) return;
            if (!transfer->file) {
                const auto header = socket->peek(8193);
                const auto end = header.indexOf("\r\n\r\n");
                if (end < 0) { if (header.size() > 8192) socket->abort(); return; }
                if (end + 4 > 8192) { socket->abort(); return; }
                socket->read(end + 4);
                const auto lines = header.left(end).split('\n');
                const auto line = lines[0].trimmed().split(' ');
                if (line.size() != 3 || (line[2] != "HTTP/1.1" && line[2] != "HTTP/1.0")) { reply({}, "text/plain", "400 Bad Request"); return; }
                if (line[0] == "GET") {
                    QByteArray body, type = "text/plain; charset=utf-8", status = "404 Not Found";
                    const QHash<QByteArray, QByteArray> files{{"/", ":/remote/index.html"}, {"/style.css", ":/remote/style.css"},
                        {"/app.mjs", ":/remote/app.mjs"}, {"/protocol.mjs", ":/remote/protocol.mjs"}, {"/document.mjs", ":/remote/document.mjs"},
                        {"/manifest.webmanifest", ":/remote/manifest.webmanifest"}, {"/icon.svg", ":/remote/icon.svg"},
                        {"/icon-192.png", ":/remote/icon-192.png"}, {"/icon-512.png", ":/remote/icon-512.png"}};
                    if (line[1] == "/config.json") { body = QJsonDocument(config()).toJson(QJsonDocument::Compact); type = "application/json"; status = "200 OK"; }
                    else if (files.contains(line[1])) {
                        QFile file(QString::fromUtf8(files.value(line[1])));
                        if (file.open(QIODevice::ReadOnly)) {
                            body = file.readAll(); status = "200 OK";
                            if (line[1] == "/") type = "text/html; charset=utf-8";
                            else if (line[1].endsWith(".css")) type = "text/css; charset=utf-8";
                            else if (line[1].endsWith(".png")) type = "image/png";
                            else if (line[1].endsWith(".svg")) type = "image/svg+xml";
                            else if (line[1].endsWith(".webmanifest")) type = "application/manifest+json";
                            else type = "text/javascript; charset=utf-8";
                        }
                    }
                    reply(body, type, status); return;
                }
                if (line[0] != "POST" || line[1] != "/import.docx") { reply({}, "text/plain", "405 Method Not Allowed"); return; }
                QHash<QByteArray, QByteArray> fields;
                for (int i = 1; i < lines.size(); ++i) {
                    const auto colon = lines[i].indexOf(':');
                    const auto key = lines[i].left(colon).trimmed().toLower();
                    if (colon <= 0 || fields.contains(key)) { json(error("format")); return; }
                    fields.insert(key, lines[i].mid(colon + 1).trimmed());
                }
                bool lengthOk = false, revisionOk = false;
                const auto length = fields.value("content-length").toLongLong(&lengthOk);
                const auto revision = fields.value("x-scenepad-revision").toInt(&revisionOk);
                const auto origin = QString("http://%1:%2").arg(server_.serverAddress().toString()).arg(server_.serverPort()).toUtf8();
                if (fields.contains("transfer-encoding") || !lengthOk || length <= 0 || !revisionOk || revision < 0 ||
                    fields.value("content-type") != "application/vnd.openxmlformats-officedocument.wordprocessingml.document" ||
                    (fields.contains("origin") && fields.value("origin") != origin)) { json(error("format")); return; }
                if (length > 5 * maxRemoteText) { json(error("size")); return; }
                transfer->input = {{"action", "CheckImport"}, {"access", QString::fromUtf8(fields.value("x-scenepad-access"))},
                    {"session", QString::fromUtf8(fields.value("x-scenepad-session"))}, {"scene", QString::fromUtf8(fields.value("x-scenepad-scene"))},
                    {"epoch", QString::fromUtf8(fields.value("x-scenepad-epoch"))}, {"revision", revision}};
                const auto checked = handle(transfer->input);
                if (!checked.value("ok").toBool()) { json(checked); return; }
                if (uploading_) { json(error("busy")); return; }
                // Stream one authenticated upload to a disposable file, without buffering its body.
                transfer->directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/scenepad-upload-XXXXXX");
                if (!transfer->directory->isValid()) { json(error("upload")); return; }
                transfer->file = std::make_unique<QFile>(transfer->directory->filePath("document.docx"));
                if (!transfer->file->open(QIODevice::WriteOnly)) { json(error("upload")); return; }
                uploading_ = socket;
                transfer->remaining = length;
                socket->setReadBufferSize(64 * 1024);
                timeout->start(30000);
            }
            const auto chunk = socket->read(qMin<qint64>(transfer->remaining, 64 * 1024));
            if (transfer->file->write(chunk) != chunk.size()) { json(error("upload")); return; }
            transfer->remaining -= chunk.size();
            if (transfer->remaining > 0) return;
            if (socket->bytesAvailable() || socket->state() != QAbstractSocket::ConnectedState) { json(error("format")); return; }
            const auto checked = handle(transfer->input);
            if (!checked.value("ok").toBool()) { json(checked); return; }
            if (!transfer->file->flush()) { json(error("upload")); return; }
            transfer->file->close();
            const auto imported = importDocx(transfer->file->fileName());
            if (!imported.error.isEmpty()) { json(error(imported.error == text("TextLimit") ? "limit" : "word")); return; }
            json(replaceDocument(transfer->input, imported.html, imported.simplified));
        });
    }
}

void Remote::showSettings()
{
    QDialog dialog(&dock_);
    dialog.setObjectName("remoteSettings");
    dialog.setWindowTitle(text("RemoteAccess"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *enabled = new QCheckBox(text("RemoteEnable"), &dialog);
    enabled->setObjectName("remoteEnabled");
    enabled->setChecked(server_.isListening());
    layout->addWidget(enabled);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *address = new QComboBox(&dialog);
    address->setObjectName("remoteAddress");
    address->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (!(interface.flags() & QNetworkInterface::IsUp)) continue;
        for (const auto &entry : interface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback())
                address->addItem(entry.ip().toString() + " · " + interface.humanReadableName(), entry.ip().toString());
    }
    address->addItem("127.0.0.1", "127.0.0.1");
    const int saved = address->findData(settings_.value("remote/address"));
    if (saved >= 0) address->setCurrentIndex(saved);
    auto *port = new QSpinBox(&dialog);
    port->setObjectName("remotePort"); port->setKeyboardTracking(false);
    port->setRange(1024, 65535); port->setValue(settings_.value("remote/port", 8800).toInt());
    form->addRow(text("RemoteAddress"), address);
    form->addRow(text("RemotePort"), port);
    layout->addLayout(form);
    auto *qr = new QrLabel(&dialog); qr->setObjectName("remoteQR"); form->addRow(QString(), qr);
    auto *status = new QLabel(&dialog);
    status->setObjectName("remoteStatus");
    status->setAlignment(Qt::AlignCenter);
    status->setTextFormat(Qt::PlainText);
    // Keep the button row still when a status message changes.
    status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    status->setFixedHeight(status->fontMetrics().lineSpacing());
    layout->addWidget(status);
    auto setStatus = [&](const QString &message) { status->setText(message); status->setToolTip(message); };
    auto *buttons = new QHBoxLayout;
    auto *reset = new QPushButton(text("RemoteReset"), &dialog);
    auto *copy = new QPushButton(text("RemoteCopy"), &dialog);
    auto *apply = new QPushButton(text("RemoteApply"), &dialog);
    reset->setAutoDefault(false); copy->setAutoDefault(false); apply->setDefault(true);
    buttons->addWidget(reset); buttons->addStretch(); buttons->addWidget(copy); buttons->addWidget(apply);
    copy->setObjectName("remoteCopy"); reset->setObjectName("remoteReset"); apply->setObjectName("remoteApply");
    bool resetPending = false;
    auto refresh = [&] {
        const auto value = url();
        const bool hadQr = !qr->isHidden();
        copy->setEnabled(!value.isEmpty());
        qr->setVisible(!value.isEmpty());
        qr->setLink(value);
        layout->activate();
        if (!dialog.isVisible() || hadQr != !value.isEmpty()) dialog.adjustSize();
    };
    // Apply only changed settings so existing sessions survive an unchanged Apply.
    connect(apply, &QPushButton::clicked, &dialog, [&] {
        QString failure;
        const auto host = address->currentData().toString();
        const auto httpPort = static_cast<quint16>(port->value());
        if (enabled->isChecked()) {
            if (!registered_) failure = text("RemoteUnavailable");
            else if ((!server_.isListening() || server_.serverAddress() != QHostAddress(host) ||
                server_.serverPort() != httpPort) && !listen(host, httpPort))
                failure = text("RemoteListenFailed") + " " + server_.errorString();
            if (!failure.isEmpty()) { setStatus(failure); refresh(); return; }
        }
        if (!enabled->isChecked() && server_.isListening()) stop();
        settings_.setValue("remote/enabled", enabled->isChecked());
        settings_.setValue("remote/address", address->currentData());
        settings_.setValue("remote/port", port->value());
        if (resetPending) {
            access_ = capability(); settings_.setValue("remote/access", access_);
            sessions_.clear(); resetCollection(); resetPending = false;
        }
        setStatus({}); refresh();
    });
    auto pending = [&] { setStatus(text("RemotePending")); };
    connect(enabled, &QCheckBox::toggled, &dialog, pending);
    connect(address, &QComboBox::currentIndexChanged, &dialog, pending);
    connect(port, &QSpinBox::valueChanged, &dialog, pending);
    connect(copy, &QPushButton::clicked, &dialog, [&] { QApplication::clipboard()->setText(url()); });
    connect(reset, &QPushButton::clicked, &dialog, [&] { resetPending = true; pending(); });
    layout->addLayout(buttons); refresh(); dialog.exec();
}
}
