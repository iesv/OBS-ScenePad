#pragma once
#include <QList>
#include <QStringList>

namespace scenepad {
struct BackupNote {
    QString path, sceneId;
};
// File-only operations can run off the OBS UI thread.
bool backupNotes(const QString &path, const QStringList &files, QString &error);
QList<BackupNote> inspectBackup(const QString &path, const QString &staging, QString &error);
}
