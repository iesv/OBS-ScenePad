#include "editor.hpp"
#include "locale.hpp"
#include "text-limits.hpp"
#include <QAbstractTextDocumentLayout>
#include <QPainter>
#include <QDropEvent>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QScrollBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextLayout>
#include <QTextDocumentFragment>
#include <QWheelEvent>
#include <memory>

namespace scenepad {
bool Editor::acceptsText(const QString &text, const QTextCursor &selection)
{
    // Inspect inserted text and the two boundary paragraphs, never the whole note.
    bool allowed = document()->characterCount() - 1 - selection.selectionEnd() + selection.selectionStart() + text.size() <= maxNoteCharacters;
    if (!allowed) { if (limitReached) limitReached(); return false; }
    const auto first = document()->findBlock(selection.selectionStart());
    const auto last = document()->findBlock(selection.selectionEnd());
    int length = selection.selectionStart() - first.position();
    int paragraphs = document()->blockCount() - last.blockNumber() + first.blockNumber();
    for (const auto character : text) {
        if (character == '\n' || character == '\r' || character == QChar::ParagraphSeparator) {
            ++paragraphs;
            allowed = allowed && length <= maxParagraphCharacters;
            length = 0;
        } else ++length;
    }
    length += last.position() + last.length() - 1 - selection.selectionEnd();
    allowed = allowed && length <= maxParagraphCharacters && paragraphs <= maxNoteParagraphs;
    if (!allowed && limitReached) limitReached();
    return allowed;
}

void Editor::insertCheckedText(const QString &text)
{
    const auto normalized = QString(text).replace("\r\n", "\n").replace('\r', '\n');
    if (!isReadOnly() && acceptsText(normalized, textCursor())) insertPlainText(normalized);
}

void Editor::deleteSelection(bool cut)
{
    auto selection = textCursor();
    if (isReadOnly() || !selection.hasSelection() || !acceptsText({}, selection)) return;
    if (cut) QTextEdit::cut(); else selection.removeSelectedText();
}

void Editor::insertFromMimeData(const QMimeData *source)
{
    if (isReadOnly()) return;
    if (!source->hasHtml()) { insertCheckedText(source->text()); return; }
    if (source->hasText() && !acceptsText(QString(source->text()).replace("\r\n", "\n").replace('\r', '\n'), textCursor())) return;
    const auto fragment = QTextDocumentFragment::fromHtml(source->html(), document());
    QTextDocument imported;
    imported.setLayoutEnabled(false);
    QTextCursor(&imported).insertFragment(fragment);
    if (!withinTextLimits(imported)) { if (limitReached) limitReached(); return; }
    if (acceptsText(imported.toRawText(), textCursor())) textCursor().insertFragment(fragment);
}

void Editor::dropEvent(QDropEvent *event)
{
    // Reject before Qt removes the source of a move operation.
    const auto selection = cursorForPosition(event->position().toPoint());
    const auto *source = event->mimeData();
    QTextDocument imported;
    imported.setLayoutEnabled(false);
    if (source->hasHtml()) imported.setHtml(source->html()); else imported.setPlainText(source->text());
    if (!withinTextLimits(imported) || !acceptsText(imported.toRawText(), selection)) { event->ignore(); return; }
    QTextEdit::dropEvent(event);
}

void Editor::keyPressEvent(QKeyEvent *event)
{
    if (isReadOnly()) { QTextEdit::keyPressEvent(event); return; }
    auto selection = textCursor();
    if (event->matches(QKeySequence::Cut)) { deleteSelection(true); return; }
    if (event->matches(QKeySequence::Paste) || event->matches(QKeySequence::Undo) || event->matches(QKeySequence::Redo)) {
        QTextEdit::keyPressEvent(event); return;
    }
    QString inserted;
    bool changesText = false;
    if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete ||
        event->matches(QKeySequence::Backspace) || event->matches(QKeySequence::Delete) ||
        event->matches(QKeySequence::DeleteStartOfWord) || event->matches(QKeySequence::DeleteEndOfWord) ||
        event->matches(QKeySequence::DeleteEndOfLine)) {
        if (!selection.hasSelection()) {
            auto movement = event->key() == Qt::Key_Backspace || event->matches(QKeySequence::Backspace) ? QTextCursor::PreviousCharacter : QTextCursor::NextCharacter;
            if (event->matches(QKeySequence::DeleteStartOfWord)) movement = QTextCursor::PreviousWord;
            if (event->matches(QKeySequence::DeleteEndOfWord)) movement = QTextCursor::NextWord;
            if (event->matches(QKeySequence::DeleteEndOfLine)) movement = QTextCursor::EndOfBlock;
            selection.movePosition(movement, QTextCursor::KeepAnchor);
            if (!selection.hasSelection() && event->matches(QKeySequence::DeleteEndOfLine)) selection.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        }
        changesText = true;
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        inserted = event->modifiers().testFlag(Qt::ShiftModifier) ? QString(QChar::LineSeparator) : "\n";
        changesText = true;
    } else if (!event->text().isEmpty() && (!event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::AltModifier)) &&
        !event->modifiers().testFlag(Qt::MetaModifier)) {
        inserted = event->text(); changesText = true;
    }
    if (changesText && !acceptsText(inserted, selection)) { event->accept(); return; }
    QTextEdit::keyPressEvent(event);
}

