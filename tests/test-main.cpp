#include "test-helpers.hpp"
#include "dock.hpp"
#include "editor.hpp"
#include "icons.hpp"
#include "docx.hpp"
#include "notes.hpp"
#include "remote.hpp"
#include "locale.hpp"
#include <QAction>
#include <QApplication>
#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QColorDialog>
#include <QCheckBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTextBlock>
#include <QTextList>
#include <QTextLayout>
#include <QToolButton>
#include <QTimer>
#include <QToolBar>
#include <QToolTip>
#include <QWheelEvent>
#include <QtMath>
#include <obs.hpp>
#include <cstdio>

namespace {
using scenepad::test::require;
using scenepad::test::checks;
lookup_t *locale = nullptr;
QRect geometryIn(QWidget *widget, QWidget *parent) { return QRect(widget->mapTo(parent, QPoint()), widget->size()); }
void waitEvents(int milliseconds) { scenepad::test::wait(milliseconds, 2); }
void waitSearch(scenepad::Dock &dock)
{
    auto *status = dock.findChild<QLabel *>("searchStatus");
    QElapsedTimer timer; timer.start();
    while (!status->isVisible() && timer.elapsed() < 2000) waitEvents(10);
}
void key(QWidget *widget, int code, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, code, mods);
    QApplication::sendEvent(widget, &event);
}
void wheel(QTextEdit *editor, int delta, Qt::KeyboardModifiers modifiers)
{
    QWheelEvent event(QPointF(10, 10), editor->viewport()->mapToGlobal(QPoint(10, 10)),
        QPoint(), QPoint(0, delta), Qt::NoButton, modifiers, Qt::NoScrollPhase, false);
    QApplication::sendEvent(editor->viewport(), &event);
}
}

const char *obs_module_text(const char *key)
{
    const char *out = key;
    if (locale) text_lookup_getstr(locale, key, &out);
    return out;
}

#include "return.hpp"
#include "editing.hpp"
#include "dock-regressions.hpp"

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
    QApplication::setEffectEnabled(Qt::UI_FadeMenu, false);
#ifdef __APPLE__
    auto font = app.font();
    font.setPointSize(9);
    app.setFont(font);
#else
    app.setFont(QFont("Segoe UI", 9));
