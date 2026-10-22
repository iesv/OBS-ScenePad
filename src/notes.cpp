#include "notes.hpp"
#include "locale.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>
#include <obs.hpp>
#include <utility>

namespace scenepad {
QString noteName(QString name)
{
    static const QRegularExpression invalid(R"([<>:"/\\|?*\x00-\x1f])");
    static const QRegularExpression reserved(R"(^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$))",
        QRegularExpression::CaseInsensitiveOption);
    name = name.normalized(QString::NormalizationForm_C).replace(invalid, "_").trimmed().left(100);
    if (!name.isEmpty() && name.at(name.size() - 1).isHighSurrogate()) name.chop(1);
    while (name.toUtf8().size() > 180) name.chop(name.at(name.size() - 1).isLowSurrogate() ? 2 : 1);
    while (name.endsWith('.') || name.endsWith(' ')) name.chop(1);
    if (name.isEmpty()) name = "Scene";
    if (reserved.match(name).hasMatch()) name.prepend('_');
    return name;
}

QByteArray encodeNote(const NoteFile &note)
{
    return QJsonDocument(QJsonObject{{"format", "OBS-ScenePad"}, {"version", 1},
        {"sceneId", note.sceneId}, {"sceneName", note.sceneName}, {"html", note.html}})
        .toJson(QJsonDocument::Compact);
}

bool decodeNote(const QByteArray &data, NoteFile &note, QString &error)
{
    if (data.size() > maxNoteBytes) { error = text("BackupTooLarge"); return false; }
    const auto doc = QJsonDocument::fromJson(data);
    const auto obj = doc.object();
    if (!doc.isObject() || obj.value("format") != "OBS-ScenePad" || obj.value("version") != 1 ||
        !obj.value("sceneId").isString() || QUuid(obj.value("sceneId").toString()).isNull() ||
        !obj.value("sceneName").isString() || !obj.value("html").isString()) {
        error = text("BackupInvalid"); return false;
    }
    note = {obj.value("sceneId").toString(), obj.value("sceneName").toString(), obj.value("html").toString()};
    return true;
}

bool saveNoteFile(const QString &path, const QByteArray &data, QString &error, const QDateTime &modified)
{
    if (data.size() > maxNoteBytes) { error = text("BackupTooLarge"); return false; }
    QSaveFile file(path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) { error = text("NoteSaveFailed"); return false; }
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() ||
        (modified.isValid() && (!file.flush() || !file.setFileTime(modified, QFileDevice::FileModificationTime))) || !file.commit()) {
        error = file.errorString(); return false;
    }
    return true;
}

NoteStore::NoteStore(QString directory) : directory_(QDir::cleanPath(std::move(directory))) {}

QString NoteStore::filePath(obs_source_t *scene) const
{
    if (!scene) return {};
    OBSDataAutoRelease settings = obs_source_get_private_settings(scene);
    OBSDataAutoRelease ref = obs_data_get_obj(settings, "scenepad");
    if (qstrcmp(obs_data_get_string(ref, "id"), obs_source_get_uuid(scene)) != 0) return {};
    const auto name = QString::fromUtf8(obs_data_get_string(ref, "file"));
    if (name.isEmpty() || name != QFileInfo(name).fileName() || name.contains('\\') ||
        !name.endsWith(".json", Qt::CaseInsensitive)) return {};
    return QDir(directory_).filePath(name);
}

Note NoteStore::read(obs_source_t *scene, QString *error) const
{
    const auto path = filePath(scene);
    if (path.isEmpty()) return {};
    QFile file(path);
    QString failure;
    NoteFile note;
    if (QFileInfo(path).isSymLink()) failure = text("FileReadFailed") + path;
    else if (!file.open(QIODevice::ReadOnly)) {
        // A deleted note becomes editable and is recreated on the next save.
        if (file.error() == QFileDevice::OpenError && !QFileInfo::exists(path)) return {};
        failure = text("FileReadFailed") + path;
    }
    else if (file.size() > maxNoteBytes) failure = text("BackupTooLarge");
    else if (decodeNote(file.readAll(), note, failure) && note.sceneId != QString::fromUtf8(obs_source_get_uuid(scene)))
        failure = text("NoteSceneMismatch");
    if (!failure.isEmpty()) {
        if (error) *error = failure;
        return {};
    }
    return {note.html};
}

