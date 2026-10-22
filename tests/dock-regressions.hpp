#pragma once
#include <QDockWidget>
#include <QInputMethodEvent>
#include <QMainWindow>

void dockRegressions(QApplication &app, const QString &preview)
{
    QTemporaryDir temp;
    require(temp.isValid(), "focused temporary directory");
    OBSSceneAutoRelease scene = obs_scene_create("ScenePad focused"), other = obs_scene_create("Other");
    auto *source = obs_scene_get_source(scene);
    QMainWindow window;
    auto *container = new QDockWidget("ScenePad", &window);
    auto *dock = new scenepad::Dock(temp.filePath("settings.ini"), container);
    container->setWidget(dock); window.addDockWidget(Qt::BottomDockWidgetArea, container);
    window.resize(900, 420); dock->bindScene(source); window.show(); window.activateWindow(); waitEvents(40);
    auto *editor = dock->findChild<QTextEdit *>("noteEditor");
    auto *search = dock->findChild<QLineEdit *>("searchText");
    const QColor markerColor(dock->palette().color(QPalette::Window).lightness() < 128 ? "#ffb347" : "#b85b00");
    auto type = [&](const QString &text) { QInputMethodEvent event; event.setCommitString(text); QApplication::sendEvent(editor, &event); waitEvents(5); };
    auto press = [&](int code) { QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier); QApplication::sendEvent(editor, &event); waitEvents(5); };

    editor->setPlainText("prefix tail"); auto cursor = editor->textCursor(); cursor.setPosition(6); editor->setTextCursor(cursor); editor->setFocus();
    search->setText("kept search"); waitEvents(5);
    auto *toolbar = dock->findChild<QToolBar *>("formatToolbar");
    auto *action = dock->findChild<QAction *>("insertMark");
    auto *button = qobject_cast<QToolButton *>(toolbar->widgetForAction(action));
    const auto center = button->rect().center();
    QMouseEvent down(QEvent::MouseButtonPress, center, button->mapToGlobal(center), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent up(QEvent::MouseButtonRelease, center, button->mapToGlobal(center), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(button, &down); QApplication::sendEvent(button, &up); waitEvents(40);
    require(editor->toPlainText() == QString::fromUtf8("prefix◆[] tail"), "marker preserves surrounding text");
    require(editor->hasFocus() && editor->textCursor().position() == 8 && search->text() == "kept search", "marker focuses the brackets after mouse release and queued events");
    require(!dock->findChild<QWidget *>("markNoteEditor"), "mark notes use body editing");
    type(QString::fromUtf8("Camera 中文"));
    require(editor->toPlainText() == QString::fromUtf8("prefix◆[Camera 中文] tail"), "IME enters the note in the body");
    require(editor->document()->find(QString::fromUtf8("Camera 中文")).charFormat().foreground().color() == markerColor, "note inherits marker color");
    press(Qt::Key_Right); type("X");
    require(editor->toPlainText() == QString::fromUtf8("prefix◆[Camera 中文]X tail"), "typing continues outside the brackets");
    require(editor->document()->find("X").charFormat().foreground().color() != markerColor, "marker color does not leak");
    editor->undo(); auto end = editor->textCursor(); end.setPosition(6 + int(QString::fromUtf8("◆[Camera 中文]").size())); editor->setTextCursor(end);
    press(Qt::Key_Backspace);
    require(editor->toPlainText() == "prefix tail", "Backspace after a mark removes the whole note");
    editor->undo(); require(editor->toPlainText() == QString::fromUtf8("prefix◆[Camera 中文] tail"), "whole-note deletion is undoable");
    dock->flush(); dock->bindScene(obs_scene_get_source(other)); dock->bindScene(source);
    cursor = editor->document()->find(QString::fromUtf8("Camera 中文")); cursor.setPosition(cursor.selectionEnd()); editor->setTextCursor(cursor); editor->setFocus(); type("!");
    require(editor->toPlainText().contains(QString::fromUtf8("◆[Camera 中文!]")) && !search->hasFocus(), "saved notes remain directly editable");
    std::printf("PASS MARK: docked mouse click, body focus, IME, color, deletion, undo and reload\n");

    dock->flush(); const auto path = dock->notes().filePath(source);
    require(QFile::remove(path), "delete the current JSON fixture");
    editor->moveCursor(QTextCursor::End); type("memory"); require(dock->flush() && QFileInfo::exists(path), "an in-memory note recreates its deleted file");
    require(QFile::remove(path), "delete the saved JSON fixture");
    dock->bindScene(obs_scene_get_source(other)); dock->bindScene(source);
    require(!editor->isReadOnly() && !dock->noteUnavailable() && editor->document()->isEmpty(), "missing referenced JSON opens empty and editable");
    QString error; require(dock->notes().read(source, &error).html.isEmpty() && error.isEmpty() && !QFileInfo::exists(path), "reading a missing note does not write an empty file");
    editor->setFocus(); type("rebuilt"); require(dock->flush() && dock->notes().filePath(source) == path && QFileInfo::exists(path), "editing rebuilds the same JSON path");
    require(scenepad::saveNoteFile(path, "{broken", error), "write invalid JSON fixture");
    dock->bindScene(obs_scene_get_source(other)); dock->bindScene(source);
    require(editor->isReadOnly() && dock->noteUnavailable(), "invalid JSON retains read-only protection");
    QFile corrupt(path); require(corrupt.open(QIODevice::ReadOnly) && corrupt.readAll() == "{broken", "invalid JSON was not overwritten"); corrupt.close();
    require(QFile::remove(path), "delete a failed note fixture"); dock->bindScene(source);
    require(!editor->isReadOnly() && !dock->noteUnavailable(), "the same scene recovers after a failed note is deleted");
    require(dock->notes().write(source, {"<p>theme fixture</p>"}), "restore a valid note fixture");
    dock->bindScene(obs_scene_get_source(other)); dock->bindScene(source);
    std::printf("PASS FILE: deletion while loaded, missing JSON, lazy recreation, same path and corrupt-file protection\n");

    app.setStyle("Fusion");
    for (const bool light : {false, true}) {
        auto palette = app.palette(); const QColor text(light ? "#202020" : "#eeeeee"), accent(light ? "#315ecb" : "#579be6");
        palette.setColor(QPalette::Window, QColor(light ? "#f1f1f1" : "#303030")); palette.setColor(QPalette::WindowText, text); palette.setColor(QPalette::Text, text); palette.setColor(QPalette::Highlight, accent); app.setPalette(palette);
        app.setStyleSheet(QString("QLabel { color: %1; } QToolButton { padding: 3px; }").arg(text.name()));
        auto *name = dock->findChild<QLabel *>("sceneLabel");
        for (const bool locked : {true, false, true}) {
            if (dock->isLocked() != locked) dock->command(scenepad::Command::ToggleLock);
            waitEvents(25); const auto expected = locked ? accent : text; const auto pixels = name->grab().toImage(); int matches = 0;
            for (int y=0;y<pixels.height();++y) for (int x=0;x<pixels.width();++x) {
                const auto ink=pixels.pixelColor(x,y); if (qAbs(ink.red()-expected.red())<4 && qAbs(ink.green()-expected.green())<4 && qAbs(ink.blue()-expected.blue())<4) ++matches;
            }
            require(matches > 2, "painted scene title follows lock and theme despite QLabel stylesheet rules");
        }
        if (!preview.isEmpty()) require(window.grab().save(preview + (light ? "-light.png" : "-dark.png")), "save locked theme evidence");
    }
    std::printf("PASS THEME: rendered title pixels in dark/light themes, lock/unlock and live palette changes\n");
    dock->shutdown(); window.close();
}
