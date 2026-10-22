#include "backup.hpp"
#include "notes.hpp"
#include "locale.hpp"
#include "text-limits.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QtEndian>
#include <zlib.h>

namespace scenepad {
namespace {
constexpr qint64 maxBackupBytes = 512 * 1024 * 1024;
constexpr qint64 maxArchiveBytes = maxBackupBytes + 16 * 1024 * 1024;
constexpr int maxBackupFiles = 10000;
quint16 u16(const QByteArray &data, qsizetype offset) { return qFromLittleEndian<quint16>(data.constData() + offset); }
quint32 u32(const QByteArray &data, qsizetype offset) { return qFromLittleEndian<quint32>(data.constData() + offset); }
void put16(QByteArray &data, quint16 value) { char out[2]; qToLittleEndian(value, out); data.append(out, 2); }
void put32(QByteArray &data, quint32 value) { char out[4]; qToLittleEndian(value, out); data.append(out, 4); }
quint32 checksum(const QByteArray &data)
{
    return static_cast<quint32>(crc32(0, reinterpret_cast<const Bytef *>(data.constData()), static_cast<uInt>(data.size())));
}
bool readJson(const QString &path, QByteArray &data, NoteFile &note, QString &error)
{
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly)) { error = text("FileReadFailed") + path; return false; }
    if (file.size() > maxNoteBytes) { error = text("BackupTooLarge"); return false; }
    data = file.readAll();
    return decodeNote(data, note, error);
}
}

bool backupNotes(const QString &path, const QStringList &files, QString &error)
{
    if (files.isEmpty() || files.size() > maxBackupFiles) { error = text("BackupEmpty"); return false; }
    if (path.endsWith(".json", Qt::CaseInsensitive)) {
        QByteArray data; NoteFile note;
        return files.size() == 1 && readJson(files.front(), data, note, error) && saveNoteFile(path, data, error);
    }
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) { error = output.errorString(); return false; }
    QByteArray directory;
    QSet<QString> ids;
    qint64 total = 0;
    for (const auto &file : files) {
        QByteArray data; NoteFile note;
        if (!readJson(file, data, note, error)) return false;
        total += data.size();
        if (total > maxBackupBytes) { error = text("BackupTooLarge"); return false; }
        if (ids.contains(note.sceneId)) { error = text("BackupInvalid"); return false; }
        ids.insert(note.sceneId);
        const auto name = QFileInfo(file).fileName().toUtf8();
        const auto compressed = qCompress(data, 1);
        if (compressed.size() < 10) { error = text("BackupFailed"); return false; }
        // qCompress adds a size prefix and zlib wrapper; ZIP stores raw deflate.
        const auto packedSize = static_cast<quint32>(compressed.size() - 10);
        const auto crc = checksum(data);
        const auto offset = static_cast<quint32>(output.pos());
        QByteArray header;
        put32(header, 0x04034b50); put16(header, 20); put16(header, 0x800); put16(header, 8);
        put16(header, 0); put16(header, 0x21); put32(header, crc);
        put32(header, packedSize); put32(header, static_cast<quint32>(data.size()));
        put16(header, static_cast<quint16>(name.size())); put16(header, 0); header += name;
        if (output.write(header) != header.size() || output.write(compressed.constData() + 6, packedSize) != packedSize) {
            error = output.errorString(); return false;
        }
        put32(directory, 0x02014b50); put16(directory, 20); put16(directory, 20); put16(directory, 0x800); put16(directory, 8);
        put16(directory, 0); put16(directory, 0x21); put32(directory, crc);
        put32(directory, packedSize); put32(directory, static_cast<quint32>(data.size()));
        put16(directory, static_cast<quint16>(name.size())); put16(directory, 0); put16(directory, 0);
        put16(directory, 0); put16(directory, 0); put32(directory, 0); put32(directory, offset); directory += name;
    }
    const auto offset = static_cast<quint32>(output.pos());
    QByteArray end;
    put32(end, 0x06054b50); put16(end, 0); put16(end, 0);
    put16(end, static_cast<quint16>(files.size())); put16(end, static_cast<quint16>(files.size()));
    put32(end, static_cast<quint32>(directory.size())); put32(end, offset); put16(end, 0);
    if (output.write(directory) != directory.size() || output.write(end) != end.size() || !output.commit()) {
        error = output.errorString(); return false;
    }
    return true;
}