void Editor::inputMethodEvent(QInputMethodEvent *event)
{
    auto selection = textCursor();
    std::unique_ptr<QTextDocument> replacement;
    // IME reconversion can replace a selection and a second range in one commit.
    if (selection.hasSelection() && (event->replacementLength() || event->replacementStart())) {
        replacement.reset(document()->clone());
        replacement->setLayoutEnabled(false);
        QTextCursor copy(replacement.get());
        copy.setPosition(selection.selectionStart()); copy.setPosition(selection.selectionEnd(), QTextCursor::KeepAnchor);
        copy.removeSelectedText(); selection = copy;
    }
    if (event->replacementLength() || event->replacementStart()) {
        const int start = selection.selectionStart() + event->replacementStart();
        const int end = selection.document()->characterCount() - 1;
        selection.setPosition(qBound(0, start, end));
        selection.setPosition(qBound(0, start + event->replacementLength(), end), QTextCursor::KeepAnchor);
    }
    bool allowed = true;
    if (replacement) {
        selection.insertText(event->commitString()); allowed = withinTextLimits(*replacement);
        if (!allowed && limitReached) limitReached();
    } else if (!event->commitString().isEmpty() || event->replacementLength()) allowed = acceptsText(event->commitString(), selection);
    if (!allowed) {
        QInputMethodEvent cancel;
        QTextEdit::inputMethodEvent(&cancel);
        event->accept(); return;
    }
    QTextEdit::inputMethodEvent(event);
}

Editor::Editor(QWidget *parent) : QTextEdit(parent)
{
    // Defaults apply during parsing; explicit paragraph spacing takes precedence.
    document()->setDefaultStyleSheet("p, li { line-height: 140%; }");
    loadHtml({});
    // Prefer word boundaries; split only overlong runs.
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    numbers_ = new QWidget(this);
    numbers_->setObjectName("lineNumbers");
    numbers_->setAccessibleName(text("LineNumbers"));
    numbers_->setAttribute(Qt::WA_TransparentForMouseEvents);
    numbers_->setAttribute(Qt::WA_OpaquePaintEvent);
    numbers_->installEventFilter(this);
    connect(document(), &QTextDocument::blockCountChanged, this, &Editor::updateNumberArea);
    connect(document(), &QTextDocument::contentsChanged, numbers_, [this] { numbers_->update(); });
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::update, numbers_, [this] { numbers_->update(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, numbers_, [this] { numbers_->update(); });
    connect(this, &QTextEdit::cursorPositionChanged, numbers_, [this] { numbers_->update(); });
    updateNumberArea();
}

void Editor::loadHtml(const QString &html)
{
    setHtml(html);
    if (document()->isEmpty() && !textCursor().blockFormat().hasProperty(QTextFormat::LineHeight)) {
        // Empty documents have no paragraph styles to inherit.
        const bool undo = document()->isUndoRedoEnabled();
        document()->setUndoRedoEnabled(false);
        auto format = textCursor().blockFormat();
        format.setLineHeight(140, QTextBlockFormat::ProportionalHeight);
        textCursor().setBlockFormat(format);
        document()->setUndoRedoEnabled(undo);
    }
}

void Editor::setSearchMatch(bool active)
{
    if (searchMatch_ == active) return;
    searchMatch_ = active;
    // Override theme selection colors only while a search result is selected.
    setProperty("searchMatch", active);
    style()->unpolish(this);
    style()->polish(this);
    viewport()->update();
}

void Editor::setSideSpace(int width)
{
    if (viewportMargins().right() == width) return;
    // Keep the side controls outside the text layout.
    auto margins = viewportMargins();
    margins.setRight(width);
    setViewportMargins(margins);
    updateNumberArea();
}

void Editor::updateNumberArea()
{
    auto numberFont = font();
    numberFont.setBold(true);
    numbers_->setFont(numberFont);
    // Reserve the widest digit to keep the gutter stable.
    const auto metrics = numbers_->fontMetrics();
    int digitWidth = 0;
    for (const auto digit : QStringLiteral("0123456789")) digitWidth = qMax(digitWidth, metrics.horizontalAdvance(digit));
    const int digits = static_cast<int>(QString::number(document()->blockCount()).size());
    const int width = digits * digitWidth + 14;
    if (viewportMargins().left() != width) setViewportMargins(width, 0, viewportMargins().right(), 0);
    numbers_->setGeometry(contentsRect().left(), viewport()->y(), width, viewport()->height());
    numbers_->update();
}

bool Editor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == numbers_ && event->type() == QEvent::Paint) {
        paintNumbers();
        return true;
    }
    return QTextEdit::eventFilter(watched, event);
}

