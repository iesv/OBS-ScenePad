#include "test-helpers.hpp"
#include "backup.hpp"
#include "dock.hpp"
#include "locale.hpp"
#include "remote.hpp"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <cstdio>
#include <functional>

const char *obs_module_text(const char *key) { return key; }
namespace {
using scenepad::test::require;
using scenepad::test::checks;
QList<obs_source_t *> scenes;
obs_source_t *program = nullptr, *preview = nullptr;
bool studio = false;
using scenepad::test::wait;
QString sceneId(obs_source_t *source) { return QString::fromUtf8(obs_source_get_uuid(source)); }
void age(const QString &path, int days)
{
    QFile file(path);
    require(file.open(QIODevice::ReadWrite) && file.setFileTime(QDateTime::currentDateTimeUtc().addDays(-days),
        QFileDevice::FileModificationTime), "set a test note's age");
}
QByteArray bytes(const QString &path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "read test note"); return file.readAll();
}
void about(scenepad::Dock &dock, const std::function<void(QDialog *)> &edit)
{
    auto *editor = dock.findChild<QTextEdit *>("noteEditor");
    bool opened = false;
    dock.activateWindow(); wait(30);
    scenepad::test::contextMenu(editor, [&](QMenu *menu) {
        require(menu != nullptr, "open context menu");
        auto *action = menu->findChild<QAction *>("ctxAbout");
        require(action && action->isEnabled(), "About action is available");
        QTimer::singleShot(20, &dock, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            require(dialog && dialog->objectName() == "aboutDialog", "open About settings");
            opened = true;
            edit(dialog);
        });
        action->trigger();
    });
    require(opened, "About was inspected");
}
void configure(scenepad::Dock &dock, int days, bool all, const QSet<QString> &selection = {}, bool cancel = false)
{
    about(dock, [&](QDialog *dialog) {
        auto *period = dialog->findChild<QSpinBox *>("autoClearDays");
        auto *scope = dialog->findChild<QComboBox *>("autoClearScope");
        period->setValue(days);
        scope->setCurrentIndex(all ? 1 : 0);
        if (days && !all) {
            QTimer::singleShot(0, dialog, [&] {
                auto *choose = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                auto *list = choose->findChild<QListWidget *>("autoClearScenes");
                require(list && list->count() == scenes.size(), "selection lists the current collection");
                for (int i = 0; i < list->count(); ++i) {
                    auto *item = list->item(i);
                    require(item->text() == QString::fromUtf8(obs_source_get_name(scenes[i])) && item->data(Qt::UserRole).toString() == sceneId(scenes[i]),
                        "selection follows OBS order and stable scene IDs");
                    item->setCheckState(selection.contains(item->data(Qt::UserRole).toString()) ? Qt::Checked : Qt::Unchecked);
                }
                choose->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
            });
            dialog->findChild<QPushButton *>("autoClearChoose")->click();
        }
        dialog->findChild<QDialogButtonBox *>()->button(cancel ? QDialogButtonBox::Cancel : QDialogButtonBox::Ok)->click();
    });
}
void sweep(scenepad::Dock &dock)
{
    auto *timer = dock.findChild<QTimer *>("autoClearTimer");
    timer->start(0);
    QElapsedTimer clock; clock.start();
    while (clock.elapsed() < 5000 && timer->interval() != 86400000) wait(2);
    require(timer->isActive() && timer->interval() == 86400000, "one sweep schedules the next daily check");
}
}

