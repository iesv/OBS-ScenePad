#include "test-helpers.hpp"
#include "editor.hpp"
#include "locale.hpp"
#include "text-limits.hpp"
#include "web-document.hpp"
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMimeData>
#include <QTextBlock>
#include <cstdio>

const char *obs_module_text(const char *key) { return key; }
namespace {
using scenepad::test::require;
using scenepad::test::checks;
void key(scenepad::Editor &editor, int value, const QString &text = {})
{
    QKeyEvent event(QEvent::KeyPress, value, Qt::NoModifier, text);
    QApplication::sendEvent(&editor, &event);
}
void at(scenepad::Editor &editor, int start, int end = -1)
{
    auto cursor = editor.textCursor(); cursor.setPosition(start);
    if (end >= 0) cursor.setPosition(end, QTextCursor::KeepAnchor);
    editor.setTextCursor(cursor);
}
}
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    try {
        scenepad::Editor editor;
        editor.resize(500, 400); editor.show();
        require(editor.document()->begin().blockFormat().lineHeight() == 140, "empty script uses 1.4x spacing");
        editor.insertPlainText("First\nSecond");
        require(editor.document()->lastBlock().blockFormat().lineHeight() == 140, "new paragraphs inherit default spacing");
        editor.setHtml("<p style='line-height:180%'>Custom</p><p>Default</p>");
        require(editor.document()->begin().blockFormat().lineHeight() == 180 &&
            editor.document()->lastBlock().blockFormat().lineHeight() == 140, "HTML keeps custom spacing and defaults unstyled paragraphs");
        int rejected = 0; editor.limitReached = [&] { ++rejected; };
        const QString boundary = QString(10000, 'A') + ("\n" + QString(9999, 'B')).repeated(19);
        require(boundary.size() == 200000, "boundary fixture");
        editor.setPlainText(boundary); at(editor, 200000);
        require(scenepad::withinTextLimits(*editor.document()), "exact note limit is valid");
        key(editor, Qt::Key_X, "x");
        require(editor.toPlainText() == boundary && rejected == 1, "typing beyond the total cap preserves the note");
        at(editor, 199999, 200000); key(editor, Qt::Key_X, "x");
        require(editor.document()->characterCount() == 200001 && editor.toPlainText().endsWith('x'), "selected text can be replaced at the cap");
        editor.undo(); require(editor.toPlainText() == boundary, "rejected input does not damage undo history");
        auto *plain = new QMimeData;
        plain->setText(QString(boundary).replace("\n", "\r\n"));
        QApplication::clipboard()->setMimeData(plain);
        editor.setPlainText("Replace"); editor.selectAll(); editor.paste();
        require(editor.toPlainText() == boundary && scenepad::withinTextLimits(*editor.document()), "plain paste normalizes CRLF before checking the cap");
        editor.undo(); require(editor.toPlainText() == "Replace", "plain paste remains a single undo step");
        editor.setPlainText(QString(19999, '\n')); at(editor, 19999);
        require(scenepad::withinTextLimits(*editor.document()), "exact paragraph-count limit is valid");
        key(editor, Qt::Key_Return);
        require(editor.document()->blockCount() == 20000, "blank paragraphs cannot bypass the layout limit");
        key(editor, Qt::Key_X, "x");
        require(editor.toPlainText().endsWith('x'), "text remains editable at the paragraph-count limit");
        editor.setPlainText(QString(10000, 'A')); at(editor, 10000);
        key(editor, Qt::Key_X, "x");
        require(editor.toPlainText().size() == 10000, "single-paragraph cap");
        key(editor, Qt::Key_Return); key(editor, Qt::Key_X, "x");
        require(editor.document()->blockCount() == 2 && editor.toPlainText().endsWith("\nx"), "Enter starts a new paragraph");
        const QString merge = QString(6000, 'A') + "\n" + QString(6000, 'B');
        editor.setPlainText(merge); at(editor, 6001); key(editor, Qt::Key_Backspace);
        require(editor.toPlainText() == merge, "backspace cannot create an oversized paragraph");
        at(editor, 6000, 6001); editor.deleteSelection(true);
        require(editor.toPlainText() == merge, "cut cannot bypass the paragraph cap");
        at(editor, 0, 6001); editor.insertCheckedText("New\n");
        require(editor.toPlainText().startsWith("New\n") && editor.document()->blockCount() == 2, "multi-paragraph replacement validates both boundaries");
        editor.setPlainText(QString(9999, 'A')); at(editor, 9999);
        QInputMethodEvent commit; commit.setCommitString(QString::fromUtf8("中"));
        QApplication::sendEvent(&editor, &commit);
        require(editor.toPlainText().size() == 10000, "IME can commit at the exact limit");
        QApplication::sendEvent(&editor, &commit);
        require(editor.toPlainText().size() == 10000, "IME cannot exceed the limit");
        commit.setCommitString("B", -1, 1); QApplication::sendEvent(&editor, &commit);
        require(editor.toPlainText().endsWith('B') && editor.toPlainText().size() == 10000, "IME replacement remains available at the cap");
        auto *mime = new QMimeData;
        mime->setHtml("<p><b>" + QString(10001, 'W') + "</b></p>");
        QApplication::clipboard()->setMimeData(mime);
        editor.selectAll(); editor.paste();
        require(editor.toPlainText().size() == 10000 && editor.toPlainText().endsWith('B'), "oversized rich paste preserves the selection");
        mime = new QMimeData; mime->setHtml("<p><b>Bold</b></p>"); QApplication::clipboard()->setMimeData(mime);
        editor.paste();
        require(editor.toPlainText() == "Bold" && editor.document()->find("Bold").charFormat().fontWeight() >= 700, "accepted paste preserves formatting");
        const auto reconversion = QString(4000, 'A') + "\n" + QString(4000, 'B') + "\n" + QString(4000, 'C');
        editor.setPlainText(reconversion); at(editor, 4000, 4001);
        commit.setCommitString("x", 4000, 1); QApplication::sendEvent(&editor, &commit);
        require(editor.toPlainText() == reconversion, "IME reconversion cannot merge three paragraphs past the cap");
        require(scenepad::withinHtmlLimits("<p>" + QString(10000, 'W') + "</p>") &&
            !scenepad::withinHtmlLimits("<p>" + QString(10001, 'W') + "</p>"), "HTML validates exact paragraph boundaries");
        const auto document = scenepad::webDocument("<p>Short</p>");
        auto oversized = document;
        auto blocks = oversized.value("blocks").toArray(); auto block = blocks[0].toObject();
        auto runs = block.value("runs").toArray(); auto run = runs[0].toObject(); run.insert("text", QString(10001, 'W'));
        runs[0] = run; block.insert("runs", runs); blocks[0] = block; oversized.insert("blocks", blocks);
        QString output = "untouched";
        bool limited = false;
        require(!scenepad::applyWebDocument("<p>Short</p>", oversized, output, &limited) && limited && output == "untouched",
            "server conversion rejects oversized data without modifying output");
        editor.setPlainText(boundary); at(editor, 50000);
        QElapsedTimer timer; timer.start();
        for (int i = 0; i < 10000; ++i) require(!editor.acceptsText("x", editor.textCursor()), "repeated boundary rejection");
        const auto elapsed = timer.elapsed();
        require(elapsed < 1000, "normal limit checks must not scan the complete note");
        const QString huge(10000000, 'W'); timer.restart();
        for (int i = 0; i < 1000; ++i) require(!editor.acceptsText(huge, editor.textCursor()), "huge paste rejection");
        require(timer.elapsed() < 500, "oversized pastes must be rejected before scanning their content");
        std::printf("LIMITS: %d checks; 10000 native boundary checks in %lld ms\n", checks, elapsed);
    } catch (const std::exception &error) { std::fprintf(stderr, "FAIL limits: %s\n", error.what()); return 1; }
    return 0;
}