QString NoteStore::destination(const QString &name, const QString &previous, const QSet<QString> &reserved) const
{
    const QDir dir(directory_);
    const auto base = noteName(name);
    // Reserve names case-insensitively so backups remain portable between platforms.
    QSet<QString> occupied = reserved;
    const auto files = dir.entryList(QDir::Files | QDir::System | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &file : files)
        if (dir.filePath(file) != previous) occupied.insert(file.toCaseFolded());
    for (int suffix = 1; ; ++suffix) {
        const auto candidate = base + (suffix == 1 ? QString() : "_" + QString::number(suffix)) + ".json";
        if (!occupied.contains(candidate.toCaseFolded())) return dir.filePath(candidate);
    }
}

bool NoteStore::write(obs_source_t *scene, const Note &note, QString *error, const QDateTime &modified)
{
    QString failure;
    if (!scene) { if (error) *error = text("SelectScene"); return false; }
    const auto target = prepare(scene);
    // A single atomic replacement saves only the changed note.
    if (QFileInfo(target.path).isSymLink() || !saveNoteFile(target.path,
        encodeNote({target.sceneId, target.sceneName, note.html}), failure, modified)) {
        if (error) *error = failure.isEmpty() ? text("NoteSaveFailed") : failure;
        return false;
    }
    accept(scene, target);
    return true;
}

NoteTarget NoteStore::prepare(obs_source_t *scene, const QSet<QString> &reserved) const
{
    const auto name = QString::fromUtf8(obs_source_get_name(scene));
    const auto previous = filePath(scene);
    OBSDataAutoRelease settings = obs_source_get_private_settings(scene);
    OBSDataAutoRelease ref = obs_data_get_obj(settings, "scenepad");
    const bool renamed = QString::fromUtf8(obs_data_get_string(ref, "name")) != name;
    // Keep the existing spelling for case-only changes on every filesystem.
    const bool sameName = !previous.isEmpty() &&
        QFileInfo(previous).completeBaseName().compare(noteName(name), Qt::CaseInsensitive) == 0;
    const auto path = previous.isEmpty() || (renamed && !sameName) ? destination(name, previous, reserved) : previous;
    return {QString::fromUtf8(obs_source_get_uuid(scene)), name, path, previous};
}

void NoteStore::accept(obs_source_t *scene, const NoteTarget &target)
{
    OBSDataAutoRelease settings = obs_source_get_private_settings(scene);
    OBSDataAutoRelease ref = obs_data_get_obj(settings, "scenepad");
    const bool renamed = QString::fromUtf8(obs_data_get_string(ref, "name")) != target.sceneName;
    if (!renamed && target.previous == target.path) return;
    OBSDataAutoRelease updated = obs_data_create();
    obs_data_set_string(updated, "id", target.sceneId.toUtf8().constData());
    obs_data_set_string(updated, "file", QFileInfo(target.path).fileName().toUtf8().constData());
    obs_data_set_string(updated, "name", target.sceneName.toUtf8().constData());
    obs_data_set_obj(settings, "scenepad", updated);
    if (target.previous != target.path || renamed) referencesChanged_ = true;
    if (!target.previous.isEmpty() && target.previous.compare(target.path, Qt::CaseInsensitive) != 0 &&
        !QFileInfo(target.previous).isSymLink()) QFile::remove(target.previous);
}

bool NoteStore::rename(obs_source_t *scene, QString *error)
{
    if (!scene || filePath(scene).isEmpty()) return true;
    OBSDataAutoRelease settings = obs_source_get_private_settings(scene);
    OBSDataAutoRelease ref = obs_data_get_obj(settings, "scenepad");
    if (QString::fromUtf8(obs_data_get_string(ref, "name")) == QString::fromUtf8(obs_source_get_name(scene))) return true;
    QString failure;
    // Renaming a scene does not restart the body's retention period.
    const auto modified = QFileInfo(filePath(scene)).lastModified();
    const auto note = read(scene, &failure);
    if (failure.isEmpty()) return write(scene, note, error, modified);
    if (error) *error = failure;
    return false;
}

bool NoteStore::clearExpired(obs_source_t *scene, const QDateTime &cutoff, QString *error)
{
    const auto path = filePath(scene);
    const QFileInfo info(path);
    // Ordinary checks read metadata only; parse a body only when it is due.
    if (path.isEmpty() || !cutoff.isValid() || !info.exists() || !info.lastModified().isValid() || info.lastModified() > cutoff) return false;
    QString failure;
    const auto note = read(scene, &failure);
    if (!failure.isEmpty()) { if (error) *error = failure; return false; }
    if (note.html.isEmpty()) return false;
    return write(scene, {}, error);
}

bool NoteStore::takeReferenceChanges()
{
    return std::exchange(referencesChanged_, false);
}
}