void Editor::paintNumbers()
{
    QPainter painter(numbers_);
    painter.fillRect(numbers_->rect(), palette().color(isReadOnly() || !hasFocus() ? QPalette::Base : QPalette::Window));
    auto foreground = palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, QPalette::WindowText);
    const auto accent = (parentWidget() ? parentWidget()->palette() : QPalette()).color(QPalette::Highlight);
    const auto current = (parentWidget() ? parentWidget()->palette() : QPalette()).color(QPalette::HighlightedText);
    foreground.setAlpha(170);
    painter.setFont(numbers_->font());
    auto *layout = document()->documentLayout();
    const int scroll = verticalScrollBar()->value();
    const int active = textCursor().blockNumber();
    // Paint visible paragraph numbers.
    for (auto block = cursorForPosition(QPoint(0, 0)).block(); block.isValid(); block = block.next()) {
        const auto bounds = layout->blockBoundingRect(block);
        if (bounds.top() - scroll > numbers_->height()) break;
        if (!block.isVisible() || !block.layout() || block.layout()->lineCount() == 0) continue;
        const auto line = block.layout()->lineAt(0);
        if (isEnabled() && block.blockNumber() == active)
            painter.fillRect(QRectF(0, bounds.top() + line.y() - scroll, numbers_->width(), line.height()), accent);
        const qreal baseline = bounds.top() + line.y() + line.ascent() - scroll;
        const auto number = QString::number(block.blockNumber() + 1);
        painter.setPen(block.blockNumber() == active ? current : foreground);
        painter.drawText(QPointF(numbers_->width() - 7 - painter.fontMetrics().horizontalAdvance(number), baseline), number);
    }
}

void Editor::resizeEvent(QResizeEvent *event)
{
    QTextEdit::resizeEvent(event);
    updateNumberArea();
}

bool Editor::event(QEvent *event)
{
    if (event->type() != QEvent::ReadOnlyChange && event->type() != QEvent::FocusIn && event->type() != QEvent::FocusOut)
        return QTextEdit::event(event);
    const bool handled = QTextEdit::event(event);
    if (numbers_) {
        if (event->type() == QEvent::ReadOnlyChange) {
            // Refresh lock colors without resetting document fonts or layout.
            restyling_ = true;
            setStyleSheet(styleSheet());
            restyling_ = false;
        }
        numbers_->update();
        update();
        viewport()->update();
    }
    return handled;
}

void Editor::changeEvent(QEvent *event)
{
    if (restyling_ && event->type() == QEvent::FontChange) return;
    QTextEdit::changeEvent(event);
    if (numbers_ && !restyling_ && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
        event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::EnabledChange)) {
        updateNumberArea();
    }
}

void Editor::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() == Qt::ControlModifier) {
        if (!isReadOnly()) {
            wheelDelta_ += event->angleDelta().y();
            const int steps = wheelDelta_ / 120;
            wheelDelta_ %= 120;
            if (steps) resizeText(steps);
        }
        event->accept();
        return;
    }
    wheelDelta_ = 0;
    QTextEdit::wheelEvent(event);
}

void Editor::resizeText(int steps)
{
    // Preserve rich formatting and group each resize into one undo step.
    struct Run { int start; int length; QTextCharFormat format; };
    QList<Run> runs;
    for (auto block = document()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            auto format = fragment.charFormat();
            const auto f = format.font().resolve(document()->defaultFont());
            const qreal size = f.pointSizeF() > 0 ? f.pointSizeF() : f.pixelSize() * 72.0 / logicalDpiY();
            format.clearProperty(QTextFormat::FontPixelSize);
            format.setFontPointSize(qBound(6.0, size + steps, 144.0));
            if (format != fragment.charFormat()) runs.append({fragment.position(), fragment.length(), format});
        }
    if (runs.isEmpty()) {
        if (document()->isEmpty()) setFontPointSize(qBound(6.0, currentFont().pointSizeF() + steps, 144.0));
        return;
    }
    QTextCursor edit(document());
    edit.beginEditBlock();
    for (const auto &run : runs) {
        edit.setPosition(run.start);
        edit.setPosition(run.start + run.length, QTextCursor::KeepAnchor);
        edit.setCharFormat(run.format);
    }
    edit.endEditBlock();
}
}
