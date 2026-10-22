#include "test-helpers.hpp"
#include "backup.hpp"
#include "dock.hpp"
#include "notes.hpp"
#include "locale.hpp"
#include <QApplication>
#include <QAbstractButton>
#include <QAction>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTextDocument>
#include <QTimer>
#include <cstdio>
#include <vector>

const char *obs_module_text(const char *key) {
    if (qstrcmp(key, "CharacterCount") == 0) return "%1 chars";
    if (qstrcmp(key, "CharacterRemaining") == 0) return "Used: %1 / %2; remaining: %3";
    if (qstrcmp(key, "ExportFilter") == 0) return "HTML (*.html);;Plain text (*.txt)";
    if (qstrcmp(key, "ImportFilter") == 0) return "Documents (*.docx *.html *.htm *.txt)";
    return key;
}
namespace {
using scenepad::test::require;
using scenepad::test::checks;
QByteArray read(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read fixture");
    return file.readAll();
}
void write(const QString &path, const QByteArray &data)
{
    QString error;
    require(scenepad::saveNoteFile(path, data, error), "write fixture");
}
int menu(scenepad::Dock &dock, const char *key, const QString &path)
{
    bool activated = false, selected = false, completed = false, unexpected = false;
    int ticks = 0;
    QTimer drive;
    drive.setInterval(2);
    QObject::connect(&drive, &QTimer::timeout, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (!widget->isVisible()) continue;
            if (qobject_cast<QProgressDialog *>(widget)) ++ticks;
            if (auto *popup = qobject_cast<QMenu *>(widget); popup && !activated) {
                auto *action = popup->findChild<QAction *>(QString("ctx") + key);
                if (!action || !action->isEnabled()) { unexpected = true; popup->close(); continue; }
                activated = true;
                QTimer::singleShot(0, popup, [&, action, popup] { action->trigger(); popup->close(); completed = true; });
            } else if (auto *dialog = qobject_cast<QFileDialog *>(widget); dialog && !selected) {
                selected = true;
                dialog->setDirectory(QFileInfo(path).absolutePath());
                dialog->selectFile(QFileInfo(path).fileName());
                if (auto *input = dialog->findChild<QLineEdit *>("fileNameEdit")) input->setText(path);
                QObject::connect(dialog, &QDialog::finished, [dialog](int result) {
                    std::fprintf(stderr, "File dialog result=%d selected=%s\n", result, qPrintable(dialog->selectedFiles().join(',')));
                });
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            } else if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                if (auto *yes = message->button(QMessageBox::Yes)) yes->click();
                else { unexpected = true; message->accept(); }
            }
        }
    });
    QEventLoop loop;
    bool done = false;
    QObject::connect(&drive, &QTimer::timeout, [&] {
        if (completed) { done = true; loop.quit(); }
    });
    drive.start();
    QTimer::singleShot(0, &dock, [&] {
        auto *editor = dock.findChild<QTextEdit *>("noteEditor");
        QMetaObject::invokeMethod(editor, "customContextMenuRequested", Q_ARG(QPoint, QPoint(8, 8)));
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(60000);
    loop.exec();
    require(done && selected && !unexpected, "backup/restore context menu and dialogs complete");
    return ticks;
}
void storage()
{
    QTemporaryDir temp;
    scenepad::NoteStore notes(temp.filePath("notes"));
    OBSSceneAutoRelease a = obs_scene_create("HOME"), b = obs_scene_create("One/Two"), c = obs_scene_create("One:Two");
    auto *source = obs_scene_get_source(a);
    const QString body = "<p><b>independent-body-marker</b></p>";
    require(notes.write(source, {body}), "save note");
    require(QFileInfo(notes.filePath(source)).fileName() == "HOME.json", "human-readable filename");
    require(notes.takeReferenceChanges(), "first save creates a reference");
    require(notes.read(source).html == body, "rich text roundtrip");
    require(notes.write(source, {body + "next"}) && !notes.takeReferenceChanges(), "body edits do not request scene saves");
    OBSDataAutoRelease saved = obs_save_source(source);
    const auto sceneJson = QByteArray(obs_data_get_json(saved));
    require(sceneJson.size() < 4096 && !sceneJson.contains("independent-body-marker") && sceneJson.contains("HOME.json"),
        "scene exports contain only a small note reference");
    obs_source_set_name(source, "Intro");
    require(notes.rename(source), "rename note");
    require(QFileInfo(notes.filePath(source)).fileName() == "Intro.json" && !QFileInfo::exists(temp.filePath("notes/HOME.json")), "rename moves the file");
    obs_source_set_name(source, "intro");
    require(notes.rename(source) && QFileInfo::exists(notes.filePath(source)), "case-only rename preserves the file");
    require(notes.read(source).html == body + "next", "rename preserves content");
    require(notes.write(obs_scene_get_source(b), {"<p>B</p>"}) && notes.write(obs_scene_get_source(c), {"<p>C</p>"}), "colliding names save");
    require(QFileInfo(notes.filePath(obs_scene_get_source(b))).fileName() == "One_Two.json" &&
        QFileInfo(notes.filePath(obs_scene_get_source(c))).fileName() == "One_Two_2.json", "sanitized collisions get separate filenames");
    require(scenepad::noteName("NUL") == "_NUL" && scenepad::noteName("  . ") == "Scene" &&
        scenepad::noteName("a<b:c/\\d?") == "a_b_c__d_", "filenames are portable");
    require(scenepad::noteName(QString(100, QChar(0x4e00))).toUtf8().size() <= 180, "long CJK names fit filesystem byte limits");
    OBSSceneAutoRelease clone = obs_scene_duplicate(a, "Duplicate", OBS_SCENE_DUP_PRIVATE_REFS);
    require(clone && notes.read(obs_scene_get_source(clone)).html.isEmpty(), "a duplicated scene cannot overwrite the original note");
    require(notes.write(obs_scene_get_source(clone), {"<p>clone</p>"}) && notes.read(source).html == body + "next", "duplicate saves independently");
    QString error;
    write(notes.filePath(source), "{broken");
    notes.read(source, &error);
    require(!error.isEmpty(), "corrupt note reports an error");
    write(temp.filePath("blocked"), "file");
    scenepad::NoteStore blocked(temp.filePath("blocked/notes"));
    require(!blocked.write(source, {"lost"}), "failed writes are reported");
    require(read(notes.filePath(source)) == "{broken", "failed write leaves the previous file intact");
}
void backupFixtures(const QString &folder)
{
    QTemporaryDir stage;
    QString error;
    auto entries = scenepad::inspectBackup(QDir(folder).filePath("backup-external.zip"), stage.path(), error);
    require(error.isEmpty() && entries.size() == 2, "stored/deflated ZIP entries and UTF-8 names import");
    for (const auto *name : {"backup-traversal.zip", "backup-duplicate.zip", "backup-corrupt.zip", "backup-oversized.zip", "backup-truncated.zip"}) {
        error.clear();
        require(scenepad::inspectBackup(QDir(folder).filePath(name), stage.path(), error).isEmpty() && !error.isEmpty(),
            "invalid archives are rejected before restoration");
    }
    require(!QFileInfo::exists(QDir(stage.path()).filePath("../escape.json")), "ZIP entries cannot escape staging");
    const auto oversized = stage.filePath("over-limit.json");
    const auto content = scenepad::encodeNote({"00000000-0000-4000-8000-000000000001", "Over limit", "<p>" + QString(10001, 'W') + "</p>"});
    write(oversized, content); error.clear();
    require(scenepad::inspectBackup(oversized, stage.path(), error).isEmpty() && error == "TextLimit" && read(oversized) == content,
        "restoration rejects over-limit text without truncating the backup");
}
void stress()
{
    QTemporaryDir temp;
    scenepad::NoteStore notes(temp.filePath("notes"));
    std::vector<OBSSceneAutoRelease> scenes;
    const QString html = "<p>" + QString(9999, 'W') + QString("</p><p>" + QString(9999, 'W')).repeated(19) + "</p>";
    QElapsedTimer clock; clock.start();
    for (int i = 0; i < 32; ++i) {
        scenes.emplace_back(obs_scene_create(QString("Large %1").arg(i).toUtf8().constData()));
        require(notes.write(obs_scene_get_source(scenes.back()), {html}), "large independent save");
    }
    const auto createMs = clock.elapsed();
    auto *source = obs_scene_get_source(scenes.front());
    clock.restart();
    qint64 peak = 0;
    for (int i = 0; i < 100; ++i) {
        QElapsedTimer single; single.start();
        require(notes.write(source, {html}) && notes.read(source).html == html, "large repeated save/read preserves content");
        peak = qMax(peak, single.elapsed());
    }
    const auto cyclesMs = clock.elapsed();
    OBSDataAutoRelease saved = obs_save_source(source);
    require(QByteArray(obs_data_get_json(saved)).size() < 4096, "large body does not enlarge scene exports");
    QStringList files;
    const QDir dir(notes.directory());
    for (const auto &file : dir.entryList({"*.json"}, QDir::Files)) files.append(dir.filePath(file));
    QString error;
    const auto zip = temp.filePath("all.zip");
    clock.restart();
    require(scenepad::backupNotes(zip, files, error), "boundary notes backup");
    const auto backupMs = clock.elapsed();
    QTemporaryDir staging;
    clock.restart();
    const auto entries = scenepad::inspectBackup(zip, staging.path(), error);
    require(error.isEmpty() && entries.size() == 32, "boundary backup validates and stages");
    const auto inspectMs = clock.elapsed();
    for (const auto &entry : entries) {
        scenepad::NoteFile note;
        require(scenepad::decodeNote(read(entry.path), note, error) && note.html == html, "every staged body matches");
    }
    std::printf("NOTES: 32 x 199999 characters create=%lldms; 100 save/read=%lldms peak=%lldms; ZIP=%lldms inspect=%lldms\n",
        createMs, cyclesMs, peak, backupMs, inspectMs);
    require(peak < 2000 && cyclesMs < 30000 && backupMs < 15000 && inspectMs < 15000, "large file operations stay within stress budgets");
}
void ui()
{
    QTemporaryDir temp;
    scenepad::Dock dock(temp.filePath("settings.ini"));
    OBSSceneAutoRelease a = obs_scene_create("UI first"), b = obs_scene_create("UI second");
    auto *source = obs_scene_get_source(a);
    dock.resize(600, 480); dock.bindScene(source); dock.show();
    auto *editor = dock.findChild<QTextEdit *>("noteEditor");
    editor->setPlainText(QString::fromUtf8("剧本 😀\nB\u2028C"));
    auto *characters = dock.findChild<QLabel *>("characterCount");
    require(characters && characters->text() == "9 chars" && characters->toolTip().contains(QLocale().toString(199991)),
        "character count and remaining capacity use UTF-16 units");
    require(dock.flush(), "save the initial note before simulating failure");
    const auto directory = dock.notes().directory(), held = directory + ".held";
    require(QDir().rename(directory, held), "hold original note files outside the failing path");
    { QFile blocked(directory); require(blocked.open(QIODevice::WriteOnly), "create a blocked note directory"); }
    editor->insertPlainText(" pending");
    require(!dock.flush(), "blocked storage fails without replacing the note");
    const auto pending = editor->toPlainText();
    dock.bindScene(obs_scene_get_source(b));
    require(dock.currentSceneId() == QString::fromUtf8(obs_source_get_uuid(source)) && editor->toPlainText() == pending,
        "failed save prevents scene switching and retains pending edits");
    auto *failure = dock.findChild<QLabel *>("saveError");
    require(failure && !failure->isHidden() && failure->text().contains("NoteSaveFailed"), "save failure stays visibly marked");
    editor->insertPlainText(" retry");
    require(!failure->isHidden(), "further typing does not dismiss an unsaved failure");
    require(QFile::remove(directory) && QDir().rename(held, directory) && dock.flush(), "a repaired path saves pending text");
    require(failure->isHidden() && dock.notes().read(source).html.contains("retry"), "successful saving clears the error and preserves pending edits");
    for (int i = 0; i < 100; ++i) {
        const auto text = QString::fromUtf8("Pending 剧本 😀 %1").arg(i);
        editor->setPlainText(text);
        auto *saved = i % 2 ? obs_scene_get_source(b) : source;
        auto *next = i % 2 ? source : obs_scene_get_source(b);
        dock.bindScene(next);
        QTextDocument stored; stored.setHtml(dock.notes().read(saved).html);
        require(stored.toPlainText() == text, "rapid scene switches flush pending Unicode edits without waiting for debounce");
    }
    dock.bindScene(source);
    editor->setHtml(QString::fromUtf8("<p><b>剧本 😀</b></p><p>Second<br>Third</p>"));
    const auto originalHtml = editor->toHtml(), originalText = editor->toPlainText();
    for (const bool plain : {false, true}) {
        const auto path = temp.filePath(plain ? "export-text" : "export-html");
        bool selected = false;
        QElapsedTimer clock; clock.start();
        QTimer drive;
        QObject::connect(&drive, &QTimer::timeout, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) if (auto *dialog = qobject_cast<QFileDialog *>(widget); dialog && dialog->isVisible()) {
                if (clock.elapsed() > 15000) { dialog->reject(); continue; }
                if (selected) continue;
                selected = true;
                dialog->selectNameFilter(plain ? "Plain text (*.txt)" : "HTML (*.html)");
                dialog->setDirectory(temp.path()); dialog->selectFile(QFileInfo(path).fileName());
                if (auto *input = dialog->findChild<QLineEdit *>("fileNameEdit")) input->setText(path);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            }
        });
        drive.start(2);
        dock.findChild<QAction *>("exportDocument")->trigger();
        const auto file = path + (plain ? ".txt" : ".html");
        require(selected && QFileInfo::exists(file), "one export button supports both file filters and appends the selected suffix");
        require(QString::fromUtf8(read(file)) == (plain ? originalText : originalHtml), "HTML and UTF-8 text exports retain Unicode and line breaks");
        require(editor->toHtml() == originalHtml, "export does not modify the note or its formatting");
    }
    const QString boundary = QString(10000, 'A') + ("\n" + QString(9999, 'B')).repeated(19);
    for (const bool oversized : {false, true}) {
        const auto path = temp.filePath("import.txt");
        write(path, QString(boundary + (oversized ? "x" : "")).replace("\n", "\r\n").toUtf8());
        bool selected = false;
        bool expired = false;
        QElapsedTimer clock; clock.start();
        QTimer drive;
        QObject::connect(&drive, &QTimer::timeout, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (!widget->isVisible()) continue;
                if (auto *dialog = qobject_cast<QFileDialog *>(widget); dialog && !selected) {
                    selected = true; dialog->setDirectory(temp.path()); dialog->selectFile("import.txt");
                    if (auto *input = dialog->findChild<QLineEdit *>("fileNameEdit")) input->setText(path);
                    QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
                } else if (auto *message = qobject_cast<QMessageBox *>(widget)) {
                    if (auto *yes = message->button(QMessageBox::Yes)) yes->click();
                    else { expired = true; message->accept(); }
                }
                if (clock.elapsed() > 15000) if (auto *dialog = qobject_cast<QDialog *>(widget)) { expired = true; dialog->reject(); }
            }
        });
        drive.start(2);
        dock.findChild<QAction *>("importDocument")->trigger();
        require(selected && !expired && editor->toPlainText() == boundary, "TXT import accepts the exact cap and preserves it when a larger import is rejected");
    }
    editor->setPlainText("First original");
    const auto json = temp.filePath("single.json"), zip = temp.filePath("all.zip");
    menu(dock, "BackupCurrent", json);
    if (!QFileInfo::exists(json)) std::fprintf(stderr, "Backup files: %s\n", qPrintable(QDir(temp.path()).entryList(QDir::Files).join(',')));
    require(QFileInfo::exists(json), "current backup exports JSON");
    dock.bindScene(obs_scene_get_source(b));
    editor->setPlainText("Second original");
    menu(dock, "RestoreBackup", json);
    require(editor->toPlainText() == "First original", "single JSON restores into the current scene");
    editor->setPlainText("Second original"); dock.flush();
    // Use valid notes to exercise worker progress without editor layout.
    QList<OBSScene> largeNotes;
    const auto largeHtml = "<p>" + QString(9998, 'W') + QString("</p><p>" + QString(9998, 'W')).repeated(9) + "</p>";
    for (int i = 0; i < 512; ++i) {
        OBSSceneAutoRelease large = obs_scene_create(qPrintable(QString("UI large %1").arg(i)));
        require(dock.notes().write(obs_scene_get_source(large), {largeHtml}), "inactive boundary note");
        largeNotes.push_back(OBSScene(large));
    }
    const auto backupTicks = menu(dock, "BackupAll", zip);
    require(QFileInfo::exists(zip), "all backup exports ZIP");
    editor->setPlainText("Changed"); dock.flush();
    const auto restoreTicks = menu(dock, "RestoreBackup", zip);
    require(editor->toPlainText() == "Second original" && dock.notes().read(source).html.contains("First original"), "ZIP restores matching scenes and refreshes the editor");
    std::printf("NOTES UI: backup ticks=%d; restore ticks=%d\n", backupTicks, restoreTicks);
    require(backupTicks > 0 && restoreTicks > 0, "UI timers keep running during backup and restore file jobs");
    editor->setPlainText(QString::fromUtf8("Final pending 剧本 😀"));
    dock.shutdown();
    scenepad::Dock reopened(temp.filePath("settings.ini"));
    reopened.bindScene(obs_scene_get_source(b));
    require(reopened.findChild<QTextEdit *>("noteEditor")->toPlainText() == QString::fromUtf8("Final pending 剧本 😀"),
        "shutdown flushes pending edits before the debounce timer fires");
}
}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (!obs_startup("en-US", nullptr, nullptr)) return 2;
    int result = 0;
    try {
        if (!app.arguments().contains("--ui-only")) {
            storage(); backupFixtures(QFileInfo(QString::fromUtf8(argv[1])).absolutePath()); stress();
        }
        ui();
        std::printf("PASS: %d independent storage, backup and restore checks\n", checks);
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL notes: %s\n", e.what()); result = 1; }
    obs_shutdown();
    return result;
}