#endif
    if (argc > 4) {
        app.setStyle("Fusion");
        auto palette = app.palette();
        palette.setColor(QPalette::Window, QColor("#2b2b2b"));
        palette.setColor(QPalette::WindowText, QColor("#eeeeee"));
        palette.setColor(QPalette::Base, QColor("#242424"));
        palette.setColor(QPalette::Text, QColor("#eeeeee"));
        palette.setColor(QPalette::Button, QColor("#353535"));
        palette.setColor(QPalette::ButtonText, QColor("#eeeeee"));
        app.setPalette(palette);
    }
    // Include OBS toolbar margins and item limits, not only generic button padding.
    app.setStyleSheet("QWidget { selection-background-color: #336699; selection-color: #ffffff; } QSpinBox { padding: 4px 8px; min-height: 20px; } QLineEdit { padding: 4px 8px; min-height: 20px; border: 1px solid palette(mid); border-radius: 4px; } QToolButton { padding: 3px; } QToolBar { margin: 4px 0; spacing: 4px; } QToolBar QToolButton { margin: 1px; margin-top: 2px; padding: 6px; max-width: 16px; max-height: 16px; icon-size: 16px; }");
    locale = text_lookup_create(SCENEPAD_LOCALE_DIR "/en-US.ini");
    if (argc > 2 && app.arguments().value(1) != "--focused-editing") text_lookup_add(locale, argv[2]);
    if (!obs_startup("en-US", nullptr, nullptr)) return 2;
    int result = 0;
    try {
        if (app.arguments().value(1) == "--return-only") {
            returnChecks(QString());
            obs_shutdown();text_lookup_destroy(locale);return 0;
        }
        editingChecks(app, app.arguments().value(1) == "--focused-editing" ? app.arguments().value(2) : (argc > 4 ? app.arguments().value(4) + ".editing" : QString()));
        if (app.arguments().value(1) == "--focused-editing") {
            obs_shutdown(); text_lookup_destroy(locale); return 0;
        }
        {
            scenepad::Editor editor;
            editor.setFont(QFont("Arial", 12));
            editor.show();
            const auto sample = QString::fromUtf8("alpha Bravo charlie delta, echo. (Foxtrot) hotel. 他说：“你好，世界！”（继续阅读）测试。 ").repeated(12);
            const auto latin = [](QChar c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
            for (const int width : {180, 240, 360}) {
                editor.resize(width, 250);
                editor.setPlainText(sample);
                editor.document()->documentLayout()->documentSize();
                QApplication::processEvents();
                const auto *layout = editor.document()->begin().layout();
                bool words = true, punctuation = true;
                for (int i = 0; i < layout->lineCount(); ++i) {
                    const auto line = layout->lineAt(i);
                    const int start = line.textStart();
                    const auto part = sample.mid(start, line.textLength()).trimmed();
                    words = words && (!start || !latin(sample[start - 1]) || !latin(sample[start]));
                    punctuation = punctuation && (part.isEmpty() ||
                        (!QString::fromUtf8("，。！？；：、）》」』】”’").contains(part.front()) &&
                         !QString::fromUtf8("（《「『【“‘").contains(part.back())));
                }
                require(layout->lineCount() > 1 && words && punctuation, "natural wrapping preserves words and CJK punctuation");
                require(editor.toPlainText() == sample && editor.document()->blockCount() == 1, "visual wrapping preserves the stored paragraph");
                editor.setPlainText(QString(9998, 'W'));
                editor.document()->documentLayout()->documentSize();
                layout = editor.document()->begin().layout();
                bool fits = layout->lineCount() > 1;
                for (int i = 0; i < layout->lineCount(); ++i)
                    fits = fits && layout->lineAt(i).naturalTextWidth() <= editor.viewport()->width();
                require(fits, "overlong words wrap within the viewport");
            }
        }
        QTemporaryDir temp;
        scenepad::NoteStore notes(temp.filePath("notes"));
        require(temp.isValid(), "temporary directory");
        OBSSceneAutoRelease sceneA = obs_scene_create("ScenePad test A");
        OBSSceneAutoRelease sceneB = obs_scene_create("ScenePad test B");
        auto *a = obs_scene_get_source(sceneA);
        auto *b = obs_scene_get_source(sceneB);
        const scenepad::Note original{"<p><b>hello</b> <span style='color:#ff0000'>red</span></p>"};
        notes.write(a, original);
        require(notes.read(a).html == original.html, "rich HTML storage");
        obs_source_set_name(a, "ScenePad renamed");
        require(notes.read(a).html == original.html, "rename keeps note");
        require(notes.read(b).html.isEmpty(), "scene isolation");
        OBSDataAutoRelease serialized = obs_save_source(a);
        const auto json = QByteArray(obs_data_get_json(serialized));
        require(json.contains("scenepad") && !json.contains("hello"), "scene exports contain references without document bodies");
        OBSDataAutoRelease copied = obs_data_create_from_json(json.constData());
        OBSSourceAutoRelease restored = obs_load_source(copied);
        require(restored != nullptr, "import source");
        require(notes.read(restored).html.isEmpty(), "import with an existing UUID cannot share its note file");
        OBSDataAutoRelease external;
        {
            OBSSceneAutoRelease originalScene = obs_scene_create("External note roundtrip");
            notes.write(obs_scene_get_source(originalScene), original);
            external = obs_save_source(obs_scene_get_source(originalScene));
            obs_source_remove(obs_scene_get_source(originalScene));
        }
        obs_queue_task(OBS_TASK_DESTROY, [](void *) {}, nullptr, true);
        OBSSourceAutoRelease externalScene = obs_load_source(external);
        require(externalScene && notes.read(externalScene).html == original.html,
            "scene reload resolves its independent note when the UUID is available");

        {
            QSettings preferences(temp.filePath("prefs.ini"), QSettings::IniFormat);
            preferences.setValue("startCountdown", false);
            preferences.setValue("locked", true);
            preferences.sync();
            scenepad::Dock dock(temp.filePath("prefs.ini"));
            dock.resize(500, 480);
            dock.show();
            dock.bindScene(a);
            auto *editor = dock.findChild<QTextEdit *>("noteEditor");
            auto *bold = dock.findChild<QAction *>("formatBold");
            auto *read = dock.findChild<QToolButton *>("readMode");
            require(editor && bold && read, "UI controls exist");
            require(read->isChecked() && read->icon().pixmap(QSize(24, 24), QIcon::Normal, QIcon::On).toImage() !=
                read->icon().pixmap(QSize(24, 24), QIcon::Normal, QIcon::Off).toImage(),
                "loaded reading lock shows its checked state and distinct lock/unlock icons");
            require(read->defaultAction()->shortcuts().isEmpty(), "reading lock has no keyboard shortcut");
            auto clickBody = [&](Qt::KeyboardModifiers modifiers) {
                auto *view = editor->viewport();
                const QPointF local(8, 8), global = view->mapToGlobal(QPoint(8, 8));
                QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, modifiers);
                QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, modifiers);
                QApplication::sendEvent(view, &press); QApplication::sendEvent(view, &release);
            };
            const auto lockHtml = editor->toHtml();
            clickBody(Qt::AltModifier);
            require(!read->isChecked() && !editor->isReadOnly() && editor->toHtml() == lockHtml, "Alt click unlocks without changing the script");
            clickBody(Qt::AltModifier);
            require(read->isChecked() && editor->isReadOnly(), "Alt click restores the shared lock");
            clickBody(Qt::ControlModifier); clickBody(Qt::AltModifier | Qt::ShiftModifier);
            require(read->isChecked() && editor->isReadOnly(), "other modified clicks leave the shared lock unchanged");
            dock.resize(860, 180);
            waitEvents(30);
            auto *controls = dock.findChild<QWidget *>("bottomControls");
            auto *play = dock.findChild<QToolButton *>("scrollPlay");
            auto *slider = dock.findChild<QSlider *>("scrollSlider");
            auto *speed = dock.findChild<QDoubleSpinBox *>("scrollSpeed");
            const int wideSearchWidth = dock.findChild<QLineEdit *>("searchText")->width();
            const int wideDockWidth = dock.width();
            auto *formatTools = dock.findChild<QToolBar *>("formatToolbar");
            const int wideToolbarWidth = formatTools->width();
            dock.resize(1060, 180);
            waitEvents(20);
            const int dockGrowth = dock.width() - wideDockWidth;
            const int searchGrowth = dock.findChild<QLineEdit *>("searchText")->width() - wideSearchWidth;
            if (formatTools->width() != wideToolbarWidth || dockGrowth < 100 || qAbs(searchGrowth - dockGrowth) > 2)
                std::fprintf(stderr, "HEADER: dock=%d search=%d toolbar=%d/%d\n", dockGrowth, searchGrowth, wideToolbarWidth, formatTools->width());
            require(formatTools->width() == wideToolbarWidth && dockGrowth >= 100 && qAbs(searchGrowth - dockGrowth) <= 2,
                "spare header width goes to search without stretching the toolbar");
            dock.resize(860, 180);
            waitEvents(20);
            require(speed && speed->minimum() == 0.1 && speed->maximum() == 5.0 &&
                speed->suffix() == QString::fromUtf8("×") && dock.speed() == 0.5 && slider->value() == 5 && slider->minimum() == 1 && slider->maximum() == 50,
                "speed starts at 0.5x within the 0.1-5.0x range");
            for (int step = 1; step <= 50; ++step) {
                slider->setValue(step);
                require(qAbs(dock.speed() - step / 10.0) < 0.0001 && speed->value() == dock.speed(), "all slider speeds reach the input");
                speed->setValue((51 - step) / 10.0);
                require(slider->value() == 51 - step, "all input speeds reach the slider");
            }
            dock.setSpeed(0);
            dock.command(scenepad::Command::Slower);
            require(dock.speed() == 0.1 && slider->value() == 1, "speed clamps to 0.1x");
            dock.command(scenepad::Command::Faster);
            require(dock.speed() == 0.2 && slider->value() == 2, "slow speeds support 0.1x steps");
            dock.setSpeed(qQNaN());
            require(dock.speed() == 0.5 && slider->value() == 5, "invalid speed returns to the default");
            dock.setSpeed(9.0);
            require(dock.speed() == 5.0 && slider->value() == 50, "speed clamps to 5x");
            slider->setValue(23);
            require(dock.speed() == 2.3 && speed->value() == 2.3, "slider updates the multiplier");
            speed->setValue(4.2);
            require(dock.speed() == 4.2 && slider->value() == 42, "multiplier input updates the slider");
            require(controls && controls->height() <= 44, "bottom controls occupy one compact row");
            require(play->parentWidget() == dock.findChild<QWidget *>("readingControls") && slider->parentWidget() == play->parentWidget() && speed->parentWidget() == play->parentWidget(),
                "scroll controls share one row");
            auto *topControls = dock.findChild<QWidget *>("topControls");
            require(dock.findChild<QToolBar *>("formatToolbar")->parentWidget() == topControls,
                "editing tools stay at the top");
            require(speed->width() < controls->width() / 3, "speed input remains compact");
            require(dock.findChild<QLabel *>("saveStatus")->parentWidget() == topControls,
                "save status sits beside the scene name");
            auto *sceneName = dock.findChild<QLabel *>("sceneLabel");
            dock.command(scenepad::Command::ToggleLock);
            require(sceneName->font().bold(), "scene name remains bold");
            if (sceneName->palette().color(QPalette::WindowText) != dock.palette().color(QPalette::WindowText))
                qWarning() << "TITLE PALETTE" << sceneName->palette().color(QPalette::WindowText) << dock.palette().color(QPalette::WindowText);
            require(sceneName->palette().color(QPalette::WindowText) == dock.palette().color(QPalette::WindowText),
                "unlocked scene name uses the normal theme text color");
            dock.command(scenepad::Command::ToggleLock);
            require(sceneName->palette().color(QPalette::WindowText) == dock.palette().color(QPalette::Highlight), "locked scene name uses the theme accent");
            dock.command(scenepad::Command::ToggleLock);
            require(sceneName->palette().color(QPalette::WindowText) == dock.palette().color(QPalette::WindowText), "unlock restores the normal scene name color");
            dock.command(scenepad::Command::ToggleLock);
            require(dock.findChild<QLabel *>("scenePrefix")->text() == "[" &&
                dock.findChild<QLabel *>("saveStatus")->text() == "]", "scene name is enclosed in brackets");
            auto *formatToolbar = dock.findChild<QToolBar *>("formatToolbar");
            auto *paragraphToolbar = dock.findChild<QToolBar *>("paragraphToolbar");
            require(paragraphToolbar && paragraphToolbar->parentWidget() == play->parentWidget(),
                "paragraph buttons stay in the bottom toolbar");
            bool uniformButtons = controls->height() >= play->height() && play->size() == QSize(28, 24) && read->size() == play->size() &&
                dock.findChild<QToolButton *>("scrollTop")->size() == play->size();
            for (auto *bar : {formatToolbar, paragraphToolbar})
                for (auto *action : bar->actions())
                    if (auto *button = qobject_cast<QToolButton *>(bar->widgetForAction(action))) {
                        uniformButtons = uniformButtons && button->size() == play->size() && button->iconSize() == play->iconSize();
                        if (button->icon().isNull()) continue;
                        const auto image = button->icon().pixmap(48).toImage();
                        QRect ink;
                        for (int y = 0; y < image.height(); ++y)
                            for (int x = 0; x < image.width(); ++x)
                                if (image.pixelColor(x, y).alpha() > 64) ink |= QRect(x, y, 1, 1);
                        require(!ink.isEmpty() && qAbs(ink.center().x() - image.rect().center().x()) <= 1 &&
                            qAbs(ink.center().y() - image.rect().center().y()) <= 1, "toolbar artwork is centered inside its icon canvas");
                    }
            require(uniformButtons, "bottom buttons have matching compact dimensions and icon sizes");
            auto probePalette = dock.palette(); probePalette.setColor(QPalette::WindowText, Qt::magenta);
            for (auto *bar : {formatToolbar, paragraphToolbar}) {
                auto *row = bar == formatToolbar ? topControls : play->parentWidget();
                const int center = row->rect().center().y();
                require(qAbs(geometryIn(bar, row).center().y() - center) <= 1, "toolbar is centered in the complete control row");
                for (auto *action : bar->actions()) {
                    auto *button = qobject_cast<QToolButton *>(bar->widgetForAction(action));
                    if (!button || !button->isVisible()) continue;
                    if (qAbs(geometryIn(button, row).center().y() - center) > 1)
                        qWarning() << "Row center" << row->rect() << "button" << action->objectName() << geometryIn(button, row);
                    require(qAbs(geometryIn(button, row).center().y() - center) <= 1, "toolbar button is vertically centered in its complete row");
                    const auto originalIcon = button->icon();
                    button->setIcon(scenepad::makeIcon(scenepad::Icon::Mark, probePalette));
                    const auto image = button->grab().toImage(); button->setIcon(originalIcon);
                    QRect ink;
                    for (int y = 0; y < image.height(); ++y)
                        for (int x = 0; x < image.width(); ++x) {
                            const auto pixel = image.pixelColor(x, y);
                            if (pixel.red() > pixel.green() + 20 && pixel.blue() > pixel.green() + 20) ink |= QRect(x, y, 1, 1);
                        }
                    require(!ink.isEmpty() && qAbs(ink.center().y() - image.rect().center().y()) <= qCeil(image.devicePixelRatio()),
                        "painted button icon stays vertically centered under OBS toolbar constraints");
                }
            }
            require(read->geometry().left() < play->geometry().left(), "reading lock is the leftmost control");
            auto *fontSize = dock.findChild<QSpinBox *>("fontSize");
            auto *fontInput = fontSize->findChild<QLineEdit *>();
            const int initialSize = fontSize->value();
            fontSize->setValue(144);
            waitEvents(10);
            require(fontInput && fontInput->contentsRect().height() >= fontInput->fontMetrics().height() &&
                fontInput->contentsRect().width() >= fontInput->fontMetrics().horizontalAdvance("144") &&
                fontSize->suffix().isEmpty() && fontSize->width() <= 60,
                "three-digit font size remains readable with OBS-style padding");
            fontSize->setValue(initialSize);
            dock.resize(360, 220);
            waitEvents(30);
            require(dock.width() == 700 && dock.height() == 300, "undersized dock requests clamp to 700 by 300 logical pixels");
            if (!controls->rect().contains(geometryIn(play, controls)) || !controls->rect().contains(geometryIn(speed, controls)) ||
                !controls->rect().contains(geometryIn(read, controls)))
                qWarning() << "Scroll row" << controls->rect() << "play" << play->geometry() << "speed" << speed->geometry() << "lock" << read->geometry();
            require(controls->rect().contains(geometryIn(play, controls)) &&
                controls->rect().contains(geometryIn(speed, controls)) && controls->rect().contains(geometryIn(read, controls)),
                "narrow dock retains all scrolling controls in one row");
            require(controls->rect().contains(geometryIn(dock.findChild<QCheckBox *>("startCountdown"), controls)),
                "countdown checkbox fits inside a narrow dock");
            require(dock.findChild<QLineEdit *>("searchText")->width() < wideSearchWidth,
                "search width adapts to the available header space");
            dock.resize(500, 480);
            waitEvents(30);
            if (argc > 3) require(dock.findChild<QToolButton *>("scrollPlay")->text() == app.arguments().value(3),
                "localized interface label");
            const auto *import = dock.findChild<QAction *>("importDocument");
            const auto *exportAction = dock.findChild<QAction *>("exportDocument");
            require(import && exportAction && !import->isEnabled() && exportAction->isEnabled(),
                "the shared lock blocks imports and preserves exports");
            editor->selectAll();
            bool translatedMenu = false, readOnlyMenu = false, countdownInMenu = false, findInMenu = false, qrInMenu = false;
            const bool isolatedClipboard = app.platformName() == "minimal";
            if (isolatedClipboard) QApplication::clipboard()->clear();
            scenepad::test::contextMenu(editor, [&](QMenu *menu) {
                require(!menu->findChild<QAction *>("ctxGoTo"), "context menu omits inline navigation");
                const auto entries=menu->actions();
                for(int i=1;i<entries.size();++i)
                    require(!entries[i-1]->isSeparator() || !entries[i]->isSeparator(), "context menu has no adjacent separators");
                auto *copy = menu->findChild<QAction *>("ctxCopy");
                auto *cut = menu->findChild<QAction *>("ctxCut");
                auto *paste = menu->findChild<QAction *>("ctxPaste");
                auto *undo = menu->findChild<QAction *>("ctxUndo");
                auto *clear = menu->findChild<QAction *>("ctxClearBody");
                auto *plainPaste = menu->findChild<QAction *>("ctxPastePlain");
                translatedMenu = copy && copy->text().startsWith(scenepad::text("Copy") + "\t") &&
                    undo && undo->text().startsWith(scenepad::text("Undo") + "\t") && plainPaste &&
                    plainPaste->text() == scenepad::text("PastePlain") + "\t" +
                        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_V).toString(QKeySequence::NativeText);
                for (auto *action : menu->actions()) {
                    if (action->text() == scenepad::text("Countdown")) countdownInMenu = true;
                    if (action->objectName() == "findText") findInMenu = true;
                    if (action->text() == scenepad::text("RemoteQR")) qrInMenu = true;
                }
                readOnlyMenu = copy && copy->isEnabled() && cut && !cut->isEnabled() && paste && !paste->isEnabled() && clear && !clear->isEnabled();
                if (copy && isolatedClipboard) copy->trigger();
                if (argc > 4) menu->grab().save(app.arguments().value(4) + ".context-menu.png");
            });
            require(translatedMenu, "context menu follows the OBS locale");
            require(readOnlyMenu, "read-only context menu allows copying and prevents editing");
            require(!countdownInMenu, "countdown is a toolbar checkbox rather than a context menu entry");
            require(!findInMenu && !qrInMenu, "context menu omits Find and hides QR while LAN access is disabled");
            if (isolatedClipboard) require(QApplication::clipboard()->text() == "hello red", "context menu copies selected text");
            bool aboutVersion = false, aboutLink = false, helpTable = false;
            auto inspectDialog = [&](const char *key, auto inspect) {
                scenepad::test::contextMenu(editor, [&, key, inspect](QMenu *menu) {
                    for (auto *action : menu->actions()) {
                        if (action->text() != scenepad::text(key)) continue;
                        QTimer::singleShot(20, &dock, [&, inspect] {
                            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                                inspect(dialog);
                                if (dialog->isVisible()) dialog->accept();
                            }
                        });
                        action->trigger();
                        break;
                    }
                });
            };
            inspectDialog("About", [&](QDialog *about) {
                auto *help = about->findChild<QLabel *>("ShortcutBody");
                require(help != nullptr, "About help is available");
                QTextDocument document;
                document.setHtml(about->findChild<QLabel *>("AboutBody")->text());
                const auto lines = document.toPlainText().split('\n', Qt::SkipEmptyParts);
                aboutVersion = lines.size() >= 3 && lines[0] == scenepad::text("ScenePad") && lines[1].contains(SCENEPAD_VERSION);
                helpTable = help->text().contains("<table") &&
                    help->text().contains(QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText)) &&
                    !help->text().contains("F3") && !help->text().contains("Esc");
                for (auto *label : about->findChildren<QLabel *>()) {
                    aboutLink = aboutLink || (label->text().contains("href=\"https://haliluya.net\"") &&
                        label->openExternalLinks() && label->textInteractionFlags().testFlag(Qt::LinksAccessibleByMouse));
                }
                if (argc > 4) about->grab().save(app.arguments().value(4) + ".about.png");
            });
            require(aboutVersion, "About shows the plugin name and version on separate lines");
            require(aboutLink, "About provides a clickable author website link");
            require(helpTable, "About includes the aligned shortcut help without removed key bindings");
            const auto originalPalette = dock.palette();
            auto iconPalette = originalPalette;
            iconPalette.setColor(QPalette::WindowText, QColor("#e0e0e0"));
            dock.setPalette(iconPalette);
            const auto lightIcon = play->icon().pixmap(24).toImage();
            iconPalette.setColor(QPalette::WindowText, QColor("#303030"));
            dock.setPalette(iconPalette);
            const auto darkIcon = play->icon().pixmap(24).toImage();
            require(!lightIcon.isNull() && lightIcon != darkIcon, "icons redraw after theme palette changes");
            dock.setPalette(originalPalette);
            read->click();
            require(!editor->isReadOnly(), "unlock editor");
            auto confirmClear = [&](QMessageBox::StandardButton choice, bool lock = false) {
                bool prompted = false;
                inspectDialog("ClearBody", [&](QDialog *dialog) {
                    auto *box = qobject_cast<QMessageBox *>(dialog);
                    require(box && box->text() == scenepad::text("ClearBodyPrompt") &&
                        box->defaultButton() == box->button(QMessageBox::No), "clearing asks for confirmation with No as the default");
                    prompted = true;
                    if (argc > 4) box->grab().save(app.arguments().value(4) + ".clear-confirm.png");
                    if (lock) dock.command(scenepad::Command::ToggleLock);
                    box->button(choice)->click();
                });
                require(prompted, "clear confirmation was inspected");
            };
            const auto beforeClear = editor->toHtml();
            confirmClear(QMessageBox::No);
            require(editor->toHtml() == beforeClear, "cancelling clear preserves the whole document");
            confirmClear(QMessageBox::Yes, true);
            require(dock.isLocked() && editor->toHtml() == beforeClear, "locking during confirmation prevents clearing");
            read->click();
            const auto largeClear = QString(9999, 'W') + QString("\n" + QString(9999, 'W')).repeated(19);
            editor->setPlainText(largeClear);
            require(dock.flush(false), "save a boundary-sized clear fixture");
            const auto otherNote = notes.read(b).html;
            QElapsedTimer clearClock; clearClock.start();
            confirmClear(QMessageBox::Yes);
            const auto clearMs = clearClock.elapsed();
            QTextDocument clearedNote; clearedNote.setHtml(notes.read(a).html);
            require(editor->document()->isEmpty(), "confirmed clear empties the editor");
            require(clearedNote.isEmpty(), "confirmed clear persists the empty script");
            require(notes.read(b).html == otherNote, "clearing preserves other scene scripts");
            require(!QApplication::activeModalWidget(), "successful clearing adds no dialog");
            bool emptyClearDisabled = false;
            scenepad::test::contextMenu(editor, [&](QMenu *menu) {
                auto *clear = menu->findChild<QAction *>("ctxClearBody");
                emptyClearDisabled = clear && !clear->isEnabled();
            });
            require(emptyClearDisabled, "empty scripts keep a disabled Clear action");
            editor->undo();
            require(editor->toPlainText() == largeClear && dock.flush(false), "one undo restores all cleared text and saves it");
            require(clearMs < 5000, "clearing a boundary-sized script stays within the stress budget");
            std::printf("MANUAL-CLEAR: 199999 characters confirmation/delete/save=%lldms\n", clearMs);
            const auto notePath = notes.filePath(a);
            require(QFile::rename(notePath, notePath + ".kept") && QDir().mkdir(notePath), "block the test note's save destination");
            confirmClear(QMessageBox::Yes);
            require(editor->toPlainText() == largeClear && !dock.findChild<QLabel *>("saveError")->isVisible() &&
                !QApplication::activeModalWidget(), "failed clear restores the text without a failure notification");
            require(QDir().rmdir(notePath) && QFile::rename(notePath + ".kept", notePath), "restore the test note's save destination");
            editor->setPlainText("alpha");
            editor->selectAll();
            fontSize->setValue(144);
            fontSize->setFocus();
            fontSize->selectAll();
            QKeyEvent fontDigits(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier, "10");
            QApplication::sendEvent(fontInput, &fontDigits);
            key(fontInput, Qt::Key_Return);
            waitEvents(20);
            require(fontSize->value() == 10 && fontInput->text() == "10" && editor->currentCharFormat().fontPointSize() == 10,
                "enter commits the typed font size to the selected text");
            require(!fontInput->hasSelectedText(), "font size has no leftover selection after committing");
            if (argc > 4) fontSize->grab().save(app.arguments().value(4) + ".font-size.png");
            bold->trigger();
            auto chooseColor = [&](const char *name, const QColor &color) {
                bool opened = false;
                QTimer::singleShot(20, &dock, [&, color] {
                    if (auto *dialog = qobject_cast<QColorDialog *>(QApplication::activeModalWidget())) {
                        opened = true; dialog->setCurrentColor(color); dialog->accept();
                    }
                });
                dock.findChild<QAction *>(name)->trigger();
                require(opened, "format color dialog opened");
            };
            chooseColor("textColor", QColor("#ff0000"));
            require(editor->textCursor().hasSelection() && editor->palette().color(QPalette::HighlightedText) == QColor("#ff0000"),
                "selected text previews its new color without losing the selection");
            chooseColor("highlight", QColor("#fff200")); waitEvents(10);
            require(editor->textCursor().hasSelection() && editor->palette().color(QPalette::Highlight) == QColor("#fff200"),
                "selected text previews its new highlight color");
            const auto colorImage = editor->viewport()->grab().toImage();
            bool redVisible = false;
            for (int y = 0; y < colorImage.height() && !redVisible; ++y)
                for (int x = 0; x < colorImage.width(); ++x) {
                    const auto pixel = colorImage.pixelColor(x, y);
                    if (pixel.red() > pixel.green() + 40 && pixel.red() > pixel.blue() + 40) { redVisible = true; break; }
                }
            require(redVisible, "the new text color is visible inside the active selection");
            if (argc > 4) dock.grab().save(app.arguments().value(4) + ".selected-color.png");
            editor->moveCursor(QTextCursor::End);
            require(editor->palette().color(QPalette::HighlightedText) != QColor("#ff0000"), "color preview clears when selection changes");
            dock.flush(false);
            OBSDataAutoRelease immediateExport = obs_save_source(a);
            require(!QByteArray(obs_data_get_json(immediateExport)).contains("alpha"), "scene serialization excludes document bodies");
            require(notes.read(a).html.contains("font-weight:700"), "toolbar formats selected text");
            dock.bindScene(b);
            require(editor->toPlainText().isEmpty(), "switch loads separate scene");
            editor->setPlainText("beta");
            dock.bindScene(a);
            require(editor->toPlainText() == "alpha", "switch preserves previous note");
            QTextDocument doc;
            doc.setHtml(notes.read(b).html);
            require(doc.toPlainText() == "beta", "switch flushes unsaved edits");
            dock.findChild<QAction *>("alignCenter")->trigger();
            require(editor->alignment().testFlag(Qt::AlignHCenter) && dock.findChild<QAction *>("alignCenter")->isChecked() &&
                !dock.findChild<QAction *>("alignLeft")->isChecked(), "center alignment button and exclusive selection");
            dock.findChild<QAction *>("increaseIndent")->trigger();
            require(editor->textCursor().blockFormat().indent() == 1, "increase indentation button");
            dock.findChild<QAction *>("decreaseIndent")->trigger();
            require(editor->textCursor().blockFormat().indent() == 0, "decrease indentation button");
            editor->selectAll();
            dock.findChild<QAction *>("clearFormat")->trigger();
            require(editor->document()->find("alpha").charFormat().fontWeight() < QFont::Bold, "clear formatting button");
            editor->moveCursor(QTextCursor::End);
            dock.findChild<QAction *>("insertTime")->trigger();
            require(QRegularExpression("\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2} ").match(editor->toPlainText()).hasMatch(),
                "timestamp button inserts both the current date and time");
            if (isolatedClipboard) {
                QTextCharFormat plainFormat;
                plainFormat.setFontWeight(QFont::Normal);
                editor->mergeCurrentCharFormat(plainFormat);
                QTextEdit clipboardSource;
                clipboardSource.setHtml("<b>plain paste</b>");
                clipboardSource.selectAll();
                clipboardSource.copy();
                require(QApplication::clipboard()->text() == "plain paste", "clipboard plain text is ready");
                bool pasteAvailable = false;
                scenepad::test::contextMenu(editor, [&](QMenu *menu) {
                    if (auto *paste = menu->findChild<QAction *>("ctxPastePlain")) {
                        pasteAvailable = paste->isEnabled();
                        paste->trigger();
                    }
                });
                const auto plain = editor->document()->find("plain paste");
                require(pasteAvailable, "context plain paste action is enabled");
                require(!plain.isNull(), "context menu inserts clipboard text");
                require(plain.charFormat().fontWeight() < QFont::Bold, "context menu pastes text without HTML formatting");
            } else {
                std::printf("SKIP: system clipboard mutation; covered by isolated-platform tests\n");
            }
            editor->setPlainText("Alpha target beta target gamma");
            read->click();
            const auto htmlBeforeSearch = editor->toHtml();
            auto *find = dock.findChild<QAction *>("findText");
            auto *search = dock.findChild<QLineEdit *>("searchText");
            waitEvents(20);
            require(find && search && search->isVisible() && search->parentWidget() == topControls &&
                search->geometry().right() == topControls->contentsRect().right() && find->shortcut() == QKeySequence::Find,
                "search stays at the top right and provides Ctrl+F");
            require(search->placeholderText().contains(find->shortcut().toString(QKeySequence::NativeText)),
                "search placeholder shows the platform-native shortcut");
            find->trigger();
            search->setText("unfinished query");
            search->setText("target");
            auto *noMatch = dock.findChild<QLabel *>("searchStatus");
            require(!noMatch->isVisible(), "rapid query changes defer counting until typing settles");
            waitSearch(dock);
            require(search->isVisible() && editor->isReadOnly() && editor->textCursor().selectionStart() == 6,
                "search finds text in reading mode");
            require(noMatch->text() == "1/2", "search field displays the current match and total count");
            const auto searchHtml = editor->toHtml();
            require(editor->palette().color(QPalette::Active, QPalette::Highlight) == QColor("#fff200") &&
                editor->palette().color(QPalette::Inactive, QPalette::Highlight) == QColor("#fff200") &&
                editor->palette().color(QPalette::Inactive, QPalette::HighlightedText) == QColor("#202126"),
                "search uses contrasting colors while the search field has focus");
            const auto matchImage = editor->viewport()->grab().toImage();
            int yellowPixels = 0, darkPixels = 0;
            for (int y = 0; y < matchImage.height(); ++y)
                for (int x = 0; x < matchImage.width(); ++x) {
                    const auto color = matchImage.pixelColor(x, y);
                    yellowPixels += color == QColor("#fff200");
                    darkPixels += color == QColor("#202126");
                }
            require(yellowPixels > 10 && darkPixels > 0, "search paints yellow and dark text despite the theme selection stylesheet");
            dock.findChild<QToolButton *>("findNext")->click();
            require(editor->textCursor().selectionStart() == 18 && noMatch->text() == "2/2", "search advances and updates its cached match index");
            dock.findChild<QToolButton *>("findNext")->click();
            require(editor->textCursor().selectionStart() == 6, "forward search wraps to the first match");
            dock.findChild<QToolButton *>("findPrevious")->click();
            require(editor->toHtml() == searchHtml, "search colors never modify saved text formatting");
            require(editor->textCursor().selectionStart() == 18, "backward search wraps to the final match");
            key(search, Qt::Key_Return);
            require(editor->textCursor().selectionStart() == 6, "Enter in the search field advances to the next match");
            search->setText("GAMMA");
            waitSearch(dock);
            require(editor->textCursor().selectedText() == "gamma", "search ignores letter case");
            if (argc > 4) dock.grab().save(app.arguments().value(4) + ".search-count.png");
            search->setText("missing phrase");
            waitSearch(dock);
            require(noMatch->isVisible() && noMatch->parentWidget() == search && noMatch->text() == "0/0" &&
                search->accessibleDescription() == scenepad::text("NoMatch") &&
                editor->toHtml() == htmlBeforeSearch, "missing search reports status without changing rich text");
            if (argc > 4) {
                waitEvents(20);
                dock.grab().save(app.arguments().value(4) + ".search.png");
            }
            dock.resize(360, 220);
            waitEvents(20);
            const auto margins = search->textMargins();
            require(dock.width() == 700 && topControls->rect().contains(search->geometry()) &&
                search->width() - margins.left() - margins.right() >= search->fontMetrics().horizontalAdvance("MM"),
                "compact search preserves two readable query characters beside its feedback and buttons");
            if (argc > 4) dock.grab().save(app.arguments().value(4) + ".search-narrow.png");
            dock.resize(500, 480);
            waitEvents(20);
            key(editor, Qt::Key_Escape);
            bool removedBindings = true;
            for (auto *action : dock.findChildren<QAction *>())
                for (const auto &shortcut : action->shortcuts())
                    removedBindings = removedBindings && shortcut != QKeySequence(Qt::Key_F3) &&
                        shortcut != QKeySequence(Qt::SHIFT | Qt::Key_F3) && shortcut != QKeySequence(Qt::Key_Escape);
            require(removedBindings && search->text() == "missing phrase", "F3, Shift+F3 and Escape are not bound by the plugin");
            find->trigger();
            bool embeddedButtons = true;
            for (const auto *name : {"findPrevious", "findNext", "clearSearch"}) {
                auto *button = dock.findChild<QToolButton *>(name);
                embeddedButtons = embeddedButtons && button && button->parentWidget() == search && search->rect().contains(button->geometry());
            }
            require(embeddedButtons, "previous, next and clear buttons are inside the search field");
            dock.findChild<QToolButton *>("clearSearch")->click();
            require(search->isVisible() && search->text().isEmpty() && !noMatch->isVisible(), "clear search removes query and no-match feedback");
            read->click();
            QString longText;
            for (int i = 0; i < 120; ++i) longText += QString::fromUtf8("第 %1 行：阅读带保持位置，文字平稳向上滚动。\n").arg(i + 1);
            editor->setPlainText(longText);
            editor->selectAll();
            editor->setFontPointSize(18);
            editor->moveCursor(QTextCursor::Start);
            dock.setSpeed(2.5);
            waitEvents(60);
            dock.command(scenepad::Command::Top);
            const auto htmlBeforeScroll = editor->toHtml();
            QElapsedTimer scrollElapsed;
            dock.startScroll();
            scrollElapsed.start();
            require(dock.isScrolling() && editor->isReadOnly(), "scroll starts in read mode");
            auto *band = dock.findChild<QWidget *>("readingBand");
            auto *guide = dock.findChild<QToolButton *>("readingGuide");
            require(band && band->isVisible() && band->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                qAbs(band->geometry().center().y() - qRound(editor->viewport()->height() * 0.382)) <= 1,
                "reading band is centered at the upper golden section without intercepting input");
            guide->click();
            require(!band->isVisible() && dock.isScrolling(), "reading band can be hidden without pausing");
            guide->click();
            require(band->isVisible() && dock.isScrolling(), "reading band can be restored during scrolling");
            waitEvents(300);
            const int firstPosition = editor->verticalScrollBar()->value();
            if (qAbs(firstPosition - scrollElapsed.elapsed() * 25.0 / 1000.0) > 2.0)
                std::fprintf(stderr, "Scroll: position=%d elapsed=%lld scrolling=%d\n", firstPosition, scrollElapsed.elapsed(), dock.isScrolling());
            require(qAbs(firstPosition - scrollElapsed.elapsed() * 25.0 / 1000.0) <= 2.0,
                "2.5x scroll advances at 25 pixels per second (1x baseline is 10)");
            require(editor->toHtml() == htmlBeforeScroll, "reading band does not change saved or exported HTML");
            wheel(editor, -120, Qt::NoModifier);
            waitEvents(180);
            const int manualPosition = editor->verticalScrollBar()->value();
            require(dock.isScrolling() && manualPosition > firstPosition, "manual wheel preserves playback mode");
            waitEvents(600);
            require(editor->verticalScrollBar()->value() > manualPosition, "playback continues after wheel easing");
            waitEvents(650);
            require(dock.isScrolling() && editor->verticalScrollBar()->value() > manualPosition, "playback resumes from the manually chosen position");
            auto *scrollBar = editor->verticalScrollBar();
            scrollBar->setSliderDown(true); scrollBar->setValue(manualPosition / 2);
            waitEvents(1100);
            require(dock.isScrolling() && scrollBar->value() == manualPosition / 2, "dragging the thumb keeps playback active without fighting the drag");
            scrollBar->setSliderDown(false); waitEvents(600);
            require(scrollBar->value() == manualPosition / 2, "thumb release starts a fresh hold");
            waitEvents(650);
            require(scrollBar->value() > manualPosition / 2 && dock.isScrolling(), "playback continues after thumb release");
            if (argc > 4) {
                dock.resize(760, 320);
                waitEvents(30);
                require(dock.grab().save(app.arguments().value(4) + ".scroll.png"), "save active reading-band preview");
                dock.resize(500, 480);
                waitEvents(10);
            }
            key(editor, Qt::Key_Space);
            require(!dock.isScrolling(), "space pauses");
            require(!band->isVisible(), "paused scrolling hides the reading band");
            const int paused = editor->verticalScrollBar()->value();
            waitEvents(90);
            require(editor->verticalScrollBar()->value() == paused, "pause holds position");
            key(editor, Qt::Key_Plus);
            require(dock.speed() == 2.6, "keyboard speed up by 0.1x");
            key(editor, Qt::Key_Minus);
            require(dock.speed() == 2.5, "keyboard speed down by 0.1x");
            key(editor, Qt::Key_Space);
            require(dock.isScrolling(), "space resumes");
            dock.bindScene(b);
            require(!dock.isScrolling(), "scene switch pauses scroll");
            dock.bindScene(a);
            waitEvents(30);
            require(editor->verticalScrollBar()->value() >= paused, "scene reading position restored");
            auto *bar = editor->verticalScrollBar();
            editor->document()->documentLayout()->documentSize();
            waitEvents(20);
            bar->setValue(bar->maximum() - 3);
            dock.setSpeed(4.0);
            const int nearEnd = bar->value(), endMaximum = bar->maximum();
            dock.startScroll();
            const int startedAt = bar->value();
            QElapsedTimer endWait; endWait.start();
            while (dock.isScrolling() && endWait.elapsed() < 2000) waitEvents(10);
            if (dock.isScrolling() || bar->value() != bar->maximum())
                std::fprintf(stderr, "Scroll end: before=%d started=%d final=%d max=%d -> %d scrolling=%d\n",
                    nearEnd, startedAt, bar->value(), endMaximum, bar->maximum(), dock.isScrolling());
            require(!dock.isScrolling() && bar->value() == bar->maximum(), "end automatically stops");
            dock.command(scenepad::Command::Top);
            require(bar->value() == 0, "return to top");
            bar->setValue(42);
            dock.startScroll();
            dock.hide();
            require(!dock.isScrolling(), "hidden dock pauses");
            dock.shutdown();
            const auto stoppedSpeed = dock.speed();
            dock.command(scenepad::Command::Faster);
            dock.startScroll();
            require(dock.speed() == stoppedSpeed && !dock.isScrolling(), "shutdown prevents queued commands from restarting the dock");
        }
        {
            scenepad::Dock dock(temp.filePath("prefs.ini"));
            require(dock.speed() == 0.5, "reopening resets the multiplier to 0.5x after adjustment");
            dock.resize(500, 480);
            dock.bindScene(a);
            dock.show();
            waitEvents(40);
            require(dock.findChild<QTextEdit *>("noteEditor")->verticalScrollBar()->value() == 42,
                "reopening restores the scene reading position from local preferences");
            notes.write(b, {"<p>collection note</p>"});
            dock.bindScene(b);
            auto *editor = dock.findChild<QTextEdit *>("noteEditor");
            require(editor->toPlainText() == "collection note", "collection note is loaded");
            require(dock.isLocked() && editor->isReadOnly(), "the shared lock persists and follows scene changes");
            dock.command(scenepad::Command::ToggleLock);
            require(!dock.isLocked() && !editor->isReadOnly(), "unlocking restores editing for the new scene");
            editor->setPlainText("collection edit");
            dock.flush(false);
            require(notes.read(b).html.contains("collection edit"), "editing saves directly in the independent note file");
            obs_source_remove(b);
            dock.bindScene(nullptr);
            require(!editor->isEnabled(), "deleted scene disables editor safely");
        }
        {
            OBSSceneAutoRelease scene = obs_scene_create("List selection");
            scenepad::Dock dock(temp.filePath("list-selection.ini"));
            dock.bindScene(obs_scene_get_source(scene));
            auto *editor = dock.findChild<QTextEdit *>("noteEditor");
            auto selectFirst = [&] {
                QTextCursor cursor(editor->document());
                cursor.setPosition(0);
                cursor.setPosition(editor->document()->begin().next().position(), QTextCursor::KeepAnchor);
                editor->setTextCursor(cursor);
            };
            editor->setHtml("<ul><li>First</li><li>Second</li></ul>");
            selectFirst();
            dock.findChild<QToolButton *>("listStyle")->menu()->actions().last()->trigger();
            require(!editor->document()->begin().textList() && editor->document()->begin().next().textList(),
                "removing a selected list item preserves the following unselected item");
            editor->setHtml("<ul><li><b>First</b></li><li><b>Second</b></li></ul>");
            selectFirst();
            dock.findChild<QAction *>("clearFormat")->trigger();
            require(editor->document()->begin().next().textList() &&
                editor->document()->find("Second").charFormat().fontWeight() >= QFont::Bold,
                "clearing selected formatting preserves the following list item");
        }
        {
            OBSSceneAutoRelease first = obs_scene_create("Search counts");
            OBSSceneAutoRelease second = obs_scene_create("Search counts empty");
            scenepad::Dock dock(temp.filePath("search-counts.ini"));
            dock.resize(760, 400);
            dock.bindScene(obs_scene_get_source(first));
            dock.show();
            auto *editor = dock.findChild<QTextEdit *>("noteEditor");
            auto *search = dock.findChild<QLineEdit *>("searchText");
            auto *count = dock.findChild<QLabel *>("searchStatus");
            editor->setHtml(QString::fromUtf8("<p><b>Repeat</b> repeat</p><p>重复 重复</p>"));
            search->setText("repeat");
            waitSearch(dock);
            require(count->text() == "1/2", "match counting ignores case across rich-text runs");
            editor->append("repeat");
            const auto editedCursor = editor->textCursor();
            const int editedScroll = editor->verticalScrollBar()->value();
            waitSearch(dock);
            require(count->text().endsWith("/3") && editor->textCursor() == editedCursor &&
                editor->verticalScrollBar()->value() == editedScroll, "editing refreshes counts without moving the cursor or scroll position");
            editor->undo();
            waitSearch(dock);
            require(count->text().endsWith("/2"), "undo invalidates cached match positions");
            search->setText(QString::fromUtf8("重复"));
            waitSearch(dock);
            require(count->text() == "1/2" && editor->textCursor().selectedText() == QString::fromUtf8("重复"),
                "Chinese queries select and count matching text");
            search->setText("repeat");
            search->clear();
            waitEvents(200);
            require(!count->isVisible(), "clearing a pending query cancels its delayed result");
            search->setText("repeat");
            dock.bindScene(obs_scene_get_source(second));
            waitSearch(dock);
            require(count->text() == "0/0", "scene switching discards matches from the previous document");
            dock.bindScene(obs_scene_get_source(first));
            waitSearch(dock);
            require(count->text().endsWith("/2"), "returning to a scene recounts its current document");
            editor->setPlainText("aaaa");
            search->setText("aa");
            waitSearch(dock);
            require(count->text() == "1/2", "counting uses the same non-overlapping matches as navigation");
            editor->setPlainText(QString::fromUtf8("A\u00a0B a b\nA B"));
            search->setText("a b");
            waitSearch(dock);
            require(count->text() == "1/3", "search preserves case and non-breaking space matching across paragraphs");
            dock.findChild<QToolButton *>("clearSearch")->click();
            require(!count->isVisible() && search->text().isEmpty(), "clearing removes both count and query");
        }
        {
            OBSSceneAutoRelease scene = obs_scene_create("Marks");
            scenepad::Dock dock(temp.filePath("marks.ini"));
            dock.bindScene(obs_scene_get_source(scene)); dock.show();
            auto *editor = dock.findChild<QTextEdit *>("noteEditor");
            auto *insert = dock.findChild<QAction *>("insertMark");
            auto *next = dock.findChild<QAction *>("nextMark");
            auto *search = dock.findChild<QLineEdit *>("searchText");
            auto *count = dock.findChild<QLabel *>("searchStatus");
            auto *toolbar = dock.findChild<QToolBar *>("formatToolbar");
            auto *insertButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(insert));
            auto *nextButton = qobject_cast<QToolButton *>(toolbar->widgetForAction(next));
            waitEvents(20);
            require(insert && next && insertButton && nextButton && search && count, "mark controls exist");
            require(geometryIn(insertButton, &dock).right() < geometryIn(nextButton, &dock).left() &&
                geometryIn(nextButton, &dock).right() < geometryIn(search, &dock).left() &&
                insertButton->height() == nextButton->height(), "mark controls align to the left of search");
            editor->setPlainText("alpha beta");
            editor->setTextCursor(editor->document()->find("alpha"));
            insert->trigger();
            const auto mark = QStringLiteral("\u25c6");
            require(editor->toPlainText() == "alpha" + mark + "[] beta", "mark insertion preserves selected words");
            const auto markColor = editor->document()->find(mark).charFormat().foreground().color();
            require(markColor == QColor("#ffb347") || markColor == QColor("#b85b00"), "inserted mark uses a visible orange");
            require(editor->textCursor().position() == 7 && editor->hasFocus(), "marker leaves the caret in its empty brackets");
            editor->undo();
            require(editor->toPlainText() == "alpha beta", "one undo removes only the mark");
            auto noteCaret = editor->textCursor(); noteCaret.setPosition(5); editor->setTextCursor(noteCaret);
            dock.command(scenepad::Command::InsertMark);
            waitEvents(20);
            require(editor->hasFocus() && !search->hasFocus(), "insertion keeps focus in the body after queued events");
            editor->insertPlainText("Camera one");
            const auto annotated = "alpha" + mark + "[Camera one] beta";
            require(editor->toPlainText() == annotated, "notes are typed directly between brackets");
            require(editor->document()->find(mark + "[Camera one]").charFormat().foreground().color() == markColor, "the complete mark uses one orange color");
            auto end = editor->textCursor(); end.movePosition(QTextCursor::NextCharacter); editor->setTextCursor(end);
            key(editor, Qt::Key_Backspace);
            require(editor->toPlainText() == "alpha beta", "deleting a mark removes its whole note");
            editor->undo(); require(editor->toPlainText() == annotated, "whole-mark deletion is undoable");
            editor->setPlainText("alpha beta"); editor->moveCursor(QTextCursor::Start);
            auto clickMark = [&] {
                const QPoint point = editor->cursorRect(QTextCursor(editor->document())).center();
                const QPointF global = editor->viewport()->mapToGlobal(point);
                const auto mods = Qt::AltModifier | Qt::ShiftModifier;
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), global, Qt::LeftButton, Qt::LeftButton, mods);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), global, Qt::LeftButton, Qt::NoButton, mods);
                QApplication::sendEvent(editor->viewport(), &press); QApplication::sendEvent(editor->viewport(), &release);
            };
            editor->moveCursor(QTextCursor::End); clickMark();
            require(editor->toPlainText() == mark + "[]alpha beta" && !dock.isLocked(), "Alt Shift click inserts at the clicked position without toggling lock");
            editor->undo();
            editor->setPlainText("first " + mark + " second " + mark + " end");
            editor->moveCursor(QTextCursor::Start);
            next->trigger();
            require(search->text() == mark && count->text() == "1/2" && editor->textCursor().selectedText() == mark,
                "next mark selects and counts the first mark");
            next->trigger(); require(count->text() == "2/2", "next mark advances");
            next->trigger(); require(count->text() == "1/2", "next mark wraps to the beginning");
            dock.command(scenepad::Command::ToggleLock);
            require(!insert->isEnabled() && next->isEnabled(), "lock disables insertion while preserving navigation");
            const auto lockedHtml = editor->toHtml();
            dock.command(scenepad::Command::InsertMark); clickMark(); next->trigger();
            require(editor->toHtml() == lockedHtml && count->text() == "2/2", "locked mark navigation preserves the document");
            dock.command(scenepad::Command::ToggleLock);
            editor->setPlainText("No marks here"); next->trigger();
            require(count->text() == "0/0" && editor->toPlainText() == "No marks here", "missing marks leave the document unchanged");
            editor->setPlainText((QString(98, 'a') + mark + "\n").repeated(2000));
            editor->moveCursor(QTextCursor::Start);
            QElapsedTimer navigation; navigation.start();
            next->trigger(); const auto firstMs = navigation.elapsed(); navigation.restart();
            for (int i = 0; i < 1000; ++i) next->trigger();
            std::printf("MARKS: chars=200000 marks=2000 first=%lldms next-1000=%lldms\n", firstMs, navigation.elapsed());
            require(navigation.elapsed() < 3000 && count->text() == "1001/2000", "mark navigation reuses cached matches in a full script");
            editor->moveCursor(QTextCursor::End);
            dock.command(scenepad::Command::InsertMark);
            require(editor->document()->characterCount() - 1 == 200000, "mark insertion respects the script limit");
            const auto beforeJump = editor->toHtml();
            navigation.restart();
            for (int i = 0; i < 1000; ++i)
                require(dock.goToPosition(i % 2 ? QString::number(1 + i % 2000) : QString::number(i % 101) + '%'), "line and percentage navigation accepts valid targets");
            std::printf("GOTO: chars=200000 jumps=1000 elapsed=%lldms\n", navigation.elapsed());
            require(navigation.elapsed() < 3000 && editor->toHtml() == beforeJump, "jumping uses existing layout and preserves formatting");
            require(dock.goToPosition("2000") && editor->textCursor().blockNumber() == 1999, "line numbers locate logical paragraphs");
            require(dock.goToPosition("100%") && editor->verticalScrollBar()->value() == editor->verticalScrollBar()->maximum(), "percentage navigation reaches the bottom");
            const auto bottom = editor->verticalScrollBar()->value();
            require(!dock.goToPosition("0") && !dock.goToPosition("2002") && !dock.goToPosition("101%") && !dock.goToPosition("text") &&
                editor->verticalScrollBar()->value() == bottom, "invalid jump targets preserve the reading position");
            auto *progress = dock.findChild<QLabel *>("scrollPercent");
            QToolTip::hideText();
            QElapsedTimer tooltipWait; tooltipWait.start();
            while (QToolTip::isVisible() && tooltipWait.elapsed() < 1000) waitEvents(10);
            dock.activateWindow(); editor->setFocus(); waitEvents(20);
            QMouseEvent jumpClick(QEvent::MouseButtonRelease, QPointF(2, 2), progress->mapToGlobal(QPoint(2, 2)),
                Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            auto clickJump = [&] {
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), progress->mapToGlobal(QPoint(2, 2)),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(progress, &press); QApplication::sendEvent(progress, &jumpClick); waitEvents(10);
            };
            clickJump();
            auto *jumpInput = dock.findChild<QLineEdit *>("goToInput");
            if (!jumpInput || !jumpInput->isVisible() || !jumpInput->hasFocus())
                std::fprintf(stderr, "GOTO-FOCUS: visible=%d focused=%d active=%d current=%s popup=%d\n", jumpInput && jumpInput->isVisible(), jumpInput && jumpInput->hasFocus(), dock.isActiveWindow(), QApplication::focusWidget() ? QApplication::focusWidget()->objectName().toUtf8().constData() : "none", QApplication::activePopupWidget() != nullptr);
            require(jumpInput && jumpInput->isVisible() && jumpInput->hasFocus() && !QApplication::activeModalWidget(), "navigation uses a focused inline input without a dialog");
            const auto rowSize = dock.findChild<QWidget *>("bottomControls")->size();
            require(jumpInput->selectedText() == jumpInput->text(), "inline navigation selects the percentage unit too");
            jumpInput->setText("50"); key(jumpInput, Qt::Key_Return);
            require(jumpInput->isHidden() && editor->textCursor().blockNumber() == 49, "digits locate a line from the percentage entry");
            clickJump();
            jumpInput->setText("50%"); key(jumpInput, Qt::Key_Return);
            require(jumpInput->isHidden() && qAbs(dock.scrollProgress() - .5) < .01, "an explicit percentage locates progress");
            clickJump();
            jumpInput->setText("101%"); key(jumpInput, Qt::Key_Return);
            require(jumpInput->isVisible() && jumpInput->property("invalid").toBool() && !QApplication::activeModalWidget(), "invalid navigation stays in the input");
            key(jumpInput, Qt::Key_Escape);
            require(jumpInput->isHidden() && qAbs(dock.scrollProgress() - .5) < .01 &&
                dock.findChild<QWidget *>("bottomControls")->size() == rowSize, "canceling inline navigation preserves position and toolbar geometry");
            clickJump();
            search->setFocus(); waitEvents(10);
            require(search->hasFocus(), "search receives focus after leaving inline navigation");
            require(jumpInput->isHidden(), "leaving inline navigation cancels the edit");
            dock.command(scenepad::Command::ToggleLock);
            require(dock.goToPosition("1") && editor->isReadOnly() && editor->toHtml() == beforeJump, "locked reading supports navigation without editing");
            dock.command(scenepad::Command::ToggleLock);
            if (argc > 4) {
                editor->setPlainText("First cue\nSecond cue\nLast cue"); editor->moveCursor(QTextCursor::Start);
                insert->trigger(); editor->moveCursor(QTextCursor::End); insert->trigger(); editor->moveCursor(QTextCursor::Start);
                next->trigger(); waitEvents(20);
                dock.grab().save(app.arguments().value(4) + ".marks.png");
            }
        }
        {
            scenepad::Dock dock(temp.filePath("countdown.ini"));
            dock.resize(500, 480);
            dock.show();
            dock.bindScene(a);
            dock.command(scenepad::Command::Top);
            waitEvents(10);
            auto *play = dock.findChild<QToolButton *>("scrollPlay");
            auto *countdown = dock.findChild<QCheckBox *>("startCountdown");
            require(countdown && !countdown->isChecked(), "three-second countdown is disabled by default");
            auto *guide = dock.findChild<QToolButton *>("readingGuide");
            require(countdown->parentWidget() == guide->parentWidget() && countdown->geometry().left() > guide->geometry().right(),
                "countdown checkbox sits immediately after the reading guide");
            dock.startScroll();
            require(dock.isScrolling() && !dock.isCountingDown(), "default play starts immediately");
            dock.pauseScroll();
            countdown->setChecked(true);
            dock.startScroll();
            require(dock.isCountingDown() && !dock.isScrolling() && play->text() == "3",
                "starting shows the countdown before moving text");
            if (argc > 4) dock.grab().save(app.arguments().value(4) + ".countdown.png");
            dock.command(scenepad::Command::Toggle);
            require(!dock.isCountingDown() && !dock.isScrolling(), "play toggle cancels a pending countdown");
            QElapsedTimer delay;
            delay.start();
            dock.startScroll();
            waitEvents(3150);
            require(delay.elapsed() >= 3000 && dock.isScrolling() && !dock.isCountingDown(),
                "scrolling starts after the full three-second delay");
            dock.pauseScroll();
            dock.startScroll();
            dock.findChild<QAction *>("findText")->trigger();
            require(!dock.isCountingDown() && !dock.isScrolling(), "opening search cancels the pending countdown");
            dock.findChild<QToolButton *>("clearSearch")->click();
            dock.startScroll();
            dock.bindScene(nullptr);
            require(!dock.isCountingDown() && !dock.isScrolling(), "scene changes cancel the pending countdown");
            dock.bindScene(a);
            dock.startScroll();
            dock.hide();
            require(!dock.isCountingDown() && !dock.isScrolling(), "hiding the dock cancels the pending countdown");
            dock.show();
            countdown->setChecked(false);
            dock.startScroll();
            require(dock.isScrolling() && !dock.isCountingDown(), "disabling the countdown starts scrolling immediately");
            dock.shutdown();
            scenepad::Dock reopened(temp.filePath("countdown.ini"));
            require(!reopened.findChild<QCheckBox *>("startCountdown")->isChecked(), "countdown preference persists");
        }
        {
            OBSSceneAutoRelease scene = obs_scene_create("ScenePad metrics");
            auto *source = obs_scene_get_source(scene);
            scenepad::Dock dock(temp.filePath("metrics.ini"));
            dock.resize(760, 400);
            dock.bindScene(source);
            dock.show();
            auto *editor = dock.findChild<QTextEdit *>("noteEditor");
            auto *numbers = editor->findChild<QWidget *>("lineNumbers");
            auto *status = dock.findChild<QLabel *>("documentStatus");
            auto *percent = dock.findChild<QLabel *>("scrollPercent");
            auto *controls = dock.findChild<QWidget *>("bottomControls");
            editor->setHtml("<p style='font-size:10pt;color:#f06060'>Small red</p>"
                "<p style='font-size:24pt'><b>Large bold</b></p><p>" + QString("Wrapped text ").repeated(250) +
                "</p><p>Final paragraph</p>");
            editor->moveCursor(QTextCursor::Start);
            waitEvents(30);
            const auto originalPalette = dock.palette();
            const auto themeHtml = editor->toHtml();
            const auto themeUndo = editor->document()->availableUndoSteps();
            auto darkPalette = originalPalette;
            darkPalette.setColor(QPalette::Window, QColor("#2b2b2b"));
            darkPalette.setColor(QPalette::WindowText, QColor("#eeeeee"));
            darkPalette.setColor(QPalette::Text, QColor("#eeeeee"));
            dock.setPalette(darkPalette); waitEvents(10);
            auto lightPalette = darkPalette;
            lightPalette.setColor(QPalette::Window, QColor("#f1f1f1"));
            lightPalette.setColor(QPalette::WindowText, QColor("#202020"));
            lightPalette.setColor(QPalette::Text, Qt::white);
            auto *slider = dock.findChild<QSlider *>("scrollSlider");
            const auto darkSlider = slider->styleSheet();
            dock.setPalette(lightPalette); waitEvents(10);
            QTextEdit themeReference(&dock); themeReference.ensurePolished();
            require(editor->palette().color(QPalette::Text) == QColor("#202020"),
                "light theme overrides stale white default text");
            for (const auto group : {QPalette::Active, QPalette::Inactive})
                for (const auto role : {QPalette::Highlight, QPalette::HighlightedText})
                    require(editor->palette().color(group, role) == themeReference.palette().color(group, role),
                        "ordinary selection inherits the OBS theme");
            require(editor->toHtml() == themeHtml && editor->document()->availableUndoSteps() == themeUndo &&
                editor->document()->find("Small red").charFormat().foreground().color() == QColor("#f06060"),
                "theme changes preserve text, explicit colors and undo history");
            require(slider->styleSheet() != darkSlider, "speed slider follows the light theme");
            dock.setPalette(darkPalette); waitEvents(10);
            require(editor->palette().color(QPalette::Text) == darkPalette.color(QPalette::WindowText) &&
                slider->styleSheet() == darkSlider && editor->toHtml() == themeHtml &&
                editor->document()->availableUndoSteps() == themeUndo, "dark theme restores colors without changing text or undo history");
            dock.setPalette(originalPalette); waitEvents(10);
            const auto stateStyle = editor->styleSheet();
            auto *searchField = dock.findChild<QLineEdit *>("searchText");
            searchField->setFocus(); waitEvents(10);
            const auto searchBackground = editor->viewport()->grab().toImage().pixelColor(editor->viewport()->width() - 4, editor->viewport()->height() - 4);
            const auto searchGutter = numbers->grab().toImage().pixelColor(1, numbers->height() - 2);
            dock.command(scenepad::Command::ToggleLock); waitEvents(10);
            require(searchBackground == editor->viewport()->grab().toImage().pixelColor(editor->viewport()->width() - 4, editor->viewport()->height() - 4) &&
                searchGutter == numbers->grab().toImage().pixelColor(1, numbers->height() - 2),
                "search focus and reading lock use identical document and gutter backgrounds");
            dock.command(scenepad::Command::ToggleLock); editor->setFocus(); waitEvents(10);
            const auto htmlBeforeState = editor->toHtml();
            editor->setStyleSheet(stateStyle + " QTextEdit[readOnly=\"false\"] { background-color: #162431; } QTextEdit[readOnly=\"true\"] { background-color: #323232; }");
            editor->ensurePolished();
            dock.command(scenepad::Command::ToggleLock);
            require(editor->isReadOnly() && editor->palette().color(QPalette::Base) == QColor("#323232") &&
                editor->toHtml() == htmlBeforeState, "locking refreshes read-only theme colors immediately without mouse events");
            waitEvents(10);
            dock.command(scenepad::Command::ToggleLock);
            require(!editor->isReadOnly() && editor->palette().color(QPalette::Base) == QColor("#162431") &&
                editor->toHtml() == htmlBeforeState, "unlocking restores editable theme colors immediately without mouse events");
            waitEvents(10);
            require(editor->extraSelections().isEmpty(), "lock changes add no current-line body tint");
            editor->setStyleSheet(stateStyle);
            waitEvents(20);
            dock.command(scenepad::Command::ToggleLock);
            waitEvents(20);
            require(editor->palette().color(QPalette::Base).lightness() < dock.palette().color(QPalette::Window).lightness() &&
                numbers->grab().toImage().pixelColor(1, numbers->height() - 2) == editor->palette().color(QPalette::Base),
                "locked text and line numbers share a darker theme background");
            require(!dock.findChild<QToolBar *>("paragraphToolbar")->isHidden() && !dock.findChild<QToolBar *>("paragraphToolbar")->isEnabled(),
                "reading mode keeps paragraph tools visible and disabled");
            dock.command(scenepad::Command::ToggleLock);
            waitEvents(20);
            require(!dock.findChild<QToolBar *>("paragraphToolbar")->isHidden(), "unlocking keeps paragraph tools visible");
            require(numbers && numbers->isVisible() && numbers->font().bold() &&
                numbers->geometry().right() < editor->viewport()->x() && numbers->y() == editor->viewport()->y() &&
                numbers->height() == editor->viewport()->height(), "bold line-number gutter aligns with the text viewport");
            require(status && percent && status->parentWidget() == dock.findChild<QWidget *>("readingMetrics") && geometryIn(percent, controls).right() == controls->contentsRect().right() &&
                status->text() == scenepad::text("DocumentStatus").arg(4) && percent->text() == "0%",
                "line count and initial scroll percentage appear at the bottom right");
            const auto htmlBeforeHighlight = editor->toHtml();
            auto highlightCursor = editor->document()->find("Large bold");
            highlightCursor.clearSelection();
            editor->setTextCursor(highlightCursor);
            waitEvents(10);
            const auto bounds = editor->document()->documentLayout()->blockBoundingRect(highlightCursor.block());
            const auto line = highlightCursor.block().layout()->lineAt(0);
            const int activeY = qRound(bounds.top() + line.y() + line.height() / 2) - editor->verticalScrollBar()->value();
            const auto gutter = numbers->grab().toImage();
            require(editor->extraSelections().isEmpty() && editor->toHtml() == htmlBeforeHighlight &&
                gutter.pixelColor(qRound(gutter.devicePixelRatio()), qRound(activeY * gutter.devicePixelRatio())) == dock.palette().color(QPalette::Highlight),
                "only the active line number uses the OBS theme highlight");
            const auto body = editor->viewport()->grab().toImage();
            require(body.pixelColor(body.width() - 4, qRound(activeY * body.devicePixelRatio())) == body.pixelColor(body.width() - 4, body.height() - 4),
                "current-line body has the ordinary editor background");
            if (argc > 4) dock.grab().save(app.arguments().value(4) + ".current-line.png");
            editor->moveCursor(QTextCursor::Start);
            const int oneDigitWidth = numbers->width();
            dock.resize(360, 400);
            waitEvents(30);
            if (editor->document()->blockCount() != 4 || dock.width() != 700 ||
                !controls->rect().contains(geometryIn(status, controls)) || !controls->rect().contains(geometryIn(percent, controls)))
                std::fprintf(stderr, "Footer: blocks=%d dock=%d row=%d,%d status=%d,%d,%d,%d percent=%d,%d,%d,%d minimum=%d\n",
                    editor->document()->blockCount(), dock.width(), controls->width(), controls->height(),
                    status->x(), status->y(), status->width(), status->height(), percent->x(), percent->y(),
                    percent->width(), percent->height(), controls->minimumSizeHint().width());
            require(editor->document()->blockCount() == 4 && dock.width() == 700 &&
                controls->rect().contains(geometryIn(status, controls)) && controls->rect().contains(geometryIn(percent, controls)),
                "wrapping does not add logical lines or hide the footer");
            dock.resize(760, 400);
            waitEvents(30);
            if (argc > 4) dock.grab().save(app.arguments().value(4) + ".line-numbers.png");
            const auto html = editor->toHtml();
            const auto plain = editor->toPlainText();
            wheel(editor, 120, Qt::ControlModifier);
            auto small = editor->document()->find("Small red");
            auto large = editor->document()->find("Large bold");
            require(small.charFormat().fontPointSize() == 11 && large.charFormat().fontPointSize() == 25 &&
                small.charFormat().foreground().color() == QColor("#f06060") && large.charFormat().fontWeight() >= QFont::Bold &&
                editor->toPlainText() == plain, "Ctrl+wheel changes mixed font sizes without losing rich formatting or text");
            dock.flush(false);
            QTextDocument saved;
            saved.setHtml(notes.read(source).html);
            require(saved.find("Large bold").charFormat().fontPointSize() == 25, "Ctrl+wheel font changes are saved in scene notes");
            editor->undo();
            require(editor->toHtml() == html, "one undo restores the entire wheel adjustment");
            wheel(editor, 60, Qt::ControlModifier);
            require(editor->toHtml() == html, "high-resolution wheel accumulates a partial step");
            wheel(editor, 60, Qt::ControlModifier);
            require(editor->document()->find("Large bold").charFormat().fontPointSize() == 25,
                "two partial wheel deltas make one font-size step");
            wheel(editor, -120, Qt::ControlModifier);
            require(editor->document()->find("Large bold").charFormat().fontPointSize() == 24, "Ctrl+wheel decreases text sizes");
            dock.findChild<QToolButton *>("readMode")->click();
            const auto lockedHtml = editor->toHtml();
            if (argc > 4) dock.findChild<QToolButton *>("readMode")->grab().save(app.arguments().value(4) + ".lock.png");
            wheel(editor, 120, Qt::ControlModifier);
            require(editor->toHtml() == lockedHtml, "reading lock prevents wheel formatting changes");
            editor->verticalScrollBar()->setValue(0);
            wheel(editor, -120, Qt::NoModifier);
            waitEvents(180);
            require(editor->verticalScrollBar()->value() > 0, "ordinary wheel still scrolls vertically");
            editor->setPlainText(QString("Numbered paragraph\n").repeated(119) + "Last paragraph");
            waitEvents(30);
            require(editor->document()->blockCount() == 120 && numbers->width() > oneDigitWidth,
                "line-number gutter grows for three-digit paragraph numbers");
            auto *bar = editor->verticalScrollBar();
            bar->setValue(bar->maximum() / 2);
            require(status->text() == scenepad::text("DocumentStatus").arg(120) && percent->text() == "50%", "footer tracks the vertical scroll midpoint");
            bar->setValue(qRound(bar->maximum() / 100.0));
            waitEvents(20);
            const auto percentGeometry = percent->geometry();
            const auto countGeometry = status->geometry();
            const auto sliderGeometry = dock.findChild<QSlider *>("scrollSlider")->geometry();
            const auto speedGeometry = dock.findChild<QDoubleSpinBox *>("scrollSpeed")->geometry();
            bar->setValue(bar->maximum());
            waitEvents(20);
            require(status->text() == scenepad::text("DocumentStatus").arg(120) && percent->text() == "100%", "footer reaches 100 percent at the end");
            require(percent->geometry() == percentGeometry && status->geometry() == countGeometry &&
                dock.findChild<QSlider *>("scrollSlider")->geometry() == sliderGeometry &&
                dock.findChild<QDoubleSpinBox *>("scrollSpeed")->geometry() == speedGeometry &&
                percent->contentsRect().width() >= percent->fontMetrics().horizontalAdvance("100%"),
                "one to three digit percentages preserve footer positions and fit completely");
            editor->clear();
            waitEvents(20);
            require(status->text() == scenepad::text("DocumentStatus").arg(1) && percent->text() == "0%" && numbers->width() == oneDigitWidth,
                "empty document resets line count, scroll percentage and gutter width");
            dock.findChild<QToolButton *>("readMode")->click();
            editor->setHtml("<p><span style='font-size:6pt'>Tiny</span> <span style='font-size:144pt'>Huge</span> "
                "<span style='font-size:20px;color:#f06060'>Pixel</span></p>");
            wheel(editor, 120, Qt::ControlModifier);
            const auto pixel = editor->document()->find("Pixel").charFormat();
            require(editor->document()->find("Tiny").charFormat().fontPointSize() == 7 &&
                editor->document()->find("Huge").charFormat().fontPointSize() == 144 && pixel.font().pixelSize() == -1 &&
                qAbs(pixel.fontPointSize() - (20 * 72.0 / editor->logicalDpiY() + 1)) < 0.01 &&
                pixel.foreground().color() == QColor("#f06060"), "wheel handles pixel-sized HTML text and the upper size limit");
            wheel(editor, -120 * 200, Qt::ControlModifier);
            require(editor->document()->find("Tiny").charFormat().fontPointSize() == 6 &&
                editor->document()->find("Huge").charFormat().fontPointSize() == 6,
                "wheel font sizes stop at the lower limit");
            const auto limitHtml = editor->toHtml();
            const int undoSteps = editor->document()->availableUndoSteps();
            wheel(editor, -120, Qt::ControlModifier);
            require(editor->toHtml() == limitHtml && editor->document()->availableUndoSteps() == undoSteps,
                "wheel at the size limit skips unchanged formatting and undo entries");
            dock.bindScene(nullptr);
            dock.command(scenepad::Command::ToggleLock);
            require(dock.isLocked() && editor->isReadOnly(), "the shared lock can be set without a selected scene");
            require(status->text() == scenepad::text("DocumentStatus").arg(0) && percent->text() == "0%", "no scene shows zero lines and zero scroll");
        }
        if (argc > 1) {
            const auto word = scenepad::importDocx(QString::fromLocal8Bit(argv[1]));
            require(word.error.isEmpty(), "Word imports successfully");
            QTextDocument doc;
            doc.setHtml(word.html);
            require(doc.toPlainText().contains("Word import test"), "Word text");
            auto red = doc.find("Red bold");
            require(!red.isNull(), "Word formatted run exists");
            require(red.charFormat().foreground().color() == QColor("#ff0000"), "Word color");
            require(red.charFormat().fontWeight() >= QFont::Bold, "Word bold");
            auto heading = doc.find("Word import test");
            require(heading.charFormat().fontPointSize() == 22, "Word named style and size");
            auto item = doc.find("List item");
            require(item.block().textList() != nullptr, "Word numbered list");
            require(word.simplified, "Word unsupported layout flagged");
            const auto invalid = scenepad::importDocx(temp.filePath("missing.docx"));
            require(!invalid.error.isEmpty(), "invalid Word returns actionable error");
        }
        if (argc > 4) {
            auto palette = app.palette();
            obs_source_set_name(a, "ScenePad");
            notes.write(a, {QString::fromUtf8("<p><b>ScenePad</b> · 场景提示</p><p>顶部：导入、导出、字号与文字格式。</p><p>底部：播放 / 暂停、回顶、滚动速度与阅读锁定。</p>")});
            scenepad::Dock preview(temp.filePath("preview.ini"));
            preview.resize(760, 300);
            preview.bindScene(a);
            preview.show();
            waitEvents(80);
            require(preview.grab().save(app.arguments().value(4)), "save widget preview");
            preview.command(scenepad::Command::ToggleLock);
            waitEvents(20);
            preview.grab().save(app.arguments().value(4) + ".reading.png");
            preview.command(scenepad::Command::ToggleLock);
            waitEvents(20);
            preview.grab().save(app.arguments().value(4) + ".editing.png");
            require(preview.remote()->listen("127.0.0.1", 0), "QR preview starts on loopback");
            preview.grab().save(app.arguments().value(4) + ".qr-button.png");
            preview.findChild<QToolButton *>("remoteQrButton")->click(); waitEvents(30);
            auto *popup = preview.findChild<QLabel *>("remoteQrOverlay");
            require(popup && popup->isVisible() && popup->grab().save(app.arguments().value(4) + ".qr.png"),
                "save collapsed QR entry and popup previews");
            popup->hide();
            preview.resize(360, 140); waitEvents(30);
            preview.findChild<QToolButton *>("remoteQrButton")->click(); waitEvents(30);
            auto *textViewport = preview.findChild<QTextEdit *>("noteEditor")->viewport();
            require(popup->isVisible() && popup->height() <= textViewport->height() &&
                popup->width() == popup->height() && popup->grab().save(app.arguments().value(4) + ".qr-short.png"),
                "save square QR popup bounded by the document viewport");
            popup->hide();
            preview.resize(760, 300); waitEvents(20);
            bool qrSaved = false;
            QTimer::singleShot(30, [&] {
                if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                    qrSaved = dialog->grab().save(app.arguments().value(4) + ".remote.png");
                    dialog->reject();
                }
            });
            preview.remote()->showSettings();
            require(qrSaved, "save direct QR dialog preview");
            preview.remote()->stop();
            preview.resize(360, 260);
            waitEvents(40);
            require(preview.grab().save(app.arguments().value(4) + ".narrow.png"), "save narrow widget preview");
            palette.setColor(QPalette::Window, QColor("#f1f1f1"));
            palette.setColor(QPalette::WindowText, QColor("#202020"));
            palette.setColor(QPalette::Base, QColor("#ffffff"));
            palette.setColor(QPalette::Text, QColor("#202020"));
            palette.setColor(QPalette::Button, QColor("#e5e5e5"));
            palette.setColor(QPalette::ButtonText, QColor("#202020"));
            const auto themeGeometry = app.styleSheet();
            app.setStyleSheet({});
            app.setPalette(palette);
            auto largeUiFont = app.font();
            largeUiFont.setPointSize(12);
            app.setFont(largeUiFont);
            app.setStyleSheet(themeGeometry);
            preview.resize(760, 300);
            waitEvents(40);
            auto *fontSize = preview.findChild<QSpinBox *>("fontSize");
            auto *input = fontSize->findChild<QLineEdit *>();
            require(input->fontMetrics().height() >= preview.fontMetrics().height(), "numeric controls follow the current UI font");
            require(input->contentsRect().height() >= input->fontMetrics().height(), "font input adapts to a larger UI font");
            require(preview.grab().save(app.arguments().value(4) + ".light.png"), "save light and larger-font widget preview");
            preview.command(scenepad::Command::ToggleLock);
            waitEvents(20);
            preview.grab().save(app.arguments().value(4) + ".light-reading.png");
            auto *lightEditor = preview.findChild<QTextEdit *>("noteEditor");
            lightEditor->selectAll(); waitEvents(10);
            preview.grab().save(app.arguments().value(4) + ".light-selection.png");
        }
        dockRegressions(app, {});
        std::printf("PASS: %d checks\n", checks);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL after %d checks: %s\n", checks, error.what());
        result = 1;
    }
    obs_shutdown();
    text_lookup_destroy(locale);
    return result;
}
