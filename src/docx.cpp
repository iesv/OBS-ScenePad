#include "docx.hpp"
#include "text-limits.hpp"
#include "locale.hpp"
#include <QColor>
#include <QFileInfo>
#include <QHash>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QXmlStreamReader>
#ifdef _WIN32
#include <windows.h>
#include <msopc.h>
#include <wrl/client.h>
#else
#include <archive.h>
#include <archive_entry.h>
#include <memory>
#endif

namespace scenepad {
namespace {
#ifdef _WIN32
using Microsoft::WRL::ComPtr;
#endif
constexpr qint64 maxPartSize = 24 * 1024 * 1024;

QString attr(const QXmlStreamReader &xml, const char *name)
{
    for (const auto &a : xml.attributes())
        if (a.name() == QLatin1String(name)) return a.value().toString();
    return {};
}

bool enabled(const QXmlStreamReader &xml)
{
    const auto val = attr(xml, "val");
    return val != "0" && val != "false" && val != "off" && val != "none";
}

void runProperty(QXmlStreamReader &xml, QTextCharFormat &fmt)
{
    const auto name = xml.name();
    const auto value = attr(xml, "val");
    if (name == u"b") fmt.setFontWeight(enabled(xml) ? QFont::Bold : QFont::Normal);
    else if (name == u"i") fmt.setFontItalic(enabled(xml));
    else if (name == u"u") fmt.setFontUnderline(enabled(xml));
    else if (name == u"strike") fmt.setFontStrikeOut(enabled(xml));
    else if (name == u"sz") {
        bool ok = false;
        const auto size = value.toDouble(&ok) / 2.0;
        if (ok && size >= 1 && size <= 500) fmt.setFontPointSize(size);
    } else if (name == u"rFonts") {
        auto family = attr(xml, "eastAsia");
        if (family.isEmpty()) family = attr(xml, "ascii");
        if (family.isEmpty()) family = attr(xml, "hAnsi");
        if (!family.isEmpty()) fmt.setFontFamilies({family});
    } else if (name == u"color" && value != "auto") {
        const QColor color("#" + value);
        if (color.isValid()) fmt.setForeground(color);
    } else if (name == u"highlight") {
        static const QHash<QString, QString> colors{
            {"darkYellow", "#808000"}, {"darkBlue", "#000080"},
            {"darkCyan", "#008080"}, {"darkGreen", "#008000"},
            {"darkMagenta", "#800080"}, {"darkRed", "#800000"},
            {"lightGray", "#c0c0c0"}, {"darkGray", "#808080"}};
        const QColor color(colors.value(value, value));
        if (color.isValid()) fmt.setBackground(color);
    } else if (name == u"shd") {
        const QColor color("#" + attr(xml, "fill"));
        if (color.isValid()) fmt.setBackground(color);
    } else if (name == u"vertAlign") {
        if (value == "superscript") fmt.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
        if (value == "subscript") fmt.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    }
}

QTextCharFormat readRunProperties(QXmlStreamReader &xml)
{
    QTextCharFormat fmt;
    while (xml.readNextStartElement()) {
        runProperty(xml, fmt);
        xml.skipCurrentElement();
    }
    return fmt;
}

struct Paragraph {
    QTextBlockFormat block;
    QTextCharFormat text;
    QString style;
    int numId = -1;
    int level = 0;
};

Paragraph readParagraphProperties(QXmlStreamReader &xml)
{
    Paragraph p;
    while (xml.readNextStartElement()) {
        const auto name = xml.name();
        const auto value = attr(xml, "val");
        if (name == u"pStyle") p.style = value;
        else if (name == u"jc") {
            if (value == "center") p.block.setAlignment(Qt::AlignHCenter);
            else if (value == "right" || value == "end") p.block.setAlignment(Qt::AlignRight);
            else if (value == "both") p.block.setAlignment(Qt::AlignJustify);
            else p.block.setAlignment(Qt::AlignLeft);
        } else if (name == u"numPr") {
            while (xml.readNextStartElement()) {
                if (xml.name() == u"numId") p.numId = attr(xml, "val").toInt();
                else if (xml.name() == u"ilvl") p.level = qBound(0, attr(xml, "val").toInt(), 8);
                xml.skipCurrentElement();
            }
            continue;
        } else if (name == u"rPr") {
            p.text = readRunProperties(xml);
            continue;
        }
        xml.skipCurrentElement();
    }
    return p;
}

struct Style {
    QString parent;
    Paragraph properties;
};

QHash<QString, Style> readStyles(const QByteArray &data, QTextCharFormat &defaults)
{
    QHash<QString, Style> styles;
    QXmlStreamReader xml(data);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == u"rPrDefault") {
            while (xml.readNextStartElement()) {
                if (xml.name() == u"rPr") defaults = readRunProperties(xml);
                else xml.skipCurrentElement();
            }
        } else if (xml.name() == u"style") {
            const auto id = attr(xml, "styleId");
            Style style;
            while (xml.readNextStartElement()) {
                if (xml.name() == u"basedOn") {
                    style.parent = attr(xml, "val");
                    xml.skipCurrentElement();
                } else if (xml.name() == u"rPr") style.properties.text = readRunProperties(xml);
                else if (xml.name() == u"pPr") {
                    const auto text = style.properties.text;
                    style.properties = readParagraphProperties(xml);
                    style.properties.text.merge(text);
                } else xml.skipCurrentElement();
            }
            styles.insert(id, style);
        }
    }
    return styles;
}