// Supply a collection and the program/preview state without opening OBS.
void obs_frontend_get_scenes(obs_frontend_source_list *list)
{
    for (auto *scene : scenes) { auto *reference = obs_source_get_ref(scene); da_push_back(list->sources, &reference); }
}
obs_source_t *obs_frontend_get_current_scene() { return obs_source_get_ref(program); }
obs_source_t *obs_frontend_get_current_preview_scene() { return obs_source_get_ref(preview); }
bool obs_frontend_preview_program_mode_active() { return studio; }
void obs_frontend_save() {}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    if (!obs_startup("en-US", nullptr, nullptr)) return 2;
    int result = 0;
    try {
        QTemporaryDir temp;
        OBSSceneAutoRelease a = obs_scene_create("On air"), b = obs_scene_create("Selected"), c = obs_scene_create("Unselected"),
            d = obs_scene_create("Old"), e = obs_scene_create("Recent");
        scenes = {obs_scene_get_source(c), obs_scene_get_source(b), obs_scene_get_source(a), obs_scene_get_source(d), obs_scene_get_source(e)};
        program = obs_scene_get_source(a);
        scenepad::Dock dock(temp.filePath("settings.ini"));
        dock.show();
        auto &notes = dock.notes();
        auto *timer = dock.findChild<QTimer *>("autoClearTimer");
        for (auto *source : scenes) { require(notes.write(source, {"<p>Keep this body</p>"}), "save fixture"); age(notes.filePath(source), 7); }
        age(notes.filePath(obs_scene_get_source(e)), 4);
        require(!timer->isActive(), "clearing waits for OBS to finish loading");
        dock.frontendEvent(OBS_FRONTEND_EVENT_FINISHED_LOADING);
        require(!timer->isActive(), "automatic clearing is off by default");
        about(dock, [&](QDialog *dialog) {
            auto *days = dialog->findChild<QSpinBox *>("autoClearDays");
            require(days->value() == 0 && days->minimum() == 0 && days->maximum() == 36500 &&
                !dialog->findChild<QComboBox *>("autoClearScope")->isEnabled() &&
                !dialog->findChild<QPushButton *>("autoClearChoose")->isEnabled(), "clearing defaults to off and supports custom day counts");
            dialog->reject();
        });
        QString error;
        const auto backup = temp.filePath("kept.json");
        require(scenepad::backupNotes(backup, {notes.filePath(obs_scene_get_source(b))}, error), "create an independent backup");
        const auto backupData = bytes(backup);
        configure(dock, 5, false, {sceneId(program), sceneId(obs_scene_get_source(b))});
        auto *selected = obs_scene_get_source(b);
        const auto edited = QFileInfo(notes.filePath(selected)).lastModified();
        obs_source_set_name(selected, "Selected renamed");
        dock.sceneRenamed(selected);
        require(QFileInfo(notes.filePath(selected)).lastModified() == edited, "renaming preserves the last body-save time");
        sweep(dock);
        require(notes.read(selected).html.isEmpty() && !notes.read(program).html.isEmpty(), "selected expired notes clear while the on-air scene stays intact");
        require(!notes.read(obs_scene_get_source(c)).html.isEmpty() && bytes(backup) == backupData, "unselected notes and backups stay intact");
        const auto readTime = QFileInfo(notes.filePath(program)).lastModified();
        dock.flush(); dock.bindScene(program);
        require(QFileInfo(notes.filePath(program)).lastModified() == readTime, "reading and clean flushes do not restart the period");
        configure(dock, 5, true, {}, true);
        QSettings stored(temp.filePath("settings.ini"), QSettings::IniFormat);
        require(!stored.value("autoClear/all").toBool(), "Cancel leaves the saved scope unchanged");
        configure(dock, 5, true);
        OBSSceneAutoRelease added = obs_scene_create("New scene");
        auto *newSource = obs_scene_get_source(added);
        scenes.append(newSource);
        require(notes.write(newSource, {"<p>New expired body</p>"}), "save a newly added scene"); age(notes.filePath(newSource), 7);
        sweep(dock);
        require(notes.read(newSource).html.isEmpty() && notes.read(obs_scene_get_source(c)).html.isEmpty() &&
            !notes.read(obs_scene_get_source(e)).html.isEmpty(), "all-scenes policy includes new scenes and keeps recent notes");
        for (int days : {7, 10, 20, 30}) {
            require(notes.write(obs_scene_get_source(c), {"<p>Not due</p>"}) && notes.write(obs_scene_get_source(d), {"<p>Due</p>"}), "save period boundaries");
            age(notes.filePath(obs_scene_get_source(c)), days - 1); age(notes.filePath(obs_scene_get_source(d)), days + 1);
            configure(dock, days, true); sweep(dock);
            require(!notes.read(obs_scene_get_source(c)).html.isEmpty() && notes.read(obs_scene_get_source(d)).html.isEmpty(), "custom periods respect independent save times");
        }
        configure(dock, 20, true);
        for (auto *source : scenes) { require(notes.write(source, {"<p>Protected fixture</p>"}), "refresh protected fixtures"); age(notes.filePath(source), 21); }
        studio = true; preview = selected;
        dock.bindScene(obs_scene_get_source(e));
        auto *remote = dock.remote();
        require(remote->listen("127.0.0.1", 0), "start a loopback remote session");
        const auto access = QUrl(remote->url()).fragment();
        auto request = [&](const QString &action, QJsonObject data = {}) { data.insert("access", access); data.insert("action", action); return remote->handle(data); };
        const auto session = request("Open", {{"name", "Test phone"}}).value("session").toString();
        const auto remoteId = sceneId(obs_scene_get_source(c));
        auto document = request("GetDocument", {{"session", session}, {"scene", remoteId}});
        require(request("AcquireEdit", {{"session", session}, {"scene", remoteId}, {"epoch", document.value("epoch")}, {"revision", document.value("revision")}}).value("ok").toBool(), "acquire a remote editing turn");
        sweep(dock);
        require(!notes.read(program).html.isEmpty() && !notes.read(preview).html.isEmpty() &&
            !notes.read(obs_scene_get_source(e)).html.isEmpty() && !notes.read(obs_scene_get_source(c)).html.isEmpty() &&
            notes.read(obs_scene_get_source(d)).html.isEmpty(), "program, preview, Dock and remote editing scenes are protected");
        request("ReleaseEdit", {{"session", session}});
        studio = false; dock.bindScene(program);
        sweep(dock);
        require(!notes.read(obs_scene_get_source(c)).html.isEmpty(), "an active browser reader also protects its scene");
        request("GetDocument", {{"session", session}, {"scene", sceneId(program)}});
        sweep(dock);
        const auto cleared = request("GetDocument", {{"session", session}, {"scene", remoteId}});
        require(notes.read(obs_scene_get_source(c)).html.isEmpty() && cleared.value("revision").toInt() > document.value("revision").toInt(),
            "released inactive scenes clear and notify remote readers");
        const auto badPath = notes.filePath(selected);
        require(scenepad::saveNoteFile(badPath, "{broken", error), "save a corrupt fixture");
        sweep(dock);
        require(bytes(badPath) == "{broken" && !dock.findChild<QLabel *>("saveError")->isVisible(), "recent files are not parsed by a sweep");
        age(badPath, 21); sweep(dock);
        require(bytes(badPath) == "{broken" && dock.findChild<QLabel *>("saveError")->isVisible(), "corrupt expired notes report a failure without erasing data");
        require(notes.write(selected, {}), "repair the test note");
        QDialog modal(&dock); modal.setModal(true); modal.show(); wait(5);
        timer->start(0); wait(20);
        require(timer->interval() == 60000, "modal file operations defer automatic clearing"); modal.hide();
        dock.frontendEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING);
        require(!timer->isActive(), "collection changes cancel the pending sweep");
        dock.frontendEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED);
        require(timer->isActive() && timer->interval() == 5000, "a loaded collection schedules a delayed check");
        configure(dock, 0, true);
        require(!timer->isActive(), "disabling clears the daily timer");
        QList<OBSScene> pressure;
        const auto largeHtml = "<p>" + QString(9999, 'W') + QString("</p><p>" + QString(9999, 'W')).repeated(19) + "</p>";
        for (int i = 0; i < 32; ++i) {
            OBSSceneAutoRelease scene = obs_scene_create(qPrintable(QString("Expiry pressure %1").arg(i)));
            pressure.append(OBSScene(scene));
            auto *source = obs_scene_get_source(scene);
            scenes.append(source);
            require(notes.write(source, {largeHtml}), "save a boundary-sized expiry fixture"); age(notes.filePath(source), 2);
        }
        configure(dock, 1, true);
        int ticks = 0;
        qint64 gapPeak = 0;
        QElapsedTimer beat; beat.start();
        QTimer heartbeat;
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { gapPeak = qMax(gapPeak, beat.restart()); ++ticks; });
        heartbeat.start(1);
        QElapsedTimer clock; clock.start();
        sweep(dock);
        const auto sweepMs = clock.elapsed();
        heartbeat.stop();
        for (const auto &scene : pressure) require(notes.read(obs_scene_get_source(scene)).html.isEmpty(), "every inactive expired boundary note is cleared");
        const bool activePreserved = !notes.read(program).html.isEmpty();
        std::printf("AUTO-CLEAR: sweep=%lldms UI-ticks=%d UI-gap-peak=%lldms active-preserved=%d\n", sweepMs, ticks, gapPeak, activePreserved);
        require(ticks > 10 && gapPeak < 250 && sweepMs < 5000 && activePreserved, "expiry yields to UI timers and preserves the active scene under load");
        auto *recent = obs_scene_get_source(pressure.front());
        require(notes.write(recent, {largeHtml}), "save a recent boundary note");
        const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-1);
        clock.restart();
        for (int i = 0; i < 1000; ++i) require(!notes.clearExpired(recent, cutoff), "recent boundary notes are not parsed or cleared");
        const auto checksMs = clock.elapsed();
        require(checksMs < 2000 && notes.read(recent).html == largeHtml, "metadata-only expiry checks stay within the stress budget");
        std::printf("AUTO-CLEAR: 32 x 199999 characters sweep=%lldms UI-ticks=%d UI-gap-peak=%lldms; 1000 recent-note checks=%lldms\n", sweepMs, ticks, gapPeak, checksMs);
        configure(dock, 0, true);
        dock.shutdown();
        require(!timer->isActive() && bytes(backup) == backupData, "shutdown stops checks and preserves the backup");
        std::printf("PASS: %d auto-clear scope, age, rename, protection, persistence and failure checks\n", checks);
        scenes.clear(); program = preview = nullptr;
    } catch (const std::exception &e) { std::fprintf(stderr, "FAIL auto-clear: %s\n", e.what()); result = 1; scenes.clear(); program = preview = nullptr; }
    obs_shutdown();
    return result;
}
