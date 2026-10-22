#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QSet>
#include <QString>
#include <obs.h>

namespace scenepad {
struct Note {
    QString html;
};
struct NoteFile {
    QString sceneId, sceneName, html;
};
struct NoteTarget {
    QString sceneId, sceneName, path, previous;
};
constexpr qint64 maxNoteBytes = 64 * 1024 * 1024;
QString noteName(QString name);
QByteArray encodeNote(const NoteFile &note);
bool decodeNote(const QByteArray &data, NoteFile &note, QString &error);
bool saveNoteFile(const QString &path, const QByteArray &data, QString &error, const QDateTime &modified = {});

// Scene data contains only a filename; document bodies stay in the notes directory.
class NoteStore {
public:
    explicit NoteStore(QString directory);
    QString directory() const { return directory_; }
    QString filePath(obs_source_t *scene) const;
    Note read(obs_source_t *scene, QString *error = nullptr) const;
    bool write(obs_source_t *scene, const Note &note, QString *error = nullptr, const QDateTime &modified = {});
    bool rename(obs_source_t *scene, QString *error = nullptr);
    bool clearExpired(obs_source_t *scene, const QDateTime &cutoff, QString *error = nullptr);
    NoteTarget prepare(obs_source_t *scene, const QSet<QString> &reserved = {}) const;
    void accept(obs_source_t *scene, const NoteTarget &target);
    bool takeReferenceChanges();
private:
    QString destination(const QString &name, const QString &previous, const QSet<QString> &reserved) const;
    QString directory_;
    bool referencesChanged_ = false;
};
}
