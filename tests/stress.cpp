#include "test-helpers.hpp"
#include "dock.hpp"
#include "docx.hpp"
#include "notes.hpp"
#include "web-document.hpp"
#include "locale.hpp"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QAbstractTextDocumentLayout>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QLabel>
#include <QLocale>
#include <QToolButton>
#include <QTextEdit>
#include <QTextDocument>
#include <QScrollBar>
#include <QWheelEvent>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <mach/mach.h>
#endif
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

const char *obs_module_text(const char *key)
{
    if (std::strcmp(key, "DocumentStatus") == 0) return "%1 lines";
    if (std::strcmp(key, "CharacterCount") == 0) return "%1 chars";
    if (std::strcmp(key, "CharacterRemaining") == 0) return "Used: %1 / %2; remaining: %3";
    if (std::strcmp(key, "CountdownRemaining") == 0) return "Starting in %1 s";
    if (std::strcmp(key, "MatchCount") == 0) return "%1/%2";
    return key;
}

namespace {
using scenepad::test::require;
size_t memory()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters)))
        throw std::runtime_error("memory measurement failed");
    return counters.PrivateUsage;
#else
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        throw std::runtime_error("memory measurement failed");
    return info.resident_size;
#endif
}
void wait(int milliseconds) { scenepad::test::wait(milliseconds, 2); }
void longTextStress()
{
    QTemporaryDir temp;
    scenepad::NoteStore notes(temp.filePath("notes"));
    OBSSceneAutoRelease scene = obs_scene_create("Long text stress");
    OBSSceneAutoRelease alternate = obs_scene_create("Long text alternate");
    scenepad::Dock dock(temp.filePath("long-text.ini"));
    dock.resize(600, 500); dock.bindScene(obs_scene_get_source(scene)); dock.show();
    qApp->setStyleSheet("QWidget { font-size: 14px; }");
    auto *editor = dock.findChild<QTextEdit *>("noteEditor");
    auto *search = dock.findChild<QLineEdit *>("searchText");
    auto *next = dock.findChild<QToolButton *>("findNext");
    auto *count = dock.findChild<QLabel *>("searchStatus");
    auto *bar = editor->verticalScrollBar();
    dock.activateWindow(); editor->setFocus(); wait(50);
    const auto initialMemory = memory();
    struct Sample { const char *name; QString text; QString query; bool html = false; };
    std::vector<Sample> samples{{"paragraph-10k", QString(9998, 'W'), "W"}};
    samples.push_back({"note-200k", QString(9998, 'W') + ("\n" + QString(9998, 'W')).repeated(19), "W"});
    samples.push_back({"words-200k", (QString("alpha beta, gamma delta! ").repeated(400) + "\n").repeated(19) + "alpha", "gamma"});
    samples.push_back({"CJK-200k", (QString::fromUtf8("他说：“你好，世界！”（继续阅读）测试。").repeated(450) + "\n").repeated(19) + QString::fromUtf8("正文"), QString::fromUtf8("你好")});
    samples.push_back({"CJK-10k", QString::fromUtf8("超长单行正文").repeated(1600), QString::fromUtf8("正文")});
    samples.push_back({"soft-breaks-500", QString("alpha beta gamma\u2028").repeated(499) + "alpha", "alpha"});
    samples.push_back({"paragraphs-2000", QString("alpha beta gamma: long document content\n").repeated(1999) + "alpha", "alpha"});
    samples.push_back({"paragraphs-20000", QString("123456789\n").repeated(19999) + "123456789", "123"});
    QString rich = "<p>";
    for (int i = 0; i < 3000; ++i)
        rich += QString("<span style='font-size:%1pt;font-weight:%2;color:#26a6d1'>%3</span>")
            .arg(10 + i % 3 * 4).arg(i % 2 ? 700 : 400).arg("abc");
    samples.push_back({"rich-single-paragraph", rich + "</p>", "abc", true});
    for (const auto &sample : samples) {
        std::printf("BEGIN long-text %s\n", sample.name); std::fflush(stdout);
        search->clear(); editor->clear();
        const auto html = sample.html ? sample.text : "<p>" + sample.text.toHtmlEscaped().replace("\n", "</p><p>") + "</p>";
        QElapsedTimer clock; clock.start();
        editor->setHtml(html);
        editor->document()->documentLayout()->documentSize(); QApplication::processEvents();
        const auto loadTime = clock.elapsed();
        dock.flush(false);
        const auto original = editor->toPlainText();
        long long inputPeak = 0, scrollPeak = 0;
        int inputJump = 0, undoJump = 0;
        for (int i = 0; i < 10; ++i) {
            auto cursor = editor->textCursor(); cursor.setPosition(static_cast<int>(original.size() * (i % 3) / 2));
            editor->setTextCursor(cursor); editor->ensureCursorVisible(); QApplication::processEvents();
            const int top = editor->cursorRect().top(), scroll = bar->value();
            const int tolerance = qMax(40, editor->cursorRect().height() * 2);
            clock.restart(); editor->insertPlainText("x"); QApplication::processEvents();
            inputJump = qMax(inputJump, qAbs(editor->cursorRect().top() - top));
            require(qAbs(editor->cursorRect().top() - top) <= tolerance && qAbs(bar->value() - scroll) <= tolerance,
                "typing moved the caret or viewport by more than two lines");
            editor->undo();
            QApplication::processEvents(); inputPeak = qMax(inputPeak, clock.elapsed());
            undoJump = qMax(undoJump, qAbs(editor->cursorRect().top() - top));
            if (qAbs(editor->cursorRect().top() - top) > tolerance || qAbs(bar->value() - scroll) > tolerance)
                std::fprintf(stderr, "Undo %s cycle %d: caret %d -> %d, scroll %d -> %d\n",
                    sample.name, i, top, editor->cursorRect().top(), scroll, bar->value());
            require(qAbs(editor->cursorRect().top() - top) <= tolerance && qAbs(bar->value() - scroll) <= tolerance,
                "undo moved the caret or viewport by more than two lines");
            clock.restart(); bar->setValue(static_cast<int>(static_cast<qint64>(bar->maximum()) * (i % 2)));
            editor->viewport()->repaint(); QApplication::processEvents(); scrollPeak = qMax(scrollPeak, clock.elapsed());
        }
        int focusChanges = 0;
        const auto connection = QObject::connect(editor->document(), &QTextDocument::contentsChanged, [&] { ++focusChanges; });
        clock.restart();
        const int focusCycles = 20;
        for (int i = 0; i < focusCycles; ++i) {
            QFocusEvent in(QEvent::FocusIn, Qt::PopupFocusReason), out(QEvent::FocusOut, Qt::PopupFocusReason);
            QApplication::sendEvent(editor, &in); QApplication::sendEvent(editor, &out);
            QApplication::processEvents();
        }
        const auto focusTime = clock.elapsed(); QObject::disconnect(connection);
        require(editor->toPlainText() == original, "long-text editing/focus changed the content");
        clock.restart(); search->setText(sample.query); next->click();
        const auto searchTime = clock.elapsed();
        const auto expected = original.count(sample.query, Qt::CaseInsensitive);
        require(count->text() == QString("1/%1").arg(expected), "long-text search count mismatch");
        clock.restart();
        for (int i = 0; i < 100; ++i) next->click();
        const auto cachedTime = clock.elapsed();
        long long lockPeak = 0, resizePeak = 0, formatPeak = 0, switchPeak = 0;
        search->clear();
        editor->document()->documentLayout()->documentSize(); QApplication::processEvents();
        bar->setValue(bar->maximum() / 2);
        QApplication::processEvents();
        for (int i = 0; i < 4; ++i) {
            const int position = bar->value();
            const int maximum = bar->maximum();
            const auto font = editor->document()->defaultFont();
            clock.restart(); dock.command(scenepad::Command::ToggleLock); dock.command(scenepad::Command::ToggleLock);
            const auto lockTime = clock.elapsed();
            QApplication::processEvents(); lockPeak = qMax(lockPeak, clock.elapsed());
            if (clock.elapsed() > 500) std::printf("LOCK %s cycle=%d commands=%lldms events=%lldms\n", sample.name, i, lockTime, clock.elapsed() - lockTime);
            if (bar->value() != position)
                std::fprintf(stderr, "Lock %s cycle %d: position %d -> %d, maximum %d -> %d\n", sample.name, i, position, bar->value(), maximum, bar->maximum());
            require(bar->value() == position, "locking changed the reading position");
            require(editor->document()->defaultFont() == font, "locking changed the document font");
            clock.restart(); dock.resize(i % 2 ? 760 : 700, 500); QApplication::processEvents();
            resizePeak = qMax(resizePeak, clock.elapsed());
            clock.restart();
            QWheelEvent wheel(QPointF(10, 10), editor->viewport()->mapToGlobal(QPoint(10, 10)),
                QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(editor->viewport(), &wheel); editor->undo(); QApplication::processEvents();
            formatPeak = qMax(formatPeak, clock.elapsed());
        }
        for (int i = 0; i < 2; ++i) {
            clock.restart(); dock.bindScene(obs_scene_get_source(alternate)); dock.bindScene(obs_scene_get_source(scene));
            QApplication::processEvents(); switchPeak = qMax(switchPeak, clock.elapsed());
        }
        require(editor->toPlainText() == original, "long-text controls changed the content");
        auto cursor = editor->textCursor(); cursor.movePosition(QTextCursor::End);
        cursor.insertText("x"); editor->undo();
        clock.restart(); dock.flush(false); const auto saved = notes.read(obs_scene_get_source(scene)).html; const auto saveTime = clock.elapsed();
        require(!saved.isEmpty(), "long-text save lost the document");
        require(dock.findChild<QLabel *>("characterCount")->text() == QString("%1 chars").arg(QLocale().toString(editor->document()->characterCount() - 1)),
            "character counter stays correct after edits, undo and scene switches");
        clock.restart();
        for (int i = 0; i < 5; ++i) require(scenepad::plainText(saved) == original, "repeated TXT conversion preserves long and formatted notes");
        std::printf("TXT %s: 5 conversions in %lld ms\n", sample.name, clock.elapsed());
        require(clock.elapsed() < 5000, "TXT conversion exceeded its responsiveness budget");
        require(focusChanges == 0, "focus changes dirtied the long document");
        std::printf("LONG %s: chars=%lld blocks=%d load=%lldms input-peak=%lldms scroll-peak=%lldms focus%d=%lldms focus-document-changes=%d search=%lldms matches=%lld cached100=%lldms save=%lldms memory=%.2fMiB\n",
            sample.name, static_cast<long long>(original.size()), editor->document()->blockCount(), loadTime,
            inputPeak, scrollPeak, focusCycles, focusTime, focusChanges, searchTime, static_cast<long long>(expected), cachedTime,
            saveTime, memory() / 1048576.0); std::fflush(stdout);
        std::printf("CONTROLS %s: lock-peak=%lldms resize-peak=%lldms format-peak=%lldms switch-peak=%lldms\n",
            sample.name, lockPeak, resizePeak, formatPeak, switchPeak); std::fflush(stdout);
        std::printf("CARET %s: input-jump=%dpx undo-jump=%dpx\n", sample.name, inputJump, undoJump); std::fflush(stdout);
        require(loadTime < 5000 && inputPeak < 5000 && scrollPeak < 1000 && searchTime < 1000,
            "long-text operation exceeded its responsiveness budget");
        require(lockPeak < 5000 && resizePeak < 5000 && formatPeak < 5000 && switchPeak < 5000,
            "long-text control exceeded its responsiveness budget");
    }
    editor->clear(); search->clear(); QApplication::processEvents(); dock.shutdown();
    require(memory() < initialMemory + 1024ULL * 1024 * 1024, "long-text stress memory grew by more than 1 GiB");
    std::printf("PASS: long-text stress; final memory %.2f MiB\n", memory() / 1048576.0); std::fflush(stdout);
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    base_set_log_handler([](int, const char *, va_list, void *) {}, nullptr);
    if (!obs_startup("en-US", nullptr, nullptr)) return 2;
    int result = 0;
    if (app.arguments().contains("--long-text")) {
        try { longTextStress(); }
        catch (const std::exception &error) { std::fprintf(stderr, "FAIL long-text: %s\n", error.what()); result = 1; }
        obs_shutdown(); return result;
    }
    QElapsedTimer total;
    total.start();
    try {
        QTemporaryDir temp;
        scenepad::NoteStore notes(temp.filePath("notes"));
        QSettings preferences(temp.filePath("prefs.ini"), QSettings::IniFormat);
        preferences.setValue("startCountdown", false);
        preferences.sync();
        std::vector<OBSSceneAutoRelease> scenes;
        QString body;
        for (int i = 0; i < 250; ++i)
            body += QString("<p><b>Paragraph %1</b> <span style='color:#26a6d1'>场景笔记 stress content</span></p>").arg(i);
        for (int i = 0; i < 100; ++i) {
            const auto name = QByteArray("Stress scene ") + QByteArray::number(i);
            scenes.emplace_back(obs_scene_create(name.constData()));
            notes.write(obs_scene_get_source(scenes.back()), {body});
        }
        scenepad::Dock dock(temp.filePath("prefs.ini"));
        dock.resize(550, 500);
        dock.show();
        auto *editor = dock.findChild<QTextEdit *>("noteEditor");
        auto *center = dock.findChild<QAction *>("alignCenter");
        auto *left = dock.findChild<QAction *>("alignLeft");
        for (int i = 0; i < 50; ++i) dock.bindScene(obs_scene_get_source(scenes[i]));
        const size_t baseline = memory();
        for (int i = 0; i < 3000; ++i) {
            const int index = i % 100;
            auto *source = obs_scene_get_source(scenes[index]);
            dock.bindScene(source);
            auto cursor = editor->textCursor();
            cursor.movePosition(QTextCursor::End);
            const auto marker = QString(" verify%1end").arg(i);
            cursor.insertText(marker);
            if (i % 25 == 0) {
                dock.flush(false);
                OBSDataAutoRelease saved = obs_save_source(source);
                const QByteArray json(obs_data_get_json(saved));
                require(!json.contains(marker.trimmed().toUtf8()), "scene export contains a document body");
                require(notes.read(source).html.contains(marker), "independent note lost an edit");
            }
            (i % 2 ? center : left)->trigger();
            dock.setSpeed((1 + i % 50) / 10.0);
            if (i % 40 == 0) QApplication::processEvents();
        }
        dock.flush(false);
        for (int i = 0; i < 100; ++i) {
            const int last = 2900 + i;
            const auto html = notes.read(obs_scene_get_source(scenes[i])).html;
            require(html.contains(QString("verify%1end").arg(last)), "scene contamination or dropped save");
        }
        std::printf("PASS: 100 scenes x 250 rich paragraphs; 3000 edit/switch/paragraph/speed cycles; 120 export snapshots\n");
        std::fflush(stdout);
        dock.bindScene(obs_scene_get_source(scenes[0]));
        for (int i = 0; i < 1000; ++i) {
            dock.command(scenepad::Command::Top);
            dock.startScroll();
            require(dock.isScrolling(), "scroll did not start");
            dock.setSpeed((1 + i % 50) / 10.0);
            dock.pauseScroll();
            require(!dock.isScrolling(), "scroll did not pause");
        }
        const auto beforeRateTest = editor->toHtml();
        for (const double speed : {0.1, 0.5, 1.0, 5.0}) {
            dock.command(scenepad::Command::Top);
            dock.setSpeed(speed);
            QElapsedTimer elapsed;
            dock.startScroll(); elapsed.start();
            wait(speed == 0.1 ? 2200 : speed == 0.5 ? 1200 : 350);
            dock.pauseScroll();
            const int position = editor->verticalScrollBar()->value();
            const double expected = elapsed.elapsed() * speed * 10.0 / 1000.0;
            require(position > 0 && qAbs(position - expected) <= 2.0, "fractional speeds retain elapsed-time accuracy");
            wait(50);
            require(editor->verticalScrollBar()->value() == position, "paused low-speed scrolling stays still");
            std::printf("PASS SPEED RATE: %.1fx position=%d expected=%.2f pixels\n", speed, position, expected);
        }
        require(editor->toHtml() == beforeRateTest, "speed stress preserves rich text");
        std::printf("PASS: 1000 scroll start/speed/pause cycles and sustained timer advancement\n");
        std::fflush(stdout);
        const auto htmlBeforeSearch = editor->toHtml();
        dock.findChild<QAction *>("findText")->trigger();
        auto *search = dock.findChild<QLineEdit *>("searchText");
        for (int i = 0; i < 500; ++i) {
            const auto term = QString("Paragraph %1").arg(i % 250);
            search->setText(term);
            dock.findChild<QToolButton *>("findNext")->click();
            require(editor->textCursor().selectedText() == term, "search failed to select the requested text");
            dock.findChild<QToolButton *>("findNext")->click();
            dock.findChild<QToolButton *>("findPrevious")->click();
        }
        require(editor->toHtml() == htmlBeforeSearch, "search modified rich text during stress");
        dock.findChild<QToolButton *>("clearSearch")->click();
        auto *countdown = dock.findChild<QCheckBox *>("startCountdown");
        countdown->setChecked(true);
        for (int i = 0; i < 500; ++i) {
            dock.command(scenepad::Command::Top);
            dock.startScroll();
            require(dock.isCountingDown() && !dock.isScrolling(), "countdown failed to start");
            dock.command(scenepad::Command::Toggle);
            require(!dock.isCountingDown() && !dock.isScrolling(), "countdown failed to cancel");
        }
        countdown->setChecked(false);
        std::printf("PASS: 500 search/next/previous cycles and 500 countdown start/cancel cycles\n");
        std::fflush(stdout);
        dock.findChild<QToolButton *>("readMode")->click();
        require(!editor->isReadOnly(), "wheel stress requires editing mode");
        const auto htmlBeforeWheel = editor->toHtml();
        for (int i = 0; i < 200; ++i) {
            QWheelEvent event(QPointF(10, 10), editor->viewport()->mapToGlobal(QPoint(10, 10)),
                QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(editor->viewport(), &event);
            editor->undo();
            require(editor->toHtml() == htmlBeforeWheel, "wheel undo failed to preserve rich content");
            if (i % 10 == 0) QApplication::processEvents();
        }
        std::printf("PASS: 200 mixed-format Ctrl+wheel/undo cycles\n");
        std::fflush(stdout);
        for (int i = 0; i < 500; ++i) {
            const bool wasLocked = editor->isReadOnly();
            dock.command(scenepad::Command::ToggleLock);
            require(editor->isReadOnly() != wasLocked, "lock command failed to toggle the reading state");
            if (i % 10 == 0) QApplication::processEvents();
        }
        std::printf("PASS: 500 lock/unlock commands and immediate theme refreshes\n");
        std::fflush(stdout);
        dock.shutdown();
        {
            OBSSceneAutoRelease largeScene = obs_scene_create("Line-number stress");
            scenepad::Dock large(temp.filePath("large.ini"));
            large.resize(550, 500);
            large.bindScene(obs_scene_get_source(largeScene));
            large.show();
            auto *largeEdit = large.findChild<QTextEdit *>("noteEditor");
            largeEdit->setPlainText(QString("Line-number stress\n").repeated(9999) + "Last");
            require(largeEdit->document()->blockCount() == 10000, "large line count mismatch");
            auto *largeSearch = large.findChild<QLineEdit *>("searchText");
            QElapsedTimer searchClock;
            searchClock.start();
            largeSearch->setText("stress");
            large.findChild<QToolButton *>("findNext")->click();
            const auto countMilliseconds = searchClock.elapsed();
            require(large.findChild<QLabel *>("searchStatus")->text() == "1/9999", "large document search count mismatch");
            searchClock.restart();
            for (int i = 0; i < 500; ++i) large.findChild<QToolButton *>("findNext")->click();
            require(large.findChild<QLabel *>("searchStatus")->text() == "501/9999", "cached search index mismatch");
            std::printf("PASS: 9999 search matches counted in %lld ms; 500 cached next commands in %lld ms\n",
                countMilliseconds, searchClock.elapsed());
            large.findChild<QToolButton *>("clearSearch")->click();
            auto *bar = largeEdit->verticalScrollBar();
            for (int i = 0; i < 500; ++i) {
                bar->setValue(static_cast<int>(static_cast<qint64>(bar->maximum()) * (i % 101) / 100));
                if (i % 10 == 0) QApplication::processEvents();
            }
            require(largeEdit->toPlainText().endsWith("Last"), "gutter scrolling changed document content");
        }
        std::printf("PASS: 10000 paragraphs and 500 gutter/scroll updates\n");
        std::fflush(stdout);
        for (int i = 0; i < 250; ++i) {
            if (i % 50 == 0) std::printf("Dock lifecycle %d: process memory %.2f MiB\n", i, memory() / 1048576.0);
            scenepad::Dock reopened(temp.filePath("prefs.ini"));
            auto *source = obs_scene_get_source(scenes[i % 100]);
            reopened.bindScene(source);
            auto *edit = reopened.findChild<QTextEdit *>("noteEditor");
            edit->setReadOnly(false);
            edit->append(QString("reopen%1").arg(i));
            reopened.flush(false);
            OBSDataAutoRelease snapshot = obs_save_source(source);
            require(!QByteArray(obs_data_get_json(snapshot)).contains(QByteArray("reopen") + QByteArray::number(i)) &&
                notes.read(source).html.contains(QString("reopen%1").arg(i)), "independent save failed after reopen");
            reopened.shutdown();
            QApplication::processEvents();
        }
        std::printf("Dock lifecycle 250: process memory %.2f MiB\n", memory() / 1048576.0);
        std::printf("PASS: 250 dock construction/save/destruction cycles\n");
        std::fflush(stdout);
        if (argc > 1) {
            for (int i = 0; i < 30; ++i) {
                const auto word = scenepad::importDocx(QString::fromLocal8Bit(argv[1]));
                require(word.error.isEmpty() && word.html.contains("Word import test"), "Word stress import failed");
            }
            std::printf("PASS: 30 large Word imports\n");
        }
        if (argc > 2) {
            long long peak = 0;
            for (int i = 0; i < 5; ++i) {
                QElapsedTimer clock; clock.start();
                const auto word = scenepad::importDocx(QString::fromLocal8Bit(argv[2]));
                peak = qMax(peak, clock.elapsed());
                require(word.error == "TextLimit" && word.html.isEmpty(), "oversized Word import must fail without partial content");
            }
            std::printf("PASS: 5 rejected Word imports with 505000 characters and 3000 formats; peak %lld ms\n", peak);
            require(peak < 5000, "single-paragraph Word import exceeds 5 seconds");
        }
        QApplication::processEvents();
        const size_t finalMemory = memory();
        const auto delta = static_cast<long long>(finalMemory) - static_cast<long long>(baseline);
        require(delta < 128LL * 1024 * 1024, "unexpected memory growth exceeded 128 MiB");
        std::printf("Process memory baseline=%.2f MiB, final=%.2f MiB, delta=%.2f MiB\n",
            baseline / 1048576.0, finalMemory / 1048576.0, delta / 1048576.0);
        std::printf("PASS: stress completed in %.2f seconds\n", total.elapsed() / 1000.0);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    obs_shutdown();
    return result;
}