Paragraph resolveStyle(const QHash<QString, Style> &styles, const QString &id, int depth = 0)
{
    if (depth > 16 || !styles.contains(id)) return {};
    const auto &style = styles[id];
    auto p = resolveStyle(styles, style.parent, depth + 1);
    p.text.merge(style.properties.text);
    p.block.merge(style.properties.block);
    if (style.properties.numId >= 0) {
        p.numId = style.properties.numId;
        p.level = style.properties.level;
    }
    return p;
}

struct Numbering {
    QHash<int, int> instances;
    QHash<QString, QTextListFormat::Style> levels;
};

Numbering readNumbering(const QByteArray &data)
{
    Numbering numbering;
    QXmlStreamReader xml(data);
    int abstractId = -1, level = 0, numId = -1;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == u"abstractNum") abstractId = attr(xml, "abstractNumId").toInt();
        else if (xml.name() == u"lvl") level = attr(xml, "ilvl").toInt();
        else if (xml.name() == u"num") numId = attr(xml, "numId").toInt();
        else if (xml.name() == u"abstractNumId" && numId >= 0)
            numbering.instances.insert(numId, attr(xml, "val").toInt());
        else if (xml.name() == u"numFmt" && abstractId >= 0) {
            const auto val = attr(xml, "val");
            auto style = QTextListFormat::ListDecimal;
            if (val == "bullet") style = QTextListFormat::ListDisc;
            else if (val == "lowerLetter") style = QTextListFormat::ListLowerAlpha;
            else if (val == "upperLetter") style = QTextListFormat::ListUpperAlpha;
            else if (val == "lowerRoman") style = QTextListFormat::ListLowerRoman;
            else if (val == "upperRoman") style = QTextListFormat::ListUpperRoman;
            numbering.levels.insert(QString::number(abstractId) + ":" + QString::number(level), style);
        }
    }
    return numbering;
}

#ifdef _WIN32
QByteArray readPart(IOpcFactory *factory, IOpcPartSet *parts, const wchar_t *name)
{
    ComPtr<IOpcPartUri> uri;
    ComPtr<IOpcPart> part;
    ComPtr<IStream> stream;
    if (FAILED(factory->CreatePartUri(name, &uri)) || FAILED(parts->GetPart(uri.Get(), &part)) ||
        FAILED(part->GetContentStream(&stream))) return {};
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > maxPartSize) return {};
    QByteArray data(static_cast<qsizetype>(stat.cbSize.QuadPart), Qt::Uninitialized);
    ULONG count = 0;
    if (FAILED(stream->Read(data.data(), static_cast<ULONG>(data.size()), &count)) ||
        count != static_cast<ULONG>(data.size())) return {};
    return data;
}
#else
// Read only the XML parts needed for text and common formatting.
QHash<QString, QByteArray> readParts(const QString &path)
{
    std::unique_ptr<archive, decltype(&archive_read_free)> file(archive_read_new(), archive_read_free);
    if (!file) return {};
    archive_read_support_format_zip(file.get());
    if (archive_read_open_filename(file.get(), path.toUtf8().constData(), 65536) != ARCHIVE_OK) return {};
    QHash<QString, QByteArray> parts;
    archive_entry *entry = nullptr;
    int status;
    while ((status = archive_read_next_header(file.get(), &entry)) == ARCHIVE_OK) {
        const auto name = QString::fromUtf8(archive_entry_pathname(entry));
        if (name != "word/document.xml" && name != "word/styles.xml" && name != "word/numbering.xml") continue;
        const auto size = archive_entry_size(entry);
        if (size < 0 || size > maxPartSize) return {};
        QByteArray data(static_cast<qsizetype>(size), Qt::Uninitialized);
        qsizetype offset = 0;
        while (offset < data.size()) {
            const auto count = archive_read_data(file.get(), data.data() + offset, data.size() - offset);
            if (count <= 0) return {};
            offset += count;
        }
        parts.insert(name, data);
    }
    if (status != ARCHIVE_EOF) return {};
    return parts;
}
#endif

