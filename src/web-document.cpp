#include "web-document.hpp"
#include "text-limits.hpp"
#include <QJsonArray>
#include <QColor>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextFrame>
#include <QTextList>
#include <cmath>

namespace scenepad {
namespace {
QJsonObject runFormat(const QTextCharFormat &format)
{
    const auto families = format.fontFamilies().toStringList();
    return {{"weight", format.fontWeight()}, {"italic", format.fontItalic()},
        {"underline", format.fontUnderline()}, {"strike", format.fontStrikeOut()},
        {"size", format.fontPointSize()}, {"family", families.value(0)},
        {"color", format.foreground().style() == Qt::NoBrush ? QString() : format.foreground().color().name()},
        {"background", format.background().style() == Qt::NoBrush ? QString() : format.background().color().name()}};
}

bool simpleDocument(const QTextDocument &doc)
{
    if (!doc.rootFrame()->childFrames().isEmpty()) return false;
    for (auto block = doc.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto format = it.fragment().charFormat();
            if (format.isImageFormat() || format.isAnchor() || format.objectType() != QTextFormat::NoObject || format.font().pixelSize() > 0 ||
                format.verticalAlignment() != QTextCharFormat::AlignNormal) return false;
            for (const auto &brush : {format.foreground(), format.background()})
                if (brush.style() != Qt::NoBrush && (brush.style() != Qt::SolidPattern || brush.color().alpha() != 255)) return false;
        }
    return true;
}

bool readFormat(const QJsonObject &input, QTextCharFormat &format)
{
    const auto size = input.value("size").toDouble(-1);
    const int weight = input.value("weight").toInt(-1);
    const auto family = input.value("family").toString();
    if (!std::isfinite(size) || size < 0 || size > 500 || weight < 1 || weight > 1000 || family.size() > 200) return false;
    format.setFontWeight(weight);
    format.setFontItalic(input.value("italic").toBool());
    format.setFontUnderline(input.value("underline").toBool());
    format.setFontStrikeOut(input.value("strike").toBool());
    if (size > 0) format.setFontPointSize(size);
    if (!family.isEmpty()) format.setFontFamilies({family});
    for (const auto *key : {"color", "background"}) {
        const auto value = input.value(key).toString();
        const QColor color(value);
        if (value.size() > 9 || (!value.isEmpty() && !color.isValid())) return false;
        const QBrush brush(value.isEmpty() ? QBrush(Qt::NoBrush) : QBrush(color));
        if (qstrcmp(key, "color") == 0) format.setForeground(brush);
        else format.setBackground(brush);
    }
    return true;
}

QJsonObject exportDocument(const QTextDocument &doc, bool basic = false)
{
    QJsonArray blocks;
    int runCount = 0;
    if (!withinTextLimits(doc)) return {};
    for (auto block = doc.begin(); block.isValid(); block = block.next()) {
        QJsonArray runs;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            if (++runCount > 100000) return {};
            const auto format = it.fragment().charFormat();
            auto run = runFormat(format);
            auto text = it.fragment().text();
            if (basic) {
                // Retain common text styles while removing unsupported objects and units.
                const auto font = format.font().resolve(doc.defaultFont());
                run.insert("family", font.family().left(200));
                run.insert("size", qBound(0.0, font.pixelSize() > 0 ? font.pixelSize() * 0.75 : font.pointSizeF(), 500.0));
                if (format.foreground().style() != Qt::SolidPattern || format.foreground().color().alpha() != 255) run.insert("color", QString());
                if (format.background().style() != Qt::SolidPattern || format.background().color().alpha() != 255) run.insert("background", QString());
                text.remove(QChar::ObjectReplacementCharacter);
            }
            run.insert("text", text);
            runs.append(run);
        }
        const auto *list = block.textList();
        const auto align = block.blockFormat().alignment() & Qt::AlignHorizontal_Mask;
        blocks.append(QJsonObject{{"runs", runs}, {"align", static_cast<int>(align ? align : Qt::AlignLeft)},
            {"indent", block.blockFormat().indent()}, {"list", list ? static_cast<int>(list->format().style()) : 0},
            {"listIndent", list ? list->format().indent() : 1}, {"listGroup", list ? list->objectIndex() + 1 : 0}, {"listStart", list ? list->format().start() : 1}});
    }
    return {{"blocks", blocks}, {"editable", simpleDocument(doc)}};
}
}

QJsonObject webDocument(const QString &html)
{
    QTextDocument doc;
    // Conversion needs text and formatting, without visual layout.
    doc.setLayoutEnabled(false);
    doc.setUndoRedoEnabled(false);
    doc.setHtml(html);
    return exportDocument(doc);
}

QString plainText(const QString &html)
{
    QTextDocument doc;
    doc.setLayoutEnabled(false);
    doc.setUndoRedoEnabled(false);
    doc.setHtml(html);
    return doc.toPlainText().remove(QChar::ObjectReplacementCharacter);
}

bool basicHtml(const QTextDocument &document, QString &html)
{
    // Reuse the browser format validator and builder without visual layout.
    const auto data = exportDocument(document, true);
    return !data.isEmpty() && applyWebDocument({}, data, html);
}

