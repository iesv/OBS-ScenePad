#pragma once
#include <QTextBlock>
#include <QTextDocument>

namespace scenepad {
constexpr int maxNoteCharacters = 200000;
constexpr int maxParagraphCharacters = 10000;
constexpr int maxNoteParagraphs = 20000;

inline bool withinTextLimits(const QTextDocument &document)
{
    if (document.characterCount() - 1 > maxNoteCharacters || document.blockCount() > maxNoteParagraphs) return false;
    for (auto block = document.begin(); block.isValid(); block = block.next())
        if (block.length() - 1 > maxParagraphCharacters) return false;
    return true;
}

inline bool withinHtmlLimits(const QString &html)
{
    // Validate imported text without laying it out in the editor.
    QTextDocument document;
    document.setLayoutEnabled(false);
    document.setUndoRedoEnabled(false);
    document.setHtml(html);
    return withinTextLimits(document);
}
}