DocxResult parseDocument(const QByteArray &data, const QByteArray &styleData, const QByteArray &listData)
{
    // Import text and common Word formatting without layout or images.
    QTextDocument document;
    document.setLayoutEnabled(false);
    document.setUndoRedoEnabled(false);
    QTextCharFormat defaults;
    const auto styles = readStyles(styleData, defaults);
    const auto numbering = readNumbering(listData);
    QTextCursor cursor(&document);
    // Build the document in one edit to avoid per-run cursor layout.
    cursor.beginEditBlock();
    QXmlStreamReader xml(data);
    QHash<QString, QTextList *> lists;
    Paragraph paragraph;
    QTextCharFormat run;
    bool first = true, simplified = false, inParagraph = false;
    while (!xml.atEnd()) {
        if (document.characterCount() - 1 > maxNoteCharacters || cursor.block().length() - 1 > maxParagraphCharacters || document.blockCount() > maxNoteParagraphs) {
            cursor.endEditBlock();
            return {{}, text("TextLimit"), false};
        }
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == u"del" || name == u"moveFrom") {
                xml.skipCurrentElement();
                continue;
            }
            if (name == u"p") {
                if (!first) cursor.insertBlock(QTextBlockFormat(), defaults);
                first = false;
                inParagraph = true;
                paragraph = {};
                paragraph.text = defaults;
                paragraph.text.merge(resolveStyle(styles, "Normal").text);
                run = paragraph.text;
                cursor.setCharFormat(run);
            } else if (name == u"pPr" && inParagraph) {
                const auto direct = readParagraphProperties(xml);
                paragraph = resolveStyle(styles, direct.style.isEmpty() ? "Normal" : direct.style);
                auto base = defaults;
                base.merge(paragraph.text);
                base.merge(direct.text);
                paragraph.text = base;
                paragraph.block.merge(direct.block);
                if (direct.numId >= 0) {
                    paragraph.numId = direct.numId;
                    paragraph.level = direct.level;
                }
                cursor.setBlockFormat(paragraph.block);
                cursor.setCharFormat(paragraph.text);
                if (paragraph.numId > 0) {
                    const auto key = QString::number(paragraph.numId) + ":" + QString::number(paragraph.level);
                    if (lists.contains(key)) lists[key]->add(cursor.block());
                    else {
                        QTextListFormat format;
                        const auto abstractId = numbering.instances.value(paragraph.numId, -1);
                        format.setStyle(numbering.levels.value(QString::number(abstractId) + ":" +
                            QString::number(paragraph.level), QTextListFormat::ListDecimal));
                        format.setIndent(paragraph.level + 1);
                        lists.insert(key, cursor.createList(format));
                    }
                }
            } else if (name == u"r" && inParagraph) {
                run = paragraph.text;
                cursor.setCharFormat(run);
            } else if (name == u"rPr" && inParagraph) {
                while (xml.readNextStartElement()) {
                    if (xml.name() == u"rStyle") run.merge(resolveStyle(styles, attr(xml, "val")).text);
                    else runProperty(xml, run);
                    xml.skipCurrentElement();
                }
                cursor.setCharFormat(run);
            } else if (name == u"t" && inParagraph) {
                const auto value = xml.readElementText();
                if (value.size() > maxNoteCharacters - document.characterCount() + 1 ||
                    value.size() > maxParagraphCharacters - cursor.block().length() + 1) {
                    cursor.endEditBlock();
                    return {{}, text("TextLimit"), false};
                }
                cursor.insertText(value, run);
            } else if (name == u"tab" && inParagraph) cursor.insertText("\t", run);
            else if ((name == u"br" || name == u"cr") && inParagraph) cursor.insertText(QString(QChar::LineSeparator), run);
            else if (name == u"drawing" || name == u"pict" || name == u"object") {
                simplified = true;
                xml.skipCurrentElement();
            } else if (name == u"tbl") simplified = true;
        } else if (xml.isEndElement() && xml.name() == u"p") inParagraph = false;
    }
    cursor.endEditBlock();
    if (xml.hasError()) return {{}, text("WordXmlError") + xml.errorString(), false};
    if (!withinTextLimits(document)) return {{}, text("TextLimit"), false};
    return {document.toHtml(), {}, simplified};
}
}

DocxResult importDocx(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > 100 * 1024 * 1024)
        return {{}, text("WordSizeError"), false};
#ifdef _WIN32
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Uninitialize {
        bool needed;
        ~Uninitialize() { if (needed) CoUninitialize(); }
    } cleanup{SUCCEEDED(initialized)};
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        return {{}, text("WordInitError"), false};
    ComPtr<IOpcFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IOpcPackage> package;
    ComPtr<IOpcPartSet> parts;
    if (FAILED(CoCreateInstance(__uuidof(OpcFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStreamOnFile(reinterpret_cast<LPCWSTR>(path.utf16()), OPC_STREAM_IO_READ,
            nullptr, FILE_ATTRIBUTE_NORMAL, &stream)) ||
        FAILED(factory->ReadPackageFromStream(stream.Get(), OPC_READ_DEFAULT, &package)) ||
        FAILED(package->GetPartSet(&parts)))
        return {{}, text("WordOpenError"), false};
    const auto document = readPart(factory.Get(), parts.Get(), L"/word/document.xml");
    if (document.isEmpty()) return {{}, text("WordBodyError"), false};
    return parseDocument(document, readPart(factory.Get(), parts.Get(), L"/word/styles.xml"),
        readPart(factory.Get(), parts.Get(), L"/word/numbering.xml"));
#else
    const auto parts = readParts(path);
    const auto document = parts.value("word/document.xml");
    if (document.isEmpty()) return {{}, text("WordBodyError"), false};
    return parseDocument(document, parts.value("word/styles.xml"), parts.value("word/numbering.xml"));
#endif
}
}