bool applyWebDocument(const QString &original, const QJsonObject &input, QString &html, bool *limitExceeded)
{
    if (limitExceeded) *limitExceeded = false;
    // Parse the original once for validation and formatting preservation.
    QTextDocument doc;
    doc.setLayoutEnabled(false);
    doc.setUndoRedoEnabled(false);
    doc.setHtml(original);
    const auto before = exportDocument(doc);
    if (before.isEmpty() || !before.value("editable").toBool() || !input.value("blocks").isArray()) return false;
    const auto blocks = input.value("blocks").toArray();
    if (blocks.isEmpty()) return false;
    if (blocks.size() > maxNoteParagraphs) { if (limitExceeded) *limitExceeded = true; return false; }
    struct Run { int start; int length; QTextCharFormat format; };
    QList<Run> formats;
    QString plain;
    for (const auto &value : blocks) {
        if (!value.isObject()) return false;
        const auto block = value.toObject();
        int blockLength = 0;
        const int alignment = block.value("align").toInt();
        const int list = block.value("list").toInt(100);
        const int indent = block.value("indent").toInt(-1);
        const int listIndent = block.value("listIndent").toInt();
        const int listStart = block.value("listStart").toInt();
        if ((alignment != Qt::AlignLeft && alignment != Qt::AlignRight && alignment != Qt::AlignHCenter && alignment != Qt::AlignJustify) ||
            list > 0 || list < QTextListFormat::ListUpperRoman || indent < 0 || indent > 12 || listIndent < 1 || listIndent > 12 ||
            listStart < 1 || listStart > 10000 || !block.value("runs").isArray()) return false;
        for (const auto &entry : block.value("runs").toArray()) {
            if (!entry.isObject() || formats.size() >= 100000) return false;
            const auto run = entry.toObject();
            if (!run.value("text").isString()) return false;
            const auto text = run.value("text").toString();
            blockLength += static_cast<int>(text.size());
            if (blockLength > maxParagraphCharacters || plain.size() + text.size() > maxNoteCharacters) {
                if (limitExceeded) *limitExceeded = true;
                return false;
            }
            QTextCharFormat format;
            if (!readFormat(run, format) || text.contains(QChar::ParagraphSeparator) || text.contains('\n') ||
                text.contains(QChar::ObjectReplacementCharacter) || text.contains(QChar::Null) || text.contains('\r') || !text.isValidUtf16()) return false;
            formats.append({static_cast<int>(plain.size()), static_cast<int>(text.size()), format});
            plain += text;
        }
        plain += '\n';
    }
    plain.chop(1);
    if (plain.size() > maxNoteCharacters) { if (limitExceeded) *limitExceeded = true; return false; }
    if (blocks == before.value("blocks").toArray()) { html = original; return true; }
    const auto previous = doc.toPlainText();
    int prefix = 0;
    while (prefix < previous.size() && prefix < plain.size() && previous[prefix] == plain[prefix]) ++prefix;
    if (prefix > 0 && prefix < previous.size() && previous[prefix].isLowSurrogate()) --prefix;
    int suffix = 0;
    while (suffix < previous.size() - prefix && suffix < plain.size() - prefix &&
        previous[previous.size() - 1 - suffix] == plain[plain.size() - 1 - suffix]) ++suffix;
    if (suffix > 0 && suffix < previous.size() - prefix && previous[previous.size() - suffix].isLowSurrogate()) --suffix;
    // Preserve untouched Qt formatting when the browser changes text.
    QTextCursor cursor(&doc);
    // Batch formatting to avoid per-run cursor layout.
    cursor.beginEditBlock();
    cursor.setPosition(prefix);
    cursor.setPosition(static_cast<int>(previous.size()) - suffix, QTextCursor::KeepAnchor);
    cursor.insertText(plain.mid(prefix, plain.size() - prefix - suffix));
    for (const auto &run : formats) {
        cursor.setPosition(run.start);
        cursor.setPosition(run.start + run.length, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(run.format);
    }
    cursor.endEditBlock();
    auto block = doc.begin();
    QTextList *currentList = nullptr;
    int previousStyle = 0, previousIndent = 0, previousGroup = 0;
    for (const auto &value : blocks) {
        if (!block.isValid()) return false;
        const auto data = value.toObject();
        cursor.setPosition(block.position());
        auto format = block.blockFormat();
        format.setAlignment(static_cast<Qt::Alignment>(data.value("align").toInt()));
        format.setIndent(data.value("indent").toInt());
        cursor.setBlockFormat(format);
        const int style = data.value("list").toInt();
        const int indent = data.value("listIndent").toInt();
        const int group = data.value("listGroup").toInt();
        if (!style) {
            if (auto *list = block.textList()) list->remove(block);
            currentList = nullptr;
        } else if (currentList && group == previousGroup && style == previousStyle && indent == previousIndent) currentList->add(block);
        else if (block.textList() && block.textList()->objectIndex() + 1 == group && block.textList()->format().style() == style &&
            block.textList()->format().indent() == indent && block.textList()->format().start() == data.value("listStart").toInt())
            currentList = block.textList();
        else {
            QTextListFormat list;
            list.setStyle(static_cast<QTextListFormat::Style>(style));
            list.setIndent(indent);
            list.setStart(data.value("listStart").toInt());
            currentList = cursor.createList(list);
        }
        previousStyle = style;
        previousIndent = indent;
        previousGroup = group;
        block = block.next();
    }
    html = doc.toHtml();
    return true;
}
}
