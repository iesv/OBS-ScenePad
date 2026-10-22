#pragma once
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTimer>
#include <atomic>
#include <memory>
#include <obs.h>

class QSettings;
class QLabel;
class QTcpSocket;
class QWidget;
namespace scenepad {
class Dock;

// All document and lease state belongs to the Qt UI thread.
class Remote final : public QObject {
public:
    Remote(Dock &dock, QSettings &settings);
    ~Remote() override;
    void initialize();
    void shutdown();
    void showSettings();
    void toggleQr(QWidget *bounds);
    bool hideQr();
    bool listen(const QString &address, quint16 port);
    void stop();
    QString url() const;
    void lockChanged();
    QJsonObject handle(const QJsonObject &input);
    void noteChanged(const QString &id);
    void stateChanged();
    void scrollChanged();
    void resetCollection();
    void setCollectionChanging(bool changing);
    bool sceneInUse(const QString &id) const;
private:
    struct Session { QString name; qint64 seen = 0; QString scene; };
    static void request(obs_data_t *input, obs_data_t *output, void *data);
    void acceptConnections();
    void expire();
    void releaseEditor();
    void publish();
    QJsonObject state();
    QJsonObject document(const QString &id);
    QJsonObject config() const;
    quint16 websocketPort() const;
    QJsonObject replaceDocument(QJsonObject input, const QString &html, bool simplified = false);
    Dock &dock_;
    QSettings &settings_;
    QTcpServer server_;
    QPointer<QTcpSocket> uploading_;
    QPointer<QLabel> qrOverlay_;
    QString qrLink_;
    QTimer maintenance_, events_;
    QHash<QString, Session> sessions_;
    QHash<QString, int> revisions_;
    QSet<QString> changed_;
    QString access_, epoch_, editorSession_, editorScene_;
    qint64 leaseUntil_ = 0;
    qint64 localUntil_ = 0;
    void *vendor_ = nullptr;
    bool registered_ = false;
    bool collectionChanging_ = false;
    bool stateDirty_ = false;
    std::atomic<bool> stopping_{false};
    std::shared_ptr<std::atomic<int>> pending_ = std::make_shared<std::atomic<int>>(0);
};
}