QList<BackupNote> inspectBackup(const QString &path, const QString &staging, QString &error)
{
    QList<BackupNote> notes;
    QSet<QString> ids;
    qint64 total = 0;
    auto stage = [&](const QByteArray &data, const NoteFile &note) {
        if (!withinHtmlLimits(note.html)) { error = text("TextLimit"); return false; }
        total += data.size();
        if (total > maxBackupBytes || notes.size() >= maxBackupFiles) { error = text("BackupTooLarge"); return false; }
        if (ids.contains(note.sceneId)) { error = text("BackupInvalid"); return false; }
        const auto target = QDir(staging).filePath(QString::number(notes.size()) + ".json");
        if (!saveNoteFile(target, data, error)) return false;
        ids.insert(note.sceneId);
        notes.push_back({target, note.sceneId});
        return true;
    };
    if (path.endsWith(".json", Qt::CaseInsensitive)) {
        QByteArray data; NoteFile note;
        if (!readJson(path, data, note, error) || !stage(data, note)) return {};
        return notes;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return {}; }
    auto invalid = [&]() -> QList<BackupNote> { error = text("BackupInvalid"); return {}; };
    if (file.size() < 22 || file.size() > maxArchiveBytes) return invalid();
    file.seek(qMax<qint64>(0, file.size() - 65557));
    const auto tail = file.readAll();
    qsizetype end = tail.size() - 22;
    for (; end >= 0; --end)
        if (u32(tail, end) == 0x06054b50 && end + 22 + u16(tail, end + 20) == tail.size()) break;
    if (end < 0 || u16(tail, end + 4) || u16(tail, end + 6) ||
        u16(tail, end + 8) != u16(tail, end + 10)) return invalid();
    const int count = u16(tail, end + 10);
    const qint64 directorySize = u32(tail, end + 12), directoryOffset = u32(tail, end + 16);
    const qint64 endOffset = file.size() - tail.size() + end;
    if (!count || count > maxBackupFiles || directorySize > 8 * 1024 * 1024 ||
        directoryOffset + directorySize != endOffset || !file.seek(directoryOffset)) return invalid();
    const auto directory = file.read(directorySize);
    qsizetype pos = 0;
    for (int i = 0; i < count; ++i) {
        if (pos + 46 > directory.size() || u32(directory, pos) != 0x02014b50) return invalid();
        const auto flags = u16(directory, pos + 8), method = u16(directory, pos + 10);
        const auto crc = u32(directory, pos + 16), packed = u32(directory, pos + 20), size = u32(directory, pos + 24);
        const auto nameSize = u16(directory, pos + 28), extraSize = u16(directory, pos + 30), commentSize = u16(directory, pos + 32);
        const qint64 offset = u32(directory, pos + 42);
        if (pos + 46 + nameSize + extraSize + commentSize > directory.size() || u16(directory, pos + 34) ||
            (flags & ~0x808u) || (method != 0 && method != 8) || packed > maxNoteBytes + 65536 || size > maxNoteBytes) return invalid();
        const auto rawName = directory.mid(pos + 46, nameSize);
        const auto name = QString::fromUtf8(rawName);
        pos += 46 + nameSize + extraSize + commentSize;
        if (name.endsWith('/')) continue;
        if (!name.endsWith(".json", Qt::CaseInsensitive) || name.contains('\\') || name.startsWith('/') ||
            name.contains(':') || name.split('/').contains("..") || offset + 30 > directoryOffset || !file.seek(offset)) return invalid();
        const auto header = file.read(30);
        if (header.size() != 30 || u32(header, 0) != 0x04034b50 || u16(header, 6) != flags || u16(header, 8) != method ||
            u16(header, 26) != nameSize || file.read(nameSize) != rawName) return invalid();
        const qint64 dataOffset = offset + 30 + nameSize + u16(header, 28);
        if (dataOffset + packed > directoryOffset || !file.seek(dataOffset)) return invalid();
        const auto input = file.read(packed);
        if (input.size() != packed) return invalid();
        QByteArray data;
        if (method == 0) { if (packed != size) return invalid(); data = input; }
        else {
            data.resize(static_cast<qsizetype>(size));
            z_stream stream{};
            stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
            stream.avail_in = packed;
            stream.next_out = reinterpret_cast<Bytef *>(data.data()); stream.avail_out = size;
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return invalid();
            const auto status = inflate(&stream, Z_FINISH);
            const bool valid = status == Z_STREAM_END && stream.total_out == size && stream.total_in == packed;
            inflateEnd(&stream);
            if (!valid) return invalid();
        }
        if (checksum(data) != crc) return invalid();
        NoteFile note;
        if (!decodeNote(data, note, error) || !stage(data, note)) return {};
    }
    if (pos != directory.size() || notes.isEmpty()) return invalid();
    return notes;
}
}
